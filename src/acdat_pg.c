#include "postgres.h"

#include "acdat_core.h"
#include "acdat_format.h"

#include "catalog/pg_type_d.h"
#include "common/cryptohash.h"
#include "common/sha2.h"
#include "access/detoast.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"
#include "utils/varlena.h"

#include <stdlib.h>

PG_MODULE_MAGIC;

#define ACDAT_COMPILER_VERSION_MAJOR 0U
#define ACDAT_COMPILER_VERSION_MINOR 2U
#define ACDAT_COMPILER_VERSION_PATCH 0U
#define ACDAT_MIN_BASE_CANDIDATES 1048576ULL
#define ACDAT_BASE_CANDIDATES_PER_PATTERN_BYTE 128ULL

static void check_postgres_interrupts(void *context);

static uint64
default_base_candidate_limit(const AcdatPatternInput *patterns,
                             uint32 pattern_count)
{
    uint64 limit = ACDAT_MIN_BASE_CANDIDATES;
    uint32 index;

    for (index = 0; index < pattern_count; index++)
    {
        uint64 bytes = patterns[index].byte_length;

        if (bytes > (UINT64_MAX - limit) /
                    ACDAT_BASE_CANDIDATES_PER_PATTERN_BYTE)
            return UINT64_MAX;
        limit += bytes * ACDAT_BASE_CANDIDATES_PER_PATTERN_BYTE;
    }
    return limit;
}

typedef struct PgPatternWork
{
    AcdatPatternInput pattern;
} PgPatternWork;

typedef struct PgMachineCache
{
    MemoryContext automaton_context;
    AcdatAutomaton *automaton;
    uint32 payload_size;
    uint16 input_kind;
    uint32 database_encoding;
    uint8 build_fingerprint[ACDAT_FINGERPRINT_SIZE];
    uint8 artifact_checksum[ACDAT_FINGERPRINT_SIZE];
    bool has_external_key;
    varatt_external external_key;
} PgMachineCache;

typedef struct PgCompileAggregateState
{
    MemoryContext context;
    uint16 input_kind;
    uint32 pattern_count;
    uint32 pattern_capacity;
    AcdatPatternInput *patterns;
} PgCompileAggregateState;

typedef enum PgMatchPolicy
{
    PG_MATCH_ALL_OVERLAPPING,
    PG_MATCH_LEFTMOST_LONGEST,
    PG_MATCH_LEFTMOST_PRIORITY
} PgMatchPolicy;

typedef struct PgCollectedMatch
{
    AcdatMatch match;
    uint32 start_char;
} PgCollectedMatch;

typedef struct PgMatchCollector
{
    const uint8 *input;
    uint32 input_length;
    bool text_input;
    uint32 scanned_bytes;
    uint32 scanned_chars;
    PgCollectedMatch *matches;
    uint32 match_count;
    uint32 match_capacity;
} PgMatchCollector;

static void
raise_core_error(const char *operation, AcdatStatus status,
                 const AcdatError *error)
{
    int sqlstate = ERRCODE_DATA_EXCEPTION;

    switch (status)
    {
        case ACDAT_ERR_INVALID_ARGUMENT:
        case ACDAT_ERR_EMPTY_PATTERN:
        case ACDAT_ERR_DUPLICATE_PATTERN_ID:
            sqlstate = ERRCODE_INVALID_PARAMETER_VALUE;
            break;
        case ACDAT_ERR_OUT_OF_MEMORY:
            sqlstate = ERRCODE_OUT_OF_MEMORY;
            break;
        case ACDAT_ERR_SIZE_OVERFLOW:
            sqlstate = ERRCODE_PROGRAM_LIMIT_EXCEEDED;
            break;
        case ACDAT_ERR_BUILD_FAILED:
        case ACDAT_OK:
            sqlstate = ERRCODE_DATA_EXCEPTION;
            break;
    }

    ereport(ERROR,
            (errcode(sqlstate),
             errmsg("acdat %s failed: %s", operation,
                    error != NULL && error->message[0] != '\0' ?
                    error->message : acdat_status_name(status))));
}

static void *
pg_allocator_alloc(void *context, size_t size)
{
    return MemoryContextAlloc((MemoryContext) context, size);
}

static void *
pg_allocator_realloc(void *context, void *pointer, size_t size)
{
    (void) context;
    return repalloc(pointer, size);
}

static void
pg_allocator_free(void *context, void *pointer)
{
    (void) context;
    pfree(pointer);
}

static AcdatAllocator
pg_allocator(MemoryContext context)
{
    AcdatAllocator allocator;

    allocator.context = context;
    allocator.alloc = pg_allocator_alloc;
    allocator.realloc = pg_allocator_realloc;
    allocator.free = pg_allocator_free;
    return allocator;
}

static pg_cryptohash_ctx *
sha256_create(void)
{
    pg_cryptohash_ctx *context = pg_cryptohash_create(PG_SHA256);

    if (context == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("acdat could not create SHA-256 context")));
    if (pg_cryptohash_init(context) < 0)
    {
        pg_cryptohash_free(context);
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("acdat could not initialize SHA-256 context")));
    }
    return context;
}

static void
sha256_update(pg_cryptohash_ctx *context, const void *data, size_t size)
{
    if (size > 0 && pg_cryptohash_update(context, data, size) < 0)
    {
        pg_cryptohash_free(context);
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("acdat SHA-256 update failed")));
    }
}

static void
sha256_final(pg_cryptohash_ctx *context,
             uint8 destination[PG_SHA256_DIGEST_LENGTH])
{
    if (pg_cryptohash_final(context, destination,
                            PG_SHA256_DIGEST_LENGTH) < 0)
    {
        pg_cryptohash_free(context);
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("acdat SHA-256 finalization failed")));
    }
    pg_cryptohash_free(context);
}

static void
write_le32(uint8 destination[4], uint32 value)
{
    destination[0] = (uint8) value;
    destination[1] = (uint8) (value >> 8);
    destination[2] = (uint8) (value >> 16);
    destination[3] = (uint8) (value >> 24);
}

static void
write_le64(uint8 destination[8], uint64 value)
{
    write_le32(destination, (uint32) value);
    write_le32(destination + 4, (uint32) (value >> 32));
}

static void
sha256_update_u32(pg_cryptohash_ctx *context, uint32 value)
{
    uint8 bytes[4];

    write_le32(bytes, value);
    sha256_update(context, bytes, sizeof(bytes));
}

static void
sha256_update_u64(pg_cryptohash_ctx *context, uint64 value)
{
    uint8 bytes[8];

    write_le64(bytes, value);
    sha256_update(context, bytes, sizeof(bytes));
}

static void
compute_artifact_checksum(const uint8 *data, uint32 size,
                          uint8 checksum[PG_SHA256_DIGEST_LENGTH])
{
    static const uint8 zeros[PG_SHA256_DIGEST_LENGTH] = {0};
    pg_cryptohash_ctx *context;

    if (size < ACDAT_FORMAT_HEADER_SIZE)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("acdat machine is shorter than its header")));
    context = sha256_create();
    sha256_update(context, data, ACDAT_HEADER_CHECKSUM_OFFSET);
    sha256_update(context, zeros, sizeof(zeros));
    sha256_update(context,
                  data + ACDAT_HEADER_CHECKSUM_OFFSET +
                  PG_SHA256_DIGEST_LENGTH,
                  size - ACDAT_HEADER_CHECKSUM_OFFSET -
                  PG_SHA256_DIGEST_LENGTH);
    sha256_final(context, checksum);
}

static bool
checksum_matches(const AcdatMachineView *view)
{
    uint8 checksum[PG_SHA256_DIGEST_LENGTH];

    compute_artifact_checksum(view->data, view->total_size, checksum);
    return memcmp(checksum, view->artifact_checksum,
                  PG_SHA256_DIGEST_LENGTH) == 0;
}

static AcdatStatus
validate_payload(const uint8 *data, size_t size, AcdatMachineView *view,
                 AcdatError *error)
{
    AcdatStatus status = acdat_parse_machine(data, size, view, error);

    if (status == ACDAT_OK && !checksum_matches(view))
    {
        if (error != NULL)
        {
            error->code = ACDAT_ERR_INVALID_ARGUMENT;
            error->pattern_index = SIZE_MAX;
            strlcpy(error->message, "artifact checksum does not match",
                    sizeof(error->message));
        }
        return ACDAT_ERR_INVALID_ARGUMENT;
    }
    return status;
}

static struct varlena *
copy_payload_as_machine(const uint8 *data, size_t size)
{
    struct varlena *machine;

    if (size > MaxAllocSize - VARHDRSZ)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("acdat machine exceeds PostgreSQL allocation limit")));
    machine = (struct varlena *) palloc(size + VARHDRSZ);
    SET_VARSIZE(machine, size + VARHDRSZ);
    memcpy(VARDATA(machine), data, size);
    return machine;
}

static void
require_valid_payload(const uint8 *data, size_t size, AcdatMachineView *view)
{
    AcdatError error;
    AcdatStatus status = validate_payload(data, size, view, &error);

    if (status != ACDAT_OK)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("invalid acdat machine: %s", error.message)));
}

static int
compare_pg_patterns(const void *left_pointer, const void *right_pointer)
{
    const AcdatPatternInput *left =
        &((const PgPatternWork *) left_pointer)->pattern;
    const AcdatPatternInput *right =
        &((const PgPatternWork *) right_pointer)->pattern;
    uint32 common = Min(left->byte_length, right->byte_length);
    int comparison = common == 0 ? 0 :
                     memcmp(left->pattern, right->pattern, common);

    if (comparison != 0)
        return comparison;
    if (left->byte_length != right->byte_length)
        return left->byte_length < right->byte_length ? -1 : 1;
    if (left->pattern_id != right->pattern_id)
        return left->pattern_id < right->pattern_id ? -1 : 1;
    if (left->priority != right->priority)
        return left->priority < right->priority ? -1 : 1;
    if (left->has_replacement != right->has_replacement)
        return left->has_replacement ? 1 : -1;
    common = Min(left->replacement_length, right->replacement_length);
    comparison = common == 0 ? 0 :
                 memcmp(left->replacement, right->replacement, common);
    if (comparison != 0)
        return comparison;
    if (left->replacement_length != right->replacement_length)
        return left->replacement_length < right->replacement_length ? -1 : 1;
    return 0;
}

static void
compute_source_fingerprint(const AcdatPatternInput *patterns,
                           uint32 pattern_count, uint16 input_kind,
                           uint32 database_encoding,
                           uint8 digest[PG_SHA256_DIGEST_LENGTH])
{
    static const char prefix[] = "acdat-source-v1";
    PgPatternWork *work;
    pg_cryptohash_ctx *context;
    uint32 index;

    work = palloc(sizeof(*work) * Max(pattern_count, 1U));
    for (index = 0; index < pattern_count; index++)
        work[index].pattern = patterns[index];
    if (pattern_count > 1)
        qsort(work, pattern_count, sizeof(*work), compare_pg_patterns);

    context = sha256_create();
    sha256_update(context, prefix, sizeof(prefix) - 1U);
    sha256_update_u32(context, input_kind);
    sha256_update_u32(context, database_encoding);
    sha256_update_u32(context, pattern_count);
    for (index = 0; index < pattern_count; index++)
    {
        const AcdatPatternInput *pattern = &work[index].pattern;
        uint8 replacement_flag = pattern->has_replacement ? 1U : 0U;

        sha256_update_u64(context, (uint64) pattern->pattern_id);
        sha256_update_u32(context, (uint32) pattern->priority);
        sha256_update_u32(context, pattern->byte_length);
        sha256_update_u32(context, pattern->char_length);
        sha256_update(context, &replacement_flag, sizeof(replacement_flag));
        sha256_update_u32(context, pattern->replacement_length);
        sha256_update(context, pattern->pattern, pattern->byte_length);
        if (pattern->has_replacement)
            sha256_update(context, pattern->replacement,
                          pattern->replacement_length);
    }
    sha256_final(context, digest);
    pfree(work);
}

static void
compute_serialized_build_fingerprint(
    const uint8 *data, uint32 size,
    uint8 digest[PG_SHA256_DIGEST_LENGTH])
{
    static const char prefix[] = "acdat-build-v1";
    static const uint8 zeros[PG_SHA256_DIGEST_LENGTH] = {0};
    pg_cryptohash_ctx *context = sha256_create();

    if (size < ACDAT_FORMAT_HEADER_SIZE)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("acdat machine is shorter than its header")));
    sha256_update(context, prefix, sizeof(prefix) - 1U);
    sha256_update(context, data, ACDAT_HEADER_BUILD_FINGERPRINT_OFFSET);
    sha256_update(context, zeros, sizeof(zeros));
    sha256_update(context, zeros, sizeof(zeros));
    sha256_update(context,
                  data + ACDAT_HEADER_CHECKSUM_OFFSET +
                  PG_SHA256_DIGEST_LENGTH,
                  size - ACDAT_HEADER_CHECKSUM_OFFSET -
                  PG_SHA256_DIGEST_LENGTH);
    sha256_final(context, digest);
}

static void
check_one_dimensional(ArrayType *array, const char *argument_name)
{
    if (ARR_NDIM(array) > 1)
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("acdat %s must be a one-dimensional array",
                        argument_name)));
}

static void
deconstruct_expected_array(ArrayType *array, Oid element_type,
                           int16 element_length, bool element_by_value,
                           char element_alignment, Datum **values,
                           bool **nulls, int *count, const char *argument_name)
{
    check_one_dimensional(array, argument_name);
    deconstruct_array(array, element_type, element_length, element_by_value,
                      element_alignment, values, nulls, count);
}

static void
require_supported_text_encoding(void)
{
    int encoding = GetDatabaseEncoding();

    if (encoding != PG_UTF8 && pg_encoding_max_length(encoding) != 1)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("acdat text matching does not support database encoding %s",
                        GetDatabaseEncodingName()),
                 errhint("Use a UTF8 or single-byte database, or use bytea patterns.")));
}

static struct varlena *
compile_patterns(AcdatPatternInput *patterns, uint32 pattern_count,
                 uint16 input_kind)
{
    AcdatAutomaton *automaton = NULL;
    AcdatAllocator allocator = pg_allocator(CurrentMemoryContext);
    AcdatBuildControl build_control;
    AcdatFormatOptions format_options;
    AcdatError error;
    AcdatStatus status;
    struct varlena *machine;
    uint32 serialized_size;
    uint8 checksum[PG_SHA256_DIGEST_LENGTH];

    if (input_kind == ACDAT_INPUT_TEXT)
        require_supported_text_encoding();

    memset(&build_control, 0, sizeof(build_control));
    build_control.progress_callback = check_postgres_interrupts;
    build_control.base_candidate_limit =
        default_base_candidate_limit(patterns, pattern_count);
    status = acdat_build_ex(patterns, pattern_count, &allocator,
                            &build_control, NULL, &automaton, &error);
    if (status != ACDAT_OK)
        raise_core_error("compile", status, &error);

    memset(&format_options, 0, sizeof(format_options));
    format_options.input_kind = input_kind;
    format_options.compiler_major = ACDAT_COMPILER_VERSION_MAJOR;
    format_options.compiler_minor = ACDAT_COMPILER_VERSION_MINOR;
    format_options.compiler_patch = ACDAT_COMPILER_VERSION_PATCH;
    format_options.database_encoding = input_kind == ACDAT_INPUT_TEXT ?
        GetDatabaseEncoding() : 0;
    compute_source_fingerprint(patterns, pattern_count, input_kind,
                               format_options.database_encoding,
                               format_options.source_fingerprint);
    status = acdat_serialized_size(automaton, &serialized_size, &error);
    if (status != ACDAT_OK)
        raise_core_error("serialize", status, &error);
    if (serialized_size > MaxAllocSize - VARHDRSZ)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("compiled acdat machine exceeds PostgreSQL's varlena limit")));

    machine = (struct varlena *) palloc(serialized_size + VARHDRSZ);
    SET_VARSIZE(machine, serialized_size + VARHDRSZ);
    status = acdat_serialize(automaton, &format_options,
                             (uint8 *) VARDATA(machine), serialized_size,
                             &error);
    if (status != ACDAT_OK)
        raise_core_error("serialize", status, &error);
    compute_serialized_build_fingerprint(
        (const uint8 *) VARDATA(machine), serialized_size,
        format_options.build_fingerprint);
    acdat_set_build_fingerprint((uint8 *) VARDATA(machine),
                                format_options.build_fingerprint);
    compute_artifact_checksum((const uint8 *) VARDATA(machine),
                              serialized_size, checksum);
    acdat_set_artifact_checksum((uint8 *) VARDATA(machine), checksum);
    acdat_free(automaton);
    return machine;
}

static struct varlena *
compile_array(FunctionCallInfo fcinfo, uint16 input_kind)
{
    ArrayType *pattern_array;
    ArrayType *id_array = NULL;
    ArrayType *replacement_array = NULL;
    ArrayType *priority_array = NULL;
    Jsonb *options = NULL;
    Datum *pattern_values;
    bool *pattern_nulls;
    int pattern_count;
    Datum *id_values = NULL;
    bool *id_nulls = NULL;
    int id_count = 0;
    Datum *replacement_values = NULL;
    bool *replacement_nulls = NULL;
    int replacement_count = 0;
    Datum *priority_values = NULL;
    bool *priority_nulls = NULL;
    int priority_count = 0;
    AcdatPatternInput *patterns;
    int index;

    if (PG_ARGISNULL(0))
        return NULL;
    pattern_array = PG_GETARG_ARRAYTYPE_P(0);
    if (!PG_ARGISNULL(1))
        id_array = PG_GETARG_ARRAYTYPE_P(1);
    if (!PG_ARGISNULL(2))
        replacement_array = PG_GETARG_ARRAYTYPE_P(2);
    if (!PG_ARGISNULL(3))
        priority_array = PG_GETARG_ARRAYTYPE_P(3);
    if (!PG_ARGISNULL(4))
        options = PG_GETARG_JSONB_P(4);

    if (options != NULL &&
        (!JB_ROOT_IS_OBJECT(options) || JB_ROOT_COUNT(options) != 0))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("acdat compile options are not implemented yet"),
                 errhint("Pass an empty JSON object.")));

    if (input_kind == ACDAT_INPUT_TEXT)
        deconstruct_expected_array(pattern_array, TEXTOID, -1, false, 'i',
                                   &pattern_values, &pattern_nulls,
                                   &pattern_count, "patterns");
    else
        deconstruct_expected_array(pattern_array, BYTEAOID, -1, false, 'i',
                                   &pattern_values, &pattern_nulls,
                                   &pattern_count, "patterns");

    if (id_array != NULL)
        deconstruct_expected_array(id_array, INT8OID, sizeof(int64), true, 'd',
                                   &id_values, &id_nulls, &id_count,
                                   "pattern_ids");
    if (replacement_array != NULL)
        deconstruct_expected_array(replacement_array,
                                   input_kind == ACDAT_INPUT_TEXT ?
                                   TEXTOID : BYTEAOID,
                                   -1, false, 'i', &replacement_values,
                                   &replacement_nulls, &replacement_count,
                                   "replacements");
    if (priority_array != NULL)
        deconstruct_expected_array(priority_array, INT4OID, sizeof(int32),
                                   true, 'i', &priority_values,
                                   &priority_nulls, &priority_count,
                                   "priorities");

    if ((id_array != NULL && id_count != pattern_count) ||
        (replacement_array != NULL && replacement_count != pattern_count) ||
        (priority_array != NULL && priority_count != pattern_count))
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("acdat compile arrays must have equal lengths")));
    if ((uint64) pattern_count > UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("acdat pattern count exceeds uint32 range")));

    patterns = palloc0(sizeof(*patterns) * Max(pattern_count, 1));
    for (index = 0; index < pattern_count; index++)
    {
        struct varlena *pattern_value;
        struct varlena *replacement_value = NULL;
        Size byte_length;

        if (pattern_nulls[index])
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("acdat pattern at array position %d is NULL",
                            index + 1)));
        if (id_array != NULL && id_nulls[index])
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("acdat pattern ID at array position %d is NULL",
                            index + 1)));

        pattern_value = (struct varlena *)
            PG_DETOAST_DATUM_PACKED(pattern_values[index]);
        byte_length = VARSIZE_ANY_EXHDR(pattern_value);
        if (byte_length > UINT32_MAX)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("acdat pattern at array position %d is too large",
                            index + 1)));
        patterns[index].pattern =
            (const uint8 *) VARDATA_ANY(pattern_value);
        patterns[index].byte_length = (uint32) byte_length;
        patterns[index].char_length = input_kind == ACDAT_INPUT_TEXT ?
            (uint32) pg_mbstrlen_with_len(VARDATA_ANY(pattern_value),
                                          byte_length) : 0;
        patterns[index].pattern_id = id_array == NULL ?
            (int64) index + 1 : DatumGetInt64(id_values[index]);
        patterns[index].priority = priority_array == NULL ||
            priority_nulls[index] ? 0 : DatumGetInt32(priority_values[index]);

        if (replacement_array != NULL && !replacement_nulls[index])
        {
            Size replacement_length;

            replacement_value = (struct varlena *)
                PG_DETOAST_DATUM_PACKED(replacement_values[index]);
            replacement_length = VARSIZE_ANY_EXHDR(replacement_value);
            if (replacement_length > UINT32_MAX)
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("acdat replacement at array position %d is too large",
                                index + 1)));
            patterns[index].replacement =
                (const uint8 *) VARDATA_ANY(replacement_value);
            patterns[index].replacement_length =
                (uint32) replacement_length;
            patterns[index].has_replacement = true;
        }
    }

    return compile_patterns(patterns, (uint32) pattern_count, input_kind);
}

static PgCompileAggregateState *
compile_aggregate_transition(FunctionCallInfo fcinfo, uint16 input_kind,
                             bool has_replacement_and_priority)
{
    MemoryContext aggregate_context;
    PgCompileAggregateState *state;
    struct varlena *pattern_value;
    struct varlena *replacement_value = NULL;
    AcdatPatternInput *pattern;
    Size byte_length;
    MemoryContext old_context;

    if (AggCheckCallContext(fcinfo, &aggregate_context) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("acdat compile transition called outside aggregate context")));
    if (PG_ARGISNULL(1))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("acdat aggregate pattern must not be NULL")));
    if (PG_ARGISNULL(2))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("acdat aggregate pattern ID must not be NULL")));

    old_context = MemoryContextSwitchTo(aggregate_context);
    if (PG_ARGISNULL(0))
    {
        state = palloc0(sizeof(*state));
        state->context = aggregate_context;
        state->input_kind = input_kind;
    }
    else
        state = (PgCompileAggregateState *) PG_GETARG_POINTER(0);

    if (state->input_kind != input_kind)
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("acdat aggregate input kind changed within one state")));
    if (state->pattern_count == UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("acdat aggregate contains too many patterns")));
    if (state->pattern_count == state->pattern_capacity)
    {
        uint64 new_capacity = state->pattern_capacity == 0 ? 128U :
            (uint64) state->pattern_capacity +
            state->pattern_capacity / 2U + 64U;

        if (new_capacity > UINT32_MAX ||
            new_capacity > MaxAllocSize / sizeof(AcdatPatternInput))
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("acdat aggregate state exceeds allocation limit")));
        if (state->patterns == NULL)
            state->patterns = palloc0((Size) new_capacity *
                                      sizeof(AcdatPatternInput));
        else
        {
            uint32 old_capacity = state->pattern_capacity;

            state->patterns = repalloc(state->patterns,
                                      (Size) new_capacity *
                                      sizeof(AcdatPatternInput));
            memset(state->patterns + old_capacity, 0,
                   ((Size) new_capacity - old_capacity) *
                   sizeof(AcdatPatternInput));
        }
        state->pattern_capacity = (uint32) new_capacity;
    }

    pattern = &state->patterns[state->pattern_count];
    pattern_value = (struct varlena *)
        PG_DETOAST_DATUM_PACKED(PG_GETARG_DATUM(1));
    byte_length = VARSIZE_ANY_EXHDR(pattern_value);
    if (byte_length == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("acdat aggregate pattern must not be empty")));
    if (byte_length > UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("acdat aggregate pattern is too large")));
    pattern->pattern = MemoryContextAlloc(aggregate_context, byte_length);
    memcpy((void *) pattern->pattern, VARDATA_ANY(pattern_value), byte_length);
    pattern->byte_length = (uint32) byte_length;
    pattern->char_length = input_kind == ACDAT_INPUT_TEXT ?
        (uint32) pg_mbstrlen_with_len(VARDATA_ANY(pattern_value),
                                      byte_length) : 0;
    pattern->pattern_id = PG_GETARG_INT64(2);
    pattern->priority = has_replacement_and_priority && !PG_ARGISNULL(4) ?
        PG_GETARG_INT32(4) : 0;

    if (has_replacement_and_priority && !PG_ARGISNULL(3))
    {
        Size replacement_length;

        replacement_value = (struct varlena *)
            PG_DETOAST_DATUM_PACKED(PG_GETARG_DATUM(3));
        replacement_length = VARSIZE_ANY_EXHDR(replacement_value);
        if (replacement_length > UINT32_MAX)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("acdat aggregate replacement is too large")));
        pattern->replacement = MemoryContextAlloc(aggregate_context,
                                                   Max(replacement_length, 1));
        if (replacement_length > 0)
            memcpy((void *) pattern->replacement,
                   VARDATA_ANY(replacement_value), replacement_length);
        pattern->replacement_length = (uint32) replacement_length;
        pattern->has_replacement = true;
    }

    state->pattern_count++;
    MemoryContextSwitchTo(old_context);
    return state;
}

static Datum
compile_aggregate_final(FunctionCallInfo fcinfo)
{
    PgCompileAggregateState *state;
    struct varlena *machine;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();
    state = (PgCompileAggregateState *) PG_GETARG_POINTER(0);
    machine = compile_patterns(state->patterns, state->pattern_count,
                               state->input_kind);
    PG_RETURN_POINTER(machine);
}

static void
hex_fingerprint(const uint8 *source, char destination[65])
{
    static const char digits[] = "0123456789abcdef";
    int index;

    for (index = 0; index < PG_SHA256_DIGEST_LENGTH; index++)
    {
        destination[index * 2] = digits[source[index] >> 4];
        destination[index * 2 + 1] = digits[source[index] & 0x0F];
    }
    destination[64] = '\0';
}

static PgMachineCache *
get_machine_cache(FunctionCallInfo fcinfo, Datum machine_datum,
                  uint16 expected_input_kind)
{
    struct varlena *raw_machine =
        (struct varlena *) DatumGetPointer(machine_datum);
    struct varlena *machine;
    bool external_on_disk = VARATT_IS_EXTERNAL_ONDISK(raw_machine);
    varatt_external external_key;
    bool detoasted_copy;
    PgMachineCache *cache = (PgMachineCache *) fcinfo->flinfo->fn_extra;
    bool cache_matches;
    AcdatMachineView peek;
    AcdatError error;
    AcdatStatus status;
    const uint8 *data;
    uint32 size;

    if (external_on_disk)
    {
        VARATT_EXTERNAL_GET_POINTER(external_key, raw_machine);
        if (cache != NULL && cache->automaton != NULL &&
            cache->has_external_key &&
            memcmp(&cache->external_key, &external_key,
                   sizeof(external_key)) == 0)
            return cache;
    }

    machine = (struct varlena *) PG_DETOAST_DATUM_PACKED(machine_datum);
    detoasted_copy = machine != raw_machine;
    data = (const uint8 *) VARDATA_ANY(machine);
    size = (uint32) VARSIZE_ANY_EXHDR(machine);

    status = acdat_peek_machine(data, size, &peek, &error);
    if (status != ACDAT_OK)
        raise_core_error("read machine header", status, &error);
    if (peek.input_kind != expected_input_kind)
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("acdat machine input kind does not match function input")));
    if (expected_input_kind == ACDAT_INPUT_TEXT &&
        peek.database_encoding != (uint32) GetDatabaseEncoding())
        ereport(ERROR,
                (errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
                 errmsg("acdat machine database encoding does not match current database"),
                 errhint("Recompile the machine in this database.")));
    if (expected_input_kind == ACDAT_INPUT_TEXT)
        require_supported_text_encoding();

    cache_matches = cache != NULL && cache->automaton != NULL &&
        cache->payload_size == size && cache->input_kind == peek.input_kind &&
        cache->database_encoding == peek.database_encoding &&
        memcmp(cache->build_fingerprint, peek.build_fingerprint,
               ACDAT_FINGERPRINT_SIZE) == 0 &&
        memcmp(cache->artifact_checksum, peek.artifact_checksum,
               ACDAT_FINGERPRINT_SIZE) == 0;
    if (cache_matches)
    {
        cache->has_external_key = external_on_disk;
        if (external_on_disk)
            cache->external_key = external_key;
        if (detoasted_copy)
            pfree(machine);
        return cache;
    }

    if (cache == NULL)
    {
        cache = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt,
                                       sizeof(*cache));
        fcinfo->flinfo->fn_extra = cache;
    }
    {
        MemoryContext old_context = cache->automaton_context;

        cache->automaton_context = NULL;
        cache->automaton = NULL;
        if (old_context != NULL)
            MemoryContextDelete(old_context);
    }
    cache->automaton_context = AllocSetContextCreate(fcinfo->flinfo->fn_mcxt,
                                                     "acdat machine cache",
                                                     ALLOCSET_DEFAULT_SIZES);

    {
        AcdatMachineView view;
        AcdatAllocator allocator = pg_allocator(cache->automaton_context);

        status = validate_payload(data, size, &view, &error);
        if (status != ACDAT_OK)
            raise_core_error("validate machine", status, &error);
        status = acdat_materialize_machine(&view, &allocator,
                                           &cache->automaton, &error);
        if (status != ACDAT_OK)
            raise_core_error("materialize machine", status, &error);
        status = acdat_prepare_runtime(cache->automaton, &error);
        if (status != ACDAT_OK)
            raise_core_error("prepare machine runtime", status, &error);
        cache->payload_size = size;
        cache->input_kind = view.input_kind;
        cache->database_encoding = view.database_encoding;
        memcpy(cache->build_fingerprint, view.build_fingerprint,
               ACDAT_FINGERPRINT_SIZE);
        memcpy(cache->artifact_checksum, view.artifact_checksum,
               ACDAT_FINGERPRINT_SIZE);
        cache->has_external_key = external_on_disk;
        if (external_on_disk)
            cache->external_key = external_key;
    }
    if (detoasted_copy)
        pfree(machine);
    return cache;
}

static PgMatchPolicy
parse_match_policy(text *policy_text)
{
    char *policy = text_to_cstring(policy_text);
    PgMatchPolicy result;

    if (strcmp(policy, "all_overlapping") == 0)
        result = PG_MATCH_ALL_OVERLAPPING;
    else if (strcmp(policy, "leftmost_longest") == 0)
        result = PG_MATCH_LEFTMOST_LONGEST;
    else if (strcmp(policy, "leftmost_priority") == 0)
        result = PG_MATCH_LEFTMOST_PRIORITY;
    else
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("unknown acdat match policy: %s", policy),
                 errhint("Use all_overlapping, leftmost_longest, or leftmost_priority.")));
    pfree(policy);
    return result;
}

static void
check_postgres_interrupts(void *context)
{
    (void) context;
    CHECK_FOR_INTERRUPTS();
}

static bool
collect_pg_match(const AcdatMatch *match, void *context_pointer)
{
    PgMatchCollector *collector = (PgMatchCollector *) context_pointer;
    PgCollectedMatch *collected;

    if (collector->match_count == collector->match_capacity)
    {
        uint64 new_capacity = collector->match_capacity == 0 ? 128U :
            (uint64) collector->match_capacity +
            collector->match_capacity / 2U + 64U;

        if (new_capacity > UINT32_MAX ||
            new_capacity > MaxAllocSize / sizeof(PgCollectedMatch))
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("acdat match result exceeds allocation limit")));
        if (collector->matches == NULL)
            collector->matches = palloc((Size) new_capacity *
                                        sizeof(PgCollectedMatch));
        else
            collector->matches = repalloc(collector->matches,
                                          (Size) new_capacity *
                                          sizeof(PgCollectedMatch));
        collector->match_capacity = (uint32) new_capacity;
    }

    collected = &collector->matches[collector->match_count++];
    collected->match = *match;
    collected->start_char = 0;
    if (collector->text_input)
    {
        uint32 end_byte = match->start_byte + match->byte_length - 1U;

        while (collector->scanned_bytes < end_byte)
        {
            int character_bytes = pg_mblen(
                (const char *) collector->input + collector->scanned_bytes);

            if (character_bytes <= 0 ||
                collector->scanned_bytes + (uint32) character_bytes >
                collector->input_length)
                ereport(ERROR,
                        (errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
                         errmsg("acdat encountered an invalid multibyte input")));
            collector->scanned_bytes += (uint32) character_bytes;
            collector->scanned_chars++;
        }
        if (collector->scanned_bytes != end_byte ||
            collector->scanned_chars < match->char_length)
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("acdat match does not end on a character boundary")));
        collected->start_char =
            collector->scanned_chars - match->char_length + 1U;
    }
    if ((collector->match_count & 1023U) == 0)
        CHECK_FOR_INTERRUPTS();
    return true;
}

static void
collect_matches(const AcdatAutomaton *automaton, const uint8 *input,
                uint32 input_length, bool compute_char_positions,
                int32 max_matches, PgMatchCollector *collector)
{
    AcdatError error;
    AcdatStatus status;
    uint64 match_count = 0;

    if (max_matches <= 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("acdat max_matches must be positive")));
    memset(collector, 0, sizeof(*collector));
    collector->input = input;
    collector->input_length = input_length;
    collector->text_input = compute_char_positions;
    status = acdat_scan_ex(automaton, input, input_length, collect_pg_match,
                           collector, (uint64) max_matches, &match_count,
                           check_postgres_interrupts, NULL, &error);
    if (status != ACDAT_OK)
        raise_core_error("scan", status, &error);
    if (match_count != collector->match_count)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("acdat match count invariant failed")));
}

static int
compare_match_longest(const void *left_pointer, const void *right_pointer)
{
    const PgCollectedMatch *left = left_pointer;
    const PgCollectedMatch *right = right_pointer;

    if (left->match.start_byte != right->match.start_byte)
        return left->match.start_byte < right->match.start_byte ? -1 : 1;
    if (left->match.byte_length != right->match.byte_length)
        return left->match.byte_length > right->match.byte_length ? -1 : 1;
    if (left->match.priority != right->match.priority)
        return left->match.priority < right->match.priority ? -1 : 1;
    if (left->match.pattern_id != right->match.pattern_id)
        return left->match.pattern_id < right->match.pattern_id ? -1 : 1;
    if (left->match.canonical_ordinal != right->match.canonical_ordinal)
        return left->match.canonical_ordinal < right->match.canonical_ordinal ?
               -1 : 1;
    return 0;
}

static int
compare_match_priority(const void *left_pointer, const void *right_pointer)
{
    const PgCollectedMatch *left = left_pointer;
    const PgCollectedMatch *right = right_pointer;

    if (left->match.start_byte != right->match.start_byte)
        return left->match.start_byte < right->match.start_byte ? -1 : 1;
    if (left->match.priority != right->match.priority)
        return left->match.priority < right->match.priority ? -1 : 1;
    if (left->match.byte_length != right->match.byte_length)
        return left->match.byte_length > right->match.byte_length ? -1 : 1;
    if (left->match.pattern_id != right->match.pattern_id)
        return left->match.pattern_id < right->match.pattern_id ? -1 : 1;
    if (left->match.canonical_ordinal != right->match.canonical_ordinal)
        return left->match.canonical_ordinal < right->match.canonical_ordinal ?
               -1 : 1;
    return 0;
}

static void
select_non_overlapping(PgMatchCollector *collector, PgMatchPolicy policy)
{
    uint32 input_cursor = 1;
    uint32 read_index;
    uint32 write_index = 0;

    if (collector->match_count == 0)
        return;
    qsort(collector->matches, collector->match_count,
          sizeof(PgCollectedMatch),
          policy == PG_MATCH_LEFTMOST_LONGEST ?
          compare_match_longest : compare_match_priority);

    for (read_index = 0; read_index < collector->match_count; read_index++)
    {
        PgCollectedMatch *candidate = &collector->matches[read_index];

        if (candidate->match.start_byte < input_cursor)
            continue;
        collector->matches[write_index++] = *candidate;
        input_cursor = candidate->match.start_byte +
                       candidate->match.byte_length;
    }
    collector->match_count = write_index;
}

static Tuplestorestate *
initialize_materialized_srf(FunctionCallInfo fcinfo, TupleDesc *tuple_desc)
{
    ReturnSetInfo *return_info = (ReturnSetInfo *) fcinfo->resultinfo;
    MemoryContext old_context;
    Tuplestorestate *tuple_store;

    if (return_info == NULL || !IsA(return_info, ReturnSetInfo) ||
        (return_info->allowedModes & SFRM_Materialize) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("acdat.matches requires materialize mode")));
    old_context = MemoryContextSwitchTo(
        return_info->econtext->ecxt_per_query_memory);
    if (get_call_result_type(fcinfo, NULL, tuple_desc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("acdat.matches could not determine result row type")));
    *tuple_desc = BlessTupleDesc(*tuple_desc);
    tuple_store = tuplestore_begin_heap(true, false, work_mem);
    return_info->returnMode = SFRM_Materialize;
    return_info->setResult = tuple_store;
    return_info->setDesc = *tuple_desc;
    MemoryContextSwitchTo(old_context);
    return tuple_store;
}

static Datum
matches_common(FunctionCallInfo fcinfo, uint16 input_kind)
{
    struct varlena *input = PG_GETARG_VARLENA_PP(0);
    Datum machine_datum = PG_GETARG_DATUM(1);
    text *policy_text = PG_GETARG_TEXT_PP(2);
    int32 max_matches = PG_GETARG_INT32(3);
    PgMatchPolicy policy = parse_match_policy(policy_text);
    PgMachineCache *cache = get_machine_cache(fcinfo, machine_datum,
                                              input_kind);
    PgMatchCollector collector;
    TupleDesc tuple_desc;
    Tuplestorestate *tuple_store;
    uint32 index;

    collect_matches(cache->automaton,
                    (const uint8 *) VARDATA_ANY(input),
                    (uint32) VARSIZE_ANY_EXHDR(input),
                    input_kind == ACDAT_INPUT_TEXT,
                    max_matches, &collector);
    if (policy != PG_MATCH_ALL_OVERLAPPING)
        select_non_overlapping(&collector, policy);

    tuple_store = initialize_materialized_srf(fcinfo, &tuple_desc);
    for (index = 0; index < collector.match_count; index++)
    {
        const PgCollectedMatch *match = &collector.matches[index];
        Datum values[6];
        bool nulls[6] = {false, false, false, false, false, false};

        values[0] = Int64GetDatum(match->match.pattern_id);
        values[1] = Int32GetDatum((int32) match->match.start_byte);
        values[2] = Int32GetDatum((int32) match->match.byte_length);
        if (input_kind == ACDAT_INPUT_TEXT)
        {
            values[3] = Int32GetDatum((int32) match->start_char);
            values[4] = Int32GetDatum((int32) match->match.char_length);
        }
        else
        {
            values[3] = (Datum) 0;
            values[4] = (Datum) 0;
            nulls[3] = true;
            nulls[4] = true;
        }
        values[5] = Int32GetDatum(match->match.priority);
        tuplestore_putvalues(tuple_store, tuple_desc, values, nulls);
    }
    PG_FREE_IF_COPY(policy_text, 2);
    PG_FREE_IF_COPY(input, 0);
    PG_RETURN_NULL();
}

static Datum
replace_common(FunctionCallInfo fcinfo, uint16 input_kind)
{
    struct varlena *input = PG_GETARG_VARLENA_PP(0);
    Datum machine_datum = PG_GETARG_DATUM(1);
    text *policy_text = PG_GETARG_TEXT_PP(2);
    int32 max_replacements = PG_GETARG_INT32(3);
    int64 max_output_bytes = PG_GETARG_INT64(4);
    PgMatchPolicy policy = parse_match_policy(policy_text);
    PgMachineCache *cache = get_machine_cache(fcinfo, machine_datum,
                                              input_kind);
    PgMatchCollector collector;
    uint32 input_length = (uint32) VARSIZE_ANY_EXHDR(input);
    uint64 output_length = input_length;
    struct varlena *result;
    uint8 *output;
    uint32 input_cursor = 0;
    uint32 output_cursor = 0;
    uint32 index;

    if (policy == PG_MATCH_ALL_OVERLAPPING)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("all_overlapping is not a valid replacement policy")));
    if (max_replacements <= 0 || max_output_bytes <= 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("acdat replacement limits must be positive")));

    collect_matches(cache->automaton,
                    (const uint8 *) VARDATA_ANY(input), input_length,
                    false, max_replacements, &collector);
    select_non_overlapping(&collector, policy);
    for (index = 0; index < collector.match_count; index++)
    {
        const AcdatMatch *match = &collector.matches[index].match;
        const AcdatTerminal *terminal =
            &cache->automaton->terminals[match->terminal_index];

        if (!terminal->has_replacement)
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("acdat pattern ID %lld has no replacement",
                            (long long) terminal->pattern_id)));
        output_length -= match->byte_length;
        output_length += terminal->replacement_length;
        if (output_length > (uint64) max_output_bytes ||
            output_length > MaxAllocSize - VARHDRSZ)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("acdat replacement output exceeds byte limit")));
    }

    result = palloc((Size) output_length + VARHDRSZ);
    SET_VARSIZE(result, (Size) output_length + VARHDRSZ);
    output = (uint8 *) VARDATA(result);
    for (index = 0; index < collector.match_count; index++)
    {
        const AcdatMatch *match = &collector.matches[index].match;
        const AcdatTerminal *terminal =
            &cache->automaton->terminals[match->terminal_index];
        uint32 match_start = match->start_byte - 1U;
        uint32 prefix_length = match_start - input_cursor;

        if (prefix_length > 0)
        {
            memcpy(output + output_cursor,
                   VARDATA_ANY(input) + input_cursor, prefix_length);
            output_cursor += prefix_length;
        }
        if (terminal->replacement_length > 0)
        {
            memcpy(output + output_cursor,
                   cache->automaton->replacement_arena +
                   terminal->replacement_offset,
                   terminal->replacement_length);
            output_cursor += terminal->replacement_length;
        }
        input_cursor = match_start + match->byte_length;
    }
    if (input_cursor < input_length)
    {
        uint32 suffix_length = input_length - input_cursor;

        memcpy(output + output_cursor, VARDATA_ANY(input) + input_cursor,
               suffix_length);
        output_cursor += suffix_length;
    }
    if (output_cursor != output_length)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("acdat replacement length invariant failed")));

    PG_FREE_IF_COPY(policy_text, 2);
    PG_FREE_IF_COPY(input, 0);
    PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(acdat_machine_in);
PG_FUNCTION_INFO_V1(acdat_machine_out);
PG_FUNCTION_INFO_V1(acdat_machine_recv);
PG_FUNCTION_INFO_V1(acdat_machine_send);
PG_FUNCTION_INFO_V1(acdat_compile_text_array);
PG_FUNCTION_INFO_V1(acdat_compile_bytea_array);
PG_FUNCTION_INFO_V1(acdat_compile_text_trans);
PG_FUNCTION_INFO_V1(acdat_compile_text_trans_simple);
PG_FUNCTION_INFO_V1(acdat_compile_bytea_trans);
PG_FUNCTION_INFO_V1(acdat_compile_bytea_trans_simple);
PG_FUNCTION_INFO_V1(acdat_compile_final);
PG_FUNCTION_INFO_V1(acdat_contains_text);
PG_FUNCTION_INFO_V1(acdat_contains_bytea);
PG_FUNCTION_INFO_V1(acdat_matches_text);
PG_FUNCTION_INFO_V1(acdat_matches_bytea);
PG_FUNCTION_INFO_V1(acdat_replace_text);
PG_FUNCTION_INFO_V1(acdat_replace_bytea);
PG_FUNCTION_INFO_V1(acdat_machine_info);
PG_FUNCTION_INFO_V1(acdat_machine_validate);
PG_FUNCTION_INFO_V1(acdat_machine_export);
PG_FUNCTION_INFO_V1(acdat_machine_import);
PG_FUNCTION_INFO_V1(acdat_source_fingerprint);
PG_FUNCTION_INFO_V1(acdat_build_fingerprint);
PG_FUNCTION_INFO_V1(acdat_artifact_checksum);
PG_FUNCTION_INFO_V1(acdat_catalog_build_id);

Datum
acdat_machine_in(PG_FUNCTION_ARGS)
{
    char *input = PG_GETARG_CSTRING(0);
    bytea *raw = DatumGetByteaP(
        DirectFunctionCall1(byteain, CStringGetDatum(input)));
    const uint8 *data = (const uint8 *) VARDATA_ANY(raw);
    size_t size = VARSIZE_ANY_EXHDR(raw);
    AcdatMachineView view;

    require_valid_payload(data, size, &view);
    PG_RETURN_POINTER(copy_payload_as_machine(data, size));
}

Datum
acdat_machine_out(PG_FUNCTION_ARGS)
{
    struct varlena *machine = PG_GETARG_VARLENA_PP(0);
    Datum output = DirectFunctionCall1(byteaout, PointerGetDatum(machine));

    PG_FREE_IF_COPY(machine, 0);
    return output;
}

Datum
acdat_machine_recv(PG_FUNCTION_ARGS)
{
    StringInfo buffer = (StringInfo) PG_GETARG_POINTER(0);
    int size = buffer->len - buffer->cursor;
    const uint8 *data = (const uint8 *) pq_getmsgbytes(buffer, size);
    AcdatMachineView view;

    require_valid_payload(data, size, &view);
    PG_RETURN_POINTER(copy_payload_as_machine(data, size));
}

Datum
acdat_machine_send(PG_FUNCTION_ARGS)
{
    struct varlena *machine = PG_GETARG_VARLENA_PP(0);
    StringInfoData buffer;

    pq_begintypsend(&buffer);
    pq_sendbytes(&buffer, VARDATA_ANY(machine), VARSIZE_ANY_EXHDR(machine));
    PG_FREE_IF_COPY(machine, 0);
    PG_RETURN_BYTEA_P(pq_endtypsend(&buffer));
}

Datum
acdat_compile_text_array(PG_FUNCTION_ARGS)
{
    struct varlena *machine = compile_array(fcinfo, ACDAT_INPUT_TEXT);

    if (machine == NULL)
        PG_RETURN_NULL();
    PG_RETURN_POINTER(machine);
}

Datum
acdat_compile_bytea_array(PG_FUNCTION_ARGS)
{
    struct varlena *machine = compile_array(fcinfo, ACDAT_INPUT_BYTEA);

    if (machine == NULL)
        PG_RETURN_NULL();
    PG_RETURN_POINTER(machine);
}

Datum
acdat_compile_text_trans(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(compile_aggregate_transition(fcinfo, ACDAT_INPUT_TEXT,
                                                   true));
}

Datum
acdat_compile_text_trans_simple(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(compile_aggregate_transition(fcinfo, ACDAT_INPUT_TEXT,
                                                   false));
}

Datum
acdat_compile_bytea_trans(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(compile_aggregate_transition(fcinfo, ACDAT_INPUT_BYTEA,
                                                   true));
}

Datum
acdat_compile_bytea_trans_simple(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(compile_aggregate_transition(fcinfo, ACDAT_INPUT_BYTEA,
                                                   false));
}

Datum
acdat_compile_final(PG_FUNCTION_ARGS)
{
    return compile_aggregate_final(fcinfo);
}

Datum
acdat_contains_text(PG_FUNCTION_ARGS)
{
    text *input = PG_GETARG_TEXT_PP(0);
    Datum machine_datum = PG_GETARG_DATUM(1);
    PgMachineCache *cache = get_machine_cache(fcinfo, machine_datum,
                                              ACDAT_INPUT_TEXT);
    bool result = acdat_contains_ex(cache->automaton,
                                    (const uint8 *) VARDATA_ANY(input),
                                    VARSIZE_ANY_EXHDR(input),
                                    check_postgres_interrupts, NULL);

    PG_FREE_IF_COPY(input, 0);
    PG_RETURN_BOOL(result);
}

Datum
acdat_contains_bytea(PG_FUNCTION_ARGS)
{
    bytea *input = PG_GETARG_BYTEA_PP(0);
    Datum machine_datum = PG_GETARG_DATUM(1);
    PgMachineCache *cache = get_machine_cache(fcinfo, machine_datum,
                                              ACDAT_INPUT_BYTEA);
    bool result = acdat_contains_ex(cache->automaton,
                                    (const uint8 *) VARDATA_ANY(input),
                                    VARSIZE_ANY_EXHDR(input),
                                    check_postgres_interrupts, NULL);

    PG_FREE_IF_COPY(input, 0);
    PG_RETURN_BOOL(result);
}

Datum
acdat_matches_text(PG_FUNCTION_ARGS)
{
    return matches_common(fcinfo, ACDAT_INPUT_TEXT);
}

Datum
acdat_matches_bytea(PG_FUNCTION_ARGS)
{
    return matches_common(fcinfo, ACDAT_INPUT_BYTEA);
}

Datum
acdat_replace_text(PG_FUNCTION_ARGS)
{
    return replace_common(fcinfo, ACDAT_INPUT_TEXT);
}

Datum
acdat_replace_bytea(PG_FUNCTION_ARGS)
{
    return replace_common(fcinfo, ACDAT_INPUT_BYTEA);
}

Datum
acdat_machine_info(PG_FUNCTION_ARGS)
{
    struct varlena *machine = PG_GETARG_VARLENA_PP(0);
    const uint8 *data = (const uint8 *) VARDATA_ANY(machine);
    uint32 size = (uint32) VARSIZE_ANY_EXHDR(machine);
    AcdatMachineView view;
    char source_hex[65];
    char build_hex[65];
    char checksum_hex[65];
    StringInfoData json;
    Datum result;

    require_valid_payload(data, size, &view);
    hex_fingerprint(view.source_fingerprint, source_hex);
    hex_fingerprint(view.build_fingerprint, build_hex);
    hex_fingerprint(view.artifact_checksum, checksum_hex);
    initStringInfo(&json);
    appendStringInfo(&json,
                     "{\"format_major\":%u,\"format_minor\":%u,"
                     "\"compiler_version\":\"%u.%u.%u\","
                     "\"engine\":\"byte256\",\"input_kind\":\"%s\","
                     "\"database_encoding\":\"%s\",\"pattern_count\":%u,"
                     "\"state_count\":%u,\"slot_count\":%u,"
                     "\"max_pattern_bytes\":%u,\"max_pattern_chars\":%u,"
                     "\"replacement_bytes\":%u,\"compiled_bytes\":%u,"
                     "\"source_fingerprint\":\"%s\","
                     "\"build_fingerprint\":\"%s\","
                     "\"artifact_checksum\":\"%s\"}",
                     view.format_major, view.format_minor,
                     view.compiler_major, view.compiler_minor,
                     view.compiler_patch,
                     view.input_kind == ACDAT_INPUT_TEXT ? "text" : "bytea",
                     view.input_kind == ACDAT_INPUT_TEXT ?
                     pg_encoding_to_char(view.database_encoding) : "binary",
                     view.pattern_count, view.state_count, view.slot_count,
                     view.max_pattern_bytes, view.max_pattern_chars,
                     view.replacement_bytes, view.total_size,
                     source_hex, build_hex, checksum_hex);
    result = DirectFunctionCall1(jsonb_in, CStringGetDatum(json.data));
    pfree(json.data);
    PG_FREE_IF_COPY(machine, 0);
    return result;
}

Datum
acdat_machine_validate(PG_FUNCTION_ARGS)
{
    bytea *raw = PG_GETARG_BYTEA_PP(0);
    AcdatMachineView view;
    AcdatError error;
    AcdatStatus status = validate_payload((const uint8 *) VARDATA_ANY(raw),
                                          VARSIZE_ANY_EXHDR(raw),
                                          &view, &error);

    PG_FREE_IF_COPY(raw, 0);
    PG_RETURN_BOOL(status == ACDAT_OK);
}

Datum
acdat_machine_export(PG_FUNCTION_ARGS)
{
    struct varlena *machine = PG_GETARG_VARLENA_PP(0);
    bytea *result = (bytea *)
        PG_DETOAST_DATUM_COPY(PointerGetDatum(machine));

    PG_FREE_IF_COPY(machine, 0);
    PG_RETURN_BYTEA_P(result);
}

Datum
acdat_machine_import(PG_FUNCTION_ARGS)
{
    bytea *raw = PG_GETARG_BYTEA_PP(0);
    const uint8 *data = (const uint8 *) VARDATA_ANY(raw);
    size_t size = VARSIZE_ANY_EXHDR(raw);
    AcdatMachineView view;
    struct varlena *result;

    require_valid_payload(data, size, &view);
    result = copy_payload_as_machine(data, size);
    PG_FREE_IF_COPY(raw, 0);
    PG_RETURN_POINTER(result);
}

static bytea *
fingerprint_result(FunctionCallInfo fcinfo, uint32 offset)
{
    struct varlena *machine = PG_GETARG_VARLENA_PP(0);
    const uint8 *data = (const uint8 *) VARDATA_ANY(machine);
    uint32 size = (uint32) VARSIZE_ANY_EXHDR(machine);
    AcdatMachineView view;
    bytea *result;

    require_valid_payload(data, size, &view);
    result = (bytea *) palloc(VARHDRSZ + ACDAT_FINGERPRINT_SIZE);
    SET_VARSIZE(result, VARHDRSZ + ACDAT_FINGERPRINT_SIZE);
    memcpy(VARDATA(result), data + offset, ACDAT_FINGERPRINT_SIZE);
    PG_FREE_IF_COPY(machine, 0);
    return result;
}

Datum
acdat_source_fingerprint(PG_FUNCTION_ARGS)
{
    PG_RETURN_BYTEA_P(fingerprint_result(
        fcinfo, ACDAT_HEADER_SOURCE_FINGERPRINT_OFFSET));
}

Datum
acdat_build_fingerprint(PG_FUNCTION_ARGS)
{
    PG_RETURN_BYTEA_P(fingerprint_result(
        fcinfo, ACDAT_HEADER_BUILD_FINGERPRINT_OFFSET));
}

Datum
acdat_artifact_checksum(PG_FUNCTION_ARGS)
{
    PG_RETURN_BYTEA_P(fingerprint_result(
        fcinfo, ACDAT_HEADER_CHECKSUM_OFFSET));
}

Datum
acdat_catalog_build_id(PG_FUNCTION_ARGS)
{
    static const char prefix[] = "acdat-catalog-build-v1";
    text *dictionary_name = PG_GETARG_TEXT_PP(0);
    struct varlena *machine = PG_GETARG_VARLENA_PP(1);
    const uint8 *data = (const uint8 *) VARDATA_ANY(machine);
    uint32 size = (uint32) VARSIZE_ANY_EXHDR(machine);
    AcdatMachineView view;
    pg_cryptohash_ctx *context;
    uint8 digest[PG_SHA256_DIGEST_LENGTH];
    bytea *result;

    require_valid_payload(data, size, &view);
    context = sha256_create();
    sha256_update(context, prefix, sizeof(prefix) - 1U);
    sha256_update_u32(context, (uint32) VARSIZE_ANY_EXHDR(dictionary_name));
    sha256_update(context, VARDATA_ANY(dictionary_name),
                  VARSIZE_ANY_EXHDR(dictionary_name));
    sha256_update(context, view.build_fingerprint,
                  ACDAT_FINGERPRINT_SIZE);
    sha256_final(context, digest);

    result = palloc(VARHDRSZ + PG_SHA256_DIGEST_LENGTH);
    SET_VARSIZE(result, VARHDRSZ + PG_SHA256_DIGEST_LENGTH);
    memcpy(VARDATA(result), digest, PG_SHA256_DIGEST_LENGTH);
    PG_FREE_IF_COPY(machine, 1);
    PG_FREE_IF_COPY(dictionary_name, 0);
    PG_RETURN_BYTEA_P(result);
}
