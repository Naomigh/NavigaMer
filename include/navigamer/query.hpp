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
  std::uint32_t anchor_refresh_interval{8};
};

class QueryEngine {
 public:
  QueryEngine(const NavigaMerIndex& index, const SequenceStore& reference)
      : index_(index), reference_(reference) {}

  [[nodiscard]] std::vector<QueryHit> query(
      std::string_view sequence, const QueryConfig& config,
      QueryStats* stats = nullptr, PathCache* path_cache = nullptr) const;

 private:
  const NavigaMerIndex& index_;
  const SequenceStore& reference_;
  mutable EditDistance distance_;
};

}  // namespace navigamer
