#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>

namespace navigamer {

using SequenceId = std::uint64_t;
using NodeId = std::uint64_t;
inline constexpr NodeId kNoNode = std::numeric_limits<NodeId>::max();
inline constexpr SequenceId kNoSequence = std::numeric_limits<SequenceId>::max();
inline constexpr std::uint8_t kRootLayer = 3;

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
  std::array<std::uint64_t, 3> route_edit_calls{};
  std::array<std::uint64_t, 3> route_candidates{};
  std::array<std::uint64_t, 3> route_pruned{};
  std::uint64_t leaf_members{0};
  std::uint64_t exact_verifications{0};
  std::uint64_t cache_candidates{0};

  [[nodiscard]] std::uint64_t routing_edit_calls() const noexcept {
    return route_edit_calls[0] + route_edit_calls[1] + route_edit_calls[2];
  }
  [[nodiscard]] std::uint64_t total_edit_calls() const noexcept {
    return routing_edit_calls() + exact_verifications;
  }
  QueryStats& operator+=(const QueryStats& other) noexcept;
};

struct QueryRecord {
  std::string name;
  std::string sequence;
};

}  // namespace navigamer
