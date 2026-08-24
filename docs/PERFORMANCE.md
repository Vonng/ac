# ACDAT Performance Report: Go Reference vs PostgreSQL Extension

Date: 2026-08-24 (Asia/Shanghai)

Status: local engineering benchmark, not a cross-platform release claim

## Executive summary

The matcher core is fast. The dominant cost depends on the execution shape:

- the original Go program remains the best path for a specialized, one-shot,
  streaming file transformation;
- a single PostgreSQL backend is about 1.8x slower than that specialized Go
  path on the historical 10.3-million-row short-text replacement workload;
- PostgreSQL parallel execution reduces the in-server replacement computation
  from 2.007 seconds to 0.510 seconds with four workers and 0.291 seconds with
  eight workers;
- those parallel numbers exclude client transfer, durable writes, and WAL;
- on 1 KiB and 64 KiB rows, PostgreSQL `contains` sustains about 0.91 GiB/s in
  one backend, or about 94% of the matching standalone root-miss core result;
- on roughly 33-byte rows, PostgreSQL per-row executor and function-call cost
  reduces one-backend throughput to about 0.52 GiB/s;
- the C extension's cached runtime for the 1,114-pattern real dictionary is
  about 227 KiB, versus about 3.56 MiB of retained automaton heap in the Go
  reference;
- exact full-corpus replacement output is byte-for-byte identical between Go
  and PostgreSQL when SQL row order is made explicit.

The practical conclusion is that Go is the faster streaming batch tool, while
the extension is a strong in-database rule engine, especially when candidate
rows are already in PostgreSQL, rows are not extremely short, or parallel
query is available.

## Tested revisions

| Component | Revision |
|---|---|
| Original Go reference (`origin/master`) | `4fe39443915cd1879ef07474f10f87dc0694a0f4` |
| PostgreSQL extension (`main`) | `0d8a7c422639bf20c2550a976d845afd40389728` |
| Extension version | `0.2.0` |

The installed `acdat.dylib` was byte-identical to the artifact built from the
tested `main` revision.

## Test host

| Item | Value |
|---|---|
| System | Apple M5 Max, arm64 |
| CPU | 18 physical cores |
| Memory | 128 GiB |
| OS | macOS 26.5.2, build 25F84 |
| Go | go1.27.0 darwin/arm64 |
| C compiler | Apple clang 21.0.0 |
| PostgreSQL | 18.6 (Homebrew) |

No thermal or performance warning was reported by macOS. The host was not
otherwise isolated: it was a multi-user development machine with a load
average around 3-5 during the measurement window. Medians are used to reduce
short-lived scheduling noise.

The isolated PostgreSQL instance used:

- `shared_buffers = 2GB`;
- `jit = off`;
- `autovacuum = off` during the benchmark;
- `fsync = off`, `synchronous_commit = off`, and `full_page_writes = off` for
  setup speed;
- explicit `parallel_workers` relation settings and zero parallel setup/tuple
  cost when measuring fixed four- and eight-worker plans.

The scan queries were read-only, so disabled durability does not change their
CPU cost. It does mean that compile-and-store timings are not durable-commit
timings.

## Methodology and interpretation

- All primary scan results are warm-cache measurements.
- Go stage measurements use five runs and report medians.
- PostgreSQL real-corpus measurements use three runs and report medians.
- The standalone C matrix uses three runs per cell and reports medians.
- PostgreSQL 100K compile and short-row measurements use five runs.
- `four workers` and `eight workers` mean workers launched by a `Gather`; the
  leader can also participate, so the worst-case machine-memory budget is five
  or nine process-local copies.
- PostgreSQL `contains`, `matches`, and `replace` timings return one aggregate
  row. They include table scanning, executor overhead, function calls, machine
  materialization once per participating process, and result construction, but
  not transfer of every transformed row to a client.
- The Go result writes to the filesystem page cache and does not call `fsync`.
- Throughput for the historical corpus divides its 306.928 MiB source size by
  elapsed time.

These distinctions are essential. A 0.291-second parallel `replace` aggregate
is not a claim that PostgreSQL can durably export or update 322 MB in 0.291
seconds.

## Historical real corpus

The original repository supplies the contest dictionary and input corpus.

| Item | Value |
|---|---:|
| Dictionary rules | 1,114 |
| Dictionary bytes | 27,659 |
| Input rows | 10,318,163 |
| Input bytes | 321,837,358 (306.928 MiB) |
| Result bytes | 322,091,448 (307.170 MiB) |
| Maximum pattern | 40 UTF-8 bytes / 14 characters |
| Rows containing at least one pattern | 581,162 (5.632%) |
| Overlapping matches | 601,838 |

Content fingerprints:

| Artifact | SHA-256 or MD5 |
|---|---|
| `dict.txt` SHA-256 | `73f1003f290206cc15bf5aba8d6024835ec9d320dc3f5d4deb1f913216402249` |
| `video_title.txt` SHA-256 | `8665b95ca1b435a107285ac947667e8010b7efee8220b73e5acd6795da0be7fc` |
| Expected replacement result MD5 | `5d76461b53079d20c08eb0b33c46b7cd` |

The four large repository data shards and the dictionary archive were also
verified against their Git blob IDs before extraction.

## Correctness results

Correctness passed at the corpus, SQL, and extension-test levels:

1. The Go reference produced 10,318,163 rows and 322,091,448 bytes with the
   historical expected MD5.
2. PostgreSQL compiled the same 1,114 patterns, IDs, priorities, and literal
   replacements.
3. PostgreSQL produced the same row count, byte count, and MD5 under
   `leftmost_longest` replacement.
4. `make core-test` passed.
5. `make installcheck` passed both `acdat_basic` and `acdat_errors`.
6. `make cache-smoke-test` passed the 100K-machine cache invariant.

The source file uses CRLF line endings. The PostgreSQL loader preserved the
carriage return inside each text value, and a source line number was retained.
The final verification query used `ORDER BY line_no`. Without an explicit
order, a heap scan is free to return rows in physical order; an early MD5
mismatch was traced entirely to row order, not replacement content.

## Go stage breakdown

The original Go implementation is specialized for this workload: rune-based
matching, three hard-coded replacement categories, a fixed small match buffer,
and buffered streaming I/O.

An instrumented harness mirroring `lib.Run` measured the following medians:

| Stage | Median | Share of instrumented full run | Notes |
|---|---:|---:|---|
| Parse dictionary and build automaton | 1.401 ms | 0.12% | 1,114 rules |
| Read, find, and replace; discard output | 1.070 s | 92.44% | Includes input read |
| Buffered output-write increment | about 82.6 ms | 7.14% | Difference between write and discard medians |
| Harness/GC/close residual | about 3.5 ms | 0.30% | Measurement overhead |
| Instrumented full run | 1.157 s | 100% | Cached I/O, no `fsync` |
| Streaming MD5 verification, if requested | 0.388 s | separate | Reads the 322 MB result again |

A pure cached copy of the 321.8 MB input took 63.8 ms. This supports the
conclusion that matching and replacement, not cached file movement, dominate
the Go runtime.

Five independent executions of the uninstrumented one-shot Go binary had a
median of 1.11 seconds:

- 9.30 million rows/s;
- 276.5 MiB/s;
- about 10.5-11.4 MiB process peak RSS;
- exact expected output MD5.

The repository's built-in `Benchmark(10)` should not be used as the primary
memory number. It runs with `GOGC=off`, rebuilds the automaton repeatedly in one
process, and its correctness check calls `ioutil.ReadFile` on the complete
322 MB output. That benchmark reached roughly 384 MiB RSS even though a normal
one-shot streaming execution stayed near 11 MiB.

## PostgreSQL real-corpus performance

### Dictionary build and machine lifecycle

| Stage | Median or size |
|---|---:|
| Aggregate compile, returning machine size | 1.162 ms |
| Compile plus CTAS/TOAST storage | 3.095 ms |
| Stored artifact | 275,618 bytes (269.2 KiB) |
| Materialized cached runtime | 232,630 bytes (227.2 KiB) |
| States / DAT slots | 10,840 / 10,984 |
| Replacement payload | 11,182 bytes |

Dictionary build is negligible relative to scanning 10.3 million rows.

### End-to-end query operators

| Workload | One backend | Four workers | Eight workers | Result |
|---|---:|---:|---:|---:|
| Raw table scan aggregate | 0.244 s | 0.069 s | 0.045 s | 311,519,195 text bytes excluding row LF |
| `contains` | 1.522 s / 201.7 MiB/s | 0.398 s / 771.7 MiB/s | 0.228 s / 1.31 GiB/s | 581,162 hit rows |
| `replace(leftmost_longest)` | 2.007 s / 153.0 MiB/s | 0.510 s / 602.1 MiB/s | 0.291 s / 1.03 GiB/s | 322,091,448 output bytes including LF |
| `matches(all_overlapping)` | 3.678 s / 83.4 MiB/s | 0.816 s / 376.3 MiB/s | 0.476 s / 645.1 MiB/s | 601,838 matches |

Parallel speedups relative to one backend were:

| Workload | Four workers | Eight workers |
|---|---:|---:|
| `contains` | 3.83x | 6.67x |
| `replace` | 3.94x | 6.89x |
| `matches` | 4.51x | 7.73x |

### Baseline-subtracted stage view

Subtracting the 0.244-second raw scan median gives an approximate view of where
single-backend time goes. This is a query-level subtraction, not a direct C
profiler measurement.

| Workload | Total | Raw scan share | Function/executor/result increment |
|---|---:|---:|---:|
| `contains` | 1.522 s | 0.244 s (16.0%) | 1.278 s (84.0%) |
| `replace` | 2.007 s | 0.244 s (12.1%) | 1.763 s (87.9%) |
| `matches` | 3.678 s | 0.244 s (6.6%) | 3.435 s (93.4%) |

For this short-row corpus, table access is not the bottleneck. Per-row function
entry, matching, replacement construction, match tuple construction, and SRF
execution dominate.

## Direct Go-versus-PostgreSQL comparison

| Comparison | Go reference | PostgreSQL extension | Interpretation |
|---|---:|---:|---|
| Real-dictionary build | 1.401 ms | 1.162 ms, or 3.095 ms stored | Both are negligible |
| Retained machine/automaton heap | 3.56 MiB | 227 KiB cached runtime | C runtime is about 16x smaller |
| Specialized one-shot replacement | 1.11 s | 2.007 s, one-backend server computation | PostgreSQL is about 1.81x slower on short rows |
| In-server replacement, four workers | n/a | 0.510 s | About 2.18x faster than the one-shot Go wall time, before transfer/storage |
| In-server replacement, eight workers | n/a | 0.291 s | About 3.81x faster than the one-shot Go wall time, before transfer/storage |
| Full ordered result through `psql` | 1.11 s streaming file output | 4.05 s | Client buffering and ordered row transfer dominate PostgreSQL path |

The 4.05-second `psql` result is a correctness path, not a recommended export
benchmark. `psql`/libpq buffered the huge ordinary `SELECT` result and reached
about 673 MiB client RSS. Production exports should use a streaming `COPY`
shape, and in-database applications should avoid exporting rows that can be
consumed or materialized in the server.

The Go and PostgreSQL implementations are also not feature-equivalent cost
centers. The Go reference is a contest-specific replacer. The extension
validates a portable artifact, supports text and bytea, preserves IDs and
priorities, exposes byte and character coordinates, supports multiple match
policies, enforces output limits, and operates through PostgreSQL's executor.

## Memory behavior

### Go

- Retained automaton arrays: 3,731,840 bytes.
- Measured post-GC automaton heap increment: 3,735,792 bytes (3.56 MiB).
- One-shot process peak RSS: roughly 11 MiB.
- I/O remains streaming; the normal replacement path does not retain the
  322 MB input or output.
- The historical correctness helper is not streaming and should be excluded
  from production memory claims.

### PostgreSQL extension

After `acdat_prepare_runtime`, the format-major-1 cached runtime releases the
materialized `output_link` array and retains four slot arrays, the root table,
terminals, and replacements. On this arm64 build the payload calculation is:

```text
cached runtime = 120
               + 4 * slot_count * 4
               + 256 * 4
               + pattern_count * 40
               + replacement_bytes
```

Measured or calculated steady cached payloads:

| Dictionary shape | Stored artifact | Cached runtime per process | Nine-process budget (8 workers + leader) |
|---|---:|---:|---:|
| Historical 1,114-rule dictionary | 269.2 KiB | 227.2 KiB | about 2.0 MiB |
| Generated 100K shared-prefix dictionary | 5.936 MiB | 5.512 MiB | about 49.6 MiB |
| Synthetic 100K random ASCII dictionary | 28.961 MiB | 23.933 MiB | about 215.4 MiB |

These values cover the steady cached automaton payload, not full PostgreSQL
backend RSS. First use also requires a detoasted artifact and validation
scratch space before the detoasted copy is released. `matches` and `replace`
add result memory proportional to the current input row and its match/output
count.

The historical source file was 306.9 MiB while its simple PostgreSQL heap was
about 622 MiB, roughly a 2x storage/cache footprint before indexes. That is a
table representation cost, not machine memory.

## 100K-pattern standalone core matrix

Command:

```bash
BENCH_REPEATS=3 BENCH_INPUT_MIB=16 make bench-matrix
```

All cells use the materialized runtime corresponding to PostgreSQL's cache
path.

| 100K profile | Artifact | Runtime | Build | Materialize | Scan |
|---|---:|---:|---:|---:|---:|
| ASCII miss | 28.961 MiB | 23.933 MiB | 98.584 ms | 7.989 ms | 72.57 MiB/s |
| Root-only miss | 27.054 MiB | 22.407 MiB | 65.295 ms | 7.632 ms | 965.43 MiB/s |
| ASCII hit-heavy | 28.961 MiB | 23.933 MiB | 96.183 ms | 8.092 ms | 361.60 MiB/s |
| UTF-8 byte miss | 28.673 MiB | 23.703 MiB | 92.937 ms | 8.915 ms | 971.11 MiB/s |

The large spread is workload-driven. Root misses avoid most state traversal;
ordinary ASCII misses repeatedly traverse failure links and stress a much
larger working set. Dictionary count alone is not a sufficient throughput
predictor.

## 100K patterns inside PostgreSQL

The PostgreSQL compile test used generated patterns of the form
`pattern-NNNNNNNN`. Their long shared prefix makes the machine much smaller
than the random-pattern standalone matrix, so build and size values must not be
compared as if the dictionaries were identical.

| Item | Result |
|---|---:|
| Pattern count | 100,000 |
| States / slots | 111,127 / 111,191 |
| Aggregate compile | 36.910 ms |
| Compile plus CTAS/TOAST storage | 69.161 ms |
| Stored artifact | 6,224,012 bytes (5.936 MiB) |
| Cached runtime | 5,780,200 bytes (5.512 MiB) |
| One million generated short root-miss calls | 129.011 ms |

### Row-width sensitivity

All three profiles scan roughly 256-314 MiB of root-miss text through the same
100K machine.

| Row shape | One backend | Four workers | Eight workers |
|---|---:|---:|---:|
| About 33 B, 10M rows | 600.624 ms / 522 MiB/s | 140.086 ms / 2.19 GiB/s | 104.017 ms / 2.94 GiB/s |
| 1 KiB, 262,144 rows | 281.564 ms / 909 MiB/s | 73.290 ms / 3.41 GiB/s | 49.947 ms / 5.01 GiB/s |
| 64 KiB, 4,096 rows | 280.353 ms / 913 MiB/s | 70.504 ms / 3.55 GiB/s | 48.777 ms / 5.13 GiB/s |

The one-backend 1 KiB and 64 KiB results are about 94% of the 965 MiB/s
standalone root-miss result. The 33-byte case reaches about 54%. This isolates
the PostgreSQL per-row boundary: the scanner is near standalone speed once
each call performs enough useful work.

The 10M short-row four-worker speedup was 4.29x, exceeding the 2.5x four-worker
target in the PRD. The eight-worker speedup was 5.77x. A one-million-row test
did not scale as well because parallel startup and per-worker machine setup
were too large relative to only about 58 ms of one-backend work.

## Parallel `matches` planning trap

The direct relation join below was safe for correctness but produced a bad
parallel plan in this test:

```sql
SELECT count(*)
FROM bench_doc AS d
CROSS JOIN bench_machine AS m
CROSS JOIN LATERAL acdat.matches(
  d.line, m.machine, 'all_overlapping'
) AS h;
```

With eight workers, PostgreSQL placed the one-row machine scan inside the
document loop. `EXPLAIN (ANALYZE, BUFFERS)` reported:

- 10,318,163 loops over `bench_machine`;
- more than 10.3 million buffer hits attributable to that scan;
- 10.43 seconds execution time.

Making the machine an uncorrelated scalar InitPlan avoided the rescan:

```sql
SELECT count(*)
FROM bench_doc AS d
CROSS JOIN LATERAL acdat.matches(
  d.line,
  (SELECT machine FROM bench_machine),
  'all_overlapping'
) AS h;
```

The corrected eight-worker query took 0.476 seconds, about 22x faster, with the
same 601,838 matches. Applications should publish or select a machine once per
query and inspect the parallel plan for large scans. A `MATERIALIZED` shape or
an equivalent InitPlan is preferable to a join shape that can be reordered
inside the document loop.

## Scenario guidance

| Scenario | Recommended path | Reason |
|---|---|---|
| One-shot file replacement | Original Go tool or a dedicated streaming program | Lowest latency and about 11 MiB RSS |
| Data already in PostgreSQL, existence test only | `acdat.contains` | Avoids match tuples and output construction |
| In-database redaction or normalization | `acdat.replace` with candidate filtering | Keeps data local and parallelizes well |
| Entity or IOC extraction | `acdat.matches` with explicit machine InitPlan | Returns IDs/positions; requires plan discipline |
| Wide documents or log payloads | PostgreSQL extension | Reaches about 94% of standalone core in one backend |
| Millions of tiny rows | Filter candidates first; use parallelism only for sufficiently large scans | Per-row executor overhead dominates |
| Full result export | Streaming `COPY` or application streaming | Avoid ordinary `SELECT` buffering in clients |
| 100K+ machine with many workers | Budget runtime times participating processes | Each worker materializes a private machine copy |

## Limitations and remaining gates

This report does not establish:

- cold filesystem or cold TOAST latency;
- durable `INSERT`, `UPDATE`, materialized-hit-table, WAL, or replication cost;
- concurrent-client throughput or latency under mixed workloads;
- full backend peak RSS during large dictionary construction;
- Linux amd64 or arm64 performance;
- a modern large real-world CJK, URL, IOC, or binary-signature dictionary;
- comparison with regex alternation, `LIKE ANY`, `pg_trgm`, or another AC
  implementation on the exact same dictionaries;
- package-lab or production-host performance.

The historical corpus is useful because it provides exact Go compatibility
and a stable full-output checksum, but 1,114 rules are far below the extension's
100K target. The synthetic matrix covers scale and execution shape; it does not
replace a modern real dictionary benchmark.

## Verification commands

Core and extension checks:

```bash
make core-test
make installcheck
make cache-smoke-test
BENCH_REPEATS=3 BENCH_INPUT_MIB=16 make bench-matrix
```

Representative real-corpus PostgreSQL operators:

```sql
SELECT count(*) FILTER (WHERE acdat.contains(d.line, m.machine))
FROM bench_doc AS d
CROSS JOIN bench_machine AS m;

SELECT sum(octet_length(
  acdat.replace(d.line, m.machine, 'leftmost_longest')
) + 1)
FROM bench_doc AS d
CROSS JOIN bench_machine AS m;

SELECT count(*)
FROM bench_doc AS d
CROSS JOIN LATERAL acdat.matches(
  d.line,
  (SELECT machine FROM bench_machine),
  'all_overlapping'
) AS h;
```

## Final assessment

ACDAT 0.2.0 is performance-credible as an in-database exact literal-rule
engine:

- correctness matches the original Go result over the complete historical
  corpus;
- build time and machine size are small;
- the cached C runtime is substantially more compact than the Go reference;
- single-backend short-row replacement pays a visible PostgreSQL boundary
  cost;
- wide-row throughput is close to the standalone core;
- four- and eight-worker scans scale well once the workload is large enough;
- query shape, result transfer, and per-worker memory multiplication matter as
  much as the matcher itself.

Use the Go implementation when the problem is a specialized streaming file
job. Use the extension when the data and downstream work belong in
PostgreSQL, and design the SQL plan so the machine is compiled or selected once
and reused across the scan.
