#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace navigamer {

using SequenceId = std::uint64_t;
using NodeId = std::uint64_t;

inline constexpr SequenceId kNoSequence =
    std::numeric_limits<SequenceId>::max();
inline constexpr NodeId kNoNode = std::numeric_limits<NodeId>::max();
inline constexpr std::uint8_t kSyntheticLayer =
    std::numeric_limits<std::uint8_t>::max();
inline constexpr std::uint16_t kMissingDistance =
    std::numeric_limits<std::uint16_t>::max();

enum class RoutingMode : std::uint32_t {
  kNearestOwner = 0,
  kNestedBalls = 1,
};

// Persistent nodes deliberately contain only integer IDs and flat-array ranges.
// No owning pointer is serialized. A child entry is a NodeId except at the
// terminal layer, where it is a SequenceId.
struct WorldNode {
  SequenceId center_sequence_id{kNoSequence};
  std::uint64_t bwt_interval_length{0};
  std::uint64_t first_child{0};
  std::uint64_t first_beacon{0};
  std::uint32_t child_count{0};
  // Maximum exact distance from the center to any uniquely owned member.
  // It is never larger than the nominal radius of this layer.
  std::uint16_t cover_radius{0};
  std::uint8_t beacon_count{0};
  std::uint8_t layer{kSyntheticLayer};
};

static_assert(sizeof(WorldNode) == 40,
              "WorldNode disk layout unexpectedly changed");

struct LayerInfo {
  std::uint64_t first_node{0};
  std::uint64_t node_count{0};
  std::uint32_t radius{0};
  std::uint32_t reserved{0};
};

// Persistent, pointer-free BK metric directory. A MetricIndexNode represents
// one child slot in a parent's CSR range. Its outgoing edges are stored
// contiguously and labelled by exact edit distance. Every non-terminal world
// has one root entry in NavigaMerIndex::metric_roots.
struct MetricIndexNode {
  std::uint64_t child_slot{0};
  std::uint64_t first_edge{0};
  std::uint32_t edge_count{0};
  // For nested-ball routing, every occupied world ball in this BK subtree is
  // contained by a ball of this radius around this node's child center.
  // Nearest-owner indexes do not need the field, but populate it as well.
  std::uint32_t subtree_cover_radius{0};
};

struct MetricIndexEdge {
  std::uint32_t child_node{0};
  std::uint16_t distance{0};
  std::uint16_t reserved{0};
};

static_assert(sizeof(MetricIndexNode) == 24);
static_assert(sizeof(MetricIndexEdge) == 8);

struct QueryHit {
  SequenceId sequence_id{0};
  std::uint32_t distance{0};
};

struct QueryStats {
  // Actual EditDistance::operator() invocations, split by the call site's role.
  std::uint64_t edit_distance_calls{0};
  std::uint64_t top_center_edlib_calls{0};
  std::uint64_t middle_center_edlib_calls{0};
  std::uint64_t leaf_center_edlib_calls{0};
  std::uint64_t beacon_edlib_calls{0};
  std::uint64_t path_pivot_query_edlib_calls{0};
  std::uint64_t path_pivot_row_edlib_calls{0};
  std::uint64_t query_anchor_edlib_calls{0};
  std::uint64_t leaf_verification_edlib_calls{0};
  std::uint64_t uncategorized_edlib_calls{0};
  std::uint64_t worlds_considered{0};
  std::uint64_t worlds_mbb_pruned{0};
  std::uint64_t world_center_distances{0};
  std::uint64_t leaf_members_considered{0};
  std::uint64_t leaf_members_mbb_pruned{0};
  std::uint64_t exact_verifications{0};
  std::uint64_t cache_fast_paths{0};
  std::uint64_t greedy_single_steps{0};
  std::uint64_t boundary_steps{0};
};

struct QueryRecord {
  std::string name;
  std::string sequence;
};

}  // namespace navigamer
