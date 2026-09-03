\echo Use "CREATE EXTENSION acdat" to load this file. \quit

CREATE TYPE @extschema@.machine;

CREATE FUNCTION @extschema@.machine_in(cstring)
RETURNS @extschema@.machine
AS 'MODULE_PATHNAME', 'acdat_machine_in'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@.machine_out(@extschema@.machine)
RETURNS cstring
AS 'MODULE_PATHNAME', 'acdat_machine_out'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@.machine_recv(internal)
RETURNS @extschema@.machine
AS 'MODULE_PATHNAME', 'acdat_machine_recv'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@.machine_send(@extschema@.machine)
RETURNS bytea
AS 'MODULE_PATHNAME', 'acdat_machine_send'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE @extschema@.machine (
    INPUT = @extschema@.machine_in,
    OUTPUT = @extschema@.machine_out,
    RECEIVE = @extschema@.machine_recv,
    SEND = @extschema@.machine_send,
    INTERNALLENGTH = VARIABLE,
    ALIGNMENT = double,
    STORAGE = external
);

CREATE TYPE @extschema@.hit AS (
    pattern_id  bigint,
    start_byte  integer,
    byte_length integer,
    start_char  integer,
    char_length integer,
    priority    integer
);

CREATE FUNCTION @extschema@._compile_text_trans(
    internal, text, bigint, text, integer
)
RETURNS internal
AS 'MODULE_PATHNAME', 'acdat_compile_text_trans'
LANGUAGE C IMMUTABLE PARALLEL UNSAFE;

CREATE FUNCTION @extschema@._compile_text_trans_simple(
    internal, text, bigint
)
RETURNS internal
AS 'MODULE_PATHNAME', 'acdat_compile_text_trans_simple'
LANGUAGE C IMMUTABLE PARALLEL UNSAFE;

CREATE FUNCTION @extschema@._compile_bytea_trans(
    internal, bytea, bigint, bytea, integer
)
RETURNS internal
AS 'MODULE_PATHNAME', 'acdat_compile_bytea_trans'
LANGUAGE C IMMUTABLE PARALLEL UNSAFE;

CREATE FUNCTION @extschema@._compile_bytea_trans_simple(
    internal, bytea, bigint
)
RETURNS internal
AS 'MODULE_PATHNAME', 'acdat_compile_bytea_trans_simple'
LANGUAGE C IMMUTABLE PARALLEL UNSAFE;

CREATE FUNCTION @extschema@._compile_final(internal)
RETURNS @extschema@.machine
AS 'MODULE_PATHNAME', 'acdat_compile_final'
LANGUAGE C IMMUTABLE PARALLEL UNSAFE;

CREATE AGGREGATE @extschema@.compile(text, bigint, text, integer) (
    SFUNC = @extschema@._compile_text_trans,
    STYPE = internal,
    FINALFUNC = @extschema@._compile_final,
    PARALLEL = UNSAFE
);

CREATE AGGREGATE @extschema@.compile(text, bigint) (
    SFUNC = @extschema@._compile_text_trans_simple,
    STYPE = internal,
    FINALFUNC = @extschema@._compile_final,
    PARALLEL = UNSAFE
);

CREATE AGGREGATE @extschema@.compile(bytea, bigint, bytea, integer) (
    SFUNC = @extschema@._compile_bytea_trans,
    STYPE = internal,
    FINALFUNC = @extschema@._compile_final,
    PARALLEL = UNSAFE
);

CREATE AGGREGATE @extschema@.compile(bytea, bigint) (
    SFUNC = @extschema@._compile_bytea_trans_simple,
    STYPE = internal,
    FINALFUNC = @extschema@._compile_final,
    PARALLEL = UNSAFE
);

CREATE FUNCTION @extschema@.compile(
    patterns text[],
    pattern_ids bigint[] DEFAULT NULL,
    replacements text[] DEFAULT NULL,
    priorities integer[] DEFAULT NULL,
    options jsonb DEFAULT '{}'::jsonb
)
RETURNS @extschema@.machine
AS 'MODULE_PATHNAME', 'acdat_compile_text_array'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION @extschema@.compile(
    patterns bytea[],
    pattern_ids bigint[] DEFAULT NULL,
    replacements bytea[] DEFAULT NULL,
    priorities integer[] DEFAULT NULL,
    options jsonb DEFAULT '{}'::jsonb
)
RETURNS @extschema@.machine
AS 'MODULE_PATHNAME', 'acdat_compile_bytea_array'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION @extschema@.contains(text, @extschema@.machine)
RETURNS boolean
AS 'MODULE_PATHNAME', 'acdat_contains_text'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
COST 100;

CREATE FUNCTION @extschema@.contains(bytea, @extschema@.machine)
RETURNS boolean
AS 'MODULE_PATHNAME', 'acdat_contains_bytea'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
COST 100;

CREATE FUNCTION @extschema@.matches(
    input text,
    machine @extschema@.machine,
    policy text DEFAULT 'all_overlapping',
    max_matches integer DEFAULT 10000
)
RETURNS SETOF @extschema@.hit
AS 'MODULE_PATHNAME', 'acdat_matches_text'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
COST 100 ROWS 1000;

CREATE FUNCTION @extschema@.matches(
    input bytea,
    machine @extschema@.machine,
    policy text DEFAULT 'all_overlapping',
    max_matches integer DEFAULT 10000
)
RETURNS SETOF @extschema@.hit
AS 'MODULE_PATHNAME', 'acdat_matches_bytea'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
COST 100 ROWS 1000;

CREATE FUNCTION @extschema@.replace(
    input text,
    machine @extschema@.machine,
    policy text DEFAULT 'leftmost_longest',
    max_replacements integer DEFAULT 10000,
    max_output_bytes bigint DEFAULT 268435456
)
RETURNS text
AS 'MODULE_PATHNAME', 'acdat_replace_text'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
COST 100;

CREATE FUNCTION @extschema@.replace(
    input bytea,
    machine @extschema@.machine,
    policy text DEFAULT 'leftmost_longest',
    max_replacements integer DEFAULT 10000,
    max_output_bytes bigint DEFAULT 268435456
)
RETURNS bytea
AS 'MODULE_PATHNAME', 'acdat_replace_bytea'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
COST 100;

CREATE FUNCTION @extschema@.info(@extschema@.machine)
RETURNS jsonb
AS 'MODULE_PATHNAME', 'acdat_machine_info'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@.validate(bytea)
RETURNS boolean
AS 'MODULE_PATHNAME', 'acdat_machine_validate'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@.export(@extschema@.machine)
RETURNS bytea
AS 'MODULE_PATHNAME', 'acdat_machine_export'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@.import(bytea)
RETURNS @extschema@.machine
AS 'MODULE_PATHNAME', 'acdat_machine_import'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@.source_fingerprint(@extschema@.machine)
RETURNS bytea
AS 'MODULE_PATHNAME', 'acdat_source_fingerprint'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@.build_fingerprint(@extschema@.machine)
RETURNS bytea
AS 'MODULE_PATHNAME', 'acdat_build_fingerprint'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@.artifact_checksum(@extschema@.machine)
RETURNS bytea
AS 'MODULE_PATHNAME', 'acdat_artifact_checksum'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@._catalog_build_id(
    text, @extschema@.machine
)
RETURNS bytea
AS 'MODULE_PATHNAME', 'acdat_catalog_build_id'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TABLE @extschema@.dictionary (
    name       text PRIMARY KEY,
    input_kind text NOT NULL CHECK (input_kind IN ('text', 'bytea')),
    created_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE @extschema@.dictionary_version (
    dictionary_name    text NOT NULL,
    version            bigint NOT NULL,
    source_fingerprint bytea NOT NULL
        CHECK (octet_length(source_fingerprint) = 32),
    source_ref         jsonb,
    created_at         timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (dictionary_name, version),
    UNIQUE (dictionary_name, source_fingerprint),
    FOREIGN KEY (dictionary_name)
        REFERENCES @extschema@.dictionary(name)
        DEFERRABLE INITIALLY DEFERRED
);

CREATE TABLE @extschema@.dictionary_build (
    build_id          bytea PRIMARY KEY CHECK (octet_length(build_id) = 32),
    dictionary_name   text NOT NULL,
    version           bigint NOT NULL,
    compiler_version  text NOT NULL,
    format_major      integer NOT NULL,
    format_minor      integer NOT NULL,
    engine            text NOT NULL,
    artifact_checksum bytea NOT NULL
        CHECK (octet_length(artifact_checksum) = 32),
    status            text NOT NULL CHECK (status IN ('ready', 'retired')),
    info              jsonb NOT NULL,
    built_at          timestamptz NOT NULL DEFAULT now(),
    FOREIGN KEY (dictionary_name, version)
        REFERENCES @extschema@.dictionary_version(dictionary_name, version)
        DEFERRABLE INITIALLY DEFERRED
);

CREATE TABLE @extschema@.dictionary_build_data (
    build_id bytea PRIMARY KEY,
    machine  @extschema@.machine NOT NULL,
    FOREIGN KEY (build_id)
        REFERENCES @extschema@.dictionary_build(build_id)
        DEFERRABLE INITIALLY DEFERRED
);

ALTER TABLE @extschema@.dictionary_build_data
    ALTER COLUMN machine SET STORAGE EXTERNAL;

CREATE TABLE @extschema@.active_dictionary (
    dictionary_name text PRIMARY KEY,
    build_id        bytea NOT NULL UNIQUE,
    activated_at    timestamptz NOT NULL DEFAULT now(),
    FOREIGN KEY (dictionary_name)
        REFERENCES @extschema@.dictionary(name)
        DEFERRABLE INITIALLY DEFERRED,
    FOREIGN KEY (build_id)
        REFERENCES @extschema@.dictionary_build(build_id)
        DEFERRABLE INITIALLY DEFERRED
);

CREATE VIEW @extschema@.active_machine AS
SELECT d.name, d.input_kind,
       b.version, b.build_id, bd.machine, b.info
FROM @extschema@.dictionary AS d
JOIN @extschema@.active_dictionary AS a
  ON a.dictionary_name = d.name
JOIN @extschema@.dictionary_build AS b
  ON b.build_id = a.build_id
JOIN @extschema@.dictionary_build_data AS bd
  ON bd.build_id = b.build_id
WHERE b.status = 'ready';

CREATE FUNCTION @extschema@.publish(
    p_dictionary_name text,
    p_semantic_version bigint,
    p_compiled_machine @extschema@.machine,
    p_source_reference jsonb DEFAULT NULL
)
RETURNS bytea
LANGUAGE plpgsql
VOLATILE PARALLEL UNSAFE SECURITY INVOKER
SET search_path = pg_catalog, pg_temp
AS $function$
DECLARE
    machine_info jsonb := @extschema@.info(p_compiled_machine);
    machine_kind text := machine_info->>'input_kind';
    source_hash bytea := @extschema@.source_fingerprint(p_compiled_machine);
    build_hash bytea := @extschema@._catalog_build_id(
        p_dictionary_name, p_compiled_machine
    );
    checksum bytea := @extschema@.artifact_checksum(p_compiled_machine);
    stored_kind text;
    stored_source bytea;
    stored_dictionary text;
    stored_version bigint;
BEGIN
    IF p_dictionary_name IS NULL OR p_dictionary_name = '' THEN
        RAISE EXCEPTION USING
            ERRCODE = '22023',
            MESSAGE = 'acdat dictionary name must not be empty';
    END IF;

    PERFORM pg_catalog.pg_advisory_xact_lock(
        pg_catalog.hashtextextended(p_dictionary_name, 0)
    );

    INSERT INTO @extschema@.dictionary(name, input_kind)
    VALUES (p_dictionary_name, machine_kind)
    ON CONFLICT (name) DO NOTHING;

    SELECT input_kind INTO STRICT stored_kind
    FROM @extschema@.dictionary
    WHERE name = p_dictionary_name;
    IF stored_kind <> machine_kind THEN
        RAISE EXCEPTION USING
            ERRCODE = '42804',
            MESSAGE = pg_catalog.format(
                'acdat dictionary %L expects %s machine, got %s',
                p_dictionary_name, stored_kind, machine_kind
            );
    END IF;

    INSERT INTO @extschema@.dictionary_version(
        dictionary_name, version, source_fingerprint, source_ref
    )
    VALUES (
        p_dictionary_name, p_semantic_version, source_hash, p_source_reference
    )
    ON CONFLICT (dictionary_name, version) DO NOTHING;

    SELECT v.source_fingerprint INTO STRICT stored_source
    FROM @extschema@.dictionary_version AS v
    WHERE v.dictionary_name = p_dictionary_name
      AND v.version = p_semantic_version;
    IF stored_source <> source_hash THEN
        RAISE EXCEPTION USING
            ERRCODE = '23505',
            MESSAGE = pg_catalog.format(
                'acdat dictionary %L version %s already has another source',
                p_dictionary_name, p_semantic_version
            );
    END IF;

    INSERT INTO @extschema@.dictionary_build(
        build_id, dictionary_name, version, compiler_version,
        format_major, format_minor, engine, artifact_checksum,
        status, info
    )
    VALUES (
        build_hash, p_dictionary_name, p_semantic_version,
        machine_info->>'compiler_version',
        (machine_info->>'format_major')::integer,
        (machine_info->>'format_minor')::integer,
        machine_info->>'engine', checksum, 'ready', machine_info
    )
    ON CONFLICT (build_id) DO UPDATE SET status = 'ready';

    SELECT b.dictionary_name, b.version
    INTO STRICT stored_dictionary, stored_version
    FROM @extschema@.dictionary_build AS b
    WHERE b.build_id = build_hash;
    IF stored_dictionary <> p_dictionary_name OR
       stored_version <> p_semantic_version THEN
        RAISE EXCEPTION USING
            ERRCODE = '23505',
            MESSAGE = 'acdat content-addressed build ID conflict';
    END IF;

    INSERT INTO @extschema@.dictionary_build_data(build_id, machine)
    VALUES (build_hash, p_compiled_machine)
    ON CONFLICT (build_id) DO NOTHING;

    RETURN build_hash;
END
$function$;

CREATE FUNCTION @extschema@.activate(
    p_dictionary_name text,
    p_build_id bytea
)
RETURNS void
LANGUAGE plpgsql
VOLATILE PARALLEL UNSAFE SECURITY INVOKER
SET search_path = pg_catalog, pg_temp
AS $function$
BEGIN
    PERFORM pg_catalog.pg_advisory_xact_lock(
        pg_catalog.hashtextextended(p_dictionary_name, 0)
    );

    IF NOT EXISTS (
        SELECT 1
        FROM @extschema@.dictionary_build AS b
        JOIN @extschema@.dictionary_build_data AS bd USING (build_id)
        WHERE b.build_id = p_build_id
          AND b.dictionary_name = p_dictionary_name
          AND b.status = 'ready'
    ) THEN
        RAISE EXCEPTION USING
            ERRCODE = '22023',
            MESSAGE = 'acdat build is absent, retired, or has no artifact payload',
            HINT = 'Publish or recompile the build before activation.';
    END IF;

    INSERT INTO @extschema@.active_dictionary(
        dictionary_name, build_id, activated_at
    )
    VALUES (p_dictionary_name, p_build_id, pg_catalog.now())
    ON CONFLICT (dictionary_name) DO UPDATE
    SET build_id = EXCLUDED.build_id,
        activated_at = EXCLUDED.activated_at;
END
$function$;

CREATE FUNCTION @extschema@.retire_build(
    p_dictionary_name text,
    p_build_id bytea
)
RETURNS void
LANGUAGE plpgsql
VOLATILE PARALLEL UNSAFE SECURITY INVOKER
SET search_path = pg_catalog, pg_temp
AS $function$
BEGIN
    PERFORM pg_catalog.pg_advisory_xact_lock(
        pg_catalog.hashtextextended(p_dictionary_name, 0)
    );

    IF EXISTS (
        SELECT 1
        FROM @extschema@.active_dictionary
        WHERE active_dictionary.dictionary_name = p_dictionary_name
          AND build_id = p_build_id
    ) THEN
        RAISE EXCEPTION USING
            ERRCODE = '55006',
            MESSAGE = 'cannot retire the active acdat build';
    END IF;

    UPDATE @extschema@.dictionary_build
    SET status = 'retired'
    WHERE build_id = p_build_id
      AND dictionary_build.dictionary_name = p_dictionary_name;
    IF NOT FOUND THEN
        RAISE EXCEPTION USING
            ERRCODE = '22023',
            MESSAGE = 'acdat build does not exist for this dictionary';
    END IF;
END
$function$;

SELECT pg_catalog.pg_extension_config_dump('@extschema@.dictionary', '');
SELECT pg_catalog.pg_extension_config_dump('@extschema@.dictionary_version', '');
SELECT pg_catalog.pg_extension_config_dump('@extschema@.dictionary_build', '');
SELECT pg_catalog.pg_extension_config_dump('@extschema@.active_dictionary', '');
SELECT pg_catalog.pg_extension_config_dump(
    '@extschema@.dictionary_build_data',
    'WHERE build_id IN (SELECT build_id FROM @extschema@.active_dictionary)'
);

REVOKE CREATE ON SCHEMA @extschema@ FROM PUBLIC;
GRANT USAGE ON SCHEMA @extschema@ TO PUBLIC;

REVOKE ALL ON TABLE @extschema@.dictionary,
                    @extschema@.dictionary_version,
                    @extschema@.dictionary_build,
                    @extschema@.dictionary_build_data,
                    @extschema@.active_dictionary
FROM PUBLIC;

REVOKE EXECUTE ON FUNCTION @extschema@.machine_in(cstring) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.machine_out(@extschema@.machine) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.machine_recv(internal) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.machine_send(@extschema@.machine) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@._compile_text_trans(internal, text, bigint, text, integer) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@._compile_text_trans_simple(internal, text, bigint) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@._compile_bytea_trans(internal, bytea, bigint, bytea, integer) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@._compile_bytea_trans_simple(internal, bytea, bigint) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@._compile_final(internal) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@._catalog_build_id(text, @extschema@.machine) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.publish(text, bigint, @extschema@.machine, jsonb) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.activate(text, bytea) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.retire_build(text, bytea) FROM PUBLIC;
