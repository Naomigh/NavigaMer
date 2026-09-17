#pragma once

#include "navigamer/edit_distance.hpp"
#include "navigamer/index.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace navigamer {

enum class RouteMode { kMultilateration, kScan };

struct QueryConfig {
  std::uint32_t tolerance{5};
  RouteMode route_mode{RouteMode::kMultilateration};
  bool use_cache{true};
  bool both_strands{false};
  bool enable_prefetch{false};
};

struct PathCache {
  std::array<NodeId, 3> path{kNoNode, kNoNode, kNoNode};
  bool valid{false};
};

class QueryEngine {
 public:
  QueryEngine(const NavigaMerIndex& index, const SequenceStore& reference);
  [[nodiscard]] std::vector<QueryHit> query(
      std::string_view sequence, const QueryConfig& config,
      PathCache* cache = nullptr, QueryStats* stats = nullptr) const;

 private:
  [[nodiscard]] std::vector<QueryHit> query_one(
      std::string_view sequence, bool reverse, const QueryConfig& config,
      PathCache* cache, QueryStats* stats) const;
  const NavigaMerIndex& index_;
};

std::vector<QueryHit> brute_force_query(const NavigaMerIndex& index,
                                      const SequenceStore& reference,
                                      std::string_view query,
                                      std::uint32_t tolerance,
                                      bool reverse = false);

}  // namespace navigamer
