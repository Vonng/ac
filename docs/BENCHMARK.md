# acdat benchmark notes

The comprehensive real-corpus Go-versus-PostgreSQL report for the current
Apple M5 Max test host is recorded in
[`PERFORMANCE.md`](PERFORMANCE.md).

## 2026-08-24: v0.2.0 benchmark-gated changes

Environment:

- Apple arm64, Apple clang 21.0.0
- `-O3 -DNDEBUG`
- 16 MiB deterministic input per scan
- three runs per cell; table values are medians
- materialized format-major-1 runtime, matching the PostgreSQL cache path

Command:

```bash
BENCH_REPEATS=3 BENCH_INPUT_MIB=16 make bench-matrix
```

The comparison baseline was captured with the expanded benchmark immediately
before the v0.2.0 core changes. The workload generator and compiler settings
were otherwise identical.

| 100K profile | v0.1 build | v0.2 build | v0.1 scan | v0.2 scan |
|---|---:|---:|---:|---:|
| ASCII miss | 0.280 s | 0.116 s | 59.97 MiB/s | 64.80 MiB/s |
| Root-only miss | 0.278 s | 0.077 s | 449.67 MiB/s | 936.06 MiB/s |
| ASCII hit-heavy | 0.278 s | 0.115 s | 226.42 MiB/s | 402.27 MiB/s |
| UTF-8 byte miss | 19.269 s | 0.109 s | 485.55 MiB/s | 928.29 MiB/s |

The UTF-8 build regression was caused by globally unique BASE values, a
constraint needed by label-only Compact CHECK but not by acdat's parent-state
Basic CHECK. Removing the constraint reduced the 100K UTF-8 base search to
1,206,195 candidates, produced 1,303,302 slots for 1,303,082 states, and
removed the `used_base` builder array. This simple change was sufficient; a
free-list, XOR placement, and tail fallback were not added.

The runtime output-chain preparation retains the format-major-1 artifact but
releases the materialized `output_link` array. At 100K ASCII patterns, runtime
bytes fell from 31,051,372 to 25,095,160 (19.2%). Artifact bytes also fell from
31,051,452 to 30,367,716 because the relaxed BASE placement produced fewer
vacant slots. A 1 KiB root transition table accounts for the small fixed
runtime overhead.

Two paper-derived experiments were rejected and removed:

- LexDFS improved build time and the hit-heavy scan, but slowed the primary
  100K ASCII-miss workload by about 60% and did not consistently improve the
  UTF-8 miss workload. LexBFS remains the only placement path.
- Packed array-of-struct runtime states improved some large-machine cells but
  regressed a 1K machine by roughly 25%, slightly regressed the root-only cell,
  and added a second full materialization copy. The single SoA runtime remains.

PostgreSQL 18.4, 100K generated `pattern-NNNNNNNN` rules, and one million
short root-miss rows completed in a median 127.54 ms over three warm runs. The
corresponding v0.1.0 note below recorded 275 ms. This is a PostgreSQL
end-to-end measurement including function-call and executor overhead, not just
the standalone scanner.

These are local engineering results, not cross-platform release claims. Linux
amd64/arm64, PostgreSQL short-row, parallel-worker, and package-lab results
remain separate release gates.

## 2026-08-22: byte256 ACDAT baseline

Environment:

- Apple arm64, Apple clang 21.0.0
- `-O3 -DNDEBUG`
- Synthetic deterministic lowercase patterns, 16 bytes each
- Deterministic lowercase random no-hit input

Command:

```bash
PATTERNS=100000 PATTERN_BYTES=16 INPUT_MIB=16 make core-bench
```

Results after the next-check-position density optimization:

| Patterns | States | DAT slots | Occupancy | Artifact | Build | Scan |
|---:|---:|---:|---:|---:|---:|---:|
| 10,000 | 140,703 | 144,561 | 97.3% | 3.29 MB | 0.023 s | 88 MiB/s |
| 100,000 | 1,318,279 | 1,356,597 | 97.2% | 31.13 MB | 0.271 s | 65 MiB/s |

The pre-optimization 10K build took 5.5 seconds, and the 100K build did not
finish within 150 seconds. The improvement comes from advancing the global
base-search cursor when the scanned check-array window is at least 95% full,
instead of repeatedly scanning isolated early holes.

These are engineering baselines, not release claims. Follow-up benchmarks must
include real Chinese/URL/IOC dictionaries, hit-heavy and overlap-heavy inputs,
cold/warm TOAST, PostgreSQL per-row overhead, parallel workers, and a packed or
sparse AC comparator.

## 2026-08-22: PostgreSQL wrapper and TOAST cache

PostgreSQL 18.4, local temporary table, 100K generated text patterns:

- In-database aggregate compile + serialize: 86 ms
- Artifact: 6.22 MB, 111,127 states, 111,205 slots
- 1,000,000 short no-hit rows through one function expression: 275 ms

The first implementation called `PG_GETARG_VARLENA_PP` before checking
`fn_extra`; a 6.22 MB out-of-line value was therefore detoasted once per row and
the same query ran for more than four minutes before cancellation. The fixed
path compares the small on-disk TOAST pointer first and detoasts, validates, and
materializes only when the build changes. This cache behavior is a release
correctness/performance invariant and has dedicated regression/benchmark
coverage.

## 2026-08-22: historical Chinese dictionary

The original repository's `dict.txt` contains 1,114 Chinese movie/music
patterns. PostgreSQL 18.4 results:

- Aggregate compile + serialize: 5.4 ms
- 10,840 states / 11,357 DAT slots (95.4% occupancy)
- Artifact: 283,074 bytes, including 11,182 replacement bytes
- Maximum pattern: 40 UTF-8 bytes / 14 characters
- 100,000 short hit-heavy rows through `acdat.contains`: 22.2 ms

This confirms exact UTF-8 byte matching on the historical workload, but it is
far smaller than the 100K target and does not replace a modern real-world CJK
corpus benchmark.
