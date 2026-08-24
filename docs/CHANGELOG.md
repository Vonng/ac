# Changelog

## 0.2.0 - 2026-08-24

- Removed the unnecessary globally unique BASE constraint from the Basic
  parent-state CHECK builder, eliminating pathological UTF-8 dictionary build
  searches while improving DAT occupancy and reducing builder memory.
- Added cancellable PostgreSQL compilation, a base-candidate safety budget,
  and standalone build-work metrics.
- Added a materialized runtime output chain that releases one four-byte array
  per DAT slot without changing the format-major-1 artifact.
- Added a 256-entry root transition table for low-hit and root-miss scans.
- Made machine-cache replacement exception-safe and made serialization reject
  runtime-prepared automata explicitly.
- Expanded the standalone benchmark with ASCII miss, root miss, hit-heavy,
  UTF-8 byte-oriented, and worst-overlap profiles plus materialization and
  runtime-memory measurements.
- Evaluated and rejected Packed runtime states and LexDFS placement because
  their workload-specific wins did not justify permanent selection complexity.
- Preserved the 0.1.0 SQL API and machine format, with an extension update path
  to 0.2.0.

## 0.1.0 - 2026-08-22

- Rebuilt the original ACDAT experiment as a C and PGXS PostgreSQL extension.
- Added deterministic `text` and `bytea` dictionary compilation.
- Added `contains`, `matches`, `replace`, machine inspection, validation,
  import, export, and fingerprint APIs.
- Added immutable dictionary builds with transactional publish, activate,
  rollback, and retirement workflows in the fixed `acdat` schema.
- Added PostgreSQL 14-18 regression, sanitizer, dump/restore, cache, and
  cross-version tests.
- Added a detailed usage guide, runnable SQL demonstration, and performance
  baselines.
