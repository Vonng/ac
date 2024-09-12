#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

#define ACDAT_DICTIONARY_MAGIC 0x41434431U
#define ACDAT_DICTIONARY_VERSION 2U
#define ACDAT_ALPHABET_SIZE 256
#define ACDAT_NO_REPLACEMENT PG_UINT32_MAX

typedef struct AcdatDictionaryHeader
{
    uint32 magic;
    uint32 version;
    uint32 node_count;
    uint32 pattern_count;
    uint32 records_offset;
    uint32 records_bytes;
} AcdatDictionaryHeader;

typedef struct AcdatNode
{
    int32 next[ACDAT_ALPHABET_SIZE];
    uint32 fail;
    uint32 terminal;
} AcdatNode;

typedef struct AcdatPatternRecord
{
    uint32 pattern_length;
    uint32 replacement_length;
} AcdatPatternRecord;

PG_FUNCTION_INFO_V1(acdat_version);
PG_FUNCTION_INFO_V1(acdat_compile);
PG_FUNCTION_INFO_V1(acdat_contains);
PG_FUNCTION_INFO_V1(acdat_replace);

static void
acdat_invalid_dictionary(const char *detail)
{
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
             errmsg("invalid acdat dictionary"),
             errdetail_internal("%s", detail)));
}

static void
acdat_init_node(AcdatNode *node)
{
    int symbol;

    for (symbol = 0; symbol < ACDAT_ALPHABET_SIZE; symbol++)
        node->next[symbol] = -1;
    node->fail = 0;
    node->terminal = 0;
}

static void
acdat_read_dictionary(bytea *dictionary,
                      AcdatDictionaryHeader **header_out,
                      AcdatNode **nodes_out,
                      const char **records_out)
{
    Size size = VARSIZE_ANY_EXHDR(dictionary);
    char *data = VARDATA_ANY(dictionary);
    AcdatDictionaryHeader *header;
    AcdatNode *nodes;
    Size expected_records_offset;
    const char *cursor;
    const char *end;
    uint32 i;

    if (size < sizeof(AcdatDictionaryHeader))
        acdat_invalid_dictionary("header is truncated");

    header = (AcdatDictionaryHeader *) data;
    if (header->magic != ACDAT_DICTIONARY_MAGIC ||
        header->version != ACDAT_DICTIONARY_VERSION ||
        header->node_count == 0)
        acdat_invalid_dictionary("header values are not recognized");

    if (header->node_count >
        (MaxAllocSize - sizeof(*header)) / sizeof(AcdatNode))
        acdat_invalid_dictionary("node count is out of range");
    expected_records_offset =
        sizeof(*header) + header->node_count * sizeof(AcdatNode);
    if (header->records_offset != expected_records_offset ||
        header->records_offset > size ||
        header->records_bytes != size - header->records_offset)
        acdat_invalid_dictionary("record section size is inconsistent");

    nodes = (AcdatNode *) (header + 1);
    for (i = 0; i < header->node_count; i++)
    {
        int symbol;

        if (nodes[i].fail >= header->node_count)
            acdat_invalid_dictionary("failure link is out of range");
        for (symbol = 0; symbol < ACDAT_ALPHABET_SIZE; symbol++)
        {
            if (nodes[i].next[symbol] < -1 ||
                nodes[i].next[symbol] >= (int32) header->node_count)
                acdat_invalid_dictionary("transition is out of range");
        }
    }

    cursor = data + header->records_offset;
    end = cursor + header->records_bytes;
    for (i = 0; i < header->pattern_count; i++)
    {
        AcdatPatternRecord record;
        Size payload;

        if ((Size) (end - cursor) < sizeof(record))
            acdat_invalid_dictionary("pattern record is truncated");
        memcpy(&record, cursor, sizeof(record));
        cursor += sizeof(record);
        if (record.pattern_length == 0)
            acdat_invalid_dictionary("pattern record is empty");
        payload = record.pattern_length;
        if (record.replacement_length != ACDAT_NO_REPLACEMENT)
        {
            if (payload > MaxAllocSize - record.replacement_length)
                acdat_invalid_dictionary("pattern record is too large");
            payload += record.replacement_length;
        }
        if ((Size) (end - cursor) < payload)
            acdat_invalid_dictionary("pattern payload is truncated");
        cursor += payload;
    }
    if (cursor != end)
        acdat_invalid_dictionary("record section has trailing bytes");

    *header_out = header;
    *nodes_out = nodes;
    if (records_out != NULL)
        *records_out = data + header->records_offset;
}

Datum
acdat_version(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text("0.0.2"));
}

Datum
acdat_compile(PG_FUNCTION_ARGS)
{
    ArrayType *patterns = PG_GETARG_ARRAYTYPE_P(0);
    ArrayType *replacements = NULL;
    Datum *pattern_values;
    bool *pattern_nulls;
    Datum *replacement_values = NULL;
    bool *replacement_nulls = NULL;
    int count;
    int replacement_count = 0;
    Size max_nodes = 1;
    Size records_bytes = 0;
    Size allocation_bytes;
    bytea *result;
    AcdatDictionaryHeader *header;
    AcdatNode *nodes;
    uint32 node_count = 1;
    uint32 *queue;
    uint32 queue_head = 0;
    uint32 queue_tail = 0;
    char *record_cursor;
    int i;

    if (ARR_NDIM(patterns) != 1)
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("patterns must be a one-dimensional array")));

    deconstruct_array(patterns, TEXTOID, -1, false, TYPALIGN_INT,
                      &pattern_values, &pattern_nulls, &count);

    if (count == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("patterns must not be empty")));

    if (PG_NARGS() > 1 && !PG_ARGISNULL(1))
    {
        replacements = PG_GETARG_ARRAYTYPE_P(1);
        if (ARR_NDIM(replacements) != 1)
            ereport(ERROR,
                    (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                     errmsg("replacements must be a one-dimensional array")));
        deconstruct_array(replacements, TEXTOID, -1, false, TYPALIGN_INT,
                          &replacement_values, &replacement_nulls,
                          &replacement_count);
        if (replacement_count != count)
            ereport(ERROR,
                    (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                     errmsg("patterns and replacements must have the same length")));
    }

    for (i = 0; i < count; i++)
    {
        text *pattern;
        Size pattern_length;
        Size replacement_length = 0;

        if (pattern_nulls[i])
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("patterns must not contain nulls")));

        pattern = DatumGetTextPP(pattern_values[i]);
        pattern_length = VARSIZE_ANY_EXHDR(pattern);
        if (pattern_length == 0)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("patterns must not contain empty strings")));
        if (replacement_values != NULL && !replacement_nulls[i])
        {
            text *replacement = DatumGetTextPP(replacement_values[i]);

            replacement_length = VARSIZE_ANY_EXHDR(replacement);
        }

        if (pattern_length > PG_UINT32_MAX ||
            replacement_length > PG_UINT32_MAX ||
            max_nodes > PG_UINT32_MAX - pattern_length)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("compiled dictionary is too large")));
        max_nodes += pattern_length;

        if (records_bytes > MaxAllocSize - sizeof(AcdatPatternRecord) -
                            pattern_length - replacement_length)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("compiled dictionary is too large")));
        records_bytes += sizeof(AcdatPatternRecord) + pattern_length +
                         replacement_length;
    }

    if (max_nodes >
        (MaxAllocSize - VARHDRSZ - sizeof(*header) - records_bytes) /
        sizeof(AcdatNode))
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("compiled dictionary is too large")));

    allocation_bytes = VARHDRSZ + sizeof(*header) +
                       max_nodes * sizeof(AcdatNode) + records_bytes;
    result = (bytea *) palloc0(allocation_bytes);
    header = (AcdatDictionaryHeader *) VARDATA(result);
    nodes = (AcdatNode *) (header + 1);

    for (i = 0; i < (int) max_nodes; i++)
        acdat_init_node(&nodes[i]);

    for (i = 0; i < count; i++)
    {
        text *pattern = DatumGetTextPP(pattern_values[i]);
        const unsigned char *bytes =
            (const unsigned char *) VARDATA_ANY(pattern);
        Size length = VARSIZE_ANY_EXHDR(pattern);
        uint32 state = 0;
        Size offset;

        for (offset = 0; offset < length; offset++)
        {
            unsigned int symbol = bytes[offset];

            if (nodes[state].next[symbol] < 0)
                nodes[state].next[symbol] = (int32) node_count++;
            state = (uint32) nodes[state].next[symbol];
        }
        nodes[state].terminal = 1;
    }

    queue = (uint32 *) palloc(sizeof(uint32) * node_count);
    for (i = 0; i < ACDAT_ALPHABET_SIZE; i++)
    {
        int32 child = nodes[0].next[i];

        if (child >= 0)
            queue[queue_tail++] = (uint32) child;
    }
    while (queue_head < queue_tail)
    {
        uint32 state = queue[queue_head++];
        int symbol;

        for (symbol = 0; symbol < ACDAT_ALPHABET_SIZE; symbol++)
        {
            int32 child = nodes[state].next[symbol];
            uint32 failure;

            if (child < 0)
                continue;
            failure = nodes[state].fail;
            while (failure != 0 && nodes[failure].next[symbol] < 0)
                failure = nodes[failure].fail;
            if (nodes[failure].next[symbol] >= 0)
                failure = (uint32) nodes[failure].next[symbol];
            nodes[child].fail = failure;
            if (nodes[failure].terminal)
                nodes[child].terminal = 1;
            queue[queue_tail++] = (uint32) child;
        }
    }

    header->magic = ACDAT_DICTIONARY_MAGIC;
    header->version = ACDAT_DICTIONARY_VERSION;
    header->node_count = node_count;
    header->pattern_count = (uint32) count;
    header->records_offset =
        (uint32) (sizeof(*header) + node_count * sizeof(AcdatNode));
    header->records_bytes = (uint32) records_bytes;
    record_cursor = VARDATA(result) + header->records_offset;

    for (i = 0; i < count; i++)
    {
        text *pattern = DatumGetTextPP(pattern_values[i]);
        AcdatPatternRecord record;

        record.pattern_length = (uint32) VARSIZE_ANY_EXHDR(pattern);
        record.replacement_length = ACDAT_NO_REPLACEMENT;
        if (replacement_values != NULL && !replacement_nulls[i])
        {
            text *replacement = DatumGetTextPP(replacement_values[i]);

            record.replacement_length =
                (uint32) VARSIZE_ANY_EXHDR(replacement);
        }
        memcpy(record_cursor, &record, sizeof(record));
        record_cursor += sizeof(record);
        memcpy(record_cursor, VARDATA_ANY(pattern), record.pattern_length);
        record_cursor += record.pattern_length;
        if (record.replacement_length != ACDAT_NO_REPLACEMENT)
        {
            text *replacement = DatumGetTextPP(replacement_values[i]);

            memcpy(record_cursor, VARDATA_ANY(replacement),
                   record.replacement_length);
            record_cursor += record.replacement_length;
        }
    }

    SET_VARSIZE(result, VARHDRSZ + header->records_offset + records_bytes);
    PG_RETURN_BYTEA_P(result);
}

Datum
acdat_contains(PG_FUNCTION_ARGS)
{
    text *input = PG_GETARG_TEXT_PP(0);
    bytea *dictionary = PG_GETARG_BYTEA_PP(1);
    AcdatDictionaryHeader *header;
    AcdatNode *nodes;
    const unsigned char *bytes =
        (const unsigned char *) VARDATA_ANY(input);
    Size length = VARSIZE_ANY_EXHDR(input);
    uint32 state = 0;
    Size offset;

    acdat_read_dictionary(dictionary, &header, &nodes, NULL);
    (void) header;

    for (offset = 0; offset < length; offset++)
    {
        unsigned int symbol = bytes[offset];

        while (state != 0 && nodes[state].next[symbol] < 0)
            state = nodes[state].fail;
        if (nodes[state].next[symbol] >= 0)
            state = (uint32) nodes[state].next[symbol];
        if (nodes[state].terminal)
            PG_RETURN_BOOL(true);
    }

    PG_RETURN_BOOL(false);
}

Datum
acdat_replace(PG_FUNCTION_ARGS)
{
    text *input = PG_GETARG_TEXT_PP(0);
    bytea *dictionary = PG_GETARG_BYTEA_PP(1);
    AcdatDictionaryHeader *header;
    AcdatNode *nodes;
    const char *records;
    const char *input_data = VARDATA_ANY(input);
    Size input_length = VARSIZE_ANY_EXHDR(input);
    Size input_offset = 0;
    StringInfoData output;

    acdat_read_dictionary(dictionary, &header, &nodes, &records);
    (void) nodes;
    initStringInfo(&output);

    while (input_offset < input_length)
    {
        const char *cursor = records;
        const char *best_replacement = NULL;
        uint32 best_pattern_length = 0;
        uint32 best_replacement_length = 0;
        uint32 i;

        for (i = 0; i < header->pattern_count; i++)
        {
            AcdatPatternRecord record;
            const char *pattern_data;
            const char *replacement_data = NULL;

            memcpy(&record, cursor, sizeof(record));
            cursor += sizeof(record);
            pattern_data = cursor;
            cursor += record.pattern_length;
            if (record.replacement_length != ACDAT_NO_REPLACEMENT)
            {
                replacement_data = cursor;
                cursor += record.replacement_length;
            }

            if (replacement_data != NULL &&
                record.pattern_length <= input_length - input_offset &&
                record.pattern_length > best_pattern_length &&
                memcmp(input_data + input_offset, pattern_data,
                       record.pattern_length) == 0)
            {
                best_pattern_length = record.pattern_length;
                best_replacement_length = record.replacement_length;
                best_replacement = replacement_data;
            }
        }

        if (best_replacement != NULL)
        {
            appendBinaryStringInfo(&output, best_replacement,
                                   best_replacement_length);
            input_offset += best_pattern_length;
        }
        else
        {
            appendStringInfoCharMacro(&output, input_data[input_offset]);
            input_offset++;
        }
    }

    PG_RETURN_TEXT_P(cstring_to_text_with_len(output.data, output.len));
}
