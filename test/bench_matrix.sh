#!/usr/bin/env bash

set -euo pipefail

bench=${1:-./build/acdat-core-bench}
input_mib=${BENCH_INPUT_MIB:-16}
repeats=${BENCH_REPEATS:-3}

if [[ ! -x "$bench" ]]; then
  printf 'benchmark binary is not executable: %s\n' "$bench" >&2
  exit 1
fi
if ! [[ "$input_mib" =~ ^[1-9][0-9]*$ ]]; then
  printf 'BENCH_INPUT_MIB must be a positive integer\n' >&2
  exit 1
fi
if ! [[ "$repeats" =~ ^[1-9][0-9]*$ ]]; then
  printf 'BENCH_REPEATS must be a positive integer\n' >&2
  exit 1
fi

if [[ -n ${BENCH_PATTERNS:-} ]]; then
  read -r -a pattern_counts <<<"$BENCH_PATTERNS"
else
  pattern_counts=(10000 100000)
fi

printf 'benchmark_meta os=%s arch=%s input_mib=%s repeats=%s\n' \
  "$(uname -s)" "$(uname -m)" "$input_mib" "$repeats"

run_profile() {
  local profile=$1
  local pattern_bytes=$2
  local pattern_count=$3
  local run

  for ((run = 1; run <= repeats; run++)); do
    "$bench" "$pattern_count" "$pattern_bytes" "$input_mib" \
      "$profile" materialized 0
  done
}

for pattern_count in "${pattern_counts[@]}"; do
  if ! [[ "$pattern_count" =~ ^[1-9][0-9]*$ ]]; then
    printf 'BENCH_PATTERNS contains an invalid value: %s\n' \
      "$pattern_count" >&2
    exit 1
  fi
  run_profile ascii-miss 16 "$pattern_count"
  run_profile root-miss 16 "$pattern_count"
  run_profile ascii-hit 16 "$pattern_count"
  run_profile utf8-miss 18 "$pattern_count"
done

for ((run = 1; run <= repeats; run++)); do
  "$bench" 16 16 1 overlap materialized 0
done
