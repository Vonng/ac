#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

#define ACDAT_DICTIONARY_MAGIC 0x41434431U

typedef struct AcdatDictionaryHeader
{
    uint32 magic;
    uint32 version;
    uint32 pattern_count;
    uint32 data_bytes;
} AcdatDictionaryHeader;

PG_FUNCTION_INFO_V1(acdat_version);
PG_FUNCTION_INFO_V1(acdat_compile);

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
    Size data_bytes = 0;
    Size result_bytes;
    bytea *result;
    AcdatDictionaryHeader *header;
    char *cursor;
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
        if (length > PG_UINT32_MAX ||
            data_bytes > MaxAllocSize - sizeof(uint32) - length)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("compiled dictionary is too large")));
        data_bytes += sizeof(uint32) + length;
    }

    if (data_bytes > PG_UINT32_MAX ||
        data_bytes > MaxAllocSize - VARHDRSZ - sizeof(*header))
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("compiled dictionary is too large")));

    result_bytes = VARHDRSZ + sizeof(*header) + data_bytes;
    result = (bytea *) palloc0(result_bytes);
    SET_VARSIZE(result, result_bytes);
    header = (AcdatDictionaryHeader *) VARDATA(result);
    header->magic = ACDAT_DICTIONARY_MAGIC;
    header->version = 1;
    header->pattern_count = (uint32) count;
    header->data_bytes = (uint32) data_bytes;
    cursor = (char *) (header + 1);

    for (i = 0; i < count; i++)
    {
        text *pattern = DatumGetTextPP(values[i]);
        uint32 length = (uint32) VARSIZE_ANY_EXHDR(pattern);

        memcpy(cursor, &length, sizeof(length));
        cursor += sizeof(length);
        memcpy(cursor, VARDATA_ANY(pattern), length);
        cursor += length;
    }

    PG_RETURN_BYTEA_P(result);
}
