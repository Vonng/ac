#include "acdat_core.h"
#include "acdat_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))

#define CHECK(expression)                                                     \
    do                                                                        \
    {                                                                         \
        if (!(expression))                                                    \
        {                                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                 \
                    __FILE__, __LINE__, #expression);                         \
            exit(EXIT_FAILURE);                                               \
        }                                                                     \
    } while (0)

typedef struct MatchList
{
    AcdatMatch matches[4096];
    size_t count;
} MatchList;

static bool
collect_match(const AcdatMatch *match, void *context)
{
    MatchList *list = (MatchList *) context;

    CHECK(list->count < ARRAY_LENGTH(list->matches));
    list->matches[list->count++] = *match;
    return true;
}

static void
test_write_u32(uint8_t *pointer, uint32_t value)
{
    pointer[0] = (uint8_t) value;
    pointer[1] = (uint8_t) (value >> 8);
    pointer[2] = (uint8_t) (value >> 16);
    pointer[3] = (uint8_t) (value >> 24);
}

static AcdatAutomaton *
build_or_die(const AcdatPatternInput *patterns, size_t count)
{
    AcdatAutomaton *automaton = NULL;
    AcdatError error;
    AcdatStatus status = acdat_build(patterns, count, NULL, &automaton, &error);

    if (status != ACDAT_OK)
    {
        fprintf(stderr, "build failed: %s: %s\n",
                acdat_status_name(status), error.message);
        exit(EXIT_FAILURE);
    }
    CHECK(automaton != NULL);
    return automaton;
}

static void
test_classic_failure_links(void)
{
    static const AcdatPatternInput patterns[] = {
        {(const uint8_t *) "he", 2, 2, 1, 0, NULL, 0, false},
        {(const uint8_t *) "she", 3, 3, 2, 0, NULL, 0, false},
        {(const uint8_t *) "his", 3, 3, 3, 0, NULL, 0, false},
        {(const uint8_t *) "hers", 4, 4, 4, 0, NULL, 0, false}
    };
    static const uint8_t input[] = "ushers";
    AcdatAutomaton *automaton = build_or_die(patterns, ARRAY_LENGTH(patterns));
    MatchList list = {0};
    AcdatError error;
    uint64_t count = 0;
    AcdatStatus status;

    CHECK(automaton->state_count == 10);
    CHECK(automaton->pattern_count == 4);
    CHECK(acdat_contains(automaton, input, sizeof(input) - 1));
    CHECK(!acdat_contains(automaton, (const uint8_t *) "xyz", 3));

    status = acdat_scan(automaton, input, sizeof(input) - 1,
                        collect_match, &list, 100, &count, &error);
    CHECK(status == ACDAT_OK);
    CHECK(count == 3);
    CHECK(list.count == 3);

    CHECK(list.matches[0].pattern_id == 2);
    CHECK(list.matches[0].start_byte == 2);
    CHECK(list.matches[0].byte_length == 3);
    CHECK(list.matches[1].pattern_id == 1);
    CHECK(list.matches[1].start_byte == 3);
    CHECK(list.matches[1].byte_length == 2);
    CHECK(list.matches[2].pattern_id == 4);
    CHECK(list.matches[2].start_byte == 3);
    CHECK(list.matches[2].byte_length == 4);
    acdat_free(automaton);
}

static void
test_overlapping_matches_and_limit(void)
{
    static const AcdatPatternInput patterns[] = {
        {(const uint8_t *) "a", 1, 1, 10, 0, NULL, 0, false},
        {(const uint8_t *) "aa", 2, 2, 11, 0, NULL, 0, false},
        {(const uint8_t *) "aaa", 3, 3, 12, 0, NULL, 0, false}
    };
    AcdatAutomaton *automaton = build_or_die(patterns, ARRAY_LENGTH(patterns));
    MatchList list = {0};
    AcdatError error;
    uint64_t count = 0;
    AcdatStatus status;

    status = acdat_scan(automaton, (const uint8_t *) "aaa", 3,
                        collect_match, &list, 100, &count, &error);
    CHECK(status == ACDAT_OK);
    CHECK(count == 6);
    CHECK(list.count == 6);

    count = 0;
    status = acdat_scan(automaton, (const uint8_t *) "aaa", 3,
                        NULL, NULL, 5, &count, &error);
    CHECK(status == ACDAT_ERR_SIZE_OVERFLOW);
    CHECK(count == 5);
    CHECK(strstr(error.message, "limit 5") != NULL);
    acdat_free(automaton);
}

static void
test_utf8_bytes(void)
{
    static const char virus[] = "病毒";
    static const char signature[] = "特征码";
    static const char input[] = "发现病毒特征码";
    static const AcdatPatternInput patterns[] = {
        {(const uint8_t *) virus, sizeof(virus) - 1, 2, 20, 0,
         (const uint8_t *) "[V]", 3, true},
        {(const uint8_t *) signature, sizeof(signature) - 1, 3, 21, 0,
         (const uint8_t *) "[S]", 3, true}
    };
    AcdatAutomaton *automaton = build_or_die(patterns, ARRAY_LENGTH(patterns));
    MatchList list = {0};
    AcdatError error;
    uint64_t count = 0;
    AcdatStatus status;

    CHECK(automaton->max_pattern_bytes == 9);
    CHECK(automaton->max_pattern_chars == 3);
    CHECK(automaton->replacement_bytes == 6);
    CHECK(acdat_contains(automaton, (const uint8_t *) input,
                         sizeof(input) - 1));

    status = acdat_scan(automaton, (const uint8_t *) input,
                        sizeof(input) - 1, collect_match, &list,
                        100, &count, &error);
    CHECK(status == ACDAT_OK);
    CHECK(count == 2);
    CHECK(list.matches[0].pattern_id == 20);
    CHECK(list.matches[0].start_byte == 7);
    CHECK(list.matches[0].byte_length == 6);
    CHECK(list.matches[0].char_length == 2);
    CHECK(list.matches[1].pattern_id == 21);
    CHECK(list.matches[1].start_byte == 13);
    CHECK(list.matches[1].byte_length == 9);
    CHECK(list.matches[1].char_length == 3);
    acdat_free(automaton);
}

static void
test_validation_errors(void)
{
    static const AcdatPatternInput duplicate_ids[] = {
        {(const uint8_t *) "a", 1, 1, 7, 0, NULL, 0, false},
        {(const uint8_t *) "b", 1, 1, 7, 0, NULL, 0, false}
    };
    static const AcdatPatternInput empty_pattern[] = {
        {(const uint8_t *) "", 0, 0, 8, 0, NULL, 0, false}
    };
    AcdatAutomaton *automaton = NULL;
    AcdatError error;
    AcdatStatus status;

    status = acdat_build(duplicate_ids, ARRAY_LENGTH(duplicate_ids), NULL,
                         &automaton, &error);
    CHECK(status == ACDAT_ERR_DUPLICATE_PATTERN_ID);
    CHECK(automaton == NULL);
    CHECK(error.pattern_index == 1);

    status = acdat_build(empty_pattern, ARRAY_LENGTH(empty_pattern), NULL,
                         &automaton, &error);
    CHECK(status == ACDAT_ERR_EMPTY_PATTERN);
    CHECK(automaton == NULL);
    CHECK(error.pattern_index == 0);
}

static void
test_empty_automaton(void)
{
    AcdatAutomaton *automaton = build_or_die(NULL, 0);
    uint64_t count = 123;
    AcdatError error;

    CHECK(automaton->state_count == 1);
    CHECK(automaton->pattern_count == 0);
    CHECK(!acdat_contains(automaton, (const uint8_t *) "anything", 8));
    CHECK(acdat_scan(automaton, (const uint8_t *) "anything", 8,
                     NULL, NULL, 0, &count, &error) == ACDAT_OK);
    CHECK(count == 0);
    acdat_free(automaton);
}

static void
compare_automata(const AcdatAutomaton *left, const AcdatAutomaton *right)
{
    size_t slot_bytes;
    size_t terminal_bytes;

    CHECK(left->slot_count == right->slot_count);
    CHECK(left->state_count == right->state_count);
    CHECK(left->pattern_count == right->pattern_count);
    CHECK(left->replacement_bytes == right->replacement_bytes);
    slot_bytes = (size_t) left->slot_count * sizeof(uint32_t);
    terminal_bytes = (size_t) left->pattern_count * sizeof(AcdatTerminal);
    CHECK(memcmp(left->base, right->base, slot_bytes) == 0);
    CHECK(memcmp(left->check, right->check, slot_bytes) == 0);
    CHECK(memcmp(left->fail, right->fail, slot_bytes) == 0);
    CHECK(memcmp(left->terminal_head, right->terminal_head, slot_bytes) == 0);
    CHECK(memcmp(left->output_link, right->output_link, slot_bytes) == 0);
    CHECK(memcmp(left->terminals, right->terminals, terminal_bytes) == 0);
    CHECK(memcmp(left->replacement_arena, right->replacement_arena,
                 left->replacement_bytes) == 0);
}

static void
test_deterministic_build(void)
{
    static const AcdatPatternInput ordered[] = {
        {(const uint8_t *) "alpha", 5, 5, 101, 2,
         (const uint8_t *) "A", 1, true},
        {(const uint8_t *) "alphabet", 8, 8, 102, 1,
         (const uint8_t *) "AB", 2, true},
        {(const uint8_t *) "beta", 4, 4, 103, 3,
         NULL, 0, false}
    };
    static const AcdatPatternInput shuffled[] = {
        {(const uint8_t *) "beta", 4, 4, 103, 3,
         NULL, 0, false},
        {(const uint8_t *) "alphabet", 8, 8, 102, 1,
         (const uint8_t *) "AB", 2, true},
        {(const uint8_t *) "alpha", 5, 5, 101, 2,
         (const uint8_t *) "A", 1, true}
    };
    AcdatAutomaton *left = build_or_die(ordered, ARRAY_LENGTH(ordered));
    AcdatAutomaton *right = build_or_die(shuffled, ARRAY_LENGTH(shuffled));

    compare_automata(left, right);
    acdat_free(left);
    acdat_free(right);
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

static void
test_random_differential(void)
{
    enum { PATTERN_COUNT = 16, INPUT_LENGTH = 64, ROUNDS = 200 };
    uint32_t random_state = 0xACDA7206U;
    uint32_t round;

    for (round = 0; round < ROUNDS; round++)
    {
        uint8_t pattern_bytes[PATTERN_COUNT][6];
        AcdatPatternInput patterns[PATTERN_COUNT];
        uint8_t input[INPUT_LENGTH];
        bool expected[PATTERN_COUNT][INPUT_LENGTH];
        bool observed[PATTERN_COUNT][INPUT_LENGTH];
        AcdatAutomaton *automaton;
        MatchList list = {0};
        AcdatError error;
        uint64_t count = 0;
        uint32_t pattern_index;
        uint32_t input_index;
        AcdatStatus status;

        memset(patterns, 0, sizeof(patterns));
        memset(expected, 0, sizeof(expected));
        memset(observed, 0, sizeof(observed));
        for (pattern_index = 0; pattern_index < PATTERN_COUNT; pattern_index++)
        {
            uint32_t length = next_random(&random_state) % 6U + 1U;
            uint32_t byte_index;

            for (byte_index = 0; byte_index < length; byte_index++)
                pattern_bytes[pattern_index][byte_index] =
                    (uint8_t) ('a' + next_random(&random_state) % 4U);
            patterns[pattern_index].pattern = pattern_bytes[pattern_index];
            patterns[pattern_index].byte_length = length;
            patterns[pattern_index].char_length = length;
            patterns[pattern_index].pattern_id =
                (uint64_t) round * PATTERN_COUNT + pattern_index + 1U;
            patterns[pattern_index].priority = (int32_t) pattern_index;
        }
        for (input_index = 0; input_index < INPUT_LENGTH; input_index++)
            input[input_index] =
                (uint8_t) ('a' + next_random(&random_state) % 4U);

        for (pattern_index = 0; pattern_index < PATTERN_COUNT; pattern_index++)
        {
            uint32_t length = patterns[pattern_index].byte_length;
            uint32_t start;

            for (start = 0; start + length <= INPUT_LENGTH; start++)
            {
                if (memcmp(input + start, patterns[pattern_index].pattern,
                           length) == 0)
                    expected[pattern_index][start] = true;
            }
        }

        automaton = build_or_die(patterns, PATTERN_COUNT);
        status = acdat_scan(automaton, input, INPUT_LENGTH, collect_match,
                            &list, ARRAY_LENGTH(list.matches), &count, &error);
        CHECK(status == ACDAT_OK);
        CHECK(count == list.count);

        for (input_index = 0; input_index < list.count; input_index++)
        {
            const AcdatMatch *match = &list.matches[input_index];
            int64_t first_id = (int64_t) round * PATTERN_COUNT + 1;
            int64_t offset;
            uint32_t start;

            CHECK(match->pattern_id >= first_id);
            offset = match->pattern_id - first_id;
            CHECK(offset >= 0 && offset < PATTERN_COUNT);
            CHECK(match->start_byte > 0);
            start = match->start_byte - 1U;
            CHECK(start < INPUT_LENGTH);
            CHECK(!observed[(size_t) offset][start]);
            observed[(size_t) offset][start] = true;
        }

        CHECK(memcmp(expected, observed, sizeof(expected)) == 0);
        CHECK(acdat_contains(automaton, input, INPUT_LENGTH) == (count > 0));
        acdat_free(automaton);
    }
}

static void
test_format_round_trip(void)
{
    static const AcdatPatternInput patterns[] = {
        {(const uint8_t *) "alpha", 5, 5, 501, 2,
         (const uint8_t *) "A", 1, true},
        {(const uint8_t *) "alphabet", 8, 8, 502, 1,
         (const uint8_t *) "AB", 2, true},
        {(const uint8_t *) "beta", 4, 4, 503, 3,
         NULL, 0, false}
    };
    AcdatAutomaton *source = build_or_die(patterns, ARRAY_LENGTH(patterns));
    AcdatAutomaton *restored = NULL;
    AcdatFormatOptions options;
    AcdatMachineView view;
    AcdatError error;
    AcdatStatus status;
    uint8_t *serialized;
    uint32_t serialized_size;
    uint32_t index;

    memset(&options, 0, sizeof(options));
    options.input_kind = ACDAT_INPUT_TEXT;
    options.compiler_major = 0;
    options.compiler_minor = 1;
    options.compiler_patch = 0;
    options.database_encoding = 6;
    for (index = 0; index < ACDAT_FINGERPRINT_SIZE; index++)
    {
        options.source_fingerprint[index] = (uint8_t) index;
        options.build_fingerprint[index] = (uint8_t) (index + 32U);
        options.artifact_checksum[index] = (uint8_t) (255U - index);
    }

    status = acdat_serialized_size(source, &serialized_size, &error);
    CHECK(status == ACDAT_OK);
    CHECK(serialized_size > ACDAT_FORMAT_HEADER_SIZE);
    serialized = malloc(serialized_size);
    CHECK(serialized != NULL);
    status = acdat_serialize(source, &options, serialized, serialized_size,
                             &error);
    CHECK(status == ACDAT_OK);
    status = acdat_parse_machine(serialized, serialized_size, &view, &error);
    CHECK(status == ACDAT_OK);
    CHECK(view.pattern_count == ARRAY_LENGTH(patterns));
    CHECK(view.input_kind == ACDAT_INPUT_TEXT);
    CHECK(view.compiler_major == 0);
    CHECK(view.compiler_minor == 1);
    CHECK(view.compiler_patch == 0);
    CHECK(view.database_encoding == 6);
    CHECK(memcmp(view.source_fingerprint, options.source_fingerprint,
                 ACDAT_FINGERPRINT_SIZE) == 0);
    CHECK(memcmp(view.build_fingerprint, options.build_fingerprint,
                 ACDAT_FINGERPRINT_SIZE) == 0);
    CHECK(memcmp(view.artifact_checksum, options.artifact_checksum,
                 ACDAT_FINGERPRINT_SIZE) == 0);

    status = acdat_materialize_machine(&view, NULL, &restored, &error);
    CHECK(status == ACDAT_OK);
    compare_automata(source, restored);
    CHECK(acdat_contains(restored, (const uint8_t *) "the alphabet", 12));

    acdat_free(restored);
    acdat_free(source);
    free(serialized);
}

static void
test_format_rejects_corruption(void)
{
    static const AcdatPatternInput patterns[] = {
        {(const uint8_t *) "abc", 3, 3, 601, 0, NULL, 0, false}
    };
    AcdatAutomaton *automaton = build_or_die(patterns, ARRAY_LENGTH(patterns));
    AcdatFormatOptions options;
    AcdatMachineView view;
    AcdatError error;
    uint8_t *serialized;
    uint32_t serialized_size;

    memset(&options, 0, sizeof(options));
    options.input_kind = ACDAT_INPUT_BYTEA;
    CHECK(acdat_serialized_size(automaton, &serialized_size, &error) ==
          ACDAT_OK);
    serialized = malloc(serialized_size);
    CHECK(serialized != NULL);
    CHECK(acdat_serialize(automaton, &options, serialized, serialized_size,
                          &error) == ACDAT_OK);

    serialized[0] ^= 0xFFU;
    CHECK(acdat_parse_machine(serialized, serialized_size, &view, &error) ==
          ACDAT_ERR_INVALID_ARGUMENT);
    serialized[0] ^= 0xFFU;

    serialized[12] = 0;
    serialized[13] = 0;
    serialized[14] = 0;
    serialized[15] = 0;
    CHECK(acdat_parse_machine(serialized, serialized_size, &view, &error) ==
          ACDAT_ERR_INVALID_ARGUMENT);

    CHECK(acdat_serialize(automaton, &options, serialized, serialized_size,
                          &error) == ACDAT_OK);
    CHECK(acdat_parse_machine(serialized, serialized_size, &view, &error) ==
          ACDAT_OK);
    {
        uint32_t state;

        for (state = ACDAT_ROOT_STATE + 1U; state < view.slot_count; state++)
        {
            if (acdat_format_read_u32(serialized + view.check_offset +
                                      state * 4U) != 0)
            {
                test_write_u32(serialized + view.fail_offset + state * 4U,
                               state);
                break;
            }
        }
        CHECK(state < view.slot_count);
        CHECK(acdat_parse_machine(serialized, serialized_size, &view, &error) ==
              ACDAT_ERR_INVALID_ARGUMENT);
        CHECK(strstr(error.message, "cycle") != NULL);
    }

    free(serialized);
    acdat_free(automaton);
}

int
main(void)
{
    test_classic_failure_links();
    test_overlapping_matches_and_limit();
    test_utf8_bytes();
    test_validation_errors();
    test_empty_automaton();
    test_deterministic_build();
    test_random_differential();
    test_format_round_trip();
    test_format_rejects_corruption();
    puts("acdat core tests: ok");
    return EXIT_SUCCESS;
}
