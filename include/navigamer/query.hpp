#pragma once

#include "navigamer/edit_distance.hpp"
#include "navigamer/index.hpp"

#include <cstdint>
#include <string_view>
#include <utility>
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
  std::vector<NodeId> path;
  bool valid{false};
};

struct BatchQueryConfig {
  std::uint32_t threads{1};
  // Limits pool size and large-world splitting; small worlds are whole tasks.
  std::size_t min_queries_per_thread{1};
  bool retain_hits{true};
};

class QueryEngine {
 public:
  // Both index and reference must outlive the engine and its queries.
  QueryEngine(const NavigaMerIndex& index, const SequenceStore& reference);
  [[nodiscard]] std::vector<QueryHit> query(
      std::string_view sequence, const QueryConfig& config,
      PathCache* cache = nullptr, QueryStats* stats = nullptr) const;
  // Routes the entire collection world by world; results retain input order.
  [[nodiscard]] std::vector<std::vector<QueryHit>> query_batch(
      std::span<const std::string_view> sequences, const QueryConfig& config,
      PathCache* cache = nullptr, QueryStats* stats = nullptr,
      const BatchQueryConfig& batch_config = {}) const;

 private:
  struct RoutingScratch {
    std::string center;
    std::vector<int> distances;
    std::vector<int> beacon_distances;
    std::vector<std::pair<int, std::uint64_t>> candidates;
  };
  [[nodiscard]] NodeId route(
      NodeId parent, std::size_t depth, PreparedQuery& query,
      NodeId cached_child, const QueryConfig& config, QueryStats& stats,
      RoutingScratch& scratch) const;
  [[nodiscard]] std::vector<QueryHit> verify_leaf(
      NodeId leaf, std::string_view sequence, bool reverse,
      const QueryConfig& config, QueryStats& stats) const;
  [[nodiscard]] std::vector<QueryHit> query_one(
      std::string_view sequence, bool reverse, const QueryConfig& config,
      PathCache* cache, QueryStats* stats) const;
  const NavigaMerIndex& index_;
  const SequenceStore& reference_;
};

std::vector<QueryHit> brute_force_query(const NavigaMerIndex& index,
                                      const SequenceStore& reference,
                                      std::string_view query,
                                      std::uint32_t tolerance,
                                      bool reverse = false);

}  // namespace navigamer
