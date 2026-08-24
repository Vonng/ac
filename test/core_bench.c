#include "acdat_core.h"
#include "acdat_format.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum BenchProfile
{
    BENCH_ASCII_MISS = 0,
    BENCH_ROOT_MISS,
    BENCH_ASCII_HIT,
    BENCH_UTF8_MISS,
    BENCH_OVERLAP
} BenchProfile;

static double
monotonic_seconds(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
    {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (double) value.tv_sec + (double) value.tv_nsec / 1000000000.0;
}

static uint32_t
next_random(uint32_t *state)
{
    uint32_t value = *state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static uint32_t
parse_u32(const char *value, const char *name)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX)
    {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        exit(EXIT_FAILURE);
    }
    return (uint32_t) parsed;
}

static uint64_t
parse_u64(const char *value, const char *name)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0')
    {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        exit(EXIT_FAILURE);
    }
    return (uint64_t) parsed;
}

static BenchProfile
parse_profile(const char *value)
{
    if (strcmp(value, "ascii-miss") == 0)
        return BENCH_ASCII_MISS;
    if (strcmp(value, "root-miss") == 0)
        return BENCH_ROOT_MISS;
    if (strcmp(value, "ascii-hit") == 0)
        return BENCH_ASCII_HIT;
    if (strcmp(value, "utf8-miss") == 0)
        return BENCH_UTF8_MISS;
    if (strcmp(value, "overlap") == 0)
        return BENCH_OVERLAP;

    fprintf(stderr, "unknown profile: %s\n", value);
    exit(EXIT_FAILURE);
}

static const char *
profile_name(BenchProfile profile)
{
    switch (profile)
    {
        case BENCH_ASCII_MISS:
            return "ascii-miss";
        case BENCH_ROOT_MISS:
            return "root-miss";
        case BENCH_ASCII_HIT:
            return "ascii-hit";
        case BENCH_UTF8_MISS:
            return "utf8-miss";
        case BENCH_OVERLAP:
            return "overlap";
    }
    return "unknown";
}

static void
write_utf8_three(uint8_t *destination, uint32_t codepoint)
{
    destination[0] = (uint8_t) (0xE0U | (codepoint >> 12));
    destination[1] = (uint8_t) (0x80U | ((codepoint >> 6) & 0x3FU));
    destination[2] = (uint8_t) (0x80U | (codepoint & 0x3FU));
}

static void
initialize_ascii_pattern(uint8_t *pattern, uint32_t pattern_length,
                         uint32_t pattern_index, uint32_t *random_state,
                         bool root_only)
{
    uint32_t value = pattern_index;
    uint32_t byte_index = 0;

    if (root_only)
        pattern[byte_index++] = (uint8_t) 'z';
    for (; byte_index < 8U && byte_index < pattern_length; byte_index++)
    {
        pattern[byte_index] = (uint8_t) ('a' + value % 26U);
        value /= 26U;
    }
    for (; byte_index < pattern_length; byte_index++)
        pattern[byte_index] =
            (uint8_t) ('a' + next_random(random_state) % 26U);
}

static void
initialize_patterns(AcdatPatternInput *patterns, uint8_t *storage,
                    uint32_t pattern_count, uint32_t pattern_length,
                    BenchProfile profile, uint32_t *random_state)
{
    uint32_t pattern_index;

    for (pattern_index = 0; pattern_index < pattern_count; pattern_index++)
    {
        uint8_t *pattern = storage +
                           (size_t) pattern_index * pattern_length;
        uint32_t byte_length = pattern_length;
        uint32_t char_length = pattern_length;

        if (profile == BENCH_UTF8_MISS)
        {
            uint32_t char_index;
            uint32_t value = pattern_index;

            for (char_index = 0; char_index < pattern_length / 3U;
                 char_index++)
            {
                uint32_t digit = value % 512U;

                value /= 512U;
                if (char_index >= 3U)
                    digit = next_random(random_state) % 2048U;
                write_utf8_three(pattern + char_index * 3U,
                                 0x4E00U + digit);
            }
            char_length = pattern_length / 3U;
        }
        else if (profile == BENCH_OVERLAP)
        {
            byte_length = pattern_index + 1U;
            char_length = byte_length;
            memset(pattern, 'a', byte_length);
        }
        else
            initialize_ascii_pattern(pattern, pattern_length, pattern_index,
                                     random_state,
                                     profile == BENCH_ROOT_MISS);

        patterns[pattern_index].pattern = pattern;
        patterns[pattern_index].byte_length = byte_length;
        patterns[pattern_index].char_length = char_length;
        patterns[pattern_index].pattern_id = (int64_t) pattern_index + 1;
    }
}

static void
initialize_input(uint8_t *input, size_t input_size,
                 const uint8_t *pattern_storage, uint32_t pattern_length,
                 BenchProfile profile, uint32_t *random_state)
{
    size_t input_index;

    if (profile == BENCH_ASCII_HIT)
    {
        for (input_index = 0; input_index < input_size; input_index++)
            input[input_index] = pattern_storage[input_index % pattern_length];
        return;
    }
    if (profile == BENCH_OVERLAP)
    {
        memset(input, 'a', input_size);
        return;
    }
    if (profile == BENCH_UTF8_MISS)
    {
        for (input_index = 0; input_index + 3U <= input_size;
             input_index += 3U)
            write_utf8_three(input + input_index,
                             0x5600U + next_random(random_state) % 2048U);
        while (input_index < input_size)
            input[input_index++] = (uint8_t) 'x';
        return;
    }

    for (input_index = 0; input_index < input_size; input_index++)
    {
        uint32_t alphabet = profile == BENCH_ROOT_MISS ? 25U : 26U;

        input[input_index] =
            (uint8_t) ('a' + next_random(random_state) % alphabet);
    }
}

static uint64_t
runtime_bytes(const AcdatAutomaton *automaton)
{
    uint64_t result = sizeof(*automaton);
    uint64_t slot_bytes =
        (uint64_t) automaton->slot_count * sizeof(uint32_t);

    if (automaton->base != NULL)
        result += slot_bytes;
    if (automaton->check != NULL)
        result += slot_bytes;
    if (automaton->fail != NULL)
        result += slot_bytes;
    if (automaton->terminal_head != NULL)
        result += slot_bytes;
    if (automaton->output_link != NULL)
        result += slot_bytes;
    if (automaton->root_next != NULL)
        result += 256U * sizeof(uint32_t);
    result += (uint64_t) automaton->pattern_count * sizeof(AcdatTerminal);
    result += automaton->replacement_bytes;
    return result;
}

int
main(int argc, char **argv)
{
    uint32_t pattern_count = argc > 1 ? parse_u32(argv[1], "pattern count") :
        10000U;
    uint32_t pattern_length = argc > 2 ? parse_u32(argv[2], "pattern length") :
        16U;
    uint32_t input_megabytes = argc > 3 ? parse_u32(argv[3], "input MiB") :
        64U;
    BenchProfile profile = argc > 4 ? parse_profile(argv[4]) :
        BENCH_ASCII_MISS;
    bool materialized = argc <= 5 || strcmp(argv[5], "materialized") == 0;
    uint64_t base_candidate_limit = argc > 6 ?
        parse_u64(argv[6], "base-candidate limit") : 0;
    AcdatPatternInput *patterns;
    uint8_t *pattern_storage;
    uint8_t *input;
    AcdatAutomaton *automaton = NULL;
    AcdatError error;
    AcdatStatus status;
    AcdatBuildControl build_control;
    AcdatBuildMetrics build_metrics;
    uint32_t serialized_size;
    uint8_t *serialized = NULL;
    AcdatMachineView view;
    AcdatFormatOptions format_options;
    uint64_t matches = 0;
    uint32_t random_state = 0xACDA7206U;
    size_t input_size = (size_t) input_megabytes * 1024U * 1024U;
    double begin;
    double build_seconds;
    double materialize_seconds = 0;
    double scan_seconds;

    if (pattern_count == 0 || pattern_length == 0 || input_megabytes == 0)
    {
        fputs("pattern count, pattern length, and input MiB must be positive\n",
              stderr);
        return EXIT_FAILURE;
    }
    if (argc > 5 && strcmp(argv[5], "materialized") != 0 &&
        strcmp(argv[5], "built") != 0)
    {
        fprintf(stderr, "unknown runtime mode: %s\n", argv[5]);
        return EXIT_FAILURE;
    }
    if ((profile == BENCH_ASCII_MISS || profile == BENCH_ROOT_MISS ||
         profile == BENCH_ASCII_HIT) && pattern_length < 8U)
    {
        fputs("ASCII benchmark pattern length must be at least 8\n", stderr);
        return EXIT_FAILURE;
    }
    if (profile == BENCH_UTF8_MISS &&
        (pattern_length < 6U || pattern_length % 3U != 0))
    {
        fputs("UTF-8 benchmark pattern length must be a multiple of 3 and at least 6\n",
              stderr);
        return EXIT_FAILURE;
    }
    if (profile == BENCH_OVERLAP && pattern_count > pattern_length)
    {
        fputs("overlap benchmark requires pattern count <= pattern length\n",
              stderr);
        return EXIT_FAILURE;
    }
    if ((size_t) pattern_count > SIZE_MAX / sizeof(*patterns) ||
        (size_t) pattern_count > SIZE_MAX / pattern_length)
    {
        fputs("benchmark allocation overflow\n", stderr);
        return EXIT_FAILURE;
    }

    patterns = calloc(pattern_count, sizeof(*patterns));
    pattern_storage = malloc((size_t) pattern_count * pattern_length);
    input = malloc(input_size);
    if (patterns == NULL || pattern_storage == NULL || input == NULL)
    {
        fputs("benchmark allocation failed\n", stderr);
        return EXIT_FAILURE;
    }

    initialize_patterns(patterns, pattern_storage, pattern_count,
                        pattern_length, profile, &random_state);
    initialize_input(input, input_size, pattern_storage, pattern_length,
                     profile, &random_state);

    begin = monotonic_seconds();
    memset(&build_control, 0, sizeof(build_control));
    build_control.base_candidate_limit = base_candidate_limit;
    status = acdat_build_ex(patterns, pattern_count, NULL, &build_control,
                            &build_metrics, &automaton, &error);
    build_seconds = monotonic_seconds() - begin;
    if (status != ACDAT_OK)
    {
        fprintf(stderr, "build failed: %s: %s\n",
                acdat_status_name(status), error.message);
        return EXIT_FAILURE;
    }
    status = acdat_serialized_size(automaton, &serialized_size, &error);
    if (status != ACDAT_OK)
    {
        fprintf(stderr, "size failed: %s\n", error.message);
        return EXIT_FAILURE;
    }
    if (materialized)
    {
        AcdatAutomaton *restored = NULL;

        serialized = malloc(serialized_size);
        if (serialized == NULL)
        {
            fputs("serialized benchmark allocation failed\n", stderr);
            return EXIT_FAILURE;
        }
        memset(&format_options, 0, sizeof(format_options));
        format_options.input_kind = ACDAT_INPUT_BYTEA;
        status = acdat_serialize(automaton, &format_options, serialized,
                                 serialized_size, &error);
        if (status != ACDAT_OK)
        {
            fprintf(stderr, "serialize failed: %s: %s\n",
                    acdat_status_name(status), error.message);
            return EXIT_FAILURE;
        }
        begin = monotonic_seconds();
        status = acdat_parse_machine(serialized, serialized_size, &view,
                                     &error);
        if (status == ACDAT_OK)
            status = acdat_materialize_machine(&view, NULL, &restored, &error);
        if (status == ACDAT_OK)
            status = acdat_prepare_runtime(restored, &error);
        materialize_seconds = monotonic_seconds() - begin;
        if (status != ACDAT_OK)
        {
            fprintf(stderr, "materialize failed: %s: %s\n",
                    acdat_status_name(status), error.message);
            return EXIT_FAILURE;
        }
        acdat_free(automaton);
        automaton = restored;
    }

    begin = monotonic_seconds();
    status = acdat_scan(automaton, input, input_size, NULL, NULL, 0,
                        &matches, &error);
    scan_seconds = monotonic_seconds() - begin;
    if (status != ACDAT_OK)
    {
        fprintf(stderr, "scan failed: %s: %s\n",
                acdat_status_name(status), error.message);
        return EXIT_FAILURE;
    }

    printf("profile=%s runtime=%s patterns=%u pattern_bytes=%u "
           "states=%u slots=%u occupancy=%.3f artifact_bytes=%u "
           "runtime_bytes=%" PRIu64 " build_seconds=%.6f "
           "materialize_seconds=%.6f build_work=%" PRIu64 " "
           "base_candidates=%" PRIu64 " slot_checks=%" PRIu64 " "
           "failure_steps=%" PRIu64 " "
           "input_mib=%u scan_seconds=%.6f scan_mib_s=%.2f matches=%" PRIu64
           "\n",
           profile_name(profile), materialized ? "materialized" : "built",
           pattern_count, pattern_length, automaton->state_count,
           automaton->slot_count,
           (double) automaton->state_count / automaton->slot_count,
           serialized_size, runtime_bytes(automaton), build_seconds,
           materialize_seconds, build_metrics.work_units,
           build_metrics.base_candidates, build_metrics.slot_checks,
           build_metrics.failure_steps, input_megabytes, scan_seconds,
           input_megabytes / scan_seconds, matches);

    acdat_free(automaton);
    free(patterns);
    free(pattern_storage);
    free(input);
    free(serialized);
    return EXIT_SUCCESS;
}
