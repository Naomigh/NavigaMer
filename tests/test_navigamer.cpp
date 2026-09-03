#include "navigamer/builder.hpp"
#include "navigamer/edit_distance.hpp"
#include "navigamer/index.hpp"
#include "navigamer/query.hpp"
#include "navigamer/sequence_store.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace navigamer;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::vector<SequenceId> brute_force(const SequenceStore& reference,
                                    std::string_view query,
                                    std::uint32_t tolerance) {
  EditDistance distance;
  std::vector<SequenceId> hits;
  for (SequenceId id = 0; id < reference.size(); ++id) {
    if (distance(query, reference.sequence(id), tolerance) >= 0) hits.push_back(id);
  }
  return hits;
}

std::vector<SequenceId> ids(const std::vector<QueryHit>& hits) {
  std::vector<SequenceId> result;
  for (const auto& hit : hits) result.push_back(hit.sequence_id);
  return result;
}

void require_nested_containment(const NavigaMerIndex& index,
                                const SequenceStore& reference) {
  require(index.routing_mode == RoutingMode::kCompleteNestedBalls,
          "test index is not nested-ball routed");
  EditDistance distance;
  std::vector<std::uint32_t> memberships(reference.size(), 0);
  for (NodeId parent_id = 0; parent_id < index.nodes.size(); ++parent_id) {
    const auto& parent = index.nodes[parent_id];
    std::uint64_t represented = 0;
    for (std::uint32_t ordinal = 0; ordinal < parent.child_count; ++ordinal) {
      const auto child = index.children[parent.first_child + ordinal];
      if (index.is_terminal(parent)) {
        const int d = distance(reference.sequence(parent.center_sequence_id),
                               reference.sequence(child));
        require(d <= parent.cover_radius,
                "terminal member escapes its leaf occupied ball");
        ++memberships[child];
        ++represented;
      } else {
        const auto& child_node = index.nodes[child];
        represented += child_node.bwt_interval_length;
        if (parent.layer != kSyntheticLayer) {
          const int d = distance(reference.sequence(parent.center_sequence_id),
                                 reference.sequence(child_node.center_sequence_id));
          require(d + child_node.cover_radius <= parent.cover_radius,
                  "frozen child ball is not contained by its parent ball");
        }
      }
    }
    if (parent.child_count != 0) {
      require(represented == parent.bwt_interval_length,
              "world represented-member count is inconsistent");
    }
  }
  require(std::all_of(memberships.begin(), memberships.end(),
                      [](auto count) { return count >= 1; }),
          "complete leaves lost a reference sequence");
  for (std::uint64_t local = 0; local < index.layers.back().node_count;
       ++local) {
    const auto leaf_id = index.layers.back().first_node + local;
    const auto& leaf = index.nodes[leaf_id];
    std::vector<bool> present(reference.size(), false);
    for (std::uint32_t ordinal = 0; ordinal < leaf.child_count; ++ordinal) {
      present[index.children[leaf.first_child + ordinal]] = true;
    }
    for (SequenceId sequence = 0; sequence < reference.size(); ++sequence) {
      const bool expected = distance(reference.sequence(leaf.center_sequence_id),
                                     reference.sequence(sequence)) <=
          leaf.cover_radius;
      require(present[sequence] == expected,
              "terminal world is not a complete metric ball");
    }
  }
}

void exhaustive_no_false_negative_test() {
  std::vector<std::string> sequences{
      "ACGTACGTACGT", "CGTACGTACGTA", "GTACGTACGTAC",
      "TACGTACGTACG", "ACGTTCGTACGT", "ACGTACGAACGT",
      "TTTTACGTACGT", "ACGTACGTGGGG", "GATTACAGATTA",
      "GATTACAGACTA", "CCCCAAAATTTT", "CCCCAAAATTTA"};
  auto reference = SequenceStore::from_sequences(std::move(sequences));
  BuildConfig build_config;
  build_config.radii = {10, 7, 5};
  build_config.containment_tolerance = 2;
  build_config.max_beacons = 4;
  build_config.threads = 3;
  NavigaMerIndex index = IndexBuilder(reference).build(build_config);
  require_nested_containment(index, reference);
  QueryEngine engine(index, reference);
  QueryConfig query_config{2, 3, true};
  PathCache cache;

  std::vector<std::string> queries;
  for (SequenceId id = 0; id < reference.size(); ++id) {
    auto original = std::string(reference.sequence(id));
    queries.push_back(original);
    auto substituted = original;
    substituted[id % substituted.size()] =
        substituted[id % substituted.size()] == 'A' ? 'C' : 'A';
    queries.push_back(std::move(substituted));
    auto inserted = original;
    inserted.insert(inserted.begin() + 3, 'G');
    queries.push_back(std::move(inserted));
    auto deleted = original;
    deleted.erase(deleted.begin() + 5);
    queries.push_back(std::move(deleted));
  }
  for (const auto& query : queries) {
    const auto expected = brute_force(reference, query, query_config.tolerance);
    const auto observed = ids(engine.query(query, query_config, nullptr, &cache));
    require(observed == expected, "hierarchy result differs from brute force");
    const auto cold = ids(engine.query(query, query_config, nullptr, nullptr));
    require(cold == expected, "cold query result differs from brute force");
  }

  auto owner_config = build_config;
  owner_config.mode = BuildMode::kNearestOwner;
  owner_config.delayed_centers = true;
  const auto owner_index = IndexBuilder(reference).build(owner_config);
  require(owner_index.routing_mode == RoutingMode::kNearestOwner,
          "nearest-owner routing mode was not preserved");
  QueryEngine owner_engine(owner_index, reference);
  for (const auto& query : queries) {
    require(ids(owner_engine.query(query, query_config)) ==
                brute_force(reference, query, query_config.tolerance),
            "nearest-owner compatibility result differs from brute force");
  }

  const auto path = std::filesystem::temp_directory_path() /
                    "navigamer_roundtrip_test.nvm";
  index.save(path);
  auto loaded = NavigaMerIndex::load(path);
  loaded.validate(&reference);
  QueryEngine loaded_engine(loaded, reference);
  for (const auto& query : queries) {
    require(ids(loaded_engine.query(query, query_config)) ==
                brute_force(reference, query, query_config.tolerance),
            "serialized hierarchy result differs from brute force");
  }
  std::filesystem::remove(path);
}

void edit_distance_test() {
  EditDistance distance;
  require(distance("ACGT", "ACGT") == 0, "identity distance failed");
  require(distance("ACGT", "AGT") == 1, "deletion distance failed");
  require(distance("ACGT", "TTTT", 2) == -1, "bounded distance failed");

  std::mt19937_64 random(0x4e61766967614d65ULL);
  static constexpr char alphabet[] = "ACGTN";
  for (std::size_t trial = 0; trial < 2000; ++trial) {
    const auto query_length = 1 + random() % 220;
    const auto target_length = 1 + random() % 220;
    std::string query(query_length, 'A');
    std::string target(target_length, 'A');
    for (auto& base : query) base = alphabet[random() % 5];
    for (auto& base : target) base = alphabet[random() % 5];
    const auto prepared = distance.prepare(query);
    require(prepared(target) == distance(query, target),
            "prepared Myers distance differs from Edlib");
  }
}

void sliding_window_property_test() {
  std::mt19937_64 random(20260901);
  static constexpr char alphabet[] = "ACGT";
  std::string genome(220, 'A');
  for (auto& base : genome) base = alphabet[random() % 4];
  const auto fasta = std::filesystem::temp_directory_path() /
                     "navigamer_sliding_property.fa";
  {
    std::ofstream output(fasta);
    output << ">property\n" << genome << '\n';
  }
  auto reference = SequenceStore::from_fasta(fasta, 24, 1);
  BuildConfig build_config;
  build_config.radii = {16, 10, 7};
  build_config.containment_tolerance = 3;
  build_config.max_beacons = 4;
  build_config.threads = 4;
  build_config.delayed_centers = true;
  build_config.exact_global_reuse = false;
  build_config.top_fill_radius = 14;
  auto index = IndexBuilder(reference).build(build_config);
  QueryEngine engine(index, reference);
  QueryConfig query_config{3, 5, true};
  PathCache cache;
  std::uint64_t cache_contained = 0;
  for (SequenceId source = 0; source < reference.size(); source += 3) {
    auto query = std::string(reference.sequence(source));
    for (int edit = 0; edit < 3; ++edit) {
      const auto position = random() % query.size();
      query[position] = alphabet[random() % 4];
    }
    const auto expected = brute_force(reference, query, query_config.tolerance);
    QueryStats query_stats;
    require(ids(engine.query(query, query_config, &query_stats, &cache)) == expected,
            "sliding-window cached hierarchy differs from brute force");
    cache_contained += query_stats.leaf_cache_contained;
    require(ids(engine.query(query, query_config)) == expected,
            "sliding-window cold hierarchy differs from brute force");
  }
  require(cache_contained != 0,
          "sliding-window workload never used strict leaf containment");
  std::filesystem::remove(fasta);
}

}  // namespace

int main() {
  try {
    edit_distance_test();
    exhaustive_no_false_negative_test();
    sliding_window_property_test();
    std::cout << "all NavigaMer tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
  }
}
