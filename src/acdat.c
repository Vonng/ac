#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

#define ACDAT_DICTIONARY_MAGIC 0x41434431U
#define ACDAT_ALPHABET_SIZE 256

typedef struct AcdatDictionaryHeader
{
    uint32 magic;
    uint32 version;
    uint32 node_count;
    uint32 pattern_count;
} AcdatDictionaryHeader;

typedef struct AcdatNode
{
    int32 next[ACDAT_ALPHABET_SIZE];
    uint32 fail;
    uint32 terminal;
} AcdatNode;

PG_FUNCTION_INFO_V1(acdat_version);
PG_FUNCTION_INFO_V1(acdat_compile);

static void
acdat_init_node(AcdatNode *node)
{
    int symbol;

    for (symbol = 0; symbol < ACDAT_ALPHABET_SIZE; symbol++)
        node->next[symbol] = -1;
    node->fail = 0;
    node->terminal = 0;
}

Datum
acdat_version(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text("0.0.1"));
}

Datum
acdat_compile(PG_FUNCTION_ARGS)
{
    ArrayType *patterns = PG_GETARG_ARRAYTYPE_P(0);
    Datum *values;
    bool *nulls;
    int count;
    Size max_nodes = 1;
    Size result_bytes;
    bytea *result;
    AcdatDictionaryHeader *header;
    AcdatNode *nodes;
    uint32 node_count = 1;
    uint32 *queue;
    uint32 queue_head = 0;
    uint32 queue_tail = 0;
    int i;

    if (ARR_NDIM(patterns) != 1)
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("patterns must be a one-dimensional array")));

    deconstruct_array(patterns, TEXTOID, -1, false, TYPALIGN_INT,
                      &values, &nulls, &count);

    if (count == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("patterns must not be empty")));

    for (i = 0; i < count; i++)
    {
        text *pattern;
        Size length;

        if (nulls[i])
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("patterns must not contain nulls")));

        pattern = DatumGetTextPP(values[i]);
        length = VARSIZE_ANY_EXHDR(pattern);
        if (length == 0)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("patterns must not contain empty strings")));
        if (length > PG_UINT32_MAX || max_nodes > PG_UINT32_MAX - length)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("compiled dictionary is too large")));
        max_nodes += length;
    }

    if (max_nodes > (MaxAllocSize - VARHDRSZ - sizeof(*header)) /
                    sizeof(AcdatNode))
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("compiled dictionary is too large")));

    result_bytes = VARHDRSZ + sizeof(*header) + max_nodes * sizeof(AcdatNode);
    result = (bytea *) palloc0(result_bytes);
    header = (AcdatDictionaryHeader *) VARDATA(result);
    nodes = (AcdatNode *) (header + 1);

    for (i = 0; i < (int) max_nodes; i++)
        acdat_init_node(&nodes[i]);

    for (i = 0; i < count; i++)
    {
        text *pattern = DatumGetTextPP(values[i]);
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
    header->version = 1;
    header->node_count = node_count;
    header->pattern_count = (uint32) count;
    SET_VARSIZE(result,
                VARHDRSZ + sizeof(*header) + node_count * sizeof(AcdatNode));

    PG_RETURN_BYTEA_P(result);
}
