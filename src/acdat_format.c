#include "acdat_format.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEADER_MAGIC_OFFSET 0U
#define HEADER_MAJOR_OFFSET 4U
#define HEADER_MINOR_OFFSET 6U
#define HEADER_SIZE_OFFSET 8U
#define HEADER_TOTAL_SIZE_OFFSET 12U
#define HEADER_ENGINE_OFFSET 16U
#define HEADER_INPUT_KIND_OFFSET 18U
#define HEADER_DATABASE_ENCODING_OFFSET 20U
#define HEADER_FLAGS_OFFSET 24U
#define HEADER_PATTERN_COUNT_OFFSET 28U
#define HEADER_STATE_COUNT_OFFSET 32U
#define HEADER_SLOT_COUNT_OFFSET 36U
#define HEADER_MAX_PATTERN_BYTES_OFFSET 40U
#define HEADER_MAX_PATTERN_CHARS_OFFSET 44U
#define HEADER_REPLACEMENT_BYTES_OFFSET 48U
#define HEADER_TERMINAL_SIZE_OFFSET 52U
#define HEADER_BASE_OFFSET 56U
#define HEADER_CHECK_OFFSET 60U
#define HEADER_FAIL_OFFSET 64U
#define HEADER_TERMINAL_HEAD_OFFSET 68U
#define HEADER_OUTPUT_LINK_OFFSET 72U
#define HEADER_TERMINALS_OFFSET 76U
#define HEADER_REPLACEMENTS_OFFSET 80U
#define HEADER_COMPILER_MAJOR_OFFSET 84U
#define HEADER_COMPILER_MINOR_OFFSET 86U
#define HEADER_COMPILER_PATCH_OFFSET 88U

typedef struct AcdatLayout
{
    uint32_t total_size;
    uint32_t base_offset;
    uint32_t check_offset;
    uint32_t fail_offset;
    uint32_t terminal_head_offset;
    uint32_t output_link_offset;
    uint32_t terminals_offset;
    uint32_t replacements_offset;
} AcdatLayout;

#if defined(__GNUC__) || defined(__clang__)
static void format_set_error(AcdatError *error, AcdatStatus code,
                             const char *format, ...)
    __attribute__((format(printf, 3, 4)));
#endif

static void
format_set_error(AcdatError *error, AcdatStatus code, const char *format, ...)
{
    va_list arguments;

    if (error == NULL)
        return;
    error->code = code;
    error->pattern_index = SIZE_MAX;
    va_start(arguments, format);
    (void) vsnprintf(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
}

static void
write_u16(uint8_t *pointer, uint16_t value)
{
    pointer[0] = (uint8_t) value;
    pointer[1] = (uint8_t) (value >> 8);
}

static void
write_u32(uint8_t *pointer, uint32_t value)
{
    pointer[0] = (uint8_t) value;
    pointer[1] = (uint8_t) (value >> 8);
    pointer[2] = (uint8_t) (value >> 16);
    pointer[3] = (uint8_t) (value >> 24);
}

static void
write_u64(uint8_t *pointer, uint64_t value)
{
    write_u32(pointer, (uint32_t) value);
    write_u32(pointer + 4, (uint32_t) (value >> 32));
}

static uint16_t
read_u16(const uint8_t *pointer)
{
    return (uint16_t) pointer[0] |
           (uint16_t) ((uint16_t) pointer[1] << 8);
}

uint32_t
acdat_format_read_u32(const uint8_t *pointer)
{
    return (uint32_t) pointer[0] |
           ((uint32_t) pointer[1] << 8) |
           ((uint32_t) pointer[2] << 16) |
           ((uint32_t) pointer[3] << 24);
}

uint64_t
acdat_format_read_u64(const uint8_t *pointer)
{
    return (uint64_t) acdat_format_read_u32(pointer) |
           ((uint64_t) acdat_format_read_u32(pointer + 4) << 32);
}

static bool
add_u64(uint64_t *value, uint64_t addition)
{
    if (*value > UINT64_MAX - addition)
        return false;
    *value += addition;
    return true;
}

static bool
checked_array_size(uint32_t count, size_t element_size, size_t *result)
{
    if (element_size != 0 && (size_t) count > SIZE_MAX / element_size)
        return false;
    *result = (size_t) count * element_size;
    return true;
}

static uint64_t
align_terminal_section(uint64_t value)
{
    uint64_t remainder = value & 7U;

    if (remainder <= 4U)
        return value + (4U - remainder);
    return value + (12U - remainder);
}

static AcdatStatus
compute_layout(uint32_t slot_count, uint32_t pattern_count,
               uint32_t replacement_bytes, AcdatLayout *layout,
               AcdatError *error)
{
    uint64_t position = ACDAT_FORMAT_HEADER_SIZE;
    uint64_t array_bytes = (uint64_t) slot_count * sizeof(uint32_t);
    uint64_t terminal_bytes =
        (uint64_t) pattern_count * ACDAT_FORMAT_TERMINAL_SIZE;

    layout->base_offset = (uint32_t) position;
    if (!add_u64(&position, array_bytes) || position > UINT32_MAX)
        goto overflow;
    layout->check_offset = (uint32_t) position;
    if (!add_u64(&position, array_bytes) || position > UINT32_MAX)
        goto overflow;
    layout->fail_offset = (uint32_t) position;
    if (!add_u64(&position, array_bytes) || position > UINT32_MAX)
        goto overflow;
    layout->terminal_head_offset = (uint32_t) position;
    if (!add_u64(&position, array_bytes) || position > UINT32_MAX)
        goto overflow;
    layout->output_link_offset = (uint32_t) position;
    if (!add_u64(&position, array_bytes) || position > UINT32_MAX)
        goto overflow;

    position = align_terminal_section(position);
    if (position > UINT32_MAX)
        goto overflow;
    layout->terminals_offset = (uint32_t) position;
    if (!add_u64(&position, terminal_bytes) || position > UINT32_MAX)
        goto overflow;
    layout->replacements_offset = (uint32_t) position;
    if (!add_u64(&position, replacement_bytes) || position > UINT32_MAX)
        goto overflow;
    layout->total_size = (uint32_t) position;
    return ACDAT_OK;

overflow:
    format_set_error(error, ACDAT_ERR_SIZE_OVERFLOW,
                     "serialized machine exceeds 4 GiB format limit");
    return ACDAT_ERR_SIZE_OVERFLOW;
}

AcdatStatus
acdat_serialized_size(const AcdatAutomaton *automaton, uint32_t *size,
                      AcdatError *error)
{
    AcdatLayout layout;
    AcdatStatus status;

    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (automaton == NULL || size == NULL)
    {
        format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                         "automaton or size pointer is NULL");
        return ACDAT_ERR_INVALID_ARGUMENT;
    }
    status = compute_layout(automaton->slot_count, automaton->pattern_count,
                            automaton->replacement_bytes, &layout, error);
    if (status == ACDAT_OK)
        *size = layout.total_size;
    return status;
}

AcdatStatus
acdat_serialize(const AcdatAutomaton *automaton,
                const AcdatFormatOptions *options, uint8_t *destination,
                size_t destination_size, AcdatError *error)
{
    AcdatLayout layout;
    AcdatStatus status;
    uint32_t index;

    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (automaton == NULL || options == NULL || destination == NULL)
    {
        format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                         "serialize argument is NULL");
        return ACDAT_ERR_INVALID_ARGUMENT;
    }
    if (options->input_kind != ACDAT_INPUT_TEXT &&
        options->input_kind != ACDAT_INPUT_BYTEA)
    {
        format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                         "unknown input kind %u", options->input_kind);
        return ACDAT_ERR_INVALID_ARGUMENT;
    }
    status = compute_layout(automaton->slot_count, automaton->pattern_count,
                            automaton->replacement_bytes, &layout, error);
    if (status != ACDAT_OK)
        return status;
    if (destination_size < layout.total_size)
    {
        format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                         "destination has %zu bytes, requires %u",
                         destination_size, layout.total_size);
        return ACDAT_ERR_INVALID_ARGUMENT;
    }

    memset(destination, 0, layout.total_size);
    write_u32(destination + HEADER_MAGIC_OFFSET, ACDAT_FORMAT_MAGIC);
    write_u16(destination + HEADER_MAJOR_OFFSET, ACDAT_FORMAT_MAJOR);
    write_u16(destination + HEADER_MINOR_OFFSET, ACDAT_FORMAT_MINOR);
    write_u32(destination + HEADER_SIZE_OFFSET, ACDAT_FORMAT_HEADER_SIZE);
    write_u32(destination + HEADER_TOTAL_SIZE_OFFSET, layout.total_size);
    write_u16(destination + HEADER_ENGINE_OFFSET, ACDAT_ENGINE_BYTE256);
    write_u16(destination + HEADER_INPUT_KIND_OFFSET, options->input_kind);
    write_u32(destination + HEADER_DATABASE_ENCODING_OFFSET,
              options->database_encoding);
    write_u32(destination + HEADER_FLAGS_OFFSET, options->flags);
    write_u32(destination + HEADER_PATTERN_COUNT_OFFSET,
              automaton->pattern_count);
    write_u32(destination + HEADER_STATE_COUNT_OFFSET,
              automaton->state_count);
    write_u32(destination + HEADER_SLOT_COUNT_OFFSET,
              automaton->slot_count);
    write_u32(destination + HEADER_MAX_PATTERN_BYTES_OFFSET,
              automaton->max_pattern_bytes);
    write_u32(destination + HEADER_MAX_PATTERN_CHARS_OFFSET,
              automaton->max_pattern_chars);
    write_u32(destination + HEADER_REPLACEMENT_BYTES_OFFSET,
              automaton->replacement_bytes);
    write_u32(destination + HEADER_TERMINAL_SIZE_OFFSET,
              ACDAT_FORMAT_TERMINAL_SIZE);
    write_u32(destination + HEADER_BASE_OFFSET, layout.base_offset);
    write_u32(destination + HEADER_CHECK_OFFSET, layout.check_offset);
    write_u32(destination + HEADER_FAIL_OFFSET, layout.fail_offset);
    write_u32(destination + HEADER_TERMINAL_HEAD_OFFSET,
              layout.terminal_head_offset);
    write_u32(destination + HEADER_OUTPUT_LINK_OFFSET,
              layout.output_link_offset);
    write_u32(destination + HEADER_TERMINALS_OFFSET,
              layout.terminals_offset);
    write_u32(destination + HEADER_REPLACEMENTS_OFFSET,
              layout.replacements_offset);
    write_u16(destination + HEADER_COMPILER_MAJOR_OFFSET,
              options->compiler_major);
    write_u16(destination + HEADER_COMPILER_MINOR_OFFSET,
              options->compiler_minor);
    write_u16(destination + HEADER_COMPILER_PATCH_OFFSET,
              options->compiler_patch);
    memcpy(destination + ACDAT_HEADER_SOURCE_FINGERPRINT_OFFSET,
           options->source_fingerprint, ACDAT_FINGERPRINT_SIZE);
    memcpy(destination + ACDAT_HEADER_BUILD_FINGERPRINT_OFFSET,
           options->build_fingerprint, ACDAT_FINGERPRINT_SIZE);
    memcpy(destination + ACDAT_HEADER_CHECKSUM_OFFSET,
           options->artifact_checksum, ACDAT_FINGERPRINT_SIZE);

    for (index = 0; index < automaton->slot_count; index++)
    {
        write_u32(destination + layout.base_offset + index * 4U,
                  automaton->base[index]);
        write_u32(destination + layout.check_offset + index * 4U,
                  automaton->check[index]);
        write_u32(destination + layout.fail_offset + index * 4U,
                  automaton->fail[index]);
        write_u32(destination + layout.terminal_head_offset + index * 4U,
                  automaton->terminal_head[index]);
        write_u32(destination + layout.output_link_offset + index * 4U,
                  automaton->output_link[index]);
    }

    for (index = 0; index < automaton->pattern_count; index++)
    {
        const AcdatTerminal *terminal = &automaton->terminals[index];
        uint8_t *record = destination + layout.terminals_offset +
                          index * ACDAT_FORMAT_TERMINAL_SIZE;

        write_u64(record, (uint64_t) terminal->pattern_id);
        write_u32(record + 8, terminal->byte_length);
        write_u32(record + 12, terminal->char_length);
        write_u32(record + 16, (uint32_t) terminal->priority);
        write_u32(record + 20, terminal->replacement_offset);
        write_u32(record + 24, terminal->replacement_length);
        write_u32(record + 28, terminal->canonical_ordinal);
        write_u32(record + 32, terminal->next_terminal);
        write_u32(record + 36, terminal->has_replacement ? 1U : 0U);
    }
    if (automaton->replacement_bytes > 0)
        memcpy(destination + layout.replacements_offset,
               automaton->replacement_arena, automaton->replacement_bytes);
    return ACDAT_OK;
}

static bool
view_array_value(const AcdatMachineView *view, uint32_t offset,
                 uint32_t index, uint32_t *value)
{
    uint64_t position = (uint64_t) offset + (uint64_t) index * 4U;

    if (position + 4U > view->total_size)
        return false;
    *value = acdat_format_read_u32(view->data + position);
    return true;
}

static AcdatStatus
validate_terminal_ownership(const AcdatMachineView *view, AcdatError *error)
{
    uint8_t *terminal_seen;
    uint8_t *ordinal_seen;
    uint32_t state;
    uint32_t index;

    if (view->pattern_count == 0)
        return ACDAT_OK;
    terminal_seen = calloc(view->pattern_count, sizeof(uint8_t));
    ordinal_seen = calloc(view->pattern_count, sizeof(uint8_t));
    if (terminal_seen == NULL || ordinal_seen == NULL)
    {
        free(terminal_seen);
        free(ordinal_seen);
        format_set_error(error, ACDAT_ERR_OUT_OF_MEMORY,
                         "unable to allocate terminal validation scratch space");
        return ACDAT_ERR_OUT_OF_MEMORY;
    }

    for (state = ACDAT_ROOT_STATE; state < view->slot_count; state++)
    {
        uint32_t terminal_handle = acdat_format_read_u32(
            view->data + view->terminal_head_offset + state * 4U);

        while (terminal_handle != 0)
        {
            const uint8_t *record;

            if (terminal_handle > view->pattern_count ||
                terminal_seen[terminal_handle - 1U] != 0)
            {
                free(terminal_seen);
                free(ordinal_seen);
                format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                                 "terminal %u is multiply referenced",
                                 terminal_handle);
                return ACDAT_ERR_INVALID_ARGUMENT;
            }
            terminal_seen[terminal_handle - 1U] = 1;
            record = view->data + view->terminals_offset +
                     (terminal_handle - 1U) * ACDAT_FORMAT_TERMINAL_SIZE;
            terminal_handle = acdat_format_read_u32(record + 32);
        }
    }

    for (index = 0; index < view->pattern_count; index++)
    {
        const uint8_t *record = view->data + view->terminals_offset +
                                index * ACDAT_FORMAT_TERMINAL_SIZE;
        uint32_t ordinal = acdat_format_read_u32(record + 28);

        if (terminal_seen[index] == 0 || ordinal >= view->pattern_count ||
            ordinal_seen[ordinal] != 0)
        {
            free(terminal_seen);
            free(ordinal_seen);
            format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                             "terminal ownership or ordinal is invalid at %u",
                             index);
            return ACDAT_ERR_INVALID_ARGUMENT;
        }
        ordinal_seen[ordinal] = 1;
    }

    free(terminal_seen);
    free(ordinal_seen);
    return ACDAT_OK;
}

static AcdatStatus
validate_state_links(const AcdatMachineView *view, AcdatError *error)
{
    uint8_t *fail_colors;
    uint8_t *output_colors;
    uint32_t state;

    fail_colors = calloc(view->slot_count, sizeof(uint8_t));
    output_colors = calloc(view->slot_count, sizeof(uint8_t));
    if (fail_colors == NULL || output_colors == NULL)
    {
        free(fail_colors);
        free(output_colors);
        format_set_error(error, ACDAT_ERR_OUT_OF_MEMORY,
                         "unable to allocate link validation scratch space");
        return ACDAT_ERR_OUT_OF_MEMORY;
    }
    fail_colors[ACDAT_ROOT_STATE] = 2;
    output_colors[ACDAT_ROOT_STATE] = 2;

    for (state = ACDAT_ROOT_STATE; state < view->slot_count; state++)
    {
        uint32_t check = acdat_format_read_u32(
            view->data + view->check_offset + state * 4U);
        uint32_t current;

        if (check == 0 || fail_colors[state] == 2)
            continue;
        current = state;
        while (current != ACDAT_ROOT_STATE && fail_colors[current] == 0)
        {
            fail_colors[current] = 1;
            current = acdat_format_read_u32(
                view->data + view->fail_offset + current * 4U);
        }
        if (current != ACDAT_ROOT_STATE && fail_colors[current] == 1)
        {
            free(fail_colors);
            free(output_colors);
            format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                             "failure links contain a cycle at state %u",
                             current);
            return ACDAT_ERR_INVALID_ARGUMENT;
        }
        current = state;
        while (current != ACDAT_ROOT_STATE && fail_colors[current] == 1)
        {
            uint32_t next = acdat_format_read_u32(
                view->data + view->fail_offset + current * 4U);

            fail_colors[current] = 2;
            current = next;
        }
    }

    for (state = ACDAT_ROOT_STATE + 1U; state < view->slot_count; state++)
    {
        uint32_t check = acdat_format_read_u32(
            view->data + view->check_offset + state * 4U);
        uint32_t current;

        if (check == 0 || output_colors[state] == 2)
            continue;
        current = state;
        while (current != 0 && output_colors[current] == 0)
        {
            output_colors[current] = 1;
            current = acdat_format_read_u32(
                view->data + view->output_link_offset + current * 4U);
        }
        if (current != 0 && output_colors[current] == 1)
        {
            free(fail_colors);
            free(output_colors);
            format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                             "output links contain a cycle at state %u",
                             current);
            return ACDAT_ERR_INVALID_ARGUMENT;
        }
        current = state;
        while (current != 0 && output_colors[current] == 1)
        {
            uint32_t next = acdat_format_read_u32(
                view->data + view->output_link_offset + current * 4U);

            output_colors[current] = 2;
            current = next;
        }
    }

    free(fail_colors);
    free(output_colors);
    return ACDAT_OK;
}

static AcdatStatus
validate_machine(const AcdatMachineView *view, AcdatError *error)
{
    AcdatLayout expected;
    AcdatStatus status;
    uint32_t index;
    uint32_t observed_states = 0;

    status = compute_layout(view->slot_count, view->pattern_count,
                            view->replacement_bytes, &expected, error);
    if (status != ACDAT_OK)
        return status;
    if (expected.total_size != view->total_size ||
        expected.base_offset != view->base_offset ||
        expected.check_offset != view->check_offset ||
        expected.fail_offset != view->fail_offset ||
        expected.terminal_head_offset != view->terminal_head_offset ||
        expected.output_link_offset != view->output_link_offset ||
        expected.terminals_offset != view->terminals_offset ||
        expected.replacements_offset != view->replacements_offset)
    {
        format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                         "machine section layout is not canonical");
        return ACDAT_ERR_INVALID_ARGUMENT;
    }
    if (view->slot_count <= ACDAT_ROOT_STATE ||
        view->state_count == 0 || view->state_count > view->slot_count)
    {
        format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                         "invalid state or slot count");
        return ACDAT_ERR_INVALID_ARGUMENT;
    }

    for (index = 0; index < view->slot_count; index++)
    {
        uint32_t check;
        uint32_t fail;
        uint32_t terminal_head;
        uint32_t output_link;

        if (!view_array_value(view, view->check_offset, index, &check) ||
            !view_array_value(view, view->fail_offset, index, &fail) ||
            !view_array_value(view, view->terminal_head_offset, index,
                              &terminal_head) ||
            !view_array_value(view, view->output_link_offset, index,
                              &output_link))
        {
            format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                             "machine array is truncated");
            return ACDAT_ERR_INVALID_ARGUMENT;
        }
        if (check != 0)
        {
            uint32_t parent_check;
            uint32_t failure_check;

            observed_states++;
            if (check >= view->slot_count || fail == 0 ||
                fail >= view->slot_count)
            {
                format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                                 "state %u has invalid check/fail reference",
                                 index);
                return ACDAT_ERR_INVALID_ARGUMENT;
            }
            parent_check = acdat_format_read_u32(
                view->data + view->check_offset + check * 4U);
            failure_check = acdat_format_read_u32(
                view->data + view->check_offset + fail * 4U);
            if (parent_check == 0 || failure_check == 0)
            {
                format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                                 "state %u references an unused state", index);
                return ACDAT_ERR_INVALID_ARGUMENT;
            }
        }
        else if (fail != 0 || terminal_head != 0 || output_link != 0)
        {
            format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                             "unused slot %u contains state metadata", index);
            return ACDAT_ERR_INVALID_ARGUMENT;
        }
        if (terminal_head > view->pattern_count ||
            output_link >= view->slot_count)
        {
            format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                             "state %u has invalid output reference", index);
            return ACDAT_ERR_INVALID_ARGUMENT;
        }
        if (output_link != 0)
        {
            uint32_t output_check = acdat_format_read_u32(
                view->data + view->check_offset + output_link * 4U);
            uint32_t output_terminal = acdat_format_read_u32(
                view->data + view->terminal_head_offset + output_link * 4U);

            if (output_check == 0 || output_terminal == 0)
            {
                format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                                 "state %u has invalid output link", index);
                return ACDAT_ERR_INVALID_ARGUMENT;
            }
        }
    }
    if (observed_states != view->state_count)
    {
        format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                         "state count mismatch: header %u, observed %u",
                         view->state_count, observed_states);
        return ACDAT_ERR_INVALID_ARGUMENT;
    }
    if (acdat_format_read_u32(view->data + view->check_offset + 4U) !=
            ACDAT_ROOT_STATE ||
        acdat_format_read_u32(view->data + view->fail_offset + 4U) !=
            ACDAT_ROOT_STATE)
    {
        format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                         "root state invariant failed");
        return ACDAT_ERR_INVALID_ARGUMENT;
    }

    status = validate_state_links(view, error);
    if (status != ACDAT_OK)
        return status;

    for (index = 0; index < view->pattern_count; index++)
    {
        const uint8_t *record = view->data + view->terminals_offset +
                                index * ACDAT_FORMAT_TERMINAL_SIZE;
        uint32_t byte_length = acdat_format_read_u32(record + 8);
        uint32_t replacement_offset = acdat_format_read_u32(record + 20);
        uint32_t replacement_length = acdat_format_read_u32(record + 24);
        uint32_t canonical_ordinal = acdat_format_read_u32(record + 28);
        uint32_t next_terminal = acdat_format_read_u32(record + 32);
        uint32_t terminal_flags = acdat_format_read_u32(record + 36);
        uint64_t replacement_end = (uint64_t) replacement_offset +
                                   replacement_length;

        if (byte_length == 0 || canonical_ordinal >= view->pattern_count ||
            (next_terminal != 0 &&
             (next_terminal <= index + 1U ||
              next_terminal > view->pattern_count)) ||
            (terminal_flags & ~1U) != 0 ||
            replacement_end > view->replacement_bytes ||
            ((terminal_flags & 1U) == 0 && replacement_length != 0))
        {
            format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                             "terminal %u is invalid", index);
            return ACDAT_ERR_INVALID_ARGUMENT;
        }
    }
    return validate_terminal_ownership(view, error);
}

AcdatStatus
acdat_peek_machine(const uint8_t *data, size_t size, AcdatMachineView *view,
                   AcdatError *error)
{
    uint32_t header_size;
    uint32_t terminal_size;

    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (data == NULL || view == NULL)
    {
        format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                         "machine data or view pointer is NULL");
        return ACDAT_ERR_INVALID_ARGUMENT;
    }
    if (size < ACDAT_FORMAT_HEADER_SIZE || size > UINT32_MAX)
    {
        format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                         "machine size %zu is invalid", size);
        return ACDAT_ERR_INVALID_ARGUMENT;
    }
    if (acdat_format_read_u32(data + HEADER_MAGIC_OFFSET) !=
        ACDAT_FORMAT_MAGIC)
    {
        format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                         "machine magic is invalid");
        return ACDAT_ERR_INVALID_ARGUMENT;
    }

    memset(view, 0, sizeof(*view));
    view->data = data;
    view->format_major = read_u16(data + HEADER_MAJOR_OFFSET);
    view->format_minor = read_u16(data + HEADER_MINOR_OFFSET);
    header_size = acdat_format_read_u32(data + HEADER_SIZE_OFFSET);
    view->total_size = acdat_format_read_u32(data + HEADER_TOTAL_SIZE_OFFSET);
    view->engine = read_u16(data + HEADER_ENGINE_OFFSET);
    view->input_kind = read_u16(data + HEADER_INPUT_KIND_OFFSET);
    view->compiler_major = read_u16(data + HEADER_COMPILER_MAJOR_OFFSET);
    view->compiler_minor = read_u16(data + HEADER_COMPILER_MINOR_OFFSET);
    view->compiler_patch = read_u16(data + HEADER_COMPILER_PATCH_OFFSET);
    view->database_encoding =
        acdat_format_read_u32(data + HEADER_DATABASE_ENCODING_OFFSET);
    view->flags = acdat_format_read_u32(data + HEADER_FLAGS_OFFSET);
    view->pattern_count =
        acdat_format_read_u32(data + HEADER_PATTERN_COUNT_OFFSET);
    view->state_count =
        acdat_format_read_u32(data + HEADER_STATE_COUNT_OFFSET);
    view->slot_count =
        acdat_format_read_u32(data + HEADER_SLOT_COUNT_OFFSET);
    view->max_pattern_bytes =
        acdat_format_read_u32(data + HEADER_MAX_PATTERN_BYTES_OFFSET);
    view->max_pattern_chars =
        acdat_format_read_u32(data + HEADER_MAX_PATTERN_CHARS_OFFSET);
    view->replacement_bytes =
        acdat_format_read_u32(data + HEADER_REPLACEMENT_BYTES_OFFSET);
    terminal_size =
        acdat_format_read_u32(data + HEADER_TERMINAL_SIZE_OFFSET);
    view->base_offset = acdat_format_read_u32(data + HEADER_BASE_OFFSET);
    view->check_offset = acdat_format_read_u32(data + HEADER_CHECK_OFFSET);
    view->fail_offset = acdat_format_read_u32(data + HEADER_FAIL_OFFSET);
    view->terminal_head_offset =
        acdat_format_read_u32(data + HEADER_TERMINAL_HEAD_OFFSET);
    view->output_link_offset =
        acdat_format_read_u32(data + HEADER_OUTPUT_LINK_OFFSET);
    view->terminals_offset =
        acdat_format_read_u32(data + HEADER_TERMINALS_OFFSET);
    view->replacements_offset =
        acdat_format_read_u32(data + HEADER_REPLACEMENTS_OFFSET);
    view->source_fingerprint =
        data + ACDAT_HEADER_SOURCE_FINGERPRINT_OFFSET;
    view->build_fingerprint =
        data + ACDAT_HEADER_BUILD_FINGERPRINT_OFFSET;
    view->artifact_checksum = data + ACDAT_HEADER_CHECKSUM_OFFSET;

    if (view->format_major != ACDAT_FORMAT_MAJOR ||
        view->format_minor > ACDAT_FORMAT_MINOR ||
        header_size != ACDAT_FORMAT_HEADER_SIZE ||
        view->total_size != size || terminal_size != ACDAT_FORMAT_TERMINAL_SIZE ||
        view->engine != ACDAT_ENGINE_BYTE256 ||
        (view->input_kind != ACDAT_INPUT_TEXT &&
         view->input_kind != ACDAT_INPUT_BYTEA))
    {
        format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                         "machine header version or kind is unsupported");
        return ACDAT_ERR_INVALID_ARGUMENT;
    }

    return ACDAT_OK;
}

AcdatStatus
acdat_parse_machine(const uint8_t *data, size_t size, AcdatMachineView *view,
                    AcdatError *error)
{
    AcdatStatus status = acdat_peek_machine(data, size, view, error);

    if (status != ACDAT_OK)
        return status;
    status = validate_machine(view, error);
    if (status != ACDAT_OK)
        memset(view, 0, sizeof(*view));
    return status;
}

static void *
format_alloc(const AcdatAllocator *allocator, size_t size)
{
    if (size == 0)
        size = 1;
    return allocator->alloc(allocator->context, size);
}

AcdatStatus
acdat_materialize_machine(const AcdatMachineView *view,
                          const AcdatAllocator *allocator_argument,
                          AcdatAutomaton **result, AcdatError *error)
{
    AcdatAllocator allocator;
    AcdatAutomaton *automaton;
    size_t slot_bytes;
    size_t terminal_bytes;
    uint32_t index;

    if (result != NULL)
        *result = NULL;
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (view == NULL || result == NULL)
    {
        format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                         "view or result pointer is NULL");
        return ACDAT_ERR_INVALID_ARGUMENT;
    }
    allocator = allocator_argument == NULL ?
                acdat_default_allocator() : *allocator_argument;
    if (allocator.alloc == NULL || allocator.realloc == NULL ||
        allocator.free == NULL)
    {
        format_set_error(error, ACDAT_ERR_INVALID_ARGUMENT,
                         "allocator callbacks are incomplete");
        return ACDAT_ERR_INVALID_ARGUMENT;
    }
    if (!checked_array_size(view->slot_count, sizeof(uint32_t), &slot_bytes) ||
        !checked_array_size(view->pattern_count, sizeof(AcdatTerminal),
                            &terminal_bytes))
    {
        format_set_error(error, ACDAT_ERR_SIZE_OVERFLOW,
                         "materialized machine exceeds addressable memory");
        return ACDAT_ERR_SIZE_OVERFLOW;
    }

    automaton = format_alloc(&allocator, sizeof(*automaton));
    if (automaton == NULL)
        goto out_of_memory;
    memset(automaton, 0, sizeof(*automaton));
    automaton->allocator = allocator;
    automaton->slot_count = view->slot_count;
    automaton->state_count = view->state_count;
    automaton->pattern_count = view->pattern_count;
    automaton->max_pattern_bytes = view->max_pattern_bytes;
    automaton->max_pattern_chars = view->max_pattern_chars;
    automaton->replacement_bytes = view->replacement_bytes;

#define ALLOC_MEMBER(member, bytes)                                             \
    do                                                                          \
    {                                                                           \
        automaton->member = format_alloc(&allocator, (bytes));                  \
        if (automaton->member == NULL)                                          \
            goto out_of_memory_with_automaton;                                  \
    } while (0)

    ALLOC_MEMBER(base, slot_bytes);
    ALLOC_MEMBER(check, slot_bytes);
    ALLOC_MEMBER(fail, slot_bytes);
    ALLOC_MEMBER(terminal_head, slot_bytes);
    ALLOC_MEMBER(output_link, slot_bytes);
    if (terminal_bytes > 0)
        ALLOC_MEMBER(terminals, terminal_bytes);
    if (view->replacement_bytes > 0)
        ALLOC_MEMBER(replacement_arena, view->replacement_bytes);
#undef ALLOC_MEMBER

    for (index = 0; index < view->slot_count; index++)
    {
        automaton->base[index] = acdat_format_read_u32(
            view->data + view->base_offset + index * 4U);
        automaton->check[index] = acdat_format_read_u32(
            view->data + view->check_offset + index * 4U);
        automaton->fail[index] = acdat_format_read_u32(
            view->data + view->fail_offset + index * 4U);
        automaton->terminal_head[index] = acdat_format_read_u32(
            view->data + view->terminal_head_offset + index * 4U);
        automaton->output_link[index] = acdat_format_read_u32(
            view->data + view->output_link_offset + index * 4U);
    }
    for (index = 0; index < view->pattern_count; index++)
    {
        const uint8_t *record = view->data + view->terminals_offset +
                                index * ACDAT_FORMAT_TERMINAL_SIZE;
        AcdatTerminal *terminal = &automaton->terminals[index];

        memset(terminal, 0, sizeof(*terminal));
        terminal->pattern_id = (int64_t) acdat_format_read_u64(record);
        terminal->byte_length = acdat_format_read_u32(record + 8);
        terminal->char_length = acdat_format_read_u32(record + 12);
        terminal->priority = (int32_t) acdat_format_read_u32(record + 16);
        terminal->replacement_offset = acdat_format_read_u32(record + 20);
        terminal->replacement_length = acdat_format_read_u32(record + 24);
        terminal->canonical_ordinal = acdat_format_read_u32(record + 28);
        terminal->next_terminal = acdat_format_read_u32(record + 32);
        terminal->has_replacement =
            (acdat_format_read_u32(record + 36) & 1U) != 0;
    }
    if (view->replacement_bytes > 0)
        memcpy(automaton->replacement_arena,
               view->data + view->replacements_offset,
               view->replacement_bytes);

    *result = automaton;
    return ACDAT_OK;

out_of_memory_with_automaton:
    acdat_free(automaton);
out_of_memory:
    format_set_error(error, ACDAT_ERR_OUT_OF_MEMORY,
                     "unable to materialize machine");
    return ACDAT_ERR_OUT_OF_MEMORY;
}

void
acdat_set_artifact_checksum(uint8_t *data,
                            const uint8_t checksum[ACDAT_FINGERPRINT_SIZE])
{
    if (data != NULL && checksum != NULL)
        memcpy(data + ACDAT_HEADER_CHECKSUM_OFFSET, checksum,
               ACDAT_FINGERPRINT_SIZE);
}

void
acdat_set_build_fingerprint(
    uint8_t *data,
    const uint8_t fingerprint[ACDAT_FINGERPRINT_SIZE])
{
    if (data != NULL && fingerprint != NULL)
        memcpy(data + ACDAT_HEADER_BUILD_FINGERPRINT_OFFSET, fingerprint,
               ACDAT_FINGERPRINT_SIZE);
}
