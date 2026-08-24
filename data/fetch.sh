#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
checksum_file="$script_dir/SHA256SUMS"
target_dir=${1:-"$script_dir"}
base_url=${ACDAT_DATA_BASE_URL:-"https://github.com/Vonng/ac/releases/download/benchmark-data-2018"}
base_url=${base_url%/}

if command -v sha256sum >/dev/null 2>&1; then
    hash_tool=sha256sum
elif command -v shasum >/dev/null 2>&1; then
    hash_tool=shasum
else
    printf '%s\n' 'sha256sum or shasum is required' >&2
    exit 1
fi

hash_file() {
    if [ "$hash_tool" = sha256sum ]; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

fetch_asset() {
    asset_name=$1
    expected_size=$2
    target="$target_dir/$asset_name"
    partial="$target.part"
    url="$base_url/$asset_name"
    expected=$(awk -v name="$asset_name" '$2 == name {print $1; exit}' "$checksum_file" | tr -d '\r')

    if ! printf '%s\n' "$expected" | LC_ALL=C grep -Eq '^[0-9a-f]{64}$'; then
        printf 'invalid checksum for %s in %s\n' "$asset_name" "$checksum_file" >&2
        exit 1
    fi

    if [ -f "$target" ] && [ "$(hash_file "$target")" = "$expected" ]; then
        printf 'benchmark data is already verified: %s\n' "$target"
        return
    fi

    mkdir -p "$target_dir"
    trap 'rm -f "$partial"' 0 1 2 15

    printf 'downloading %s\n' "$url"
    curl --fail --location --retry 3 --retry-delay 2 \
        --proto '=https,file' --proto-redir '=https' --tlsv1.2 \
        --output "$partial" "$url"

    actual_size=$(wc -c < "$partial" | tr -d '[:space:]')
    if [ "$actual_size" -ne "$expected_size" ]; then
        printf '%s size mismatch: expected %s, got %s\n' \
            "$asset_name" "$expected_size" "$actual_size" >&2
        exit 1
    fi

    actual=$(hash_file "$partial")
    if [ "$actual" != "$expected" ]; then
        printf '%s checksum mismatch: expected %s, got %s\n' \
            "$asset_name" "$expected" "$actual" >&2
        exit 1
    fi

    mv -f "$partial" "$target"
    trap - 0 1 2 15
    printf 'benchmark data verified: %s\n' "$target"
}

fetch_asset dict.txt.tgz 10554
fetch_asset video_title.txt.tgz 187281061
