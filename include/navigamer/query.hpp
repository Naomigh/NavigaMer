#pragma once

#include "navigamer/edit_distance.hpp"
#include "navigamer/index.hpp"
#include "navigamer/sequence_store.hpp"
#include "navigamer/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace navigamer {

struct PathCache {
  std::string previous_query;
  std::vector<NodeId> contained_path;
  std::string root_anchor_query;
  std::vector<std::uint16_t> root_anchor_center_distances;
  std::uint32_t root_anchor_age{0};
  NodeId root_path_pivot{kNoNode};
  std::vector<std::uint16_t> root_path_pivot_center_distances;
  bool valid{false};
};

struct QueryConfig {
  std::uint32_t tolerance{5};
  std::uint32_t cache_similarity{8};
  bool enable_path_cache{true};
  bool enable_path_pivot{true};
  // Complete terminal worlds are stored in center-coordinate order. After a
  // cached leaf boundary crossing, inspect this many adjacent leaf centers
  // before falling back to the global hierarchy.
  std::uint32_t leaf_cache_neighborhood{64};
  // Keep an already-materialized root pivot row while its exact distance to
  // the current query is at most this value. The row remains an exact lower-
  // bound source at any distance; this threshold only trades pruning power
  // against the O(root children) cost of refreshing it.
  std::uint32_t path_pivot_max_distance{16};
  std::uint32_t anchor_refresh_interval{0};
};

// Inclusive wall-clock phases plus the time spent inside each Edlib call-site
// role. The Edlib fields are subsets of the inclusive phases and must not be
// added to them when reconstructing total query time.
struct QueryStageTimings {
  std::uint64_t setup_ns{0};
  std::uint64_t anchor_ns{0};
  std::vector<std::uint64_t> layer_routing_ns;
  std::uint64_t leaf_scan_ns{0};
  std::uint64_t cache_update_ns{0};
  std::uint64_t accounting_ns{0};
  std::uint64_t total_ns{0};

  std::uint64_t top_center_edlib_ns{0};
  std::uint64_t middle_center_edlib_ns{0};
  std::uint64_t leaf_center_edlib_ns{0};
  std::uint64_t beacon_edlib_ns{0};
  std::uint64_t path_pivot_query_edlib_ns{0};
  std::uint64_t path_pivot_row_edlib_ns{0};
  std::uint64_t query_anchor_edlib_ns{0};
  std::uint64_t leaf_verification_edlib_ns{0};
};

class QueryEngine {
 public:
  QueryEngine(const NavigaMerIndex& index, const SequenceStore& reference)
      : index_(index), reference_(reference) {}

  [[nodiscard]] std::vector<QueryHit> query(
      std::string_view sequence, const QueryConfig& config,
      QueryStats* stats = nullptr, PathCache* path_cache = nullptr,
      QueryStageTimings* timings = nullptr) const;

 private:
  const NavigaMerIndex& index_;
  const SequenceStore& reference_;
  mutable EditDistance distance_;
};

}  // namespace navigamer
