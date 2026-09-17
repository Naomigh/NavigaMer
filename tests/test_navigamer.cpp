#include "navigamer/builder.hpp"
#include "navigamer/edit_distance.hpp"
#include "navigamer/query.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
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

int distance(std::string_view lhs, std::string_view rhs) {
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
    assert(index.center_sequences[child_id] == reference.sequence(centers[c]));
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
    if (depth == 2) {
      const auto actual = index.members(child_id);
      assert(std::vector<navigamer::SequenceId>(actual.begin(), actual.end()) == expected);
      for (std::size_t i = 0; i < expected.size(); ++i) {
        assert(index.leaf_sequences[child.first_member + i] == reference.sequence(expected[i]));
      }
      assert(child.beacon_count == 0);
    } else {
      assert(child.first_member == 0);
      check_hierarchy(index, reference, config, child_id, expected, depth + 1);
    }
  }
  if (depth == 0) {
    assert(parent.beacon_count == 0);
  } else {
    assert(parent.beacon_count == static_cast<std::uint64_t>(
        std::ceil(config.beacon_ratios[depth - 1] * parent.child_count)));
    for (std::uint64_t row = 0; row < parent.child_count; ++row) {
      const auto child = index.children[parent.first_child + row];
      for (std::uint64_t b = 0; b < parent.beacon_count; ++b) {
        const auto ordinal = index.beacon_ordinals[parent.first_beacon + b];
        const auto beacon = index.children[parent.first_child + ordinal];
        assert(index.beacon_distances[parent.first_distance + row * parent.beacon_count + b] ==
               distance(index.center_sequences[child], index.center_sequences[beacon]));
      }
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
  const auto inputs = strings("ACN", 3);
  auto targets = strings("ACN", 4);
  targets.push_back("");
  for (const auto& lhs : inputs) {
    auto prepared = navigamer::EditDistance{}.prepare(lhs);
    for (const auto& rhs : targets) {
      const int expected = distance(lhs, rhs);
      assert(prepared.exact(rhs) == expected);
      for (int t = 0; t <= 4; ++t) {
        assert(navigamer::EditDistance::bounded(lhs, rhs, t) == (expected <= t ? expected : -1));
      }
    }
  }
  std::mt19937 rng(20260917);
  for (const std::size_t length : {0, 1, 63, 64, 65, 127, 128, 129, 150, 255, 256, 257}) {
    std::string lhs(length, 'A'), rhs(length + 1, 'A');
    for (auto& c : lhs) c = "ACGTN"[rng() % 5];
    for (auto& c : rhs) c = "ACGTN"[rng() % 5];
    auto prepared = navigamer::EditDistance{}.prepare(lhs);
    assert(prepared.exact(rhs) == distance(lhs, rhs));
    prepared.reset(rhs);
    assert(prepared.exact(lhs) == distance(lhs, rhs));
    assert(navigamer::EditDistance::exact("", rhs) == static_cast<int>(rhs.size()));
    auto edited = lhs;
    edited.insert(edited.begin() + edited.size() / 2, 'N');
    assert(navigamer::EditDistance::bounded(lhs, edited, 1) == 1);
    assert(navigamer::EditDistance::bounded(lhs, edited, 0) == -1);
  }
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

void test_index(const std::filesystem::path& directory) {
  auto values = strings("ACN", 4);
  values.push_back(values[0]);
  const auto reference = navigamer::SequenceStore::from_sequences(values);
  navigamer::BuildConfig config;
  config.center_radii = {3, 3, 3};
  config.max_tolerance = 1;
  config.beacon_ratios = {0.4, 0.6};
  const auto index = navigamer::IndexBuilder::build(reference, config);
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
        }
      }
    }
  }
  // The query reads owned center and leaf bases, not the external reference.
  const auto different_reference = navigamer::SequenceStore::from_sequences({"TTTT"});
  navigamer::QueryEngine owned_engine(restored, different_reference);
  check_hits(oracle(reference, "AAAA", 1, false), owned_engine.query("AAAA", {1}));
  for (const double ratio : {0.0, 1.0}) {
    config.beacon_ratios = {ratio, ratio};
    const auto alternate = navigamer::IndexBuilder::build(reference, config);
    check_hierarchy(alternate, reference, config, 0, members, 0);
    navigamer::QueryEngine alternate_engine(alternate, reference);
    for (const auto& query : queries4) {
      check_hits(oracle(reference, query, 1, false), alternate_engine.query(query, {1}));
      ++evaluations;
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
  config.beacon_ratios = {0.25, 0.5};
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
  }
  std::cout << "genome150_query_evaluations=" << queries.size() * 2
            << " false_negatives=0 false_positives=0\n";
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  const std::filesystem::path directory(argv[1]);
  std::filesystem::create_directories(directory);
  test_distance();
  test_exclusive_centers();
  test_index(directory);
  test_files(directory);
  test_genome_schedule();
  std::cout << "all tests passed\n";
}
