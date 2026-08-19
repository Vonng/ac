#include "acdat_core.h"
#include "acdat_format.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

int
main(int argc, char **argv)
{
    uint32_t pattern_count = argc > 1 ? parse_u32(argv[1], "pattern count") :
        10000U;
    uint32_t pattern_length = argc > 2 ? parse_u32(argv[2], "pattern length") :
        16U;
    uint32_t input_megabytes = argc > 3 ? parse_u32(argv[3], "input MiB") :
        64U;
    AcdatPatternInput *patterns;
    uint8_t *pattern_storage;
    uint8_t *input;
    AcdatAutomaton *automaton = NULL;
    AcdatError error;
    AcdatStatus status;
    uint32_t serialized_size;
    uint64_t matches = 0;
    uint32_t random_state = 0xACDA7206U;
    uint32_t pattern_index;
    size_t input_size = (size_t) input_megabytes * 1024U * 1024U;
    double begin;
    double build_seconds;
    double scan_seconds;

    if (pattern_count == 0 || pattern_length < 8 || input_megabytes == 0)
    {
        fputs("pattern_count and input MiB must be positive; pattern length >= 8\n",
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

    for (pattern_index = 0; pattern_index < pattern_count; pattern_index++)
    {
        uint8_t *pattern = pattern_storage +
                           (size_t) pattern_index * pattern_length;
        uint32_t value = pattern_index;
        uint32_t byte_index;

        for (byte_index = 0; byte_index < 7; byte_index++)
        {
            pattern[byte_index] = (uint8_t) ('a' + value % 26U);
            value /= 26U;
        }
        for (; byte_index < pattern_length; byte_index++)
            pattern[byte_index] =
                (uint8_t) ('a' + next_random(&random_state) % 26U);
        patterns[pattern_index].pattern = pattern;
        patterns[pattern_index].byte_length = pattern_length;
        patterns[pattern_index].char_length = pattern_length;
        patterns[pattern_index].pattern_id = (int64_t) pattern_index + 1;
    }
    for (size_t index = 0; index < input_size; index++)
        input[index] = (uint8_t) ('a' + next_random(&random_state) % 26U);

    begin = monotonic_seconds();
    status = acdat_build(patterns, pattern_count, NULL, &automaton, &error);
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

    printf("patterns=%u pattern_bytes=%u states=%u slots=%u "
           "occupancy=%.3f artifact_bytes=%u build_seconds=%.6f "
           "input_mib=%u scan_seconds=%.6f scan_mib_s=%.2f matches=%" PRIu64
           "\n",
           pattern_count, pattern_length, automaton->state_count,
           automaton->slot_count,
           (double) automaton->state_count / automaton->slot_count,
           serialized_size, build_seconds, input_megabytes, scan_seconds,
           input_megabytes / scan_seconds, matches);

    acdat_free(automaton);
    free(patterns);
    free(pattern_storage);
    free(input);
    return EXIT_SUCCESS;
}
