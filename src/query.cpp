#include "navigamer/query.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace navigamer {

QueryEngine::QueryEngine(const NavigaMerIndex& index, const SequenceStore&)
    : index_(index) {}

std::vector<QueryHit> QueryEngine::query(
    std::string_view sequence, const QueryConfig& config, PathCache* cache,
    QueryStats* stats) const {
  QueryStats total;
  auto hits = query_one(sequence, false, config, cache, &total);
  if (config.both_strands) {
    QueryStats reverse_stats;
    auto reverse_hits = query_one(reverse_complement(sequence), true, config,
                                  cache, &reverse_stats);
    total += reverse_stats;
    hits.insert(hits.end(), reverse_hits.begin(), reverse_hits.end());
  }
  std::sort(hits.begin(), hits.end(), [](const QueryHit& lhs, const QueryHit& rhs) {
    if (lhs.occurrence_id != rhs.occurrence_id) return lhs.occurrence_id < rhs.occurrence_id;
    if (lhs.reverse != rhs.reverse) return lhs.reverse < rhs.reverse;
    if (lhs.distance != rhs.distance) return lhs.distance < rhs.distance;
    return lhs.unique_id < rhs.unique_id;
  });
  if (stats) *stats = total;
  return hits;
}

std::vector<QueryHit> QueryEngine::query_one(
    std::string_view sequence, bool reverse, const QueryConfig& config,
    PathCache* cache, QueryStats* stats) const {
  QueryStats local;
  auto prepared = EditDistance{}.prepare(sequence);
  auto route = [&](NodeId parent_id, std::size_t depth, NodeId cached_child) {
    const auto& parent = index_.nodes[parent_id];
    std::vector<int> distances(parent.child_count, -1);
    auto evaluate = [&](std::uint64_t ordinal) {
      auto& distance = distances[ordinal];
      if (distance < 0) {
        const auto child = index_.children[parent.first_child + ordinal];
        distance = prepared.exact(index_.center_sequences[child]);
        ++local.route_edit_calls[depth];
        ++local.route_candidates[depth];
      }
      return distance;
    };
    NodeId best_child = index_.children[parent.first_child];
    int best_distance = evaluate(0);
    auto update = [&](std::uint64_t ordinal) {
      const auto child = index_.children[parent.first_child + ordinal];
      const int distance = evaluate(ordinal);
      if (distance < best_distance || (distance == best_distance && child < best_child)) {
        best_distance = distance;
        best_child = child;
      }
    };
    if (config.use_cache && cached_child != kNoNode) {
      for (std::uint64_t ordinal = 0; ordinal < parent.child_count; ++ordinal) {
        if (index_.children[parent.first_child + ordinal] == cached_child) {
          update(ordinal);
          ++local.cache_candidates;
          break;
        }
      }
    }
    std::vector<int> query_beacon_distances(parent.beacon_count);
    for (std::uint64_t b = 0; b < parent.beacon_count; ++b) {
      const auto ordinal = index_.beacon_ordinals[parent.first_beacon + b];
      update(ordinal);
      query_beacon_distances[b] = distances[ordinal];
    }
    if (config.route_mode == RouteMode::kScan) {
      for (std::uint64_t ordinal = 0; ordinal < parent.child_count; ++ordinal) update(ordinal);
      return best_child;
    }
    std::vector<std::pair<int, std::uint64_t>> candidates;
    for (std::uint64_t ordinal = 0; ordinal < parent.child_count; ++ordinal) {
      if (distances[ordinal] >= 0) continue;
      int lower_bound = 0;
      for (std::uint64_t b = 0; b < parent.beacon_count; ++b) {
        const auto center_distance = index_.beacon_distances[
            parent.first_distance + ordinal * parent.beacon_count + b];
        lower_bound = std::max(lower_bound,
                               std::abs(query_beacon_distances[b] - center_distance));
      }
      candidates.push_back({lower_bound, ordinal});
    }
    std::sort(candidates.begin(), candidates.end(), [&](const auto& lhs, const auto& rhs) {
      if (lhs.first != rhs.first) return lhs.first < rhs.first;
      return index_.children[parent.first_child + lhs.second] <
             index_.children[parent.first_child + rhs.second];
    });
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      const auto [lower_bound, ordinal] = candidates[i];
      const auto child = index_.children[parent.first_child + ordinal];
      if (lower_bound > best_distance || (lower_bound == best_distance && child >= best_child)) {
        ++local.route_pruned[depth];
        continue;
      }
#if defined(__GNUC__) || defined(__clang__)
      if (config.enable_prefetch && i + 4 < candidates.size()) {
        const auto future_child = index_.children[parent.first_child + candidates[i + 4].second];
        __builtin_prefetch(index_.center_sequences[future_child].data(), 0, 1);
      }
#endif
      update(ordinal);
    }
    return best_child;
  };

  NodeId parent = index_.root;
  std::array<NodeId, 3> path;
  for (std::size_t depth = 0; depth < path.size(); ++depth) {
    const auto cached = cache && cache->valid ? cache->path[depth] : kNoNode;
    path[depth] = route(parent, depth, cached);
    parent = path[depth];
  }
  const auto& leaf = index_.nodes[parent];
  local.leaf_members = leaf.member_count;
  std::vector<QueryHit> hits;
  for (std::uint64_t i = 0; i < leaf.member_count; ++i) {
    const auto member = leaf.first_member + i;
#if defined(__GNUC__) || defined(__clang__)
    if (config.enable_prefetch && i + 4 < leaf.member_count) {
      __builtin_prefetch(index_.leaf_sequences[member + 4].data(), 0, 1);
    }
#endif
    const int distance = EditDistance::bounded(sequence, index_.leaf_sequences[member],
                                               static_cast<int>(config.tolerance));
    ++local.exact_verifications;
    if (distance >= 0) {
      const auto id = index_.leaf_ids[member];
      hits.push_back({id, id, static_cast<std::uint32_t>(distance), reverse});
    }
  }
  if (cache) {
    cache->path = path;
    cache->valid = true;
  }
  if (stats) *stats = local;
  return hits;
}

std::vector<QueryHit> brute_force_query(const NavigaMerIndex& index,
                                      const SequenceStore& reference,
                                      std::string_view query,
                                      std::uint32_t tolerance, bool reverse) {
  std::vector<QueryHit> hits;
  for (SequenceId id = 0; id < index.reference_window_count; ++id) {
    const int distance = EditDistance::bounded(query, reference.sequence(id),
                                               static_cast<int>(tolerance));
    if (distance >= 0) hits.push_back({id, id, static_cast<std::uint32_t>(distance), reverse});
  }
  return hits;
}

}  // namespace navigamer
