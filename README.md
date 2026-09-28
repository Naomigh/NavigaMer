# NavigaMer

C++20 genome index with configurable world layers.

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

Defaults: window 150, stride 1, T 5, three layers with radii `60,35,15`,
and beacon ratio `0.1` at every layer. Threads default to the hardware thread count.

`--layers N` sets the number of world layers (1 through 255, excluding the root).
`--radii R1,...,RN` supplies one selection radius per layer, from the root's
children to the leaves. If `--layers` is omitted, the number of radii determines
the layer count. If both are supplied, their counts must match. A non-default
layer count requires an explicit radius list; no radii are interpolated.
Reports use zero-based layer indices. The last configured layer is the leaf
layer; construction and query routing stop there.

For a real one-layer index with radius 75:

```bash
build/navigamer build \
  --reference reference.fa --index reference-layer1.nvm \
  --window 150 --stride 1 --T 5 --layers 1 --radii 75 --threads 32
```

For four layers, use `--layers 4 --radii 75,50,30,15`. The C++ API derives
the layer count from `BuildConfig::center_radii.size()`.

## Continue or rebuild existing worlds

Append new layers below **every current leaf**, without reselecting existing
centers or changing existing membership scopes:

```bash
build/navigamer extend \
  --reference reference.fa --input reference-layer1.nvm --index deeper.nvm \
  --radii 60,35,15 --threads 32 --classifier adjacent
```

Alternatively, choose an existing world by its **node ID**, keep its center and
exact member set, and rebuild its descendants from scratch:

```bash
build/navigamer rebuild-world \
  --reference reference.fa --input deeper.nvm --world 42 --index world42.nvm \
  --radii 50,30,15 --threads 32 --classifier adjacent
```

The selected world becomes the new root, not an additional reselected layer.
Its old descendants are ignored; the new radii describe only layers below it.
Greedy child selection still begins with the first member in occurrence-ID
order, which need not be the retained root center. Node IDs are not sequence
IDs: list selectable nodes, parents, centers and counts with
`build/navigamer inspect --index deeper.nvm --worlds`. In the C++ API use
`index.nodes[node].center_sequence_id` to distinguish them.
`--world 0` on an ordinary index rebuilds the entire reference hierarchy.

Both commands require a new output path and explicit `--radii`. The input file
is unchanged; an existing output (including the input itself) is rejected.
Window length, stride, reference limit and T are inherited from the input;
changing T is rejected. Beacon settings describe the **new** layers only.
The total depth after `extend` must not exceed 255 layers. These operations
require a reference-backed v5 `.nvm` with complete leaf memberships, together
with its matching reference FASTA. Legacy v3/v4 owned-sequence indexes must
first be rebuilt as v5. A centers-only selection checkpoint is not sufficient.

The C++ APIs are `IndexBuilder::extend` and `IndexBuilder::rebuild_world`.
Members of an internal world are recovered by merging its sorted descendant
leaves and deduplicating occurrence IDs, not identical sequence strings. The
merge does not first copy all overlapping relationships. Results retain global
reference IDs and the original reference fingerprint. A subtree index queries
only its retained members; `verify` compares against the same restricted scope.
`inspect` reports `scope`, `scope_members` and the retained root center ID.
The binary format remains v5. A continued build has canonical BFS node/storage
order, matching a one-shot build byte for byte when radii, T and per-layer beacon
settings match. The input's previous beacon settings do not need to be supplied
again; they are retained with their existing tables.

These commands still use in-memory `.nvm` membership storage. They do **not**
directly consume the NFS R75 compressed archive or implement disk-streaming
continuation/checkpoint recovery. Do not convert the full R75 archive to dense
membership vectors to use them: overlap expansion is prohibitive. Its archive
adapter and one-pass, independently readable parent blocks require separate
storage work; the existing R75 results are not modified by these commands.

## Construction options

`--beacon-ratios B1,...,BN` gives one fraction per configured layer. In the
default three-layer hierarchy these are large centers at the root, middle
centers inside each large world, and small centers inside each middle world.
Each parent scope selects `ceil(ratio * center_count)` beacons and owns
its distance table. Fractions range from 0 to 1. Zero means exact all-center
classification at that layer without a beacon table. `--limit N` restricts
construction to the first N reference windows.

Alternatively, `--beacon-cover-radii C1,...,CN` selects beacons by greedy metric
coverage of each parent's child centers instead of by index spacing. For each
layer, the first child center is selected, then a center is added only when its
edit distance to every selected beacon exceeds that layer's cover radius. Every
child center is therefore within the radius of a beacon, and distinct beacons
are farther apart than that radius. The number of beacons is data-dependent.
This option is mutually exclusive with `--beacon-ratios`; the existing ratio
strategy remains the default. Both beacon options require exactly one value
per configured layer when supplied. Omitted ratios default to 0.1 per layer
(an empty `BuildConfig::beacon_ratios` has the same meaning).
Cover radii refer to successive parent scopes, just as the ratio entries do.
The root has no finite parent radius, so its cover radius
must be chosen explicitly. For example, `--beacon-cover-radii 78,48,25` uses
approximately intermediate radii for the lower layers of `--radii 60,35,15`.
Coverage is over child centers, not over every member sequence at that radius.
The build report includes beacon selection distance calls and selected counts;
beacon-table time includes selection. Stored beacon tables use the same index
format and can be queried without the selection parameters.

Centers belong only to their own world. Other members use the `x + 2T`
assignment rule. To retain exact nearest-child query coverage with exclusive
centers, use each selection radius greater than `2T`. Query tolerance must
not exceed construction T; the CLI rejects a larger query tolerance. The radius
relationship remains a caller precondition.

Child/anchor centers use one contiguous fixed-stride 2-bit array, with
`A=00, T=01, G=10, C=11` (four bases per byte, low bits first). Each parent's
children occupy consecutive slots. Non-ATGC symbols such as `N` are preserved
literally in a sparse side table; they are not treated as wildcards. The side
table adds overhead for ambiguity-heavy data.

Scalar edit-distance calls pass through one router. Equal-length,
uppercase ATGC pairs of at most 256 bases use the vendored AVX2 LEAP backend for
bounded thresholds 1 through 15. Exact distances, larger thresholds, unequal
lengths, ambiguous bases, unsupported CPUs, and builds configured with
`-DNAVIGAMER_ENABLE_LEAP=OFF` use Edlib. LEAP engines are reused per thread and
threshold; its SHD filter is disabled. Each sequence reuses its input preparation
across the centers it visits in a world: LEAP bit-planes and a lazily initialized
Edlib DNA query profile. Non-ATGC inputs retain the general Edlib path; k=0 uses
an equality check. On supported CPUs, short centers also have a contiguous,
32-byte-aligned runtime bit-plane array (64 bytes per center), so LEAP comparisons
do not decode or re-encode centers. Edlib still decodes one center into reusable
worker scratch. Runtime planes are rebuilt on load, counted in `center_bytes`,
and are not serialized. New leaf memberships store only sequence IDs, not copies
of their bases. Queries resolve IDs to zero-copy views into the shared reference
contigs; adjacent windows share their overlapping bases. Packed centers and
their prefetch layout are unchanged. Reference data is external to the index:
keep the matching FASTA and supply it via `--reference` as before. C++ callers
must keep both the index and reference alive while the query engine is in use.

Index format version 5 stores a reference fingerprint instead of leaf sequence
copies. The reader still accepts v3/v4 indexes with owned leaf sequences; these
retain their legacy query behavior and save as v4. Rebuild from the reference
to obtain v5 storage. Older readers cannot read v5. At reference load time a
64-bit FNV-1a fingerprint is computed over normalized used contig prefixes,
contig names/boundaries and window parameters; a mismatch rejects v5 queries.
Unused tails beyond `--limit` are excluded. This is an accidental-mismatch check,
not cryptographic authentication or comprehensive corrupt-file validation.

Membership overlap is unchanged: an ID is still stored in every applicable
leaf world. IDs/buckets are not compressed or streamed; there is no checkpoint
recovery, BK directory or resource budget check. Removing sequence copies does
not by itself make full-genome member construction fit in memory or on disk.

Construction packs a parent's selected centers once, then streams contiguous
member-ID ranges per worker with sequence prefetching. Workers classify against
that fixed center set and append directly to child buckets. Buckets are merged
in original member order before processing the next layer. Beacon pruning and
the existing exclusive-center / `x + 2T` assignment rules are unchanged.
Classification tightens its nearest-distance upper bound whenever a closer
center is found, including without anchors; remaining comparisons use U + 2T.
Classification reuses each block of 32 centers across up to 8 queries before
moving to the next block, and emits buckets in original query order. Distance
scratch is up to 8 times the child count per worker; this fixed tile is not a
hardware-specific cache budget or a residency guarantee.
Center selection reuses the previous covering center. For windows from the same
contig separated by `s` bases, their edit distance is at most `2s`; a known
query-to-center upper bound `U` therefore certifies coverage whenever
`U + 2s <= radius`. Otherwise the previous center is tried with bounded exact
distance, followed by the remaining centers. This changes the order of coverage
checks, not which centers the input-order greedy algorithm selects. Ambiguous
bases retain their literal edit-distance semantics; distinct contigs, even with
identical names, do not share a positional bound.

With multiple threads and at least 4096 members, selection uses a persistent
worker pool over batches of 4096 windows. Workers test an immutable snapshot of
the centers. A serial merge visits uncovered windows in input order and checks
them against centers added earlier in that batch before adding a new center.
This preserves the serial greedy center IDs and order. Speculative checks can
increase distance-call counts, and thread scaling depends on the workload.
Small scopes use the serial path; a radius at least the window length needs
only the first center and no selection distance calls.

For radii above 15, selection scans complete groups of four centers with an
AVX2 batch Myers kernel adapted from Minghao's AVX-512 implementation. Center
equality masks are prepared incrementally and reused across windows; each
worker owns reusable scratch. Distances are exact, including literal ambiguity
characters and multiword carry/padding correction. Hint checks, partial groups,
and small-radius comparisons retain the existing scalar router. The AVX2
function is CPU-dispatched at runtime without requiring `-march=native`;
unsupported CPUs/compilers and `-DNAVIGAMER_ENABLE_BATCH_MYERS=OFF` retain the
previous scalar paths. Classification also batches complete, uncomputed groups
of four when every lane passes beacon pruning and the bound is above 15 (or
unbounded). It retains the same exclusive-center and nearest-distance + 2T
membership rules. This does not add checkpointing or change index storage.

`--classifier adjacent` selects the exact streaming classifier ported from the
R75 production driver. It reuses query-to-center distance intervals, resolves
the nearest distance exactly, then evaluates only intervals straddling `x+2T`.
Unresolved, noncontiguous centers use gathered four-lane SIMD profiles on AVX2,
with exact Edlib fallback on unsupported CPUs or disabled SIMD builds. The
driver's eight-lane AVX-512 binary ABI is not copied into this four-lane library.
Bounds use the actual same-contig coordinate gap (`2s`), supporting strided and
filtered parent memberships. Contig boundaries reset the cache; equal windows
reuse ordinary memberships. Center exclusivity is checked by occurrence ID
before cache reuse, including when `radius <= 2T`. Each worker has its own cache.
Beacon tables are still built for query routing, but are not consulted by this
classifier. `classification_repeated` counts identical-window reuse;
`pruned_center_pairs` includes interval/cache skips in this mode. The default
`--classifier beacon` retains the existing tiled beacon-pruning classifier.
The two classifiers produce identical memberships, but neither is guaranteed
faster on every dataset. Both work with `build`, `extend` and `rebuild-world`.

Lower-layer parents are independent build tasks. Results are merged in parent
order. Threads are divided between outer parent tasks and inner selection and
classification without oversubscription.

The build report gives center-selection, beacon-table, and classification time
per layer, together with edit-distance call counts, pruning ratio, save time, and peak
resident memory. `center_bound_accepts` counts coverage checks avoided by the
positional bound; `center_hint_hits` counts successful exact checks of a reused
center. These count checks in the snapshot and merge phases, not unique windows.
`center_batch_groups` counts four-lane calls; all four evaluated lanes contribute
to `center_edit_calls`, including any redundant hint or post-hit comparisons.
`candidate_batch_groups` reports four-lane classification calls; their four
distances contribute to `candidate_distance_calls`.
Stage times sum parent durations and may overlap across workers;
`build_seconds` is the end-to-end build wall time.

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
`--no-cache`, `--prefetch`, `--no-output`, `--min-queries-per-thread N` (default 1), and
`--limit N`. Routing uses the layer count and beacon tables stored in the index;
no layer options are needed at query time. Leaf sequences are verified directly.
Routing seeds its best distance from a valid previous-child hint or an anchor
that it already needs to evaluate. Scan mode skips anchor-table work; beacon
routing filters impossible candidates before sorting and stops once the sorted
remainder cannot improve the best distance/ID pair.

CLI query and benchmark runs route the entire query collection world by world.
All queries finish at the root before any child is processed. Subsequent layers
are visited breadth first, with independent worlds processed concurrently.
The pool size P is `min(threads, max(1, total_queries / min_queries_per_thread))`.
Worlds with at most `ceil(total_queries / P)` queries stay as whole tasks, so one
worker streams their entire bucket before taking another task. Larger worlds
split into balanced, contiguous ranges, capped at
`min(P, max(1, world_queries / min_queries_per_thread))` ranges. Workers claim
tasks from a shared queue and synchronize only at layer boundaries. Results are
then bucketed in stable input order before the next layer starts. The same
whole-world/range policy applies to leaf verification. This is a workload-size
heuristic, not cache-capacity-based grouping of worlds.
With `--both-strands`, each strand counts as a separate routing work item.
`--block-size` remains an alias for `--min-queries-per-thread`; it no longer sets
a fixed task chunk or divides the collection into independent traversals. Do not
specify both options together. The threshold is a workload-dependent heuristic,
not a cache capacity or guaranteed optimal worker count.
`--prefetch` hints the whole packed center set on parent entry and upcoming
sequence data while streaming. Child buckets retain indices; sequence bases are
not copied into each bucket and may be scattered after bucketing. Hardware
decides cache placement and eviction; prefetch is optional and needs measurement.
`--no-cache` disables the previous-child routing hint, not CPU caching.
The single-sequence C++ API remains available. `query_batch` accepts the complete
collection and a `BatchQueryConfig` for workers, minimum queries per thread and hit retention.
Bucket indices and routing assignments require memory proportional to the full
query collection. Inputs are still read into memory before querying; this is
streaming through CPU caches, not streaming the input file from disk.
`--no-output` and benchmark runs discard each query's hits after verification.

Reported query latency is full collection completion time for each query in a run
(`latency_scope=collection_completion`), not single-query service time.
Throughput is total queries divided by wall time. `route_worlds=...` reports
one count per routing depth; the root count is one per full run.
Build timing reports include `layer_index`; three-layer builds retain the
`large,middle,small` labels and other layer counts use numeric labels.
Each benchmark repeat is a separate full traversal; warmup is excluded from stats.

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
