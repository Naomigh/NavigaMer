#pragma once

#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace navigamer {

using SequenceId = std::uint64_t;
using NodeId = std::uint64_t;
inline constexpr NodeId kNoNode = std::numeric_limits<NodeId>::max();
inline constexpr SequenceId kNoSequence = std::numeric_limits<SequenceId>::max();
inline constexpr std::uint8_t kRootLayer = std::numeric_limits<std::uint8_t>::max();

struct WorldNode {
  SequenceId center_sequence_id{kNoSequence};
  std::uint64_t member_count{0};
  std::uint64_t first_child{0};
  std::uint64_t first_member{0};
  std::uint64_t first_beacon{0};
  std::uint64_t first_distance{0};
  std::uint64_t child_count{0};
  std::uint64_t beacon_count{0};
  std::uint8_t layer{kRootLayer};
};

struct LayerInfo {
  std::uint64_t first_node{0};
  std::uint64_t node_count{0};
  std::uint32_t selection_radius{0};
};

struct QueryHit {
  SequenceId unique_id{0};
  SequenceId occurrence_id{0};
  std::uint32_t distance{0};
  bool reverse{false};
};

struct QueryStats {
  explicit QueryStats(std::size_t layers = 0)
      : route_worlds(layers), route_edit_calls(layers),
        route_candidates(layers), route_pruned(layers) {}
  std::vector<std::uint64_t> route_worlds;
  std::vector<std::uint64_t> route_edit_calls;
  std::vector<std::uint64_t> route_candidates;
  std::vector<std::uint64_t> route_pruned;
  std::uint64_t leaf_members{0};
  std::uint64_t exact_verifications{0};
  std::uint64_t cache_candidates{0};

  [[nodiscard]] std::uint64_t routing_edit_calls() const noexcept {
    return std::accumulate(route_edit_calls.begin(), route_edit_calls.end(), std::uint64_t{0});
  }
  [[nodiscard]] std::uint64_t total_edit_calls() const noexcept {
    return routing_edit_calls() + exact_verifications;
  }
  QueryStats& operator+=(const QueryStats& other);
};

struct QueryRecord {
  std::string name;
  std::string sequence;
};

}  // namespace navigamer
