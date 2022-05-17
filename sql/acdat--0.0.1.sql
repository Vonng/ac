\echo Use "CREATE EXTENSION acdat" to load this file. \quit

CREATE FUNCTION acdat_version()
RETURNS text
AS 'MODULE_PATHNAME', 'acdat_version'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

