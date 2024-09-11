CREATE OR REPLACE FUNCTION acdat_version()
RETURNS text
AS 'MODULE_PATHNAME', 'acdat_version'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

DROP FUNCTION acdat_compile(text[]);

CREATE FUNCTION acdat_compile(
    patterns text[],
    replacements text[] DEFAULT NULL
)
RETURNS bytea
AS 'MODULE_PATHNAME', 'acdat_compile'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION acdat_replace(input text, dictionary bytea)
RETURNS text
AS 'MODULE_PATHNAME', 'acdat_replace'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
