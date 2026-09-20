# NavigaMer

C++20 three-level genome index.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

Add `-DNAVIGAMER_NATIVE=ON` to tune compilation for the current CPU.

## Build an index

```bash
build/navigamer build \
  --reference reference.fa --index reference.nvm \
  --window 150 --stride 1 --T 5 \
  --radii 60,35,15 --beacon-ratios 0.1,0.1,0.1 --threads 32
```

Defaults: window 150, stride 1, T 5, radii `60,35,15`, and beacon ratios
`0.1,0.1,0.1`. Threads default to the hardware thread count.

`--beacon-ratios L,M,S` gives the fractions of large centers at the root,
middle centers inside each large world, and small centers inside each middle
world. Each parent scope selects `ceil(ratio * center_count)` beacons and owns
its distance table. Fractions range from 0 to 1. Zero means exact all-center
classification at that layer without a beacon table. `--limit N` restricts
construction to the first N reference windows.

Centers belong only to their own world. Other members use the `x + 2T`
assignment rule. To retain exact nearest-child query coverage with exclusive
centers, use each selection radius greater than `2T`. Query tolerance must
not exceed construction T; the CLI rejects a larger query tolerance. The radius
relationship remains a caller precondition.

All production edit distances use vendored Edlib with global distance-only
alignment. Leaf members store their actual bases. There is no sequence
deduplication, leaf compression/sharing, BK directory, first-layer mode,
checkpoint recovery, budget check, or runtime integrity validation. Index
format version 2 stores all three beacon-table levels; rebuild older indexes.

The build report gives center-selection, beacon-table, and classification time
per layer, together with Edlib call counts, pruning ratio, save time, and peak
resident memory.

## Query

```bash
build/navigamer query \
  --reference reference.fa --index reference.nvm \
  --queries reads.fastq --tolerance 5 --threads 32 --output hits.tsv
```

FASTA and FASTQ query files are supported. Output columns are query, contig,
zero-based start, edit distance, strand, and sequence ID. Every reference
window has its own ID, including repeated sequences.

Options: `--both-strands`, `--route scan` for nearest-center scan routing,
`--no-cache`, `--prefetch`, `--no-output`, `--block-size N` (default 64), and
`--limit N`. Default routing uses the stored large-, middle-, and small-center
beacon tables; leaf sequences are verified directly.

## Inspect, verify, and benchmark

```bash
build/navigamer inspect --index reference.nvm

build/navigamer verify \
  --reference reference.fa --index reference.nvm \
  --queries reads.fa --limit 100 --tolerance 5

build/navigamer benchmark \
  --reference reference.fa --index reference.nvm \
  --queries reads.fa --threads 32 --repeat 10 --warmup 100
```

`verify` is a separate diagnostic comparing full hit-and-distance sets with
brute force; it is not called by construction or querying.
