#ifndef ACDAT_CORE_H
#define ACDAT_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ACDAT_ROOT_STATE 1U
#define ACDAT_NO_INDEX 0U

typedef enum AcdatStatus
{
    ACDAT_OK = 0,
    ACDAT_ERR_INVALID_ARGUMENT,
    ACDAT_ERR_EMPTY_PATTERN,
    ACDAT_ERR_DUPLICATE_PATTERN_ID,
    ACDAT_ERR_OUT_OF_MEMORY,
    ACDAT_ERR_SIZE_OVERFLOW,
    ACDAT_ERR_BUILD_FAILED
} AcdatStatus;

typedef struct AcdatError
{
    AcdatStatus code;
    size_t pattern_index;
    char message[192];
} AcdatError;

typedef struct AcdatAllocator
{
    void *context;
    void *(*alloc)(void *context, size_t size);
    void *(*realloc)(void *context, void *pointer, size_t size);
    void (*free)(void *context, void *pointer);
} AcdatAllocator;

typedef struct AcdatPatternInput
{
    const uint8_t *pattern;
    uint32_t byte_length;
    uint32_t char_length;
    int64_t pattern_id;
    int32_t priority;
    const uint8_t *replacement;
    uint32_t replacement_length;
    bool has_replacement;
} AcdatPatternInput;

typedef struct AcdatTerminal
{
    int64_t pattern_id;
    uint32_t byte_length;
    uint32_t char_length;
    int32_t priority;
    uint32_t replacement_offset;
    uint32_t replacement_length;
    uint32_t canonical_ordinal;
    uint32_t next_terminal;
    bool has_replacement;
} AcdatTerminal;

typedef struct AcdatAutomaton
{
    AcdatAllocator allocator;
    uint32_t slot_count;
    uint32_t state_count;
    uint32_t pattern_count;
    uint32_t max_pattern_bytes;
    uint32_t max_pattern_chars;
    uint32_t replacement_bytes;

    uint32_t *base;
    uint32_t *check;
    uint32_t *fail;
    uint32_t *terminal_head;
    uint32_t *output_link;
    AcdatTerminal *terminals;
    uint8_t *replacement_arena;
} AcdatAutomaton;

typedef struct AcdatMatch
{
    int64_t pattern_id;
    uint32_t start_byte;
    uint32_t byte_length;
    uint32_t char_length;
    int32_t priority;
    uint32_t canonical_ordinal;
    uint32_t terminal_index;
} AcdatMatch;

typedef bool (*AcdatMatchCallback)(const AcdatMatch *match, void *context);
typedef void (*AcdatProgressCallback)(void *context);

AcdatAllocator acdat_default_allocator(void);

AcdatStatus acdat_build(const AcdatPatternInput *patterns,
                        size_t pattern_count,
                        const AcdatAllocator *allocator,
                        AcdatAutomaton **result,
                        AcdatError *error);

void acdat_free(AcdatAutomaton *automaton);

uint32_t acdat_direct_transition(const AcdatAutomaton *automaton,
                                 uint32_t state,
                                 uint8_t symbol);

bool acdat_contains(const AcdatAutomaton *automaton,
                    const uint8_t *input,
                    size_t input_length);

bool acdat_contains_ex(const AcdatAutomaton *automaton,
                       const uint8_t *input,
                       size_t input_length,
                       AcdatProgressCallback progress_callback,
                       void *progress_context);

AcdatStatus acdat_scan(const AcdatAutomaton *automaton,
                       const uint8_t *input,
                       size_t input_length,
                       AcdatMatchCallback callback,
                       void *callback_context,
                       uint64_t max_matches,
                       uint64_t *match_count,
                       AcdatError *error);

AcdatStatus acdat_scan_ex(const AcdatAutomaton *automaton,
                          const uint8_t *input,
                          size_t input_length,
                          AcdatMatchCallback callback,
                          void *callback_context,
                          uint64_t max_matches,
                          uint64_t *match_count,
                          AcdatProgressCallback progress_callback,
                          void *progress_context,
                          AcdatError *error);

const char *acdat_status_name(AcdatStatus status);

#ifdef __cplusplus
}
#endif

#endif
