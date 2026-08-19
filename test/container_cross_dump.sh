#!/usr/bin/env bash
set -euo pipefail

version="${1:?PostgreSQL major version is required}"
output_file="${2:?output path is required}"

cp -a /repo /tmp/acdat
cd /tmp/acdat
make clean >/dev/null
make "PG_CONFIG=/usr/lib/postgresql/${version}/bin/pg_config" \
  PG_CFLAGS='-std=c11 -Werror' >/dev/null
make install "PG_CONFIG=/usr/lib/postgresql/${version}/bin/pg_config" \
  >/dev/null
pg_ctlcluster "$version" main start
gosu postgres createdb acdat_cross

gosu postgres psql -X -v ON_ERROR_STOP=1 -d acdat_cross <<'SQL' >/dev/null
CREATE EXTENSION acdat;
CREATE TABLE public.machine_store(machine acdat.machine NOT NULL);

WITH machine AS (
  SELECT acdat.compile(
    ARRAY['病毒', 'signature'],
    ARRAY[1, 2]::bigint[]
  ) AS value
)
INSERT INTO public.machine_store SELECT value FROM machine;

WITH published AS (
  SELECT acdat.publish('cross', 1, machine) AS build_id
  FROM public.machine_store
)
SELECT acdat.activate('cross', build_id) FROM published;
SQL

gosu postgres pg_dump -Fc -f /tmp/acdat-cross.dump acdat_cross
cp /tmp/acdat-cross.dump "$output_file"
