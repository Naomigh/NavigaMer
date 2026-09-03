#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $# -lt 1 || $# -gt 3 ]]; then
  echo "usage: $0 reference.fa [window_limit] [threads]" >&2
  exit 2
fi

reference_path=$1
limit=${2:-10000}
default_threads=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
threads=${3:-$default_threads}
index_path="$project_dir/build/ecoli_${limit}.nvm"

if [[ ! -f "$reference_path" ]]; then
  echo "reference FASTA not found: $reference_path" >&2
  exit 2
fi

cmake -S "$project_dir" -B "$project_dir/build" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build "$project_dir/build" -j "$threads"
ctest --test-dir "$project_dir/build" --output-on-failure

"$project_dir/build/navigamer" build \
  --reference "$reference_path" \
  --output "$index_path" \
  --window 150 \
  --stride 1 \
  --limit "$limit" \
  --radii 90,55,30 \
  --build-mode topdown \
  --local-creation \
  --top-fill-radius 75 \
  --beacons 4 \
  --threads "$threads"

echo "index written to $index_path"
