#!/usr/bin/env bash
set -euo pipefail

versions="${PG_VERSIONS:-14 15 16 17 18}"

for version in $versions; do
  image="acdat-pg${version}-dev"
  if [[ "${MATRIX_SKIP_BUILD:-0}" != "1" ]]; then
    echo "==> building PostgreSQL ${version} test image"
    docker build --progress=plain --platform linux/amd64 \
      --build-arg "PG_MAJOR=${version}" \
      -t "$image" \
      -f test/docker/Dockerfile .
  fi

  echo "==> testing PostgreSQL ${version}"
  docker run --rm -v "$PWD:/repo:ro" "$image" sh -lc "
    set -e
    cp -a /repo /tmp/acdat
    cd /tmp/acdat
    make clean >/dev/null
    make PG_CONFIG=/usr/lib/postgresql/${version}/bin/pg_config \\
      PG_CFLAGS='-std=c11 -Werror'
    make install PG_CONFIG=/usr/lib/postgresql/${version}/bin/pg_config >/dev/null
    pg_ctlcluster ${version} main start
    chown -R postgres:postgres /tmp/acdat
    gosu postgres make installcheck \\
      PG_CONFIG=/usr/lib/postgresql/${version}/bin/pg_config
  "
done

echo "acdat PostgreSQL matrix: ok (${versions})"
