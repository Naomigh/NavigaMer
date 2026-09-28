#include "navigamer/builder.hpp"
#include "navigamer/batch_distance.hpp"
#include "navigamer/edit_distance.hpp"
#include "navigamer/query.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <set>
#include <tuple>
#include <vector>

namespace {

int dynamic_programming_distance(std::string_view lhs, std::string_view rhs) {
  std::vector<int> previous(rhs.size() + 1), current(rhs.size() + 1);
  for (std::size_t j = 0; j <= rhs.size(); ++j) previous[j] = j;
  for (std::size_t i = 1; i <= lhs.size(); ++i) {
    current[0] = i;
    for (std::size_t j = 1; j <= rhs.size(); ++j) {
      current[j] = std::min({previous[j] + 1, current[j - 1] + 1,
                            previous[j - 1] + (lhs[i - 1] != rhs[j - 1])});
    }
    previous.swap(current);
  }
  return previous.back();
}

int distance(std::string_view lhs, std::string_view rhs) {
  return navigamer::EditDistance::exact(lhs, rhs);
}

void test_batch_distance() {
  std::mt19937 random(7219);
  for (const auto length : {0u, 1u, 31u, 32u, 63u, 64u, 65u, 127u, 128u, 129u, 150u, 255u, 256u, 257u}) {
    navigamer::BatchDistance batch(length);
    navigamer::BatchDistance::Workspace workspace;
    std::vector<std::string> centers;
    for (std::size_t count = 1; count <= 13; ++count) {
      std::string center(length, 'A');
      for (auto& base : center) base = "ACGTNRYacgt"[random() % 11];
      // All byte values are literal symbols, including zero and signed chars.
      if (count == 9) for (auto& base : center) base = static_cast<char>(random() % 256);
      batch.append(center);
      centers.push_back(center);
      assert(batch.size() == count);
      for (const auto target_length : {0u, length, length + 7}) {
        std::string target(target_length, 'A');
        for (auto& base : target) base = "ACGTNZ"[random() % 6];
        for (std::size_t group = 0; group * batch.kLanes < count; ++group) {
          const auto actual = batch.exact_group(target, group, workspace);
          for (std::size_t lane = 0; lane < batch.kLanes && group * batch.kLanes + lane < count; ++lane) {
            assert(actual[lane] == dynamic_programming_distance(centers[group * batch.kLanes + lane], target));
          }
        }
      }
      const auto self = batch.exact_group(center, (count - 1) / batch.kLanes, workspace);
      assert(self[(count - 1) % batch.kLanes] == 0);
    }
    bool rejected = false;
    try { batch.append(std::string(length + 1, 'A')); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { (void)batch.exact_group("A", 4, workspace); }
    catch (const std::out_of_range&) { rejected = true; }
    assert(rejected);
  }
  navigamer::BatchDistance empty(150);
  navigamer::BatchDistance::Workspace workspace;
  bool rejected = false;
  try { (void)empty.exact_group("A", 0, workspace); }
  catch (const std::out_of_range&) { rejected = true; }
  assert(rejected);
}

std::vector<std::string> strings(std::string_view alphabet, std::size_t length) {
  std::vector<std::string> result{std::string{}};
  for (std::size_t i = 0; i < length; ++i) {
    std::vector<std::string> next;
    for (const auto& prefix : result) {
      for (const char base : alphabet) next.push_back(prefix + base);
    }
    result = std::move(next);
  }
  return result;
}

std::set<navigamer::SequenceId> descendants(const navigamer::NavigaMerIndex& index,
                                           navigamer::NodeId id) {
  if (index.is_leaf(id)) {
    const auto members = index.members(id);
    return {members.begin(), members.end()};
  }
  std::set<navigamer::SequenceId> result;
  const auto& world = index.nodes[id];
  for (std::uint64_t i = 0; i < world.child_count; ++i) {
    const auto values = descendants(index, index.children[world.first_child + i]);
    result.insert(values.begin(), values.end());
  }
  return result;
}

void check_hierarchy(const navigamer::NavigaMerIndex& index,
                     const navigamer::SequenceStore& reference,
                     const navigamer::BuildConfig& config, navigamer::NodeId parent_id,
                     const std::vector<navigamer::SequenceId>& members, std::size_t depth) {
  std::vector<navigamer::SequenceId> centers;
  for (const auto id : members) {
    bool covered = false;
    for (const auto center : centers) {
      if (distance(reference.sequence(id), reference.sequence(center)) <=
          static_cast<int>(config.center_radii[depth])) covered = true;
    }
    if (!covered) centers.push_back(id);
  }
  const auto& parent = index.nodes[parent_id];
  assert(parent.child_count == centers.size());
  for (std::size_t c = 0; c < centers.size(); ++c) {
    const auto child_id = index.children[parent.first_child + c];
    const auto& child = index.nodes[child_id];
    assert(child.center_sequence_id == centers[c]);
    assert(index.center_sequences.decode(child_id) == reference.sequence(centers[c]));
    assert(child_id == index.children[parent.first_child] + c);
    std::vector<navigamer::SequenceId> expected;
    for (const auto member : members) {
      if (std::find(centers.begin(), centers.end(), member) != centers.end()) {
        if (member == centers[c]) expected.push_back(member);
        continue;
      }
      int x = std::numeric_limits<int>::max();
      for (const auto center : centers) {
        x = std::min(x, distance(reference.sequence(member), reference.sequence(center)));
      }
      if (distance(reference.sequence(member), reference.sequence(centers[c])) <=
          x + 2 * static_cast<int>(config.max_tolerance)) expected.push_back(member);
    }
    const std::set<navigamer::SequenceId> expected_set(expected.begin(), expected.end());
    assert(descendants(index, child_id) == expected_set);
    assert(child.member_count == expected.size());
    if (depth + 1 == config.center_radii.size()) {
      const auto actual = index.members(child_id);
      assert(std::vector<navigamer::SequenceId>(actual.begin(), actual.end()) == expected);
      for (std::size_t i = 0; i < expected.size(); ++i) {
        assert(index.member_sequence(child.first_member + i, reference) == reference.sequence(expected[i]));
      }
      assert(child.beacon_count == 0);
    } else {
      assert(child.first_member == 0);
      check_hierarchy(index, reference, config, child_id, expected, depth + 1);
    }
  }
  assert(parent.beacon_count == static_cast<std::uint64_t>(
      std::ceil((config.beacon_ratios.empty() ? 0.1 : config.beacon_ratios[depth]) *
                parent.child_count)));
  for (std::uint64_t row = 0; row < parent.child_count; ++row) {
    const auto child = index.children[parent.first_child + row];
    for (std::uint64_t b = 0; b < parent.beacon_count; ++b) {
      const auto ordinal = index.beacon_ordinals[parent.first_beacon + b];
      const auto beacon = index.children[parent.first_child + ordinal];
      assert(index.beacon_distances[parent.first_distance + row * parent.beacon_count + b] ==
             distance(index.center_sequences.decode(child), index.center_sequences.decode(beacon)));
    }
  }
}

auto key(const navigamer::QueryHit& hit) {
  return std::tuple(hit.occurrence_id, hit.reverse, hit.distance, hit.unique_id);
}

std::vector<navigamer::QueryHit> oracle(const navigamer::SequenceStore& reference,
                                      std::string_view query, unsigned tolerance,
                                      bool both_strands) {
  std::vector<navigamer::QueryHit> hits;
  for (const bool reverse : {false, true}) {
    if (reverse && !both_strands) continue;
    const std::string sequence = reverse ? navigamer::reverse_complement(query) : std::string(query);
    for (navigamer::SequenceId id = 0; id < reference.size(); ++id) {
      const int d = distance(sequence, reference.sequence(id));
      if (d <= static_cast<int>(tolerance)) {
        hits.push_back({id, id, static_cast<unsigned>(d), reverse});
      }
    }
  }
  std::sort(hits.begin(), hits.end(), [](const auto& lhs, const auto& rhs) { return key(lhs) < key(rhs); });
  return hits;
}

void check_hits(const std::vector<navigamer::QueryHit>& expected,
                const std::vector<navigamer::QueryHit>& actual) {
  assert(expected.size() == actual.size());
  for (std::size_t i = 0; i < expected.size(); ++i) assert(key(expected[i]) == key(actual[i]));
}

std::string file_contents(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void test_distance() {
  using navigamer::EditDistanceBackend;
  assert(navigamer::EditDistance::route("ACGT", "ACGT", -1) ==
         EditDistanceBackend::kEdlib);
  assert(navigamer::EditDistance::route("ACGT", "ACGT", 16) ==
         EditDistanceBackend::kEdlib);
  assert(navigamer::EditDistance::route("ACGT", "ACG", 1) ==
         EditDistanceBackend::kEdlib);
  assert(navigamer::EditDistance::route("ACGN", "ACGT", 1) ==
         EditDistanceBackend::kEdlib);
  assert(navigamer::EditDistance::route(std::string(257, 'A'),
                                        std::string(257, 'A'), 1) ==
         EditDistanceBackend::kEdlib);
  assert(navigamer::EditDistance::route("ACGT", "ACGT", 1) ==
         (navigamer::EditDistance::leap_available()
              ? EditDistanceBackend::kLeap
              : EditDistanceBackend::kEdlib));

  const auto inputs = strings("ACN", 3);
  auto targets = strings("ACN", 4);
  targets.push_back("");
  for (const auto& lhs : inputs) {
    for (const auto& rhs : targets) {
      const int expected = dynamic_programming_distance(lhs, rhs);
      assert(navigamer::EditDistance::exact(lhs, rhs) == expected);
      for (int t = 0; t <= 4; ++t) {
        const int actual = navigamer::EditDistance::bounded(lhs, rhs, t);
        if (actual != (expected <= t ? expected : -1)) {
          std::cerr << "bounded mismatch lhs=" << lhs << " rhs=" << rhs
                    << " k=" << t << " expected=" << expected
                    << " actual=" << actual << '\n';
          assert(false);
        }
      }
    }
  }
  std::mt19937 rng(20260917);
  for (const std::size_t length : {0, 1, 63, 64, 65, 127, 128, 129, 150, 255, 256, 257}) {
    std::string lhs(length, 'A'), rhs(length + 1, 'A');
    for (auto& c : lhs) c = "ACGTN"[rng() % 5];
    for (auto& c : rhs) c = "ACGTN"[rng() % 5];
    assert(navigamer::EditDistance::exact(lhs, rhs) ==
           dynamic_programming_distance(lhs, rhs));
    assert(navigamer::EditDistance::exact(rhs, lhs) ==
           dynamic_programming_distance(lhs, rhs));
    assert(navigamer::EditDistance::exact("", rhs) == static_cast<int>(rhs.size()));
    auto edited = lhs;
    edited.insert(edited.begin() + edited.size() / 2, 'N');
    assert(navigamer::EditDistance::bounded(lhs, edited, 1) == 1);
    assert(navigamer::EditDistance::bounded(lhs, edited, 0) == -1);
  }

  for (const std::size_t length : {64, 127, 128, 129, 150, 255, 256}) {
    for (int pair = 0; pair < 128; ++pair) {
      std::string lhs(length, 'A');
      for (auto& base : lhs) base = "ATGC"[rng() % 4];
      auto rhs = lhs;
      for (int edit = 0; edit < pair % 20; ++edit) {
        const auto position = (pair * 29 + edit * 17) % length;
        const auto code = std::string_view("ATGC").find(rhs[position]);
        rhs[position] = "ATGC"[(code + 1 + rng() % 3) % 4];
      }
      if (pair % 3 == 0) {
        rhs.erase((pair * 11) % length, 1);
        rhs.insert((pair * 7) % length, 1, "ATGC"[rng() % 4]);
      }
      const int expected = dynamic_programming_distance(lhs, rhs);
      for (const int max_distance : {1, 5, 15}) {
        const int actual = navigamer::EditDistance::bounded(
            lhs, rhs, max_distance);
        assert(actual == (expected <= max_distance ? expected : -1));
      }
    }
  }
}

void test_prepared_distance() {
  std::mt19937 rng(2048);
  for (const std::size_t length : {0, 1, 15, 16, 63, 64, 65, 150, 255, 256, 257}) {
    std::string lhs(length, 'A');
    for (auto& base : lhs) base = "ATGC"[rng() % 4];
    navigamer::PreparedQuery query(lhs);
    navigamer::PackedCenters packed(length);
    std::vector<std::string> targets{lhs, std::string(length, 'T')};
    for (int changes : {1, 5, 15, 16}) {
      auto target = lhs;
      for (int i = 0; length && i < changes; ++i) target[rng() % length] = "ATGC"[rng() % 4];
      targets.push_back(target);
    }
    if (length) {
      auto target = lhs;
      target.erase(target.begin());
      target.push_back('C');
      targets.push_back(target);
      target[length / 2] = 'N';
      targets.push_back(target);
    }
    for (const auto& target : targets) packed.append(target);
    std::string scratch;
    for (std::size_t i = 0; i < targets.size(); ++i) {
      const auto expected = dynamic_programming_distance(lhs, targets[i]);
      for (const int k : {15, 0, -1, 1, 16, 5, 300}) {
        const int result = k < 0 || expected <= k ? expected : -1;
        assert(query.distance(targets[i], k) == result);
        assert(query.distance(packed, i, scratch, k) == result);
      }
    }
    for (const auto& target : {lhs + "N", lhs + "A", std::string{}}) {
      const auto expected = dynamic_programming_distance(lhs, target);
      assert(query.distance(target) == expected);
      assert(query.distance(target, 5) == (expected <= 5 ? expected : -1));
    }
  }
}

void test_packed_centers() {
  std::mt19937 rng(20260926);
  for (const unsigned length : {0, 1, 3, 4, 5, 31, 32, 33, 63, 64, 65, 150, 257}) {
    navigamer::PackedCenters packed(length);
    std::vector<std::string> expected;
    for (std::size_t c = 0; c < 7; ++c) {
      std::string sequence(length, 'A');
      for (auto& base : sequence) base = "ATGC"[rng() % 4];
      expected.push_back(sequence);
      packed.append(sequence);
    }
    assert(packed.data().size() == expected.size() * ((length + 3) / 4));
    const auto plane_bytes = length <= navigamer::EditDistance::kLeapMaxLength &&
        navigamer::EditDistance::leap_available() ? expected.size() * sizeof(navigamer::DnaBits) : 0;
    assert(packed.bytes() == packed.data().size() +
        (expected.size() + 1) * sizeof(std::uint64_t) + plane_bytes);
    std::string unusual(length, 'N');
    for (std::size_t i = 0; i < length; ++i) unusual[i] = "NRYSWKMBDHV-"[i % 11];
    expected.push_back(unusual);
    packed.append(unusual);
    packed.prefetch(0, packed.size());
    std::string scratch;
    for (std::size_t c = 0; c < expected.size(); ++c) {
      assert(packed.decode(c, scratch) == expected[c]);
    }
    bool rejected = false;
    try {
      packed.append(std::string(length + 1, 'A'));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    assert(rejected);
  }
  navigamer::PackedCenters codes(4);
  codes.append("ATGC");
  assert(codes.data()[0] == 0b11100100);
}

void check_batch(const navigamer::QueryEngine& engine,
                 const navigamer::SequenceStore& reference,
                 const std::vector<std::string>& queries,
                 navigamer::QueryConfig config) {
  std::vector<std::string_view> views(queries.begin(), queries.end());
  navigamer::PathCache cache;
  // A second batch exercises the previous batch's cached path.
  for (int repeat = 0; repeat < 2; ++repeat) {
    navigamer::QueryStats stats;
    const auto actual = engine.query_batch(views, config, &cache, &stats,
                                           {repeat == 0 ? 1u : 3u, 7, true});
    assert(actual.size() == queries.size());
    std::uint64_t verifications = 0;
    for (std::size_t i = 0; i < queries.size(); ++i) {
      check_hits(oracle(reference, queries[i], config.tolerance, config.both_strands), actual[i]);
      navigamer::QueryStats single_stats;
      check_hits(engine.query(queries[i], config, nullptr, &single_stats), actual[i]);
      verifications += single_stats.exact_verifications;
    }
    assert(stats.exact_verifications == verifications);
    assert(stats.route_worlds[0] == (queries.empty() ? 0 : 1));
    if (!queries.empty()) {
      navigamer::PathCache expected_path;
      (void)engine.query(queries.back(), config, &expected_path);
      assert(cache.valid && cache.path == expected_path.path);
    }
  }
}

void test_world_schedule() {
  const auto reference = navigamer::SequenceStore::from_sequences(
      {"AAAA", "AAAC", "AACC", "ACCC", "CCCC", "GGGG", "GGGT", "TTTT", "NNNN"});
  navigamer::BuildConfig build_config;
  build_config.center_radii = {2, 1, 0};
  build_config.max_tolerance = 0;
  const auto index = navigamer::IndexBuilder::build(reference, build_config);
  navigamer::QueryEngine engine(index, reference);
  const std::vector<std::string> inputs{"NNNN", "AAAA", "TTTT", "GGGG", "ACCC", "CCCC",
                                        "AAAC", "GGGT", "AACC", "", "AAA", "AACCC"};
  std::vector<std::string_view> views;
  for (int repeat = 0; repeat < 20; ++repeat) {
    for (const auto& input : inputs) views.push_back(input);
  }
  for (const bool both : {false, true}) {
    navigamer::QueryConfig config{0, navigamer::RouteMode::kMultilateration, true, both, true};
    std::vector<std::vector<navigamer::QueryHit>> expected;
    std::array<std::set<navigamer::NodeId>, 3> visited;
    for (const auto query : views) {
      expected.push_back(oracle(reference, query, 0, both));
      auto one_strand = config;
      one_strand.both_strands = false;
      for (const bool reverse : {false, true}) {
        if (reverse && !both) continue;
        navigamer::PathCache path;
        const std::string sequence = reverse ? navigamer::reverse_complement(query) : std::string(query);
        (void)engine.query(sequence, one_strand, &path);
        visited[0].insert(index.root);
        visited[1].insert(path.path[0]);
        visited[2].insert(path.path[1]);
      }
    }
    navigamer::QueryStats single_stats;
    for (const std::size_t minimum : {1, 7, 8, 16, 32, 64, 1000}) {
      navigamer::QueryStats stats;
      const auto actual = engine.query_batch(views, config, nullptr, &stats, {4, minimum, true});
      for (std::size_t i = 0; i < views.size(); ++i) check_hits(expected[i], actual[i]);
      for (std::size_t depth = 0; depth < 3; ++depth) {
        assert(stats.route_worlds[depth] == visited[depth].size());
      }
      single_stats = stats;
    }
    navigamer::QueryStats discarded_stats;
    assert(engine.query_batch(views, config, nullptr, &discarded_stats, {4, 3, false}).empty());
    assert(discarded_stats.route_worlds == single_stats.route_worlds);
    assert(discarded_stats.exact_verifications == single_stats.exact_verifications);
    // More requested threads than queries, and uneven contiguous partitions.
    for (const std::size_t count : {1, 3, 7, 31, 32, 33, 63, 64, 65}) {
      const auto actual = engine.query_batch(
          std::span<const std::string_view>(views).first(count), config,
          nullptr, nullptr, {8, 4, true});
      assert(actual.size() == count);
      for (std::size_t i = 0; i < count; ++i) check_hits(expected[i], actual[i]);
    }
    // A skewed frontier mixes one split world with many whole-world tasks.
    // Its last item differs from the heavy world, exercising final cache writes.
    std::vector<std::string_view> skewed(1001, inputs[1]);
    skewed.insert(skewed.end(), views.begin(), views.end());
    const auto heavy_hits = oracle(reference, inputs[1], 0, both);
    for (const std::uint32_t threads : {1u, 4u, 8u}) {
      navigamer::PathCache cache;
      navigamer::QueryStats stats;
      const auto actual = engine.query_batch(skewed, config, &cache, &stats, {threads, 1, true});
      assert(actual.size() == skewed.size());
      for (std::size_t i = 0; i < 1001; ++i) check_hits(heavy_hits, actual[i]);
      for (std::size_t i = 0; i < views.size(); ++i) check_hits(expected[i], actual[1001 + i]);
      for (std::size_t depth = 0; depth < 3; ++depth) {
        assert(stats.route_worlds[depth] == visited[depth].size());
      }
      navigamer::PathCache expected_path;
      (void)engine.query(skewed.back(), config, &expected_path);
      assert(cache.valid && cache.path == expected_path.path);
    }
  }
  for (const auto options : {navigamer::BatchQueryConfig{0, 64, true},
                             navigamer::BatchQueryConfig{1, 0, true}}) {
    bool rejected = false;
    try {
      (void)engine.query_batch(views, {}, nullptr, nullptr, options);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    assert(rejected);
  }
}

void test_multilateration_boundary() {
  const auto reference = navigamer::SequenceStore::from_sequences(
      {"AAAAAAAAAA", "CCCCAAAAAA", "CCCCCCCCCC",
       "CAAAAAAAAA", "CAAAAAAAAA"});
  navigamer::BuildConfig config;
  config.center_radii = {3, 3, 3};
  config.beacon_ratios = {0.1, 0.1, 0.1};
  config.max_tolerance = 1;
  config.threads = 3;
  navigamer::BuildStats stats;
  const auto index = navigamer::IndexBuilder::build(reference, config, &stats);
  assert(index.layers[0].node_count == 3);
  const auto first = descendants(index, index.children[index.nodes[0].first_child]);
  const auto second = descendants(index, index.children[index.nodes[0].first_child + 1]);
  const auto third = descendants(index, index.children[index.nodes[0].first_child + 2]);
  assert(first.contains(3) && first.contains(4));
  assert(second.contains(3) && second.contains(4));
  assert(!third.contains(3) && !third.contains(4));
  assert(first.contains(0) && !second.contains(0));
  assert(second.contains(1) && !first.contains(1));
  assert(third.contains(2) && !first.contains(2) && !second.contains(2));
  assert(stats.layers[0].member_beacon_edit_calls == 2);
  assert(stats.layers[0].candidate_edit_calls == 2);
  assert(stats.layers[0].pruned_center_pairs == 2);
  check_hierarchy(index, reference, config, 0, {0, 1, 2, 3, 4}, 0);
}

void test_exclusive_centers() {
  const auto reference = navigamer::SequenceStore::from_sequences({"AAAA", "AACC"});
  navigamer::BuildConfig config;
  config.center_radii = {1, 1, 1};
  config.max_tolerance = 1;
  const auto index = navigamer::IndexBuilder::build(reference, config);
  assert(index.layers[0].node_count == 2);
  check_hierarchy(index, reference, config, 0, {0, 1}, 0);
  for (std::size_t i = 0; i < 2; ++i) {
    assert(descendants(index, i + 1) == std::set<navigamer::SequenceId>{i});
  }
}

void test_shrinking_bound(const std::filesystem::path& directory) {
  // The first anchor is far from the C/G/T members. A later center lowers U
  // from 19 to 1, crossing the Edlib/LEAP threshold without losing overlap.
  const auto reference = navigamer::SequenceStore::from_sequences({
      std::string(20, 'A'), std::string(20, 'C'), std::string(20, 'G'),
      std::string(20, 'T'), std::string(19, 'C') + 'A',
      std::string(19, 'G') + 'A', std::string(19, 'T') + 'A',
      std::string(17, 'C') + "AAA", std::string(18, 'C') + "AA"});
  std::vector<navigamer::SequenceId> members(reference.size());
  for (std::size_t i = 0; i < members.size(); ++i) members[i] = i;
  for (const double ratio : {0.0, 0.25, 1.0}) {
    navigamer::BuildConfig config;
    config.center_radii = {3, 3, 3};
    config.max_tolerance = 1;
    config.beacon_ratios = {ratio, ratio, ratio};
    const auto single = navigamer::IndexBuilder::build(reference, config);
    check_hierarchy(single, reference, config, 0, members, 0);
    single.save(directory / "bound-single.nvm");
    config.threads = 16;
    const auto parallel = navigamer::IndexBuilder::build(reference, config);
    parallel.save(directory / "bound-parallel.nvm");
    assert(file_contents(directory / "bound-single.nvm") ==
           file_contents(directory / "bound-parallel.nvm"));
    const auto loaded = navigamer::NavigaMerIndex::load(directory / "bound-parallel.nvm");
    navigamer::QueryEngine engine(loaded, reference);
    for (const auto mode : {navigamer::RouteMode::kScan, navigamer::RouteMode::kMultilateration}) {
      navigamer::PathCache cache;
      for (const auto i : members) {
        const auto query = reference.sequence(i);
        check_hits(oracle(reference, query, 1, false), engine.query(query, {1, mode}, &cache));
      }
    }
  }
}

void test_index(const std::filesystem::path& directory) {
  auto values = strings("ACN", 4);
  values.push_back(values[0]);
  const auto reference = navigamer::SequenceStore::from_sequences(values);
  navigamer::BuildConfig config;
  config.center_radii = {3, 3, 3};
  config.max_tolerance = 1;
  config.beacon_ratios = {0.4, 0.6, 0.5};
  navigamer::BuildStats single_stats;
  const auto index = navigamer::IndexBuilder::build(reference, config, &single_stats);
  std::vector<navigamer::SequenceId> members(reference.size());
  for (std::size_t i = 0; i < members.size(); ++i) members[i] = i;
  check_hierarchy(index, reference, config, 0, members, 0);
  index.save(directory / "single.nvm");
  config.threads = 4;
  const auto parallel = navigamer::IndexBuilder::build(reference, config);
  parallel.save(directory / "parallel.nvm");
  assert(file_contents(directory / "single.nvm") == file_contents(directory / "parallel.nvm"));
  const auto restored = navigamer::NavigaMerIndex::load(directory / "single.nvm");
  restored.save(directory / "restored.nvm");
  assert(file_contents(directory / "single.nvm") == file_contents(directory / "restored.nvm"));
  check_hierarchy(restored, reference, config, 0, members, 0);

  auto queries = strings("ACN", 3);
  const auto queries4 = strings("ACN", 4), queries5 = strings("ACN", 5);
  queries.insert(queries.end(), queries4.begin(), queries4.end());
  queries.insert(queries.end(), queries5.begin(), queries5.end());
  queries.push_back("");
  queries.push_back("TTTT");
  navigamer::QueryEngine engine(restored, reference);
  std::uint64_t evaluations = 0;
  for (const bool both : {false, true}) {
    for (unsigned t = 0; t <= 1; ++t) {
      for (const auto mode : {navigamer::RouteMode::kMultilateration, navigamer::RouteMode::kScan}) {
        for (const bool use_cache : {false, true}) {
          navigamer::PathCache cache;
          navigamer::QueryConfig query_config{t, mode, use_cache, both, true};
          for (const auto& query : queries) {
            check_hits(oracle(reference, query, t, both), engine.query(query, query_config, &cache));
            ++evaluations;
          }
          check_batch(engine, reference, queries, query_config);
        }
      }
    }
  }
  assert(restored.uses_shared_reference && restored.legacy_leaf_sequences.empty());
  // New indexes require the original reference, including same-sized content.
  const auto different_reference = navigamer::SequenceStore::from_sequences({"TTTT"});
  bool rejected = false;
  try { navigamer::QueryEngine wrong(restored, different_reference); }
  catch (const std::invalid_argument&) { rejected = true; }
  assert(rejected);
  auto changed_values = values;
  changed_values.back()[0] = 'T';
  const auto changed_reference = navigamer::SequenceStore::from_sequences(changed_values);
  rejected = false;
  try { navigamer::QueryEngine wrong(restored, changed_reference); }
  catch (const std::invalid_argument&) { rejected = true; }
  assert(rejected);
  const auto member_view = restored.member_sequence(0, reference);
  assert(member_view.data() == reference.sequence(restored.leaf_ids[0]).data());
  // Legacy indexes still own their leaf bases; saving them retains v4 format.
  auto legacy = restored;
  legacy.uses_shared_reference = false;
  for (const auto id : legacy.leaf_ids) legacy.legacy_leaf_sequences.emplace_back(reference.sequence(id));
  legacy.save(directory / "legacy-v4.nvm");
  const auto legacy4 = navigamer::NavigaMerIndex::load(directory / "legacy-v4.nvm");
  assert(!legacy4.uses_shared_reference && legacy4.leaf_ids == restored.leaf_ids);
  navigamer::QueryEngine owned_engine(legacy4, different_reference);
  check_hits(oracle(reference, "AAAA", 1, false), owned_engine.query("AAAA", {1}));
  check_batch(owned_engine, reference, {"AAAA"}, {1});
  legacy4.save(directory / "legacy-roundtrip.nvm");
  assert(file_contents(directory / "legacy-v4.nvm") == file_contents(directory / "legacy-roundtrip.nvm"));
  assert(std::filesystem::file_size(directory / "single.nvm") <
         std::filesystem::file_size(directory / "legacy-v4.nvm"));

  // v3 lacks the layer-count field and uses root-layer sentinel 3.
  auto v3 = file_contents(directory / "legacy-v4.nvm");
  v3[6] = '3';
  const std::uint32_t version3 = 3;
  std::memcpy(v3.data() + 8, &version3, sizeof(version3));
  v3.erase(40, sizeof(std::uint32_t));
  const auto root_layer_offset = 40 + 3 * (2 * sizeof(std::uint64_t) + sizeof(std::uint32_t))
      + sizeof(std::uint64_t) + 8 * sizeof(std::uint64_t);
  v3[root_layer_offset] = 3;
  std::ofstream old_file(directory / "legacy-v3.nvm", std::ios::binary);
  old_file.write(v3.data(), v3.size());
  old_file.close();
  const auto legacy3 = navigamer::NavigaMerIndex::load(directory / "legacy-v3.nvm");
  assert(legacy3.nodes[legacy3.root].layer == navigamer::kRootLayer);
  navigamer::QueryEngine old_engine(legacy3, different_reference);
  check_hits(oracle(reference, "AAAA", 1, false), old_engine.query("AAAA", {1}));
  legacy3.save(directory / "upgraded-v4.nvm");
  assert(file_contents(directory / "upgraded-v4.nvm") == file_contents(directory / "legacy-v4.nvm"));
  for (const double ratio : {0.0, 1.0}) {
    config.beacon_ratios = {ratio, ratio, ratio};
    navigamer::BuildStats alternate_stats;
    const auto alternate = navigamer::IndexBuilder::build(
        reference, config, &alternate_stats);
    check_hierarchy(alternate, reference, config, 0, members, 0);
    const auto path = directory / (ratio == 0.0 ? "zero.nvm" : "full.nvm");
    alternate.save(path);
    const auto reloaded = navigamer::NavigaMerIndex::load(path);
    navigamer::QueryEngine alternate_engine(reloaded, reference);
    check_batch(alternate_engine, reference, queries4, {1});
    for (const auto& query : queries4) {
      check_hits(oracle(reference, query, 1, false), alternate_engine.query(query, {1}));
      ++evaluations;
    }
    for (const auto& layer : alternate_stats.layers) {
      if (ratio == 0.0) {
        assert(layer.member_beacon_edit_calls == 0);
        assert(layer.pruned_center_pairs == 0);
        assert(layer.candidate_edit_calls == layer.classification_center_pairs);
      } else {
        assert(layer.member_beacon_edit_calls == layer.classification_center_pairs);
        assert(layer.candidate_edit_calls == 0);
      }
    }
  }
  std::cout << "query_evaluations=" << evaluations << " false_negatives=0 false_positives=0\n";
}

void test_files(const std::filesystem::path& directory) {
  std::ofstream reference_file(directory / "reference.fa");
  reference_file << ">one description\nAACCAA\n>two\nTTGGTT\n";
  reference_file.close();
  const auto reference = navigamer::SequenceStore::from_fasta(directory / "reference.fa", 4, 1);
  assert(reference.size() == 6);
  assert(reference.sequence(0) == "AACC" && reference.sequence(3) == "TTGG");
  assert(reference.location(4).first == "two" && reference.location(4).second == 1);
  const auto limited = navigamer::SequenceStore::from_fasta(directory / "reference.fa", 4, 1, 4);
  assert(limited.size() == 4);
  std::ofstream fasta(directory / "queries.fa");
  fasta << ">q1\nAACC\n>q2\nTTGG\n";
  fasta.close();
  std::ofstream fastq(directory / "queries.fastq");
  fastq << "@q1\nAACC\n+\nIIII\n@q2\nTTGG\n+\nIIII\n";
  fastq.close();
  const auto records = navigamer::read_queries(directory / "queries.fa");
  const auto fastq_records = navigamer::read_queries(directory / "queries.fastq");
  assert(records.size() == 2 && fastq_records.size() == 2);
  assert(records[0].sequence == fastq_records[0].sequence);
  assert(navigamer::read_queries(directory / "queries.fa", 1).size() == 1);
}

void test_genome_schedule() {
  std::mt19937 rng(20260917);
  std::string bases(230, 'A');
  for (auto& c : bases) c = "ACGT"[rng() % 4];
  std::vector<std::string> values;
  for (std::size_t start = 0; start <= 80; start += 2) values.push_back(bases.substr(start, 150));
  values.push_back(std::string(150, 'N'));
  values.push_back(values.back());
  values.push_back(values[0]);
  const auto reference = navigamer::SequenceStore::from_sequences(values);
  navigamer::BuildConfig config;
  config.max_tolerance = 2;
  config.beacon_ratios = {0.25, 0.5, 0.75};
  config.threads = 3;
  const auto index = navigamer::IndexBuilder::build(reference, config);
  std::vector<navigamer::SequenceId> members(reference.size());
  for (std::size_t i = 0; i < members.size(); ++i) members[i] = i;
  check_hierarchy(index, reference, config, 0, members, 0);
  std::vector<std::string> queries;
  for (std::size_t i = 0; i < values.size(); i += 5) {
    auto value = values[i];
    queries.push_back(value);
    value[37] = value[37] == 'A' ? 'C' : 'A';
    queries.push_back(value);
    value.insert(80, 1, 'N');
    queries.push_back(value);
    value = values[i];
    value.erase(55, 1);
    queries.push_back(value);
  }
  queries.push_back(std::string(150, 'N'));
  navigamer::QueryEngine engine(index, reference);
  for (const auto mode : {navigamer::RouteMode::kMultilateration, navigamer::RouteMode::kScan}) {
    navigamer::PathCache cache;
    for (const auto& query : queries) {
      check_hits(oracle(reference, query, 2, true), engine.query(query, {2, mode, true, true}, &cache));
    }
    for (const bool prefetch : {false, true}) {
      check_batch(engine, reference, queries, {2, mode, true, true, prefetch});
    }
  }
  std::cout << "genome150_query_evaluations=" << queries.size() * 2
            << " false_negatives=0 false_positives=0\n";
}

void test_variable_layers(const std::filesystem::path& directory) {
  const auto reference = navigamer::SequenceStore::from_sequences({
      "AAAAAAAA", "CCCCCCCC", "GGGGGGGG", "TTTTTTTT", "NNNNNNNN",
      "CAAAAAAA", "CCAAAAAA", "CCCAAAAA", "AAAAAAAA"});
  std::vector<navigamer::SequenceId> members(reference.size());
  std::iota(members.begin(), members.end(), 0);
  const std::vector<std::string> queries{
      "AAAAAAAA", "CCAAAAAA", "NNNNNNNN", "CAAAAAA", "GCCCCCCCC", ""};
  const std::vector<std::string_view> views(queries.begin(), queries.end());
  navigamer::PathCache cache;  // Reused across indexes with different depths.
  for (const auto& radii : {std::vector<std::uint32_t>{3}, {5, 3},
                            {7, 5, 4, 3}, {5, 4, 3}, {3}}) {
    for (const bool cover : {false, true}) {
      navigamer::BuildConfig config;
      config.center_radii = radii;
      config.max_tolerance = 1;
      if (cover) config.beacon_cover_radii = std::vector<std::uint32_t>(radii.size(), 3);
      navigamer::BuildStats build_stats;
      const auto single = navigamer::IndexBuilder::build(reference, config, &build_stats);
      assert(single.layers.size() == radii.size());
      assert(build_stats.layers.size() == radii.size());
      assert(!single.is_leaf(single.root));
      if (!cover) check_hierarchy(single, reference, config, single.root, members, 0);
      std::size_t expected_nodes = 1;
      for (std::size_t layer = 0; layer < radii.size(); ++layer) {
        const auto& info = single.layers[layer];
        assert(info.first_node == expected_nodes);
        assert(info.selection_radius == radii[layer]);
        expected_nodes += info.node_count;
        for (auto id = info.first_node; id < expected_nodes; ++id) {
          assert(single.nodes[id].layer == layer);
          assert(single.is_leaf(id) == (layer + 1 == radii.size()));
          assert((single.nodes[id].child_count == 0) == single.is_leaf(id));
        }
      }
      assert(single.nodes.size() == expected_nodes);
      single.save(directory / "variable-single.nvm");
      config.threads = 4;
      const auto parallel = navigamer::IndexBuilder::build(reference, config);
      parallel.save(directory / "variable-parallel.nvm");
      assert(file_contents(directory / "variable-single.nvm") ==
             file_contents(directory / "variable-parallel.nvm"));
      const auto restored = navigamer::NavigaMerIndex::load(directory / "variable-single.nvm");
      restored.save(directory / "variable-restored.nvm");
      assert(file_contents(directory / "variable-single.nvm") ==
             file_contents(directory / "variable-restored.nvm"));
      navigamer::QueryEngine engine(restored, reference);
      for (const auto mode : {navigamer::RouteMode::kScan, navigamer::RouteMode::kMultilateration}) {
        navigamer::QueryStats stats;
        navigamer::QueryConfig query_config{1, mode, true, true, true};
        const auto batch = engine.query_batch(views, query_config, &cache, &stats, {4, 1, true});
        assert(cache.path.size() == radii.size());
        assert(stats.route_worlds.size() == radii.size());
        for (std::size_t i = 0; i < queries.size(); ++i) {
          const auto expected = oracle(reference, queries[i], 1, true);
          check_hits(expected, batch[i]);
          check_hits(expected, engine.query(queries[i], query_config, &cache, &stats));
          assert(stats.route_worlds == std::vector<std::uint64_t>(radii.size(), 2));
        }
        check_batch(engine, reference, queries, query_config);
        check_batch(engine, reference, {}, query_config);
      }
    }
  }
  // Exercise the uint8 layer boundary and root sentinel without a large tree.
  const auto tiny = navigamer::SequenceStore::from_sequences({"AAAA"});
  navigamer::BuildConfig deep;
  deep.center_radii.assign(255, 3);
  const auto deepest = navigamer::IndexBuilder::build(tiny, deep);
  deepest.save(directory / "deep.nvm");
  const auto loaded = navigamer::NavigaMerIndex::load(directory / "deep.nvm");
  assert(loaded.layers.size() == 255 && loaded.nodes.size() == 256);
  assert(loaded.nodes[0].layer == navigamer::kRootLayer && !loaded.is_leaf(0));
  assert(loaded.nodes.back().layer == 254 && loaded.is_leaf(255));
  navigamer::QueryStats stats;
  check_hits(oracle(tiny, "AAAA", 0, false),
             navigamer::QueryEngine(loaded, tiny).query("AAAA", {0}, &cache, &stats));
  assert(stats.route_worlds.size() == 255 && stats.routing_edit_calls() == 255);

  for (int invalid = 0; invalid < 4; ++invalid) {
    navigamer::BuildConfig config;
    if (invalid == 0) config.center_radii.clear();
    if (invalid == 1) config.center_radii.assign(256, 3);
    if (invalid == 2) config.beacon_ratios = {0.1};
    if (invalid == 3) config.beacon_cover_radii = std::vector<std::uint32_t>{3};
    bool rejected = false;
    try { (void)navigamer::IndexBuilder::build(tiny, config); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
  }
}

void test_center_selection_reuse(const std::filesystem::path& directory) {
  std::mt19937 rng(20260928);
  std::string first(4500, 'A'), third(500, 'A');
  for (auto& c : first) c = "ACGT"[rng() % 4];
  for (auto& c : third) c = "ACGTNR"[rng() % 6];
  const auto fasta = directory / "center-reuse.fa";
  std::ofstream out(fasta);
  // Repeated names must not imply overlapping windows across contig boundaries.
  out << ">same\n" << first << "\n>same\n" << std::string(4500, 'N')
      << "\n>same\n" << third << '\n';
  out.close();
  const auto reference = navigamer::SequenceStore::from_fasta(fasta, 12, 1);
  std::vector<navigamer::SequenceId> members(reference.size());
  std::iota(members.begin(), members.end(), 0);
  navigamer::BuildConfig config;
  config.center_radii = {8};
  config.max_tolerance = 1;
  navigamer::BuildStats stats;
  const auto serial = navigamer::IndexBuilder::build(reference, config, &stats);
  assert(stats.layers[0].center_bound_accepts > 0);
  assert(stats.layers[0].center_hint_hits > 0);
  check_hierarchy(serial, reference, config, serial.root, members, 0);
  serial.save(directory / "center-serial.nvm");
  for (const auto threads : {2u, 8u}) {
    config.threads = threads;
    const auto parallel = navigamer::IndexBuilder::build(reference, config, &stats);
    parallel.save(directory / "center-parallel.nvm");
    assert(file_contents(directory / "center-serial.nvm") ==
           file_contents(directory / "center-parallel.nvm"));
    navigamer::QueryEngine engine(parallel, reference);
    for (const auto id : {0u, 4095u, 4096u, 4488u, 4489u, 8192u, 8978u}) {
      check_hits(oracle(reference, reference.sequence(id), 1, false),
                 engine.query(reference.sequence(id), {1}));
    }
  }
  // Full-radius coverage takes no edit calls and keeps the first center.
  config.center_radii = {12};
  const auto full = navigamer::IndexBuilder::build(reference, config, &stats);
  assert(stats.layers[0].center_edit_calls == 0);
  assert(full.layers[0].node_count == 1 && full.nodes[1].center_sequence_id == 0);
  assert(full.leaf_ids == members);
}

void test_batch_center_selection(const std::filesystem::path& directory) {
  std::mt19937 random(98271);
  std::vector<std::string> prototypes(19, std::string(150, 'A'));
  for (auto& sequence : prototypes) {
    for (auto& base : sequence) base = "ACGT"[random() % 4];
  }
  prototypes[17] = std::string(150, 'N');
  prototypes[18] = std::string(150, 'R');
  std::vector<std::string> sequences;
  for (std::size_t i = 0; i < 4109; ++i) sequences.push_back(prototypes[i % prototypes.size()]);
  const auto reference = navigamer::SequenceStore::from_sequences(std::move(sequences));
  navigamer::BuildConfig config;
  config.center_radii = {75};
  navigamer::BuildStats stats;
  const auto serial = navigamer::IndexBuilder::build(reference, config, &stats);
  if (navigamer::BatchDistance::simd_available()) {
    assert(stats.layers[0].center_batch_groups > 0);
    assert(stats.layers[0].candidate_batch_groups > 0);
  } else {
    assert(stats.layers[0].center_batch_groups == 0);
    assert(stats.layers[0].candidate_batch_groups == 0);
  }

  // Independent scalar greedy selection and all-center classification oracle.
  std::vector<navigamer::SequenceId> centers;
  for (navigamer::SequenceId i = 0; i < reference.size(); ++i) {
    bool covered = false;
    for (const auto c : centers) {
      if (distance(reference.sequence(i), reference.sequence(c)) <= 75) { covered = true; break; }
    }
    if (!covered) centers.push_back(i);
  }
  assert(serial.layers[0].node_count == centers.size());
  std::vector<std::vector<navigamer::SequenceId>> expected(centers.size());
  std::vector<int> distances(centers.size());
  for (navigamer::SequenceId i = 0; i < reference.size(); ++i) {
    const auto own = std::find(centers.begin(), centers.end(), i);
    if (own != centers.end()) { expected[own - centers.begin()].push_back(i); continue; }
    for (std::size_t c = 0; c < centers.size(); ++c) {
      distances[c] = distance(reference.sequence(i), reference.sequence(centers[c]));
    }
    const int limit = *std::min_element(distances.begin(), distances.end()) + 10;
    for (std::size_t c = 0; c < centers.size(); ++c) {
      if (distances[c] <= limit) expected[c].push_back(i);
    }
  }
  for (std::size_t c = 0; c < centers.size(); ++c) {
    const auto node = serial.children[serial.nodes[serial.root].first_child + c];
    assert(serial.nodes[node].center_sequence_id == centers[c]);
    const auto actual = serial.members(node);
    assert(std::vector<navigamer::SequenceId>(actual.begin(), actual.end()) == expected[c]);
  }
  serial.save(directory / "batch-serial.nvm");
  for (const auto threads : {2u, 8u}) {
    config.threads = threads;
    const auto parallel = navigamer::IndexBuilder::build(reference, config);
    parallel.save(directory / "batch-parallel.nvm");
    assert(file_contents(directory / "batch-serial.nvm") == file_contents(directory / "batch-parallel.nvm"));
  }
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  const std::filesystem::path directory(argv[1]);
  std::filesystem::create_directories(directory);
  test_distance();
  test_prepared_distance();
  test_batch_distance();
  test_packed_centers();
  test_world_schedule();
  test_exclusive_centers();
  test_shrinking_bound(directory);
  test_multilateration_boundary();
  test_index(directory);
  test_files(directory);
  test_genome_schedule();
  test_variable_layers(directory);
  test_center_selection_reuse(directory);
  test_batch_center_selection(directory);
  std::cout << "all tests passed\n";
}
