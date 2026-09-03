# NavigaMer

## Requirements

- CMake 3.20 or newer
- A C++20 compiler (GCC or Clang)
- Git (CMake fetches the pinned Edlib dependency during configuration)

## Build

```bash
git clone https://github.com/Naomigh/NavigaMer.git
cd NavigaMer

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j 16
ctest --test-dir build --output-on-failure
```

The executable is `build/navigamer`.

## Command help

```bash
./build/navigamer --help
```

## Build an index

The reference must be FASTA. This example creates 150 bp windows at stride 1
and builds the three default world layers.

```bash
./build/navigamer build \
  --reference reference.fa \
  --output reference.nvm \
  --window 150 \
  --stride 1 \
  --radii 90,55,30 \
  --build-mode topdown \
  --local-creation \
  --top-fill-radius 75 \
  --beacons 4 \
  --threads 16 \
  --stats-output build_stats.tsv
```

Build options:

| Option | Default | Usage |
|---|---:|---|
| `--window N` | `150` | Reference-window length |
| `--stride N` | `1` | Distance between consecutive windows |
| `--radii A,B,C` | `90,55,30` | Tuned 150 bp E. coli world radii, coarse to fine |
| `--build-mode topdown\|nested\|owner` | `topdown` | Hierarchy construction mode |
| `--local-creation` | off | Use the cache-local incremental build mode |
| `--top-fill-radius N` | `0` | Cap the actual occupied radius of top worlds; `0` uses the nominal top radius |
| `--beacons N` | `4` | Maximum beacons per world |
| `--hot-cache N` | `16` | Recent worlds retained by local creation |
| `--threads N` | online CPUs | Worker count |
| `--limit N` | unlimited | Index at most N reference windows |
| `--no-delayed-centers` | off | Disable delayed-center selection |
| `--stats-output FILE` | none | Write build statistics |

## Inspect an index

```bash
./build/navigamer inspect --index reference.nvm --worlds-output worlds.tsv
```

## Query an index

Queries may be FASTA or FASTQ. Use reads with the same length as the indexed
windows. Querying requires the unchanged reference FASTA used to build the
index.

```bash
./build/navigamer query \
  --index reference.nvm \
  --reference reference.fa \
  --queries reads.fastq \
  --tolerance 3 \
  --threads 16 \
  --anchor-refresh 0 \
  --block-size 64 \
  --output hits.tsv \
  --timings query_timings.tsv \
  --stats-output query_stats.tsv
```

Query options:

| Option | Default | Usage |
|---|---:|---|
| `--tolerance N` | `5` | Maximum edit distance |
| `--threads N` | online CPUs | Worker count |
| `--both-strands` | off | Query the forward and reverse-complement strands |
| `--no-cache` | off | Disable the query path cache |
| `--no-path-pivot` | off | Disable the dynamic exact root-pivot row (diagnostics) |
| `--path-pivot-max-distance N` | `16` | Reuse a root-pivot row while its exact query distance is at most N |
| `--anchor-refresh N` | `0` | Experimental root-anchor refresh interval; `0` avoids redundant rows |
| `--block-size N` | `64` | Consecutive queries assigned per work block |
| `--limit N` | unlimited | Query at most N records |
| `--output FILE` | stdout | Write hits as TSV |
| `--timings FILE` | none | Write per-query counters and timings as TSV |
| `--stats-output FILE` | none | Write aggregate query statistics |

The hit table columns are:

```text
query  strand  sequence_id  contig  position  distance
```

## Verify against exhaustive edit distance

`verify` compares NavigaMer results with every indexed reference window. Use a
small `--limit` for large references.

```bash
./build/navigamer verify \
  --index reference.nvm \
  --reference reference.fa \
  --queries reads.fastq \
  --tolerance 3 \
  --limit 100 \
  --threads 16 \
  --with-cache \
  --anchor-refresh 0 \
  --output verification.tsv
```

## Convenience scripts

Build a small index:

```bash
scripts/run_ecoli_small.sh reference.fa [window_limit] [threads]
```

Run cached, uncached, and exhaustive-check queries:

```bash
scripts/benchmark_ecoli_small.sh \
  reference.fa build/ecoli_10000.nvm reads.fastq [threads] [verify_limit]
```
