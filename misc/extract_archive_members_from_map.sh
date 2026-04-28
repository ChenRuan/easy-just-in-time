#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 <linker-map> [output-dir]" >&2
  exit 2
fi

map_file=$1
out_dir=${2:-.}

mkdir -p "$out_dir"

if [[ ! -f "$map_file" ]]; then
  echo "error: linker map not found: $map_file" >&2
  exit 1
fi

extract_one() {
  local archive=$1
  local out_file=$2

  local escaped_archive
  escaped_archive=$(printf '%s\n' "$archive" | sed 's/[.[\*^$()+?{}|\\]/\\&/g')

  rg "/$escaped_archive\\(" "$map_file" |
    sed -E "s#.*$escaped_archive\\(([^)]*)\\).*#\\1#" |
    sort -u > "$out_file" || true
}

extract_one "libc++.a" "$out_dir/libcxx-members.txt"
extract_one "libc++abi.a" "$out_dir/libcxxabi-members.txt"
extract_one "libunwind.a" "$out_dir/libunwind-members.txt"

{
  sed 's#^#libc++.a\t#' "$out_dir/libcxx-members.txt"
  sed 's#^#libc++abi.a\t#' "$out_dir/libcxxabi-members.txt"
  sed 's#^#libunwind.a\t#' "$out_dir/libunwind-members.txt"
} | awk 'NF >= 2' | sort -u > "$out_dir/archive-members.tsv"

printf 'libc++.a members: %s\n' "$(wc -l < "$out_dir/libcxx-members.txt")"
printf 'libc++abi.a members: %s\n' "$(wc -l < "$out_dir/libcxxabi-members.txt")"
printf 'libunwind.a members: %s\n' "$(wc -l < "$out_dir/libunwind-members.txt")"
printf 'combined TSV: %s\n' "$out_dir/archive-members.tsv"
