\echo Use "CREATE EXTENSION acdat" to load this file. \quit

CREATE FUNCTION acdat_version()
RETURNS text
AS 'MODULE_PATHNAME', 'acdat_version'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION acdat_compile(
    patterns text[],
    replacements text[] DEFAULT NULL
)
RETURNS bytea
AS 'MODULE_PATHNAME', 'acdat_compile'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION acdat_contains(input text, dictionary bytea)
RETURNS boolean
AS 'MODULE_PATHNAME', 'acdat_contains'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
