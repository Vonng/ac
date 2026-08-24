#!/usr/bin/env bash

set -euo pipefail

test_db="acdat_upgrade_$$"

cleanup() {
  dropdb --if-exists "$test_db" >/dev/null 2>&1 || true
}
trap cleanup EXIT

createdb "$test_db"
actual="$(psql -X -v ON_ERROR_STOP=1 -At -d "$test_db" <<'SQL'
CREATE EXTENSION acdat VERSION '0.1.0';
CREATE TABLE saved_machine AS
SELECT acdat.compile(
         ARRAY['he', 'she', 'his', 'hers'],
         ARRAY[1, 2, 3, 4]::bigint[]
       ) AS machine;
SELECT extversion FROM pg_extension WHERE extname = 'acdat';
ALTER EXTENSION acdat UPDATE TO '0.2.0';
SELECT extversion FROM pg_extension WHERE extname = 'acdat';
SELECT acdat.contains('ushers', machine) FROM saved_machine;
SELECT acdat.info(machine)->>'format_major' FROM saved_machine;
SQL
)"
expected=$'CREATE EXTENSION\nSELECT 1\n0.1.0\nALTER EXTENSION\n0.2.0\nt\n1'

if [[ "$actual" != "$expected" ]]; then
  printf 'extension upgrade mismatch\nexpected:\n%s\nactual:\n%s\n' \
    "$expected" "$actual" >&2
  exit 1
fi

echo "acdat extension upgrade test: ok"
