CREATE EXTENSION acdat;

SELECT nspname
FROM pg_namespace
WHERE nspname = 'acdat';

SELECT extname, extrelocatable
FROM pg_extension
WHERE extname = 'acdat';

WITH machine AS (
    SELECT acdat.compile(
        ARRAY['he', 'she', 'his', 'hers'],
        ARRAY[1, 2, 3, 4]::bigint[]
    ) AS value
)
SELECT acdat.contains('ushers', value),
       acdat.contains('xyz', value),
       acdat.info(value)->>'engine' AS engine,
       acdat.info(value)->>'pattern_count' AS patterns,
       acdat.validate(acdat.export(value)) AS valid_roundtrip
FROM machine;

WITH machine AS (
    SELECT acdat.compile(
        ARRAY['病毒', '特征码'],
        ARRAY[10, 11]::bigint[],
        ARRAY['[V]', '[S]']
    ) AS value
)
SELECT acdat.contains('发现病毒特征码', value),
       acdat.info(value)->>'input_kind' AS input_kind,
       octet_length(acdat.source_fingerprint(value)) AS source_hash_bytes,
       octet_length(acdat.build_fingerprint(value)) AS build_hash_bytes
FROM machine;

WITH machine AS (
    SELECT acdat.compile(
        ARRAY[decode('00ff', 'hex'), decode('beef', 'hex')],
        ARRAY[20, 21]::bigint[]
    ) AS value
)
SELECT acdat.contains(decode('aa00ffbb', 'hex'), value),
       acdat.contains(decode('aabb', 'hex'), value),
       acdat.info(value)->>'input_kind' AS input_kind
FROM machine;

WITH machine AS (
    SELECT acdat.compile(
        ARRAY['a', 'aa', 'aaa'],
        ARRAY[1, 2, 3]::bigint[],
        ARRAY['[x]', '[yy]', '[zzz]'],
        ARRAY[0, 10, 20]
    ) AS value
)
SELECT pattern_id, start_byte, byte_length, start_char, char_length
FROM machine,
     LATERAL acdat.matches('aaa', value, 'all_overlapping')
ORDER BY start_byte, byte_length, pattern_id;

WITH machine AS (
    SELECT acdat.compile(
        ARRAY['a', 'aa', 'aaa'],
        ARRAY[1, 2, 3]::bigint[],
        ARRAY['[x]', '[yy]', '[zzz]'],
        ARRAY[0, 10, 20]
    ) AS value
)
SELECT acdat.replace('aaa', value, 'leftmost_longest') AS longest,
       acdat.replace('aaa', value, 'leftmost_priority') AS priority,
       (SELECT count(*)
        FROM acdat.matches('aaa', value, 'leftmost_longest')) AS longest_hits,
       (SELECT count(*)
        FROM acdat.matches('aaa', value, 'leftmost_priority')) AS priority_hits
FROM machine;

WITH machine AS (
    SELECT acdat.compile(
        ARRAY['病毒', '特征码'],
        ARRAY[10, 11]::bigint[],
        ARRAY['[V]', '[S]']
    ) AS value
)
SELECT pattern_id, start_byte, byte_length, start_char, char_length
FROM machine,
     LATERAL acdat.matches('发现病毒特征码', value)
ORDER BY start_byte;

WITH rules(id, pattern, replacement, priority) AS (
    VALUES
        (3::bigint, 'hers', NULL::text, 0),
        (1::bigint, 'he',   NULL::text, 0),
        (4::bigint, 'his',  NULL::text, 0),
        (2::bigint, 'she',  NULL::text, 0)
), aggregate_machine AS (
    SELECT acdat.compile(pattern, id, replacement, priority) AS value
    FROM rules
), array_machine AS (
    SELECT acdat.compile(
        ARRAY['he', 'she', 'his', 'hers'],
        ARRAY[1, 2, 4, 3]::bigint[]
    ) AS value
)
SELECT acdat.contains('ushers', aggregate_machine.value) AS aggregate_match,
       acdat.source_fingerprint(aggregate_machine.value) =
       acdat.source_fingerprint(array_machine.value) AS deterministic
FROM aggregate_machine, array_machine;

SELECT acdat.compile(NULL::text[]) IS NULL;

WITH machine AS (
    SELECT acdat.compile(
        ARRAY['same', 'same', 'negative'],
        ARRAY[1, 2, -5]::bigint[]
    ) AS value
)
SELECT (SELECT count(*) FROM acdat.matches('same', value)) AS alias_hits,
       (SELECT pattern_id FROM acdat.matches('negative', value)) AS negative_id,
       acdat.contains('same', (value::text)::acdat.machine) AS text_io_roundtrip
FROM machine;

CREATE TEMP TABLE published_machine AS
WITH machine AS (
    SELECT acdat.compile(
        ARRAY['blocked', 'review'],
        ARRAY[1001, 1002]::bigint[]
    ) AS value
)
SELECT value AS machine,
       acdat.publish('moderation', 1, value, '{"source":"regress"}') AS build_id
FROM machine;

SELECT octet_length(build_id) AS build_id_bytes,
       build_id = acdat.publish(
           'moderation', 1, machine, '{"source":"regress"}'
       ) AS idempotent
FROM published_machine;

SELECT acdat.activate('moderation', build_id)
FROM published_machine;

SELECT name, version,
       acdat.contains('needs review', machine) AS active_match
FROM acdat.active_machine
WHERE name = 'moderation';

SELECT cardinality(extconfig) AS config_tables,
       (SELECT count(*)
        FROM unnest(extcondition) AS condition
        WHERE condition LIKE '%active_dictionary%') AS filtered_payloads
FROM pg_extension
WHERE extname = 'acdat';

SELECT count(*)
FROM information_schema.tables
WHERE table_schema = 'acdat'
  AND table_name IN (
      'dictionary', 'dictionary_version', 'dictionary_build',
      'dictionary_build_data', 'active_dictionary'
  );

SELECT has_schema_privilege('public', 'acdat', 'USAGE') AS public_usage,
       has_schema_privilege('public', 'acdat', 'CREATE') AS public_create,
       has_function_privilege(
           'public', 'acdat.contains(text,acdat.machine)', 'EXECUTE'
       ) AS public_scan,
       has_function_privilege(
           'public',
           'acdat.publish(text,bigint,acdat.machine,jsonb)',
           'EXECUTE'
       ) AS public_publish,
       has_table_privilege(
           'public', 'acdat.dictionary_build_data', 'SELECT'
       ) AS public_payload;

-- end of acdat_basic
