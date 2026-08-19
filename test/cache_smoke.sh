#!/usr/bin/env bash
set -euo pipefail

test_db="acdat_cache_smoke_$$"

cleanup() {
  dropdb --if-exists "$test_db" >/dev/null 2>&1 || true
}
trap cleanup EXIT

createdb "$test_db"
actual="$(psql -X -v ON_ERROR_STOP=1 -At -d "$test_db" <<'SQL'
SET statement_timeout = '10s';
CREATE EXTENSION acdat;
CREATE TEMP TABLE benchmark_machine AS
SELECT acdat.compile(
         'pattern-' || lpad(i::text, 8, '0'),
         i::bigint
       ) AS machine
FROM generate_series(1, 100000) AS g(i);

SELECT count(*)
FROM generate_series(1, 250000) AS g(i)
CROSS JOIN benchmark_machine AS m
WHERE acdat.contains('ordinary-row-with-no-hit-' || i::text, m.machine);
SQL
)"

if [[ "$actual" != $'SET\nCREATE EXTENSION\nSELECT 1\n0' ]]; then
  printf 'cache smoke mismatch:\n%s\n' "$actual" >&2
  exit 1
fi

echo "acdat cache smoke test: ok"
