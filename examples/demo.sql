\set ON_ERROR_STOP on
\pset pager off

-- acdat 0.1.1 end-to-end demonstration.
-- Run as the extension owner because publish/activate are intentionally not
-- executable by PUBLIC.

CREATE EXTENSION IF NOT EXISTS acdat;

BEGIN;

\echo '1. Compile a small array dictionary and use contains'
WITH machine AS (
    SELECT acdat.compile(
        ARRAY['he', 'she', 'his', 'hers'],
        ARRAY[1, 2, 3, 4]::bigint[]
    ) AS value
)
SELECT acdat.contains('ushers', value) AS matched,
       acdat.contains('nothing', value) AS not_matched,
       acdat.info(value)->>'pattern_count' AS patterns
FROM machine;

\echo '2. Build a text dictionary from an application-owned rule table'
CREATE TEMP TABLE demo_rule (
    pattern_id  bigint PRIMARY KEY,
    pattern     text NOT NULL,
    replacement text,
    priority    integer NOT NULL,
    category    text NOT NULL,
    enabled     boolean NOT NULL DEFAULT true
);

INSERT INTO demo_rule(pattern_id, pattern, replacement, priority, category)
VALUES
    (1001, '病毒',   '[VIRUS]',     20, 'threat'),
    (1002, '特征码', '[SIGNATURE]', 20, 'threat'),
    (1003, '待复核', '[REVIEW]',     5, 'policy'),
    (1004, 'review', '[REVIEW]',     5, 'policy');

CREATE TEMP TABLE demo_machine AS
SELECT acdat.compile(pattern, pattern_id, replacement, priority) AS machine
FROM demo_rule
WHERE enabled;

SELECT jsonb_pretty(acdat.info(machine)) AS machine_info
FROM demo_machine;

\echo '3. Scan documents, enumerate matches, and join business metadata'
CREATE TEMP TABLE demo_document (
    document_id bigint PRIMARY KEY,
    body        text NOT NULL
);

INSERT INTO demo_document(document_id, body)
VALUES
    (1, '发现病毒特征码，待复核'),
    (2, 'manual review required'),
    (3, 'clean document');

SELECT d.document_id,
       h.pattern_id,
       h.start_byte,
       h.byte_length,
       h.start_char,
       h.char_length,
       substring(d.body FROM h.start_char FOR h.char_length) AS matched_text,
       r.category
FROM demo_document AS d
CROSS JOIN demo_machine AS m
CROSS JOIN LATERAL acdat.matches(
    d.body, m.machine, 'all_overlapping', 1000
) AS h
JOIN demo_rule AS r USING (pattern_id)
ORDER BY d.document_id, h.start_byte, h.pattern_id;

SELECT d.document_id, d.body
FROM demo_document AS d
CROSS JOIN demo_machine AS m
WHERE acdat.contains(d.body, m.machine)
ORDER BY d.document_id;

\echo '4. Replace matches in one non-recursive pass'
SELECT d.document_id,
       acdat.replace(
           d.body, m.machine, 'leftmost_longest', 1000, 1048576
       ) AS replaced
FROM demo_document AS d
CROSS JOIN demo_machine AS m
ORDER BY d.document_id;

\echo '5. Compare overlap policies'
WITH machine AS (
    SELECT acdat.compile(
        ARRAY['a', 'aa', 'aaa'],
        ARRAY[1, 2, 3]::bigint[],
        ARRAY['[A]', '[AA]', '[AAA]'],
        ARRAY[0, 10, 20]::integer[]
    ) AS value
)
SELECT acdat.replace('aaa', value, 'leftmost_longest') AS longest,
       acdat.replace('aaa', value, 'leftmost_priority') AS priority,
       (SELECT count(*)
        FROM acdat.matches('aaa', value, 'all_overlapping')) AS all_hits
FROM machine;

\echo '6. Match and replace binary signatures'
WITH machine AS (
    SELECT acdat.compile(
        ARRAY[decode('00ff', 'hex'), decode('beef', 'hex')],
        ARRAY[20, 21]::bigint[],
        ARRAY[decode('1122', 'hex'), decode('dead', 'hex')]
    ) AS value
)
SELECT hits.hit_ids,
       encode(
           acdat.replace(decode('aa00ffbeef', 'hex'), value),
           'hex'
       ) AS replaced
FROM machine
CROSS JOIN LATERAL (
    SELECT array_agg(pattern_id ORDER BY start_byte) AS hit_ids
    FROM acdat.matches(decode('aa00ffbeef', 'hex'), value)
) AS hits;

\echo '7. Export, validate, and import a portable machine'
SELECT acdat.validate(acdat.export(machine)) AS valid,
       acdat.contains(
           '发现病毒',
           acdat.import(acdat.export(machine))
       ) AS imported_machine_works,
       encode(acdat.source_fingerprint(machine), 'hex') AS source_sha256
FROM demo_machine;

\echo '8. Publish and atomically activate a managed dictionary'
CREATE TEMP TABLE demo_release AS
SELECT acdat.publish(
    'acdat_demo',
    1,
    machine,
    '{"source":"examples/demo.sql"}'::jsonb
) AS build_id
FROM demo_machine;

SELECT acdat.activate('acdat_demo', build_id)
FROM demo_release;

SELECT name,
       version,
       encode(build_id, 'hex') AS build_id,
       acdat.contains('needs review', machine) AS active_machine_works
FROM acdat.active_machine
WHERE name = 'acdat_demo';

\echo '9. Materialize document-to-pattern hits for reverse lookup'
CREATE TEMP TABLE demo_document_hit AS
WITH active AS MATERIALIZED (
    SELECT name, version, build_id, machine
    FROM acdat.active_machine
    WHERE name = 'acdat_demo'
)
SELECT a.name AS dictionary_name,
       a.version AS dictionary_version,
       a.build_id,
       d.document_id,
       h.pattern_id,
       h.start_byte,
       h.byte_length
FROM demo_document AS d
CROSS JOIN active AS a
CROSS JOIN LATERAL acdat.matches(
    d.body, a.machine, 'all_overlapping', 1000
) AS h;

CREATE INDEX demo_document_hit_pattern_idx
ON demo_document_hit(pattern_id, document_id);

SELECT pattern_id,
       array_agg(document_id ORDER BY document_id) AS document_ids
FROM demo_document_hit
GROUP BY pattern_id
ORDER BY pattern_id;

-- Leave no demo rules, documents, builds, or active pointer behind.
ROLLBACK;

\echo 'acdat demo: ok (transaction rolled back)'
