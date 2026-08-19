#!/usr/bin/env bash
set -euo pipefail

src_db="acdat_dump_src_$$"
dst_db="acdat_dump_dst_$$"
dump_file="$(mktemp /tmp/acdat-dump.XXXXXX)"

cleanup() {
  dropdb --if-exists "$src_db" >/dev/null 2>&1 || true
  dropdb --if-exists "$dst_db" >/dev/null 2>&1 || true
  if [[ -f "$dump_file" ]]; then
    unlink "$dump_file"
  fi
}
trap cleanup EXIT

createdb "$src_db"
psql -X -v ON_ERROR_STOP=1 -q -d "$src_db" <<'SQL'
CREATE EXTENSION acdat;

WITH machine AS (
  SELECT acdat.compile(ARRAY['old'], ARRAY[1]::bigint[]) AS value
)
SELECT acdat.publish('rules', 1, value) FROM machine;

WITH machine AS (
  SELECT acdat.compile(ARRAY['new'], ARRAY[2]::bigint[]) AS value
), published AS (
  SELECT acdat.publish('rules', 2, value) AS build_id FROM machine
)
SELECT acdat.activate('rules', build_id) FROM published;
SQL

pg_dump -Fc -f "$dump_file" "$src_db"
createdb "$dst_db"
pg_restore --exit-on-error -d "$dst_db" "$dump_file"

actual="$(psql -X -v ON_ERROR_STOP=1 -At -d "$dst_db" <<'SQL'
SELECT count(*) FROM acdat.dictionary_build;
SELECT count(*) FROM acdat.dictionary_build_data;
SELECT count(*) FROM acdat.active_dictionary;
SELECT acdat.contains('new', machine)
FROM acdat.active_machine
WHERE name = 'rules';
SQL
)"
expected=$'2\n1\n1\nt'

if [[ "$actual" != "$expected" ]]; then
  printf 'dump/restore mismatch\nexpected:\n%s\nactual:\n%s\n' \
    "$expected" "$actual" >&2
  exit 1
fi

echo "acdat dump/restore test: ok"
