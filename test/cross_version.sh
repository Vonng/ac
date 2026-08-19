#!/usr/bin/env bash
set -euo pipefail

source_version="${SOURCE_PG:-14}"
target_db="acdat_cross_target_$$"
output_dir="$(mktemp -d /tmp/acdat-cross.XXXXXX)"
dump_file="$output_dir/acdat.dump"

cleanup() {
  dropdb --if-exists "$target_db" >/dev/null 2>&1 || true
  if [[ -f "$dump_file" ]]; then
    unlink "$dump_file"
  fi
  rmdir "$output_dir" 2>/dev/null || true
}
trap cleanup EXIT

docker run --rm \
  -v "$PWD:/repo:ro" \
  -v "$output_dir:/out" \
  "acdat-pg${source_version}-dev" \
  bash /repo/test/container_cross_dump.sh \
       "$source_version" /out/acdat.dump

createdb "$target_db"
pg_restore --exit-on-error -d "$target_db" "$dump_file"

actual="$(psql -X -v ON_ERROR_STOP=1 -At -d "$target_db" <<'SQL'
SELECT acdat.contains('发现病毒', machine) FROM public.machine_store;
SELECT acdat.contains('signature', machine)
FROM acdat.active_machine
WHERE name = 'cross';
SELECT acdat.info(machine)->>'format_major' FROM public.machine_store;
SQL
)"
expected=$'t\nt\n1'

if [[ "$actual" != "$expected" ]]; then
  printf 'cross-version restore mismatch\nexpected:\n%s\nactual:\n%s\n' \
    "$expected" "$actual" >&2
  exit 1
fi

echo "acdat cross-version test: PostgreSQL ${source_version} -> local ok"
