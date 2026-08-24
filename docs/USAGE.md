# ACDAT Usage Guide

This guide explains the core workflow of `acdat` 0.2.0. The short version is:

> `acdat` compiles a set of literal patterns into one reusable `machine`. The
> first array passed to `compile` contains the patterns. The second array is
> not another pattern set: it contains the application-defined ID paired with
> each pattern, and it is optional.

The fundamental usage pattern is **compile once, scan many times**:

```text
source rules -- compile once --> acdat.machine
                                  |
                                  +-- contains: did anything match?
                                  +-- matches: which rules matched, and where?
                                  +-- replace: replace selected matches
                                               (reuse for many input values)
```

All examples use the fixed `acdat` schema and schema-qualified names, so their
behavior does not depend on `search_path`. For a complete executable
walkthrough, see [`examples/demo.sql`](../examples/demo.sql).

## 1. What acdat solves

Suppose an application has 100,000 patterns and millions of documents. Testing
every pattern independently against every document makes work grow with both
the number of documents and the size of the dictionary.

`acdat` first compiles the whole pattern set into an immutable Aho-Corasick
machine. That machine scans an input once while searching for all patterns.
Scanning then costs primarily the length of the input plus the number of
matches, rather than one separate `LIKE` or regular-expression operation per
pattern.

Typical uses include:

- policy, sensitive-term, and fixed-signature detection;
- IOC, URL, hash, path, and binary signature scanning;
- log, stack-trace, and user-agent classification;
- entity discovery with large product, company, drug, or SKU dictionaries;
- one-pass redaction, alias normalization, and literal replacement;
- ingestion-time extraction into `(document_id, pattern_id, build_id)` tables;
- one-time parallel audits of an existing table with a stable dictionary.

One boundary is essential: **acdat indexes the pattern dictionary, not the
document table.** A query such as `acdat.contains(body, machine)` still reads
the candidate rows. If an application repeatedly asks which documents matched
a particular pattern, materialize those hits during ingestion or a batch job
and index the resulting ordinary table.

The current release does not provide:

- regular expressions, wildcards, or edit distance;
- tokenization, stemming, ranking, or full-text search;
- implicit case folding, Unicode normalization, or visual equivalence;
- implicit word-boundary detection;
- in-place mutation of a compiled machine;
- a custom index access method for the document side.

For a few ad hoc patterns, prefer `LIKE`, `pg_trgm`, or PostgreSQL full-text
search. `acdat` is most useful when the pattern set is large and stable and the
same machine scans many values.

## 2. Installation

### 2.1 Build and install

Requirements:

- PostgreSQL 14 through 18 server development files;
- PGXS and `pg_config`;
- a C11 compiler;
- GNU Make.

Build against the first PostgreSQL installation in `PATH`:

```bash
make
make install
make installcheck PGDATABASE=postgres
```

Select another PostgreSQL installation explicitly:

```bash
make PG_CONFIG=/path/to/pg_config
make install PG_CONFIG=/path/to/pg_config
```

Create the extension in a database:

```sql
CREATE EXTENSION acdat;
```

`acdat` is not trusted and installation is restricted to a superuser by the
control file. The extension is non-relocatable and PostgreSQL creates its fixed
`acdat` schema automatically. Installing it into another schema is rejected:

```sql
-- Not supported.
CREATE EXTENSION acdat WITH SCHEMA other_schema;
```

Verify the installation:

```sql
SELECT extname, extversion, extrelocatable
FROM pg_extension
WHERE extname = 'acdat';
```

The expected version is `0.2.0`, and `extrelocatable` is `false`.

### 2.2 Run the demonstration

```bash
psql -X -v ON_ERROR_STOP=1 \
  -d your_database \
  -f examples/demo.sql
```

The demonstration runs source rules, managed dictionary state, and sample
documents inside a transaction and rolls them back. Only
`CREATE EXTENSION IF NOT EXISTS acdat` remains.

## 3. Core concepts

Four objects explain most of the public API:

| Object | Meaning |
|---|---|
| `pattern` | The literal text or byte sequence that participates in matching |
| `pattern_id` | The application's unique identity for one rule |
| `machine` | The immutable matcher compiled from the complete rule set |
| `hit` | One match, including its rule ID, location, length, and priority |

A typical lifecycle is:

1. Keep patterns, IDs, replacements, priorities, and business metadata in an
   application-owned rule table.
2. Call `acdat.compile` once when publishing a rule set.
3. Store the resulting `machine`, or publish it as a managed dictionary.
4. Reuse the machine with `contains`, `matches`, or `replace` for every input.
5. When rules change, compile a new machine instead of modifying the old one.

### 3.1 Source rules

Application tables remain the source of truth for patterns and business
metadata. A useful minimum schema is:

```sql
CREATE TABLE app_keyword (
    dictionary_name text    NOT NULL,
    version         bigint  NOT NULL,
    pattern_id      bigint  NOT NULL,
    pattern         text    NOT NULL,
    replacement     text,
    priority        integer NOT NULL DEFAULT 0,
    enabled         boolean NOT NULL DEFAULT true,
    metadata        jsonb,
    PRIMARY KEY (dictionary_name, version, pattern_id)
);
```

The extension does not copy source pattern text or arbitrary business metadata
into its managed catalog. Keep those values in application-owned tables.

### 3.2 Machine

`acdat.machine` is an immutable compiled artifact. It can be:

- stored in an application table;
- passed to `contains`, `matches`, and `replace`;
- exported to portable `bytea` and imported again;
- published and activated through the managed dictionary catalog.

A machine records its format version, engine, input kind, database encoding,
state counts, fingerprints, and artifact checksum. Scan functions validate the
artifact before use. Corrupt input raises an error instead of causing an
out-of-bounds backend read.

### 3.3 Pattern IDs, priority, and replacement

- `pattern_id` is an application identity of type `bigint` and must be unique
  within one machine. Negative IDs are allowed.
- The same pattern text may appear under multiple different IDs.
- A lower numeric `priority` has higher precedence. The default is zero.
- An empty replacement deletes the selected match.
- SQL `NULL` means that the rule has no replacement. It can still match, but
  `replace` raises an error if that rule is selected.

## 4. Quick start

### 4.1 The minimum call needs only the pattern array

If all you need is a Boolean answer, pass only the patterns:

```sql
WITH dictionary AS MATERIALIZED (
    SELECT acdat.compile(
        ARRAY['virus', 'trojan', 'ransomware']
    ) AS machine
)
SELECT acdat.contains('trojan detected', machine) AS matched,
       acdat.contains('clean input', machine) AS not_matched
FROM dictionary;
```

The first result is `true` and the second is `false`. No second array is
required. When IDs are omitted, `acdat` assigns `1` through N in input order.

`contains` stops after the first hit and is the cheapest API when only a
Boolean answer is required.

### 4.2 What the second array means

This call passes two rules to the compiler:

```sql
SELECT acdat.compile(
    ARRAY['virus', 'signature'],
    ARRAY[1001, 1002]::bigint[]
);
```

The arrays are parallel. Elements at the same subscript describe the same
rule:

| Array subscript | `patterns` | `pattern_ids` |
|---:|---|---:|
| 1 | `virus` | 1001 |
| 2 | `signature` | 1002 |

Therefore:

- `virus` has application rule ID `1001`;
- `signature` has application rule ID `1002`;
- the second array does not participate in text matching;
- it is not another dictionary;
- it lets `matches` report which business rule was found.

For example:

```sql
SELECT pattern_id, start_char, char_length
FROM acdat.matches(
    'virus signature found',
    acdat.compile(
        ARRAY['virus', 'signature'],
        ARRAY[1001, 1002]::bigint[]
    )
)
ORDER BY start_char;
```

The result identifies the matches as rules `1001` and `1002`. The application
can join those IDs back to its source rule table to retrieve categories, risk
levels, descriptions, owners, or any other business metadata.

Explicit stable IDs are recommended in production because generated IDs can
change when patterns are inserted, removed, or reordered. Within one machine,
explicit IDs must be non-NULL and unique. The same pattern text may occur more
than once if each occurrence has a different ID.

## 5. Building dictionaries

### 5.1 Array interface

The complete text signature is:

```sql
acdat.compile(
    patterns     text[],
    pattern_ids  bigint[]  DEFAULT NULL,
    replacements text[]    DEFAULT NULL,
    priorities   integer[] DEFAULT NULL,
    options      jsonb     DEFAULT '{}'
)
```

The `bytea` overload uses `bytea[]` for patterns and replacements.

The arguments are:

| Argument | Required | Meaning |
|---|---|---|
| `patterns` | Yes | Literal patterns that participate in matching |
| `pattern_ids` | No | Business ID paired with each pattern; defaults to `1..N` |
| `replacements` | No | Literal replacement paired with each pattern |
| `priorities` | No | Conflict priority paired with each pattern; lower wins |
| `options` | No | One JSON object for the whole compilation operation |

`pattern_ids`, `replacements`, and `priorities` are parallel arrays. Each
non-NULL array must have the same length as `patterns`, and values at the same
subscript form one rule. `options` is not a parallel array. Version 0.2.0 only
accepts the empty options object.

When IDs are omitted, `acdat` generates IDs from 1 through N:

```sql
SELECT acdat.compile(ARRAY['alpha', 'beta', 'gamma']);
```

A complete call can set IDs, replacements, and priorities:

```sql
SELECT acdat.compile(
    ARRAY['malware', 'signature', 'manual review'],
    ARRAY[1001, 1002, 1003]::bigint[],
    ARRAY['[MALWARE]', '[SIGNATURE]', '[REVIEW]'],
    ARRAY[20, 20, 5]::integer[]
);
```

That call represents these three rules:

| Pattern | ID | Replacement | Priority |
|---|---:|---|---:|
| `malware` | 1001 | `[MALWARE]` | 20 |
| `signature` | 1002 | `[SIGNATURE]` | 20 |
| `manual review` | 1003 | `[REVIEW]` | 5 |

Lower numeric priority has higher precedence, so priority `5` wins over
priority `20` when otherwise competing matches require a choice.

Array compilation rules:

- every non-NULL array must be one-dimensional and have the same length;
- a pattern cannot be NULL or empty;
- explicit pattern IDs cannot be NULL or duplicated;
- a missing priority array or NULL priority element means zero;
- a NULL replacement element means no replacement;
- `patterns IS NULL` returns NULL;
- an empty array produces a valid machine that never matches;
- version 0.2.0 accepts only an empty options object.

Because both `text[]` and `bytea[]` overloads exist, cast empty or all-NULL
arrays explicitly:

```sql
SELECT acdat.compile(ARRAY[]::text[]);
```

### 5.2 Aggregate interface

Production dictionaries commonly come from a relation. The aggregate avoids
constructing multiple parallel arrays in application code:

```sql
SELECT acdat.compile(pattern, pattern_id, replacement, priority)
FROM app_keyword
WHERE dictionary_name = 'moderation'
  AND version = 42
  AND enabled;
```

Although the query reads many source rows, the aggregate returns one machine.
Conceptually:

```text
many rule rows -- compile aggregate --> one acdat.machine
```

To build several dictionaries from one table, group by dictionary identity:

```sql
SELECT dictionary_name,
       acdat.compile(pattern, pattern_id, replacement, priority) AS machine
FROM app_keyword
WHERE version = 42
  AND enabled
GROUP BY dictionary_name;
```

The available aggregate signatures are:

```text
compile(text,  bigint, text,  integer)
compile(text,  bigint)
compile(bytea, bigint, bytea, integer)
compile(bytea, bigint)
```

Input row order has no semantic meaning. The compiler sorts a canonical key,
so the same rule set produces the same source fingerprint regardless of scan
order. An aggregate over zero rows returns NULL; reject that result before
publishing:

```sql
WITH compiled AS (
    SELECT acdat.compile(pattern, pattern_id) AS machine
    FROM app_keyword
    WHERE enabled
)
SELECT machine
FROM compiled;
```

Reject a NULL result in application code or a PL/pgSQL publishing function.

### 5.3 Determinism

The source fingerprint includes input kind, database encoding, pattern bytes,
IDs, priorities, and replacements. It does not include source row order.

```sql
SELECT acdat.source_fingerprint(
    acdat.compile(
        ARRAY['beta', 'alpha'],
        ARRAY[2, 1]::bigint[]
    )
) = acdat.source_fingerprint(
    acdat.compile(
        ARRAY['alpha', 'beta'],
        ARRAY[1, 2]::bigint[]
    )
) AS same_source;
```

The result is `true`.

## 6. Finding matches

### 6.1 Boolean lookup with contains

```sql
SELECT acdat.contains(input_text, compiled_machine);
```

Both arguments are strict: if either is NULL, the result is NULL. An empty
input returns `false`.

### 6.2 Enumerating hits with matches

```sql
SELECT *
FROM acdat.matches(
    'malware signature found',
    acdat.compile(
        ARRAY['malware', 'signature'],
        ARRAY[10, 11]::bigint[]
    ),
    'all_overlapping',
    1000
)
ORDER BY start_byte, pattern_id;
```

Each `acdat.hit` row contains:

| Field | Meaning |
|---|---|
| `pattern_id` | Application rule ID |
| `start_byte` | One-based byte offset |
| `byte_length` | Matched byte length |
| `start_char` | One-based character offset for text; NULL for bytea |
| `char_length` | Unicode code-point count for text; NULL for bytea |
| `priority` | Compiled priority |

`start_char` counts Unicode code points, not grapheme clusters. A user-visible
character can contain multiple code points.

Put the set-returning function in `FROM ... CROSS JOIN LATERAL`. Do not depend
on implicit result order; add `ORDER BY` when order matters.

### 6.3 Overlap policies

#### all_overlapping

Returns every raw match, including prefixes, suffixes, contained matches,
overlaps, and duplicate pattern text under different IDs.

```sql
WITH machine AS (
    SELECT acdat.compile(
        ARRAY['a', 'aa', 'aaa'],
        ARRAY[1, 2, 3]::bigint[]
    ) AS value
)
SELECT *
FROM machine,
     LATERAL acdat.matches('aaa', value, 'all_overlapping');
```

#### leftmost_longest

Consumes matches from the leftmost unconsumed position. At the same start
position it prefers, in order:

1. the longest pattern;
2. the lower priority value;
3. the lower pattern ID;
4. the lower canonical ordinal.

#### leftmost_priority

Consumes matches from the leftmost unconsumed position. At the same start
position it prefers, in order:

1. the lower priority value;
2. the longer pattern;
3. the lower pattern ID;
4. the lower canonical ordinal.

### 6.4 Match limits

`max_matches` must be positive and defaults to 10,000. Producing the next raw
match after that limit raises an error; results are never silently truncated.

An outer `LIMIT` is not a substitute because `matches` materializes its hit
set first. For `leftmost_*` policies, the limit applies to raw overlapping
matches before conflict resolution.

## 7. Literal replacement

```sql
SELECT acdat.replace(
    input_text,
    compiled_machine,
    'leftmost_longest',
    10000,
    268435456
);
```

The last two arguments are `max_replacements` and `max_output_bytes`.

Example:

```sql
WITH machine AS (
    SELECT acdat.compile(
        ARRAY['cat', 'catalog'],
        ARRAY[1, 2]::bigint[],
        ARRAY['dog', 'index']
    ) AS value
)
SELECT acdat.replace('catalog and cat', value, 'leftmost_longest')
FROM machine;
```

Replacement semantics:

- only `leftmost_longest` and `leftmost_priority` are valid;
- `all_overlapping` has no unique replacement result and is rejected;
- replacements are literal, not regular-expression templates;
- replacement is non-recursive and runs once;
- an empty replacement deletes a match;
- selecting a rule with a NULL replacement raises an error;
- the raw candidate limit is enforced before overlap resolution;
- oversized output raises an error before exceeding PostgreSQL datum or
  allocation limits.

Non-recursive behavior:

```sql
WITH machine AS (
    SELECT acdat.compile(
        ARRAY['cat', 'dog'],
        ARRAY[1, 2]::bigint[],
        ARRAY['dog', 'wolf']
    ) AS value
)
SELECT acdat.replace('cat', value)
FROM machine;
```

The result is `dog`, not `wolf`.

## 8. Scanning bytea

The `bytea` API handles fixed binary signatures and data without text encoding
semantics.

```sql
WITH machine AS (
    SELECT acdat.compile(
        ARRAY[decode('00ff', 'hex'), decode('beef', 'hex')],
        ARRAY[20, 21]::bigint[],
        ARRAY[decode('1122', 'hex'), decode('dead', 'hex')]
    ) AS value
)
SELECT encode(
           acdat.replace(decode('aa00ffbeef', 'hex'), value),
           'hex'
       ) AS replaced
FROM machine;
```

The result is `aa1122dead`. Text and bytea machines are not interchangeable;
an input-kind mismatch raises an explicit error.

## 9. Text, Unicode, and comparison semantics

Text matching is exact, case-sensitive matching over database-encoding bytes.
It does not:

- consult collation;
- call `lower()` automatically;
- perform NFC or NFD normalization;
- infer word boundaries.

Replacement follows original byte boundaries. The text API supports UTF-8 and
PostgreSQL single-byte server encodings. Other multibyte server encodings are
rejected; use UTF-8 or the bytea API.

If an application needs case-insensitive or normalized matching, normalize
both source patterns and inputs with the same application-level rules. Do not
use normalized offsets to edit the original text unless the application also
maintains an offset map.

## 10. Managed dictionaries

The extension includes a small control plane while leaving source rules in
application tables:

| Object | Purpose |
|---|---|
| `acdat.dictionary` | Dictionary identity and input kind |
| `acdat.dictionary_version` | Semantic version and source fingerprint |
| `acdat.dictionary_build` | Compiler, format, state, and metadata |
| `acdat.dictionary_build_data` | Machine payload |
| `acdat.active_dictionary` | Active build pointer per dictionary |
| `acdat.active_machine` | View exposing each active machine |

### 10.1 Publish

```sql
WITH machine AS (
    SELECT acdat.compile(pattern, pattern_id, replacement, priority) AS value
    FROM app_keyword
    WHERE dictionary_name = 'moderation'
      AND version = 42
      AND enabled
)
SELECT acdat.publish(
    'moderation',
    42,
    value,
    '{"source":"app_keyword"}'::jsonb
) AS build_id
FROM machine;
```

Publishing is idempotent for the same dictionary, semantic version, and
machine. Reusing a semantic version for a different source fingerprint is
rejected.

### 10.2 Activate

```sql
SELECT acdat.activate('moderation', build_id)
FROM release_candidate;
```

Activation updates a small pointer. `publish` and `activate` take a
transaction-scoped advisory lock keyed by dictionary name, so they can safely
run in one explicit transaction:

```sql
BEGIN;
-- Compile and publish a build.
-- Run validation and sample checks.
SELECT acdat.activate('moderation', build_id)
FROM release_candidate;
COMMIT;
```

### 10.3 Read the active machine

```sql
SELECT name, version, build_id, machine, info
FROM acdat.active_machine
WHERE name = 'moderation';
```

One SQL statement sees the active pointer from its MVCC snapshot. Under
`READ COMMITTED`, the next statement can see a newly activated build. A
`REPEATABLE READ` transaction continues using its older snapshot.

### 10.4 Roll back

Rollback means reactivating an older ready build; no recompilation is needed:

```sql
SELECT acdat.activate('moderation', decode(:old_build_hex, 'hex'));
```

Store the exact build ID returned by each release instead of guessing from a
semantic version.

### 10.5 Retire

```sql
SELECT acdat.retire_build(
    'moderation',
    decode(:build_id_hex, 'hex')
);
```

The active build cannot be retired. Activate another build first. Retirement
changes catalog state; it does not delete source rules or application data.

## 11. Scanning application tables

### 11.1 Filter rows

Materialize the active machine once and then scan candidate rows:

```sql
WITH active AS MATERIALIZED (
    SELECT machine
    FROM acdat.active_machine
    WHERE name = 'moderation'
)
SELECT d.document_id
FROM app_document AS d
CROSS JOIN active AS a
WHERE acdat.contains(d.body, a.machine);
```

Use ordinary indexes and partition pruning to reduce candidate rows before
calling `acdat`.

### 11.2 Join business metadata

Machines store IDs and matching data, not arbitrary application metadata:

```sql
SELECT d.document_id,
       h.pattern_id,
       h.start_byte,
       h.byte_length,
       r.metadata
FROM app_document AS d
CROSS JOIN acdat.active_machine AS a
CROSS JOIN LATERAL acdat.matches(d.body, a.machine) AS h
JOIN app_keyword AS r
  ON r.dictionary_name = a.name
 AND r.version = a.version
 AND r.pattern_id = h.pattern_id
WHERE a.name = 'moderation';
```

### 11.3 Materialize hits for reverse lookup

```sql
CREATE TABLE app_document_hit (
    dictionary_name    text   NOT NULL,
    dictionary_version bigint NOT NULL,
    build_id           bytea  NOT NULL,
    document_id        bigint NOT NULL,
    pattern_id         bigint NOT NULL,
    start_byte         integer NOT NULL,
    byte_length        integer NOT NULL
);

CREATE INDEX app_document_hit_pattern_idx
ON app_document_hit(dictionary_name, pattern_id, document_id);
```

Populate it in a batch:

```sql
INSERT INTO app_document_hit
SELECT a.name,
       a.version,
       a.build_id,
       d.document_id,
       h.pattern_id,
       h.start_byte,
       h.byte_length
FROM app_document AS d
CROSS JOIN acdat.active_machine AS a
CROSS JOIN LATERAL acdat.matches(
    d.body, a.machine, 'all_overlapping', 10000
) AS h
WHERE a.name = 'moderation';
```

Persist the dictionary version or build ID with every materialized hit so its
semantics remain auditable after future dictionary changes.

## 12. Inspecting and transporting machines

### 12.1 info

```sql
SELECT jsonb_pretty(acdat.info(machine))
FROM app_machine;
```

Important fields include:

- `format_major` and `format_minor`;
- `compiler_version` and `engine`;
- `input_kind` and `database_encoding`;
- pattern, state, and slot counts;
- maximum pattern byte and character lengths;
- replacement and compiled byte counts;
- source fingerprint, build fingerprint, and artifact checksum.

### 12.2 Three digests

```sql
SELECT encode(acdat.source_fingerprint(machine), 'hex') AS source_sha256,
       encode(acdat.build_fingerprint(machine), 'hex') AS build_sha256,
       encode(acdat.artifact_checksum(machine), 'hex') AS artifact_sha256
FROM app_machine;
```

- The source fingerprint identifies the canonical rule set.
- The build fingerprint identifies a concrete compiler, engine, and format.
- The artifact checksum detects payload corruption.

### 12.3 Export, validate, and import

```sql
SELECT acdat.validate(acdat.export(machine)) AS valid,
       acdat.import(acdat.export(machine)) AS imported
FROM app_machine;
```

`validate(bytea)` returns `false` for invalid input. `import(bytea)` raises an
error. Preserve the complete bytea value during transport.

A text machine records the database encoding used at compilation. UTF-8
machines can move between UTF-8 databases. Do not assume compatibility between
different single-byte encodings. A bytea machine is independent of database
text encoding.

## 13. Permission model

Installation requires a superuser. After installation:

- `PUBLIC` has `USAGE` on schema `acdat`, not `CREATE`;
- `PUBLIC` can execute public compile, scan, replace, and inspection APIs;
- `PUBLIC` cannot access managed catalog tables;
- `PUBLIC` cannot execute `publish`, `activate`, or `retire_build`;
- control functions are `SECURITY INVOKER` and also require table privileges.

A DBA can create a delegated role outside the extension:

```sql
CREATE ROLE acdat_admin;

GRANT USAGE ON SCHEMA acdat TO acdat_admin;
GRANT SELECT, INSERT, UPDATE, DELETE
ON acdat.dictionary,
   acdat.dictionary_version,
   acdat.dictionary_build,
   acdat.dictionary_build_data,
   acdat.active_dictionary
TO acdat_admin;

GRANT EXECUTE ON FUNCTION
    acdat.publish(text, bigint, acdat.machine, jsonb),
    acdat.activate(text, bytea),
    acdat.retire_build(text, bytea)
TO acdat_admin;
```

Grant `SELECT` on `acdat.active_machine` separately to scan-only roles that
need the current managed machine.

## 14. Backup, restore, and replication

The managed catalog is registered with `pg_extension_config_dump`:

- dictionary, version, build metadata, and active pointers are included in
  logical dumps;
- the active build payload is included;
- inactive historical payloads are omitted by default and can be recompiled
  from source rules;
- application-owned source rule tables must be backed up normally.

Physical replication, `pg_basebackup`, and PITR carry payloads stored in normal
logged tables. Restore targets and standbys still need a compatible extension
library installed.

Logical replication does not replicate `CREATE EXTENSION`. Install a compatible
extension on each subscriber first. If managed state is replicated, deliver
build metadata, payload, and active pointer in the same transaction. A simpler
operational model is to treat managed state as node-local and rebuild it from
source rules on each destination.

## 15. Performance and production guidance

### 15.1 Compile once, scan many times

Do not call `acdat.compile` in a correlated subquery for every document row.
Compile and publish once, or place one-off compilation in a `MATERIALIZED` CTE.

### 15.2 Prefer contains for existence checks

`contains` avoids constructing character offsets, hit tuples, and a tuplestore.
Use `matches` only when hit detail is needed.

### 15.3 Reduce candidate rows first

Apply ordinary indexed predicates, partition pruning, date ranges, tenant IDs,
or other selective conditions before scanning text with a machine.

### 15.4 Bound match growth

Overlap-heavy dictionaries can produce many raw matches. Set an intentional
`max_matches` or `max_replacements` value for each workload.

### 15.5 Account for parallel memory

Scan functions are parallel safe, but each worker materializes and caches its
own machine. Include machine size times worker count in memory planning.

### 15.6 Build large dictionaries deliberately

Compilation is interruptible and has a conservative input-relative
base-candidate budget. Build very large dictionaries in a controlled release
job, record fingerprints and machine information, validate representative
inputs, and activate only after the build passes checks.

Measured results and limitations are documented in:

- [`PERFORMANCE.md`](PERFORMANCE.md)
- [`BENCHMARK.md`](BENCHMARK.md)

## 16. Common errors

### Ambiguous empty arrays

Cast empty arrays to `text[]` or `bytea[]` because both overloads exist.

### Aggregate returns NULL

No input rows were compiled. Reject the release instead of publishing NULL.

### Duplicate pattern ID

IDs must be unique in one machine. The same pattern can have multiple IDs, but
the IDs themselves cannot repeat.

### Unsupported encoding

Use UTF-8, a PostgreSQL single-byte server encoding, or the bytea API.

### Match limit exceeded

Reduce overlap, narrow candidate inputs, or deliberately raise the limit after
measuring memory and output growth.

### Missing active machine

Publish a valid machine with payload and activate its exact build ID.

### Permission denied on publish or activate

Grant both function execution and the required managed table privileges to a
delegated role.

## 17. API quick reference

### Build

```text
compile(text[],  bigint[], text[],  integer[], jsonb) -> machine
compile(bytea[], bigint[], bytea[], integer[], jsonb) -> machine
compile(text,  bigint, text,  integer) aggregate -> machine
compile(text,  bigint)                aggregate -> machine
compile(bytea, bigint, bytea, integer) aggregate -> machine
compile(bytea, bigint)                 aggregate -> machine
```

### Scan and replace

```text
contains(text,  machine) -> boolean
contains(bytea, machine) -> boolean
matches(text,  machine, policy, max_matches) -> setof hit
matches(bytea, machine, policy, max_matches) -> setof hit
replace(text,  machine, policy, max_replacements, max_output_bytes) -> text
replace(bytea, machine, policy, max_replacements, max_output_bytes) -> bytea
```

### Inspect and transport

```text
info(machine) -> jsonb
validate(bytea) -> boolean
export(machine) -> bytea
import(bytea) -> machine
source_fingerprint(machine) -> bytea
build_fingerprint(machine) -> bytea
artifact_checksum(machine) -> bytea
```

### Managed control plane

```text
publish(text, bigint, machine, jsonb) -> bytea
activate(text, bytea) -> void
retire_build(text, bytea) -> void
```

## 18. Uninstall warning

```sql
DROP EXTENSION acdat;
```

Dropping the extension removes all managed dictionary state. A normal drop
fails when application objects depend on `acdat.machine`.

**`DROP EXTENSION acdat CASCADE` can drop dependent application columns or
other objects.** Inventory dependencies and back up source rules before using
`CASCADE`.
