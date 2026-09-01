#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $# -lt 3 || $# -gt 5 ]]; then
  echo "usage: $0 reference.fa index.nvm queries.fa|fastq [threads] [verify_limit]" >&2
  exit 2
fi

reference_path=$1
index_path=$2
query_path=$3
default_threads=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
threads=${4:-$default_threads}
verify_limit=${5:-100}

for required_file in "$reference_path" "$index_path" "$query_path"; do
  if [[ ! -f "$required_file" ]]; then
    echo "input not found: $required_file" >&2
    exit 2
  fi
done

echo "cache-enabled query benchmark" >&2
"$project_dir/build/navigamer" query \
  --index "$index_path" --reference "$reference_path" \
  --queries "$query_path" --tolerance 3 --threads "$threads" \
  --anchor-refresh 0 --block-size 64 \
  --output /dev/null

echo "cache-disabled query benchmark" >&2
"$project_dir/build/navigamer" query \
  --index "$index_path" --reference "$reference_path" \
  --queries "$query_path" --tolerance 3 --threads "$threads" \
  --no-cache --output /dev/null

echo "brute-force exactness audit (first $verify_limit queries)" >&2
"$project_dir/build/navigamer" verify \
  --index "$index_path" --reference "$reference_path" \
  --queries "$query_path" --tolerance 3 --threads "$threads" \
  --limit "$verify_limit"
