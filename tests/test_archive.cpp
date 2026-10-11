#include "navigamer/archive.hpp"
#include "navigamer/builder.hpp"
#include "navigamer/center_selector.hpp"
#include "archive_storage.hpp"
#include "archive_prefetch.hpp"

#include <algorithm>
#include <cassert>
#include <bit>
#include <chrono>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {
using namespace navigamer;
using namespace navigamer::archive_detail;

std::vector<SequenceId> all_members(const ArchiveIndex& archive, NodeId node, std::size_t size = 65536) {
  ArchiveMemberReader reader(archive, node);
  std::vector<SequenceId> result;
  for (;;) {
    const auto block = reader.next(size);
    if (block.empty()) break;
    assert(block.size() <= size);
    result.insert(result.end(), block.begin(), block.end());
  }
  return result;
}
void compare(const ArchiveIndex& actual, const NavigaMerIndex& expected) {
  assert(actual.metadata().tolerance == expected.max_tolerance);
  assert(actual.metadata().nodes == expected.nodes.size());
  assert(actual.metadata().radii.size() == expected.layers.size());
  for (std::size_t i = 0; i < expected.layers.size(); ++i) assert(actual.metadata().radii[i] == expected.layers[i].selection_radius);
  for (NodeId id = 0; id < expected.nodes.size(); ++id) {
    const auto a = actual.node(id); const auto& e = expected.nodes[id];
    assert(a.center == e.center_sequence_id && a.members.count == e.member_count && a.child_count == e.child_count);
    assert(a.depth == (id == expected.root ? 0 : e.layer + 1));
    for (std::size_t c = 0; c < a.child_count; ++c) assert(a.first_child + c == expected.children[e.first_child + c]);
    const auto wanted = expected.world_members(id);
    assert(all_members(actual, id) == wanted);
    if (wanted.size() < 100) assert(all_members(actual, id, 1) == wanted);
    ArchiveMemberReader reader(actual, id);
    const auto skip = wanted.size() / 2;
    reader.skip(skip);
    std::vector<SequenceId> tail;
    for (;;) { const auto chunk = reader.next(37); if (chunk.empty()) break; tail.insert(tail.end(), chunk.begin(), chunk.end()); }
    assert(tail == std::vector<SequenceId>(wanted.begin() + skip, wanted.end()));
    ArchiveMemberReader empty(actual, id); empty.skip(wanted.size()); assert(empty.next(1).empty());
  }
}
template <typename Function> void rejected(Function function) {
  bool failed = false;
  try { function(); } catch (const std::exception&) { failed = true; }
  assert(failed);
}

void test_prefetch() {
  std::promise<void> second_started, release_second;
  auto started = second_started.get_future();
  auto release = release_second.get_future();
  unsigned calls = 0;
  MemberPrefetch batches([&]() -> std::vector<SequenceId> {
    switch (calls++) {
      case 0: return {1, 2};
      case 1: second_started.set_value(); release.wait(); return {3};
      default: return {};
    }
  });
  const auto first = batches.next();
  assert((first == std::vector<SequenceId>{1, 2}));
  // The next read starts while the consumer still owns the current batch.
  assert(started.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
  release_second.set_value();
  assert((batches.next() == std::vector<SequenceId>{3}));
  assert(batches.next().empty());
  assert(batches.next().empty());

  unsigned reads = 0;
  MemberPrefetch broken([&]() -> std::vector<SequenceId> {
    if (reads++ == 0) return {4};
    throw std::runtime_error("prefetch read failed");
  });
  assert((broken.next() == std::vector<SequenceId>{4}));
  try { (void)broken.next(); assert(false); }
  catch (const std::runtime_error& error) { assert(std::string_view(error.what()) == "prefetch read failed"); }

  const auto consumer = std::this_thread::get_id();
  MemberPrefetch single_batch([&]() -> std::vector<SequenceId> {
    assert(std::this_thread::get_id() == consumer);
    return {5};
  }, false);
  assert((single_batch.next() == std::vector<SequenceId>{5}));
}

void flip_byte(const std::filesystem::path& path, std::uint64_t offset) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekg(offset); const auto value = read<unsigned char>(file) ^ 1;
  file.seekp(offset); write(file, static_cast<unsigned char>(value));
}

void test_corruption(const std::filesystem::path& complete_path, const std::filesystem::path& run) {
  for (const auto kind : {"metadata", "node", "page", "payload"}) {
    const auto output = run / (std::string("corrupt-") + kind);
    std::filesystem::copy(complete_path, output, std::filesystem::copy_options::recursive);
    if (std::string_view(kind) == "metadata") {
      flip_byte(output / "archive.bin", 16);
      rejected([&] { (void)ArchiveIndex::open(output); });
    } else if (std::string_view(kind) == "node") {
      flip_byte(output / "nodes.bin", kNodeHeaderBytes + kNodeBytes);
      rejected([&] { (void)ArchiveIndex::open(output).node(1); });
    } else {
      const auto archive = ArchiveIndex::open(output);
      if (std::string_view(kind) == "page") flip_byte(output / "members.nma", archive.node(1).members.page + 32);
      else {
        std::ifstream file(output / "members.nma", std::ios::binary);
        file.seekg(64 + 56); const auto compressed = read<std::uint64_t>(file);
        flip_byte(output / "members.nma", 128 + compressed - 1);
      }
      rejected([&] { (void)all_members(archive, 1); });
    }
  }
}

void legacy_fixture(const std::filesystem::path& directory, const SequenceStore& reference,
                    const NavigaMerIndex& first, std::uint32_t radius = 75) {
  std::filesystem::create_directories(directory / "memberships" / "chunks");
  std::vector<SequenceId> centers;
  for (NodeId id = 1; id < first.nodes.size(); ++id) centers.push_back(first.nodes[id].center_sequence_id);
  const auto center_hash = hash({reinterpret_cast<const std::uint8_t*>(centers.data()), centers.size() * 8});
  {
    std::ofstream out(directory / "checkpoint.bin", std::ios::binary);
    for (const auto value : {std::uint64_t{0x31504b43353752ULL}, reference.fingerprint(), reference.size(),
                            reference.size(), std::uint64_t(centers.size()), center_hash}) write(out, value);
    for (const auto value : {radius, 5u, 150u, 1u}) write(out, value);
    write(out, 0.0);
    for (const auto center : centers) write(out, center);
  }
  for (std::uint64_t first_window = 0; first_window < reference.size(); first_window += 65536) {
    const auto count = std::min<std::uint64_t>(65536, reference.size() - first_window), words = (count + 63) / 64;
    std::vector<std::uint64_t> bits(centers.size() * words);
    std::vector<std::uint32_t> counts(centers.size());
    for (std::size_t c = 0; c < centers.size(); ++c) {
      for (const auto id : first.members(c + 1)) {
        if (id >= first_window && id < first_window + count) {
          const auto q = id - first_window; bits[c * words + q / 64] |= std::uint64_t{1} << (q % 64); ++counts[c];
        }
      }
    }
    const auto raw = std::span(reinterpret_cast<const std::uint8_t*>(bits.data()), bits.size() * 8);
    auto* compressor = ZSTD_createCCtx(); assert(compressor);
    ZSTD_CCtx_setParameter(compressor, ZSTD_c_compressionLevel, 1);
    ZSTD_CCtx_setParameter(compressor, ZSTD_c_checksumFlag, 1);
    std::vector<std::uint8_t> compressed(ZSTD_compressBound(raw.size()));
    const auto compressed_size = ZSTD_compress2(compressor, compressed.data(), compressed.size(), raw.data(), raw.size());
    assert(!ZSTD_isError(compressed_size)); ZSTD_freeCCtx(compressor);
    std::ostringstream name; name << std::setw(12) << std::setfill('0') << first_window << ".r75z";
    std::ofstream out(directory / "memberships" / "chunks" / name.str(), std::ios::binary);
    for (const auto value : {std::uint64_t{0x314b4e4843353752ULL}, std::uint64_t{1}, reference.fingerprint(), center_hash,
        reference.size(), std::uint64_t(centers.size()), first_window, count, std::uint64_t(radius), std::uint64_t{5},
        std::uint64_t{150}, std::uint64_t{1}, std::uint64_t(std::accumulate(counts.begin(), counts.end(), std::uint64_t{0})),
        std::uint64_t(raw.size()), std::uint64_t(compressed_size), std::uint64_t{0}}) write(out, value);
    for (const auto value : counts) write(out, value);
    out.write(reinterpret_cast<const char*>(compressed.data()), compressed_size);
  }
}

void test_selector(const SequenceStore& reference) {
  std::vector<SequenceId> ids(reference.size()); std::iota(ids.begin(), ids.end(), 0);
  BuildConfig config; config.center_radii = {75};
  const auto index = IndexBuilder::build(reference, config);
  std::vector<SequenceId> expected;
  for (NodeId node = 1; node < index.nodes.size(); ++node) expected.push_back(index.nodes[node].center_sequence_id);
  for (const auto threads : {1u, 4u}) {
    CenterSelector selector(reference, 75, threads);
    for (std::size_t first = 0; first < ids.size(); first += 113) selector.consume(std::span(ids).subspan(first, std::min<std::size_t>(113, ids.size() - first)));
    assert(selector.centers() == expected);
    rejected([&] { selector.consume(std::span(ids).first(1)); });
  }
}

void test_varied(const std::filesystem::path& directory, const std::filesystem::path& run) {
  std::uint32_t state = 17;
  auto random = [&] { state = state * 1664525u + 1013904223u; return state; };
  const std::string dna = "ACGT";
  std::string seed;
  for (int i = 0; i < 150; ++i) seed.push_back(dna[(random() >> 24) % 4]);
  seed[70] = 'N';
  const auto fasta = directory / "varied.fa";
  { std::ofstream out(fasta);
    out << ">base\n" << seed << '\n';
    for (int q = 0; q < 256; ++q) {
      auto sequence = seed; std::vector<std::size_t> positions(150);
      std::iota(positions.begin(), positions.end(), 0);
      for (std::size_t i = 0; i < 48; ++i) {
        const auto chosen = i + random() % (150 - i); std::swap(positions[i], positions[chosen]);
        const auto p = positions[i];
        do { sequence[p] = dna[(random() >> 24) % 4]; } while (sequence[p] == seed[p]);
      }
      out << ">" << q << '\n' << sequence << '\n';
    }
  }
  const auto reference = SequenceStore::from_fasta(fasta, 150);
  BuildConfig base; base.center_radii = {75};
  const auto input = run / "legacy-varied";
  legacy_fixture(input, reference, IndexBuilder::build(reference, base));
  auto full = base; full.center_radii = {75, 60, 35};
  const auto expected = IndexBuilder::build(reference, full);
  assert(expected.nodes.size() > 10);
  for (const auto threads : {1u, 4u}) {
    ArchiveBuildConfig config; config.radii = {60, 35}; config.threads = threads; config.working_memory_mib = 16;
    const auto output = run / ("varied-" + std::to_string(threads));
    assert(ArchiveBuilder::extend(reference, input, output, config));
    compare(ArchiveIndex::open(output), expected);
  }
}

void test_builds(const std::filesystem::path& input, const std::filesystem::path& run,
                 const SequenceStore& reference, bool large) {
  BuildConfig base; base.center_radii = {75};
  const auto first = IndexBuilder::build(reference, base);
  legacy_fixture(input, reference, first);
  test_selector(reference);
  BuildConfig full; full.center_radii = {75, 60, 35};
  const auto expected = IndexBuilder::build(reference, full);
  ArchiveBuildConfig config; config.radii = {60, 35}; config.threads = 4;
  config.working_memory_mib = 16; config.checkpoint_members = 1;
  const auto complete_path = run / (large ? "large" : "small");
  assert(ArchiveBuilder::extend(reference, input, complete_path, config));
  const auto complete = ArchiveIndex::open(complete_path);
  compare(complete, expected);
  const auto before = std::filesystem::last_write_time(complete_path / "members.nma");
  rejected([&] { ArchiveBuilder::extend(reference, input, complete_path, config); });
  config.resume = true;
  assert(ArchiveBuilder::extend(reference, input, complete_path, config));
  assert(std::filesystem::last_write_time(complete_path / "members.nma") == before);
  config.resume = false;

  for (const auto stop_at : (large ? std::vector<int>{1, 2, 4} : std::vector<int>{1, 2, 3, 4, 5, 7, 9})) {
    auto interrupted = config;
    int calls = 0; interrupted.stop_requested = [&] { return ++calls == stop_at; };
    const auto output = run / ((large ? "large-stop-" : "small-stop-") + std::to_string(stop_at));
    assert(!ArchiveBuilder::extend(reference, input, output, interrupted));
    rejected([&] { (void)ArchiveIndex::open(output); });
    // Simulate a crash after uncommitted data/node writes. Only these owned tails
    // may be discarded; the committed prefix and original input stay intact.
    { std::ofstream out(output / "members.nma", std::ios::binary | std::ios::app); out << "uncommitted-tail"; }
    { std::ofstream out(output / "nodes.bin", std::ios::binary | std::ios::app); out << "uncommitted-node"; }
    interrupted.stop_requested = {}; interrupted.resume = true; interrupted.threads = 1;
    assert(ArchiveBuilder::extend(reference, input, output, interrupted));
    compare(ArchiveIndex::open(output), expected);
    auto wrong = interrupted; wrong.radii = {60};
    rejected([&] { ArchiveBuilder::extend(reference, input, output, wrong); });
  }
  if (large) return;

  test_corruption(complete_path, run);
  const int lock = ::open((complete_path / ".archive.lock").c_str(), O_RDWR);
  assert(lock >= 0 && ::flock(lock, LOCK_EX | LOCK_NB) == 0);
  auto locked = config; locked.resume = true;
  rejected([&] { ArchiveBuilder::extend(reference, input, complete_path, locked); });
  ::close(lock);

  for (const auto alias : {false, true}) {
    auto interrupted = config; interrupted.stop_requested = [] { return true; };
    const auto output = run / (alias ? "aliased-mutable" : "corrupt-checkpoint");
    assert(!ArchiveBuilder::extend(reference, input, output, interrupted));
    if (alias) std::filesystem::create_hard_link(output / "members.nma", run / "mutable-alias.nma");
    else flip_byte(output / "job.bin", 16);
    const auto data_size = std::filesystem::file_size(output / "members.nma");
    interrupted.resume = true; interrupted.stop_requested = {};
    rejected([&] { ArchiveBuilder::extend(reference, input, output, interrupted); });
    assert(std::filesystem::file_size(output / "members.nma") == data_size);
  }

  ArchiveBuildConfig next; next.radii = {15}; next.threads = 3; next.working_memory_mib = 16;
  const auto deeper_path = run / "deeper";
  // A complete input is readable even when its node/data files are read-only.
  const auto old_nodes_permissions = std::filesystem::status(complete_path / "nodes.bin").permissions();
  const auto old_members_permissions = std::filesystem::status(complete_path / "members.nma").permissions();
  std::filesystem::permissions(complete_path / "nodes.bin", std::filesystem::perms::owner_read);
  std::filesystem::permissions(complete_path / "members.nma", std::filesystem::perms::owner_read);
  assert(ArchiveBuilder::extend(reference, complete_path, deeper_path, next));
  std::filesystem::permissions(complete_path / "nodes.bin", old_nodes_permissions);
  std::filesystem::permissions(complete_path / "members.nma", old_members_permissions);
  full.center_radii = {75, 60, 35, 15};
  compare(ArchiveIndex::open(deeper_path), IndexBuilder::build(reference, full));
  assert(std::filesystem::equivalent(complete_path / "members.nma", deeper_path / "source-0.nma"));
  assert(std::filesystem::last_write_time(complete_path / "members.nma") == before);
  std::filesystem::rename(complete_path, run / "moved-input");
  // Output owns hard links, so moving the previous archive does not break it.
  compare(ArchiveIndex::open(deeper_path), IndexBuilder::build(reference, full));
  std::filesystem::rename(run / "moved-input", complete_path);

  auto subtree = config; subtree.world = 1;
  const auto selected_path = run / "legacy-subtree";
  assert(ArchiveBuilder::extend(reference, input, selected_path, subtree));
  auto replacement = base; replacement.center_radii = {60, 35};
  const auto selected_expected = IndexBuilder::rebuild_world(reference, first, 2, replacement);
  compare(ArchiveIndex::open(selected_path), selected_expected);
  assert(ArchiveIndex::open(selected_path).node(0).center != all_members(ArchiveIndex::open(selected_path), 0).front());

  next.radii = {50}; next.world = 1;
  const auto rebuilt_path = run / "rebuilt-internal";
  assert(ArchiveBuilder::extend(reference, complete_path, rebuilt_path, next));
  replacement.center_radii = {50};
  compare(ArchiveIndex::open(rebuilt_path), IndexBuilder::rebuild_world(reference, expected, 1, replacement));
  next.world.reset(); next.radii = {35};
  assert(ArchiveBuilder::extend(reference, rebuilt_path, run / "subtree-deeper", next));
  replacement.center_radii = {50, 35};
  compare(ArchiveIndex::open(run / "subtree-deeper"), IndexBuilder::rebuild_world(reference, expected, 1, replacement));

  auto bad = config; bad.world = 99999;
  rejected([&] { ArchiveBuilder::extend(reference, input, run / "bad-world", bad); });
  bad = config; bad.working_memory_mib = 0;
  rejected([&] { ArchiveBuilder::extend(reference, input, run / "bad-memory", bad); });
  bad = config; bad.threads = 0;
  rejected([&] { ArchiveBuilder::extend(reference, input, run / "bad-threads", bad); });
  bad = config; bad.radii.assign(255, 60);
  rejected([&] { ArchiveBuilder::extend(reference, input, run / "bad-depth", bad); });
  assert(!std::filesystem::exists(run / "bad-depth"));
  rejected([&] { ArchiveBuilder::extend(reference, input, input, config); });
  rejected([&] { ArchiveMemberReader reader(complete, 99999); });
  rejected([&] { ArchiveMemberReader reader(complete, 0); (void)reader.next(0); });
  rejected([&] { ArchiveMemberReader reader(complete, 1); reader.skip(complete.node(1).members.count + 1); });
  const auto wrong_reference = SequenceStore::from_sequences({std::string(150, 'X')});
  rejected([&] { ArchiveBuilder::extend(wrong_reference, input, run / "bad-reference", config); });
}

void test_changed_tolerance(const std::filesystem::path& input, const std::filesystem::path& run,
                            const SequenceStore& reference) {
  BuildConfig original; original.center_radii = {90};
  auto rebuilt = original; rebuilt.max_tolerance = 3; rebuilt.center_radii.assign(6, 90);
  const auto first = IndexBuilder::build(reference, original);
  const auto expected = IndexBuilder::build(reference, rebuilt);
  // Occurrence 1 is 70 edits from the A center and 80 from the C center:
  // T=5 includes both, whereas T=3 excludes it from the C world.
  const auto old_c = first.world_members(2), new_c = expected.world_members(2);
  assert(std::binary_search(old_c.begin(), old_c.end(), SequenceId{1}));
  assert(!std::binary_search(new_c.begin(), new_c.end(), SequenceId{1}));
  for (const auto threads : {1u, 4u}) {
    ArchiveBuildConfig config; config.radii.assign(5, 90); config.tolerance = 3;
    config.threads = threads; config.working_memory_mib = 16; config.checkpoint_members = 1;
    const auto output = run / ("t3-r90x6-" + std::to_string(threads));
    assert(ArchiveBuilder::extend(reference, input, output, config));
    const auto actual = ArchiveIndex::open(output); compare(actual, expected);
    const auto root = actual.node(0);
    assert(root.child_count == first.nodes[0].child_count);
    for (std::size_t c = 0; c < root.child_count; ++c) {
      assert(actual.node(root.first_child + c).center == first.nodes[c + 1].center_sequence_id);
    }
    for (const auto stop_at : {1, 3}) {
      auto interrupted = config; int calls = 0;
      interrupted.stop_requested = [&] { return ++calls == stop_at; };
      const auto paused = run / ("t3-stop-" + std::to_string(threads) + "-" + std::to_string(stop_at));
      assert(!ArchiveBuilder::extend(reference, input, paused, interrupted));
      interrupted.resume = true; interrupted.stop_requested = {};
      const auto bytes_before = std::filesystem::file_size(paused / "members.nma");
      auto wrong = interrupted; wrong.tolerance = 4;
      rejected([&] { ArchiveBuilder::extend(reference, input, paused, wrong); });
      wrong.tolerance.reset();
      rejected([&] { ArchiveBuilder::extend(reference, input, paused, wrong); });
      assert(std::filesystem::file_size(paused / "members.nma") == bytes_before);
      assert(ArchiveBuilder::extend(reference, input, paused, interrupted));
      compare(ArchiveIndex::open(paused), expected);
    }
    auto invalid = config; invalid.tolerance = 2;
    rejected([&] { ArchiveBuilder::extend(reference, output, run / "t-mismatch-new-archive", invalid); });
    assert(!std::filesystem::exists(run / "t-mismatch-new-archive"));
    invalid = config; invalid.world = 0;
    rejected([&] { ArchiveBuilder::extend(reference, input, run / "t-mismatch-subtree", invalid); });
    assert(!std::filesystem::exists(run / "t-mismatch-subtree"));
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--check-legacy") {
      const LegacyCenters centers(argv[2]);
      for (const auto first : {std::uint64_t{0}, (centers.windows - 1) / 65536 * 65536}) {
        const auto chunk = read_legacy_chunk(argv[2], centers, first, 128 * 1024 * 1024);
        std::cout << "validated_legacy_chunk_first=" << first << " windows=" << chunk.count << '\n';
      }
      std::cout << "legacy_windows=" << centers.windows << " centers=" << centers.ids.size() << '\n';
      return 0;
    }
    if (argc != 2) throw std::invalid_argument("expected test directory or --check-legacy DIRECTORY");
    const std::filesystem::path directory(argv[1]); std::filesystem::create_directories(directory);
    const auto run = directory / ("run-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(run);
    test_prefetch();
    std::vector<std::string> sequences{std::string(150, 'A'), std::string(80, 'A') + std::string(70, 'C'),
        std::string(150, 'C'), std::string(150, 'G'), std::string(150, 'T'), std::string(150, 'N'),
        std::string(149, 'A') + "C", std::string(149, 'N') + "A", std::string(150, 'A')};
    { std::ofstream out(directory / "small.fa"); for (std::size_t i = 0; i < sequences.size(); ++i) out << ">" << i << '\n' << sequences[i] << '\n'; }
    const auto small = SequenceStore::from_fasta(directory / "small.fa", 150);
    test_builds(directory / "legacy-small", run, small, false);
    {
      BuildConfig first; first.center_radii = {90};
      const auto input = directory / "legacy-r90";
      legacy_fixture(input, small, IndexBuilder::build(small, first), 90);
      ArchiveBuildConfig next; next.radii = {85}; next.working_memory_mib = 16;
      const auto output = run / "r90-r85";
      assert(ArchiveBuilder::extend(small, input, output, next));
      first.center_radii.push_back(85);
      compare(ArchiveIndex::open(output), IndexBuilder::build(small, first));
      test_changed_tolerance(input, run, small);
    }
    test_varied(directory, run);
    { std::ofstream out(directory / "large.fa"); out << ">one\n" << std::string(65553 + 149, 'A') << "\n>two\n" << std::string(150, 'C') << "\n>three\n" << std::string(150, 'G') << '\n'; }
    const auto large = SequenceStore::from_fasta(directory / "large.fa", 150);
    test_builds(directory / "legacy-large", run, large, true);
    std::cout << "archive roundtrip, continuation, subtree, checkpoint and multi-chunk tests passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "archive test error: " << error.what() << '\n'; return 1; }
}
