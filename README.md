# acdat

`acdat` is a PostgreSQL extension for compiling a large set of exact literal
patterns once and scanning each `text` or `bytea` value once. The first engine
uses an Aho-Corasick automaton packed into a Double-Array Trie.

The project is an active rewrite of the original 2018 Go benchmark. It remains
pre-1.0, but published format-major-1 machines are a compatibility contract:
new readers must continue to validate and scan them correctly.

## What it is for

- sensitive-word and policy-rule detection;
- IOC, URL, hash, path, and fixed binary signature scanning;
- log classification and routing;
- large exact entity dictionaries;
- one-pass redaction, alias normalization, and replacement;
- ingestion-time extraction into an application-owned hit table.

ACDAT indexes the pattern set, not the document table. A query over an existing
large table still reads the candidate rows. For a few ad-hoc patterns, use
`pg_trgm` or PostgreSQL full-text search. For repeated reverse lookups over a
stable corpus, materialize `(document_id, pattern_id, build_id)` matches.

## SQL namespace

The extension and its non-relocatable schema are both named `acdat`:

```ini
relocatable = false
schema = 'acdat'
trusted = false
```

PostgreSQL creates the schema automatically:

```sql
CREATE EXTENSION acdat;
```

Public objects use schema-qualified, prefix-free names:

```text
acdat.machine
acdat.hit
acdat.compile
acdat.contains
acdat.matches
acdat.replace
acdat.info
```

## Quick start

Compile an array dictionary and test a value:

```sql
WITH machine AS (
  SELECT acdat.compile(
    ARRAY['he', 'she', 'his', 'hers'],
    ARRAY[1, 2, 3, 4]::bigint[]
  ) AS value
)
SELECT acdat.contains('ushers', value)
FROM machine;
```

Compile directly from a relation:

```sql
SELECT acdat.compile(pattern, pattern_id, replacement, priority)
FROM app_keyword
WHERE enabled;
```

Enumerate overlapping matches. Text positions are 1-based; byte and character
coordinates are both returned:

```sql
SELECT *
FROM acdat.matches(
  '发现病毒特征码',
  acdat.compile(
    ARRAY['病毒', '特征码'],
    ARRAY[10, 11]::bigint[]
  )
)
ORDER BY start_byte;
```

Deterministic replacement policies:

```sql
SELECT acdat.replace(
  'aaa',
  acdat.compile(
    ARRAY['a', 'aa', 'aaa'],
    ARRAY[1, 2, 3]::bigint[],
    ARRAY['[x]', '[yy]', '[zzz]'],
    ARRAY[0, 10, 20]
  ),
  'leftmost_longest'
);
```

Supported policies are:

- `all_overlapping` for match enumeration;
- `leftmost_longest`;
- `leftmost_priority`.

Replacement is literal, non-recursive, and accepts only a non-overlapping
policy.

## Managed dictionaries

The extension ships a small optional-use control plane in the same schema:

```text
acdat.dictionary
acdat.dictionary_version
acdat.dictionary_build
acdat.dictionary_build_data
acdat.active_dictionary
acdat.active_machine
```

Source pattern tables remain application-owned. Publish and activate an
immutable build:

```sql
WITH machine AS (
  SELECT acdat.compile(pattern, pattern_id)
  FROM app_keyword
  WHERE enabled
), published AS (
  SELECT acdat.publish('moderation', 1, machine) AS build_id
  FROM machine
)
SELECT acdat.activate('moderation', build_id)
FROM published;
```

Build IDs are content-addressed SHA-256 values. Metadata is included in logical
dumps; only active machine payloads are included, so a restored active
dictionary works immediately without dumping every historical artifact.

Control functions are `SECURITY INVOKER` and are not executable by `PUBLIC`.
The installer owns the catalog. A DBA can create and grant an `acdat_admin`
role outside the extension when delegated administration is required.

## Build and test

Requirements: PostgreSQL server development headers, PGXS, a C11 compiler, and
GNU Make.

```bash
make
make install
make installcheck
make core-test
make bench-matrix
make upgrade-test
make dump-restore-test
make cache-smoke-test
make pg-matrix-test        # Docker, PostgreSQL 14–18
make cross-version-test    # PG14 Linux amd64 dump -> local PG18 restore
```

Run one standalone benchmark profile or the representative matrix:

```bash
PATTERNS=100000 PATTERN_BYTES=16 INPUT_MIB=16 make core-bench
BENCH_REPEATS=3 BENCH_INPUT_MIB=16 make bench-matrix
```

The matrix covers ordinary ASCII misses, root-only misses, hit-heavy input,
UTF-8 byte-oriented patterns, and a worst-overlap case. It reports build-search
work, artifact and materialized memory, occupancy, and scan throughput.

The current validation matrix covers PostgreSQL 14–18 on Linux amd64 with
`-Werror`, plus PostgreSQL 18.4 on macOS arm64. Linux arm64 and package-lab
verification remain release gates.

See [PRD.md](PRD.md) for the full product and format contract and
[BENCHMARK.md](BENCHMARK.md) for measured engineering baselines. A detailed
Chinese usage guide is available at [docs/usage.md](docs/usage.md), together
with a runnable end-to-end example at
[examples/demo.sql](examples/demo.sql):

```bash
psql -X -v ON_ERROR_STOP=1 -d your_database -f examples/demo.sql
```

## Safety and lifecycle

The machine type has a self-describing, checksummed, little-endian format with
text and binary I/O. Imported artifacts are structurally validated before use.
Hot scan expressions cache an already validated and materialized machine using
the TOAST pointer/build fingerprint, so a large value is not detoasted per row.
Version 0.2.0 also services PostgreSQL interrupts during DAT packing and places
a conservative input-relative limit on base-candidate searches.

`DROP EXTENSION acdat` deletes all managed dictionary state. It normally fails
when application objects depend on `acdat.machine`; **using `CASCADE` can delete
dependent user columns or objects**. Inventory dependencies and back up source
patterns before uninstalling.

## License

`acdat` is distributed under the [PostgreSQL License](LICENSE).
