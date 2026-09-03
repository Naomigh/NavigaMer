#include "navigamer/query.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace navigamer {

std::vector<QueryHit> QueryEngine::query(std::string_view sequence,
                                         const QueryConfig& config,
                                         QueryStats* stats,
                                         PathCache* path_cache,
                                         QueryStageTimings* timings) const {
  using Clock = std::chrono::steady_clock;
  const auto total_start = Clock::now();
  if (sequence.empty()) throw std::invalid_argument("query sequence is empty");
  QueryStats local_stats;
  QueryStageTimings local_timings;
  local_timings.layer_routing_ns.assign(index_.layers.size(), 0);
  const auto calls_at_start = distance_.calls();
  auto prepared_query = distance_.prepare(sequence);
  std::unordered_map<SequenceId, int> exact_distance_cache;
  exact_distance_cache.reserve(64);
  const bool complete_worlds =
      index_.routing_mode == RoutingMode::kCompleteNestedBalls;

  auto elapsed_ns = [](Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start).count());
  };

  auto exact_to_reference = [&](SequenceId id, std::uint64_t& category,
                                std::uint64_t& category_ns) {
    if (const auto found = exact_distance_cache.find(id);
        found != exact_distance_cache.end()) {
      return found->second;
    }
    const auto start = Clock::now();
    const int d = prepared_query(reference_.sequence(id));
    category_ns += elapsed_ns(start);
    ++category;
    exact_distance_cache.emplace(id, d);
    return d;
  };
  auto bounded_to_reference = [&](SequenceId id, std::uint32_t bound) {
    if (const auto found = exact_distance_cache.find(id);
        found != exact_distance_cache.end()) {
      return found->second <= static_cast<int>(bound) ? found->second : -1;
    }
    const auto start = Clock::now();
    const int d = distance_(sequence, reference_.sequence(id),
                            static_cast<int>(bound));
    local_timings.leaf_verification_edlib_ns += elapsed_ns(start);
    ++local_stats.leaf_verification_edlib_calls;
    if (d >= 0) exact_distance_cache.emplace(id, d);
    return d;
  };
  local_timings.setup_ns = elapsed_ns(total_start);

  const auto anchor_start = Clock::now();
  int root_anchor_query_distance = -1;
  if (!complete_worlds && config.enable_path_cache && path_cache &&
      config.anchor_refresh_interval != 0) {
    const auto& root = index_.nodes[index_.root];
    const bool refresh = path_cache->root_anchor_query.empty() ||
        path_cache->root_anchor_age >= config.anchor_refresh_interval ||
        path_cache->root_anchor_center_distances.size() != root.child_count;
    if (refresh) {
      path_cache->root_anchor_query.assign(sequence);
      path_cache->root_anchor_center_distances.resize(root.child_count);
      for (std::uint32_t ordinal = 0; ordinal < root.child_count; ++ordinal) {
        const auto child = index_.children[root.first_child + ordinal];
        const auto center = index_.nodes[child].center_sequence_id;
        const int d = exact_to_reference(
            center, local_stats.query_anchor_edlib_calls,
            local_timings.query_anchor_edlib_ns);
        if (d < 0 || d >= kMissingDistance) {
          throw std::length_error("query-anchor distance exceeds uint16");
        }
        path_cache->root_anchor_center_distances[ordinal] =
            static_cast<std::uint16_t>(d);
        ++local_stats.world_center_distances;
      }
      path_cache->root_anchor_age = 0;
      root_anchor_query_distance = 0;
    } else {
      const auto start = Clock::now();
      root_anchor_query_distance = distance_(
          sequence, path_cache->root_anchor_query);
      local_timings.query_anchor_edlib_ns += elapsed_ns(start);
      ++local_stats.query_anchor_edlib_calls;
    }
  }
  local_timings.anchor_ns = elapsed_ns(anchor_start);

  // A previous path is only an upper-bound hint. Its center distance is always
  // recomputed for the current query and the exact metric search still proves
  // the nearest+2t result, so no query-query similarity gate is needed.
  const bool cache_applicable =
      config.enable_path_cache && path_cache && path_cache->valid;

  struct Candidate {
    NodeId node{kNoNode};
    int distance{0};
  };

  bool strict_containment_found = false;

  auto children_in_nested_balls = [&](NodeId parent_id,
                                      NodeId cached_child) {
    const auto& parent = index_.nodes[parent_id];
    if (index_.is_terminal(parent) || parent.child_count == 0) {
      return std::vector<Candidate>{};
    }
    bool used_cached_hint = false;
    auto& center_edlib_calls = parent_id == index_.root
        ? local_stats.top_center_edlib_calls
        : parent.layer == 0
        ? local_stats.middle_center_edlib_calls
        : local_stats.leaf_center_edlib_calls;
    std::uint32_t cached_ordinal = 0;
    int cached_query_distance = 0;
    if (cached_child != kNoNode && cached_child < index_.nodes.size()) {
      const auto first = index_.children.begin() + parent.first_child;
      const auto last = first + parent.child_count;
      const auto found = std::lower_bound(first, last, cached_child);
      if (found != last && *found == cached_child) {
        cached_ordinal = static_cast<std::uint32_t>(found - first);
        cached_query_distance = exact_to_reference(
            index_.nodes[cached_child].center_sequence_id,
            center_edlib_calls,
            parent_id == index_.root
                ? local_timings.top_center_edlib_ns
                : parent.layer == 0
                ? local_timings.middle_center_edlib_ns
                : local_timings.leaf_center_edlib_ns);
        ++local_stats.world_center_distances;
        used_cached_hint = true;
        if (complete_worlds && index_.is_terminal(index_.nodes[cached_child]) &&
            cached_query_distance + static_cast<int>(config.tolerance) <=
                static_cast<int>(index_.nodes[cached_child].cover_radius)) {
          strict_containment_found = true;
          ++local_stats.strict_containment_steps;
          ++local_stats.cache_fast_paths;
          return std::vector<Candidate>{{cached_child, cached_query_distance}};
        }
      }
    }

    std::vector<int> query_beacon_distances(parent.beacon_count);
    for (std::uint32_t beacon = 0; beacon < parent.beacon_count; ++beacon) {
      query_beacon_distances[beacon] = exact_to_reference(
          index_.beacons[parent.first_beacon + beacon],
          local_stats.beacon_edlib_calls, local_timings.beacon_edlib_ns);
    }

    // A large root cannot afford an all-pairs matrix. Materialize exactly one
    // row from the previous path's leaf center and reuse it while nearby
    // queries stay in that leaf. The leaf radius keeps this pivot much closer
    // to the query than a coarse top-world center. This is still an exact
    // dynamic multilateration bound and never a candidate cap.
    NodeId root_path_pivot = kNoNode;
    int root_path_pivot_query_distance = 0;
    if (!complete_worlds && parent_id == index_.root &&
        index_.dense_pair_offsets[parent_id] == kNoNode &&
        config.enable_path_pivot && cache_applicable && path_cache) {
      // Reusing a still-proximal row avoids rebuilding one distance to every
      // top-world center whenever a source-sorted query crosses a small-world
      // boundary. Any pivot gives a valid reverse-triangle lower bound; the
      // distance threshold controls efficiency only, never correctness.
      if (path_cache->root_path_pivot != kNoNode &&
          path_cache->root_path_pivot < index_.nodes.size() &&
          path_cache->root_path_pivot_center_distances.size() ==
              parent.child_count) {
        const auto retained = path_cache->root_path_pivot;
        const int retained_distance = exact_to_reference(
            index_.nodes[retained].center_sequence_id,
            local_stats.path_pivot_query_edlib_calls,
            local_timings.path_pivot_query_edlib_ns);
        ++local_stats.world_center_distances;
        if (retained_distance <=
            static_cast<int>(config.path_pivot_max_distance)) {
          root_path_pivot = retained;
          root_path_pivot_query_distance = retained_distance;
        }
      }
      if (root_path_pivot == kNoNode) {
        for (auto it = path_cache->contained_path.rbegin();
             it != path_cache->contained_path.rend(); ++it) {
          if (*it != kNoNode && *it < index_.nodes.size()) {
            root_path_pivot = *it;
            break;
          }
        }
      }
    }
    if (root_path_pivot != kNoNode && root_path_pivot_query_distance == 0 &&
        sequence != reference_.sequence(
            index_.nodes[root_path_pivot].center_sequence_id)) {
      root_path_pivot_query_distance = exact_to_reference(
          index_.nodes[root_path_pivot].center_sequence_id,
          local_stats.path_pivot_query_edlib_calls,
          local_timings.path_pivot_query_edlib_ns);
      ++local_stats.world_center_distances;
    }
    if (parent_id == index_.root && root_path_pivot != kNoNode &&
        index_.dense_pair_offsets[parent_id] == kNoNode && path_cache &&
        (path_cache->root_path_pivot != root_path_pivot ||
         path_cache->root_path_pivot_center_distances.size() !=
             parent.child_count)) {
      path_cache->root_path_pivot = root_path_pivot;
      path_cache->root_path_pivot_center_distances.resize(parent.child_count);
      const auto pivot_center =
          index_.nodes[root_path_pivot].center_sequence_id;
      auto prepared_pivot = distance_.prepare(reference_.sequence(pivot_center));
      for (std::uint32_t ordinal = 0; ordinal < parent.child_count; ++ordinal) {
#if defined(__GNUC__) || defined(__clang__)
        if (ordinal + 16 < parent.child_count) {
          const auto future_slot = parent.first_child + ordinal + 16;
          __builtin_prefetch(&index_.children[future_slot], 0, 1);
          __builtin_prefetch(
              &index_.child_beacon_distances[
                  future_slot * index_.max_beacons], 0, 1);
        }
#endif
        const auto child = index_.children[parent.first_child + ordinal];
        const auto center = index_.nodes[child].center_sequence_id;
        const auto start = Clock::now();
        const int d = prepared_pivot(reference_.sequence(center));
        local_timings.path_pivot_row_edlib_ns += elapsed_ns(start);
        ++local_stats.path_pivot_row_edlib_calls;
        if (d < 0 || d >= kMissingDistance) {
          throw std::length_error("path-pivot distance exceeds uint16");
        }
        path_cache->root_path_pivot_center_distances[ordinal] =
            static_cast<std::uint16_t>(d);
        ++local_stats.world_center_distances;
      }
    }

    std::vector<Candidate> result;
    result.reserve(std::min<std::uint32_t>(parent.child_count, 32));
    // The root centers of a sliding-window reference form a path-like metric
    // set for which a BK tree can have very loose subtree envelopes. A flat
    // multilateration scan is cache-local and applies a candidate-specific
    // exact lower bound, so it remains complete even for a large root.
    if (parent.child_count <= 8192 || parent_id == index_.root) {
      struct DensePivot {
        std::uint32_t ordinal{0};
        int query_distance{0};
      };
      std::vector<DensePivot> dense_pivots;
      const auto dense_offset = index_.dense_pair_offsets[parent_id];
      if (dense_offset != kNoNode && parent.child_count > 16) {
        constexpr std::uint32_t kDensePivots = 16;
        dense_pivots.reserve(kDensePivots);
        for (std::uint32_t pivot = 0; pivot < kDensePivots; ++pivot) {
          const auto ordinal = static_cast<std::uint32_t>(
              static_cast<std::uint64_t>(pivot) * (parent.child_count - 1) /
              (kDensePivots - 1));
          const auto child = index_.children[parent.first_child + ordinal];
          dense_pivots.push_back({ordinal, exact_to_reference(
              index_.nodes[child].center_sequence_id, center_edlib_calls,
              parent_id == index_.root
                  ? local_timings.top_center_edlib_ns
                  : parent.layer == 0
                  ? local_timings.middle_center_edlib_ns
                  : local_timings.leaf_center_edlib_ns)});
          ++local_stats.world_center_distances;
        }
      }
      for (std::uint32_t ordinal = 0; ordinal < parent.child_count; ++ordinal) {
        const auto child_slot = parent.first_child + ordinal;
        const auto child_id = index_.children[child_slot];
        const auto& child = index_.nodes[child_id];
        int lower_bound = 0;
        for (std::uint32_t beacon = 0; beacon < parent.beacon_count; ++beacon) {
          const auto child_distance = index_.child_beacon_distances[
              child_slot * index_.max_beacons + beacon];
          lower_bound = std::max(lower_bound,
              std::abs(query_beacon_distances[beacon] -
                       static_cast<int>(child_distance)));
        }
        for (const auto pivot : dense_pivots) {
          const auto pair_distance = index_.dense_pair_distances[
              dense_offset +
              static_cast<std::uint64_t>(pivot.ordinal) * parent.child_count +
              ordinal];
          lower_bound = std::max(lower_bound,
              std::abs(pivot.query_distance - static_cast<int>(pair_distance)));
        }
        if (parent_id == index_.root && root_anchor_query_distance >= 0 &&
            path_cache->root_anchor_center_distances.size() ==
                parent.child_count) {
          lower_bound = std::max(lower_bound,
              std::abs(root_anchor_query_distance - static_cast<int>(
                  path_cache->root_anchor_center_distances[ordinal])));
        }
        if (used_cached_hint && dense_offset != kNoNode) {
          const auto pair_distance = index_.dense_pair_distances[
              dense_offset +
              static_cast<std::uint64_t>(cached_ordinal) * parent.child_count +
              ordinal];
          lower_bound = std::max(lower_bound,
              std::abs(cached_query_distance - static_cast<int>(pair_distance)));
        }
        if (parent_id == index_.root && root_path_pivot != kNoNode &&
            path_cache && path_cache->root_path_pivot == root_path_pivot &&
            path_cache->root_path_pivot_center_distances.size() ==
                parent.child_count) {
          lower_bound = std::max(lower_bound,
              std::abs(root_path_pivot_query_distance - static_cast<int>(
                  path_cache->root_path_pivot_center_distances[ordinal])));
        }
        const auto intersection_bound = static_cast<int>(child.cover_radius) +
            static_cast<int>(config.tolerance);
        if (lower_bound > intersection_bound) {
          ++local_stats.worlds_mbb_pruned;
          continue;
        }
        const int d = exact_to_reference(
            child.center_sequence_id, center_edlib_calls,
            parent_id == index_.root
                ? local_timings.top_center_edlib_ns
                : parent.layer == 0
                ? local_timings.middle_center_edlib_ns
                : local_timings.leaf_center_edlib_ns);
        ++local_stats.world_center_distances;
        ++local_stats.worlds_considered;
        if (d <= intersection_bound) {
          if (complete_worlds && index_.is_terminal(child) &&
              d + static_cast<int>(config.tolerance) <=
                  static_cast<int>(child.cover_radius)) {
            strict_containment_found = true;
            ++local_stats.strict_containment_steps;
            return std::vector<Candidate>{{child_id, d}};
          }
          result.push_back({child_id, d});
        }
      }
    } else {
      std::vector<std::uint32_t> stack{
          static_cast<std::uint32_t>(index_.metric_roots[parent_id])};
      while (!stack.empty()) {
        const auto metric_id = stack.back();
        stack.pop_back();
        const auto& metric_node = index_.metric_nodes[metric_id];
        const auto child_id = index_.children[metric_node.child_slot];
        const auto& child = index_.nodes[child_id];
        int lower_bound = 0;
        for (std::uint32_t beacon = 0; beacon < parent.beacon_count; ++beacon) {
          const auto child_distance = index_.child_beacon_distances[
              metric_node.child_slot * index_.max_beacons + beacon];
          lower_bound = std::max(lower_bound,
              std::abs(query_beacon_distances[beacon] -
                       static_cast<int>(child_distance)));
        }
        if (parent_id == index_.root && root_anchor_query_distance >= 0) {
          const auto ordinal = static_cast<std::uint32_t>(
              metric_node.child_slot - parent.first_child);
          if (ordinal < path_cache->root_anchor_center_distances.size()) {
            lower_bound = std::max(lower_bound,
                std::abs(root_anchor_query_distance - static_cast<int>(
                    path_cache->root_anchor_center_distances[ordinal])));
          }
        }
        if (lower_bound > static_cast<int>(metric_node.subtree_cover_radius) +
                              static_cast<int>(config.tolerance)) {
          ++local_stats.worlds_mbb_pruned;
          continue;
        }

        const int d = exact_to_reference(child.center_sequence_id,
                                         center_edlib_calls,
                                         parent_id == index_.root
                                             ? local_timings.top_center_edlib_ns
                                             : parent.layer == 0
                                             ? local_timings.middle_center_edlib_ns
                                             : local_timings.leaf_center_edlib_ns);
        ++local_stats.worlds_considered;
        ++local_stats.world_center_distances;
        if (d <= static_cast<int>(child.cover_radius) +
                     static_cast<int>(config.tolerance)) {
          if (complete_worlds && index_.is_terminal(child) &&
              d + static_cast<int>(config.tolerance) <=
                  static_cast<int>(child.cover_radius)) {
            strict_containment_found = true;
            ++local_stats.strict_containment_steps;
            return std::vector<Candidate>{{child_id, d}};
          }
          result.push_back({child_id, d});
        }
        for (std::uint32_t edge_ordinal = 0;
             edge_ordinal < metric_node.edge_count; ++edge_ordinal) {
          const auto& edge = index_.metric_edges[
              metric_node.first_edge + edge_ordinal];
          const auto& child_metric = index_.metric_nodes[edge.child_node];
          const int child_lower_bound =
              std::abs(d - static_cast<int>(edge.distance));
          if (child_lower_bound >
              static_cast<int>(child_metric.subtree_cover_radius) +
                  static_cast<int>(config.tolerance)) {
            ++local_stats.worlds_mbb_pruned;
          } else {
            stack.push_back(edge.child_node);
          }
        }
      }
    }

    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.distance != rhs.distance) return lhs.distance < rhs.distance;
      return lhs.node < rhs.node;
    });
    if (result.size() == 1) {
      ++local_stats.greedy_single_steps;
      if (used_cached_hint && result.front().node == cached_child) {
        ++local_stats.cache_fast_paths;
      }
    } else if (result.size() > 1) {
      ++local_stats.boundary_steps;
    }
    return result;
  };

  auto children_near_nearest = [&](NodeId parent_id, NodeId cached_child) {
    const auto& parent = index_.nodes[parent_id];
    if (index_.is_terminal(parent) || parent.child_count == 0) {
      return std::vector<Candidate>{};
    }
    auto& center_edlib_calls = parent_id == index_.root
        ? local_stats.top_center_edlib_calls
        : parent.layer == 0
        ? local_stats.middle_center_edlib_calls
        : local_stats.leaf_center_edlib_calls;
    int best_distance = std::numeric_limits<int>::max();
    NodeId best_child = kNoNode;
    bool used_cached_hint = false;
    std::uint32_t cached_ordinal = 0;
    int cached_query_distance = 0;
    if (cached_child != kNoNode && cached_child < index_.nodes.size()) {
      const auto first = index_.children.begin() + parent.first_child;
      const auto last = first + parent.child_count;
      const auto found = std::lower_bound(first, last, cached_child);
      if (found != last && *found == cached_child) {
        best_child = cached_child;
        best_distance = exact_to_reference(
            index_.nodes[cached_child].center_sequence_id,
            center_edlib_calls,
            parent_id == index_.root
                ? local_timings.top_center_edlib_ns
                : parent.layer == 0
                ? local_timings.middle_center_edlib_ns
                : local_timings.leaf_center_edlib_ns);
        cached_query_distance = best_distance;
        cached_ordinal = static_cast<std::uint32_t>(found - first);
        ++local_stats.world_center_distances;
        used_cached_hint = true;
      }
    }

    // For cache-sized child arrays, a dense multilateration directory is
    // faster than a pointer-chasing metric tree. The beacon lower bound is
    // exact by reverse triangle inequality; it only orders/prunes work.
    if (parent.child_count <= 8192) {
      std::vector<int> query_beacon_distances(parent.beacon_count);
      for (std::uint32_t beacon = 0; beacon < parent.beacon_count; ++beacon) {
        query_beacon_distances[beacon] = exact_to_reference(
            index_.beacons[parent.first_beacon + beacon],
            local_stats.beacon_edlib_calls, local_timings.beacon_edlib_ns);
      }
      struct DensePivot {
        std::uint32_t ordinal{0};
        int query_distance{0};
      };
      std::vector<DensePivot> dense_pivots;
      const auto dense_offset = index_.dense_pair_offsets[parent_id];
      if (dense_offset != kNoNode && parent.child_count > 16) {
        constexpr std::uint32_t kDensePivots = 16;
        dense_pivots.reserve(kDensePivots);
        for (std::uint32_t pivot = 0; pivot < kDensePivots; ++pivot) {
          const auto ordinal = static_cast<std::uint32_t>(
              static_cast<std::uint64_t>(pivot) * (parent.child_count - 1) /
              (kDensePivots - 1));
          const auto child = index_.children[parent.first_child + ordinal];
          dense_pivots.push_back({ordinal, exact_to_reference(
              index_.nodes[child].center_sequence_id, center_edlib_calls,
              parent_id == index_.root
                  ? local_timings.top_center_edlib_ns
                  : parent.layer == 0
                  ? local_timings.middle_center_edlib_ns
                  : local_timings.leaf_center_edlib_ns)});
          ++local_stats.world_center_distances;
        }
      }
      std::vector<Candidate> visited;
      visited.reserve(std::min<std::uint32_t>(parent.child_count, 32));
      for (std::uint32_t ordinal = 0; ordinal < parent.child_count; ++ordinal) {
        const auto child_slot = parent.first_child + ordinal;
        int lower_bound = 0;
        for (std::uint32_t beacon = 0; beacon < parent.beacon_count; ++beacon) {
          const auto child_distance = index_.child_beacon_distances[
              child_slot * index_.max_beacons + beacon];
          lower_bound = std::max(lower_bound,
              std::abs(query_beacon_distances[beacon] -
                       static_cast<int>(child_distance)));
        }
        for (const auto pivot : dense_pivots) {
          const auto pair_distance = index_.dense_pair_distances[
              dense_offset +
              static_cast<std::uint64_t>(pivot.ordinal) * parent.child_count +
              ordinal];
          lower_bound = std::max(lower_bound,
              std::abs(pivot.query_distance - static_cast<int>(pair_distance)));
        }
        if (parent_id == index_.root && root_anchor_query_distance >= 0 &&
            path_cache->root_anchor_center_distances.size() ==
                parent.child_count) {
          lower_bound = std::max(lower_bound,
              std::abs(root_anchor_query_distance - static_cast<int>(
                  path_cache->root_anchor_center_distances[ordinal])));
        }
        if (used_cached_hint && dense_offset != kNoNode) {
          const auto pair_distance = index_.dense_pair_distances[
              dense_offset +
              static_cast<std::uint64_t>(cached_ordinal) * parent.child_count +
              ordinal];
          lower_bound = std::max(lower_bound,
              std::abs(cached_query_distance - static_cast<int>(pair_distance)));
        }
        if (best_distance != std::numeric_limits<int>::max() &&
            lower_bound >
                best_distance + 2 * static_cast<int>(config.tolerance)) {
          ++local_stats.worlds_mbb_pruned;
          continue;
        }
        const auto child_id = index_.children[child_slot];
        const int d = exact_to_reference(
            index_.nodes[child_id].center_sequence_id, center_edlib_calls,
            parent_id == index_.root
                ? local_timings.top_center_edlib_ns
                : parent.layer == 0
                ? local_timings.middle_center_edlib_ns
                : local_timings.leaf_center_edlib_ns);
        ++local_stats.world_center_distances;
        ++local_stats.worlds_considered;
        visited.push_back({child_id, d});
        if (d < best_distance || (d == best_distance && child_id < best_child)) {
          best_distance = d;
          best_child = child_id;
        }
      }
      if (best_child == kNoNode) {
        throw std::logic_error("multilateration directory found no nearest child");
      }
      const auto owner_bound = best_distance +
          2 * static_cast<int>(config.tolerance);
      std::vector<Candidate> result;
      for (const auto candidate : visited) {
        const auto& child = index_.nodes[candidate.node];
        if (candidate.distance <= owner_bound &&
            candidate.distance <= static_cast<int>(child.cover_radius) +
                                      static_cast<int>(config.tolerance)) {
          result.push_back(candidate);
        }
      }
      std::sort(result.begin(), result.end(),
                [](const auto& lhs, const auto& rhs) {
        if (lhs.distance != rhs.distance) return lhs.distance < rhs.distance;
        return lhs.node < rhs.node;
      });
      if (result.size() == 1) {
        ++local_stats.greedy_single_steps;
        if (used_cached_hint && result.front().node == cached_child) {
          ++local_stats.cache_fast_paths;
        }
      } else if (result.size() > 1) {
        ++local_stats.boundary_steps;
      }
      return result;
    }

    std::vector<Candidate> visited;
    visited.reserve(std::min<std::uint32_t>(parent.child_count, 32));
    std::vector<std::uint32_t> stack{
        static_cast<std::uint32_t>(index_.metric_roots[parent_id])};
    while (!stack.empty()) {
      const auto metric_id = stack.back();
      stack.pop_back();
      const auto& metric_node = index_.metric_nodes[metric_id];
      const auto child_id = index_.children[metric_node.child_slot];
      const int d = exact_to_reference(
          index_.nodes[child_id].center_sequence_id, center_edlib_calls,
          parent_id == index_.root
              ? local_timings.top_center_edlib_ns
              : parent.layer == 0
              ? local_timings.middle_center_edlib_ns
              : local_timings.leaf_center_edlib_ns);
      ++local_stats.worlds_considered;
      ++local_stats.world_center_distances;
      visited.push_back({child_id, d});
      if (d < best_distance || (d == best_distance && child_id < best_child)) {
        best_distance = d;
        best_child = child_id;
      }
      const int cutoff = best_distance + 2 * static_cast<int>(config.tolerance);
      const int low = std::max(0, d - cutoff);
      const int high = d + cutoff;
      std::vector<std::pair<int, std::uint32_t>> eligible_edges;
      eligible_edges.reserve(metric_node.edge_count);
      for (std::uint32_t edge_ordinal = 0;
           edge_ordinal < metric_node.edge_count; ++edge_ordinal) {
        const auto& edge = index_.metric_edges[
            metric_node.first_edge + edge_ordinal];
        if (edge.distance >= low && edge.distance <= high) {
          eligible_edges.push_back(
              {std::abs(static_cast<int>(edge.distance) - d), edge.child_node});
        } else {
          ++local_stats.worlds_mbb_pruned;
        }
      }
      std::sort(eligible_edges.begin(), eligible_edges.end(),
                [](const auto& lhs, const auto& rhs) {
        if (lhs.first != rhs.first) return lhs.first > rhs.first;
        return lhs.second > rhs.second;
      });
      for (const auto& [unused_difference, child_node] : eligible_edges) {
        (void)unused_difference;
        stack.push_back(child_node);
      }
    }
    if (best_child == kNoNode) {
      throw std::logic_error("metric directory found no nearest child");
    }

    const auto owner_bound = best_distance +
        2 * static_cast<int>(config.tolerance);
    std::vector<Candidate> result;
    for (const auto candidate : visited) {
      const auto& child = index_.nodes[candidate.node];
      const auto cover_bound = static_cast<int>(child.cover_radius) +
          static_cast<int>(config.tolerance);
      if (candidate.distance <= owner_bound &&
          candidate.distance <= cover_bound) {
        result.push_back(candidate);
      }
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.distance != rhs.distance) return lhs.distance < rhs.distance;
      return lhs.node < rhs.node;
    });
    if (result.size() == 1) {
      ++local_stats.greedy_single_steps;
      if (used_cached_hint && result.front().node == cached_child) {
        ++local_stats.cache_fast_paths;
      }
    } else if (result.size() > 1) {
      ++local_stats.boundary_steps;
    }
    return result;
  };

  std::vector<NodeId> current{index_.root};
  std::vector<NodeId> new_path(index_.layers.size(), kNoNode);
  std::size_t start_depth = 0;
  if (complete_worlds && cache_applicable &&
      path_cache->contained_path.size() == index_.layers.size()) {
    const auto cached_leaf = path_cache->contained_path.back();
    if (cached_leaf != kNoNode && cached_leaf < index_.nodes.size() &&
        index_.is_terminal(index_.nodes[cached_leaf])) {
      ++local_stats.leaf_cache_probes;
      const int d = exact_to_reference(
          index_.nodes[cached_leaf].center_sequence_id,
          local_stats.leaf_center_edlib_calls,
          local_timings.leaf_center_edlib_ns);
      ++local_stats.world_center_distances;
      if (d + static_cast<int>(config.tolerance) <=
          static_cast<int>(index_.nodes[cached_leaf].cover_radius)) {
        ++local_stats.leaf_cache_contained;
        ++local_stats.cache_fast_paths;
        ++local_stats.strict_containment_steps;
        current.assign(1, cached_leaf);
        new_path = path_cache->contained_path;
        start_depth = index_.layers.size();
      } else if (config.leaf_cache_neighborhood != 0) {
        const auto& leaf_layer = index_.layers.back();
        const auto cached_ordinal = cached_leaf - leaf_layer.first_node;
        NodeId contained_leaf = kNoNode;
        for (std::uint64_t delta = 1;
             delta <= config.leaf_cache_neighborhood; ++delta) {
          const NodeId probes[2] = {
              cached_ordinal + delta < leaf_layer.node_count
                  ? leaf_layer.first_node + cached_ordinal + delta : kNoNode,
              cached_ordinal >= delta
                  ? leaf_layer.first_node + cached_ordinal - delta : kNoNode};
          for (const auto probe : probes) {
            if (probe == kNoNode) continue;
#if defined(__GNUC__) || defined(__clang__)
            const auto prefetch_ordinal = cached_ordinal + delta + 4;
            if (prefetch_ordinal < leaf_layer.node_count) {
              __builtin_prefetch(
                  &index_.nodes[leaf_layer.first_node + prefetch_ordinal],
                  0, 1);
            }
#endif
            ++local_stats.leaf_cache_neighbor_checks;
            const int probe_distance = exact_to_reference(
                index_.nodes[probe].center_sequence_id,
                local_stats.leaf_center_edlib_calls,
                local_timings.leaf_center_edlib_ns);
            ++local_stats.world_center_distances;
            if (probe_distance + static_cast<int>(config.tolerance) <=
                static_cast<int>(index_.nodes[probe].cover_radius)) {
              contained_leaf = probe;
              break;
            }
          }
          if (contained_leaf != kNoNode) break;
        }
        if (contained_leaf != kNoNode) {
          ++local_stats.leaf_cache_contained;
          ++local_stats.cache_fast_paths;
          ++local_stats.strict_containment_steps;
          current.assign(1, contained_leaf);
          new_path = path_cache->contained_path;
          new_path.back() = contained_leaf;
          start_depth = index_.layers.size();
        }
      }
    }
  }
  for (std::size_t depth = start_depth; depth < index_.layers.size(); ++depth) {
    const auto layer_start = Clock::now();
    const NodeId cached_child = cache_applicable &&
            depth < path_cache->contained_path.size()
        ? path_cache->contained_path[depth]
        : kNoNode;
    std::vector<Candidate> candidates;
    strict_containment_found = false;
    for (const auto parent : current) {
      auto local = index_.routing_mode == RoutingMode::kNestedBalls ||
              index_.routing_mode == RoutingMode::kCompleteNestedBalls
          ? children_in_nested_balls(parent, cached_child)
          : children_near_nearest(parent, cached_child);
      if (strict_containment_found) {
        candidates = std::move(local);
        break;
      }
      candidates.insert(candidates.end(), local.begin(), local.end());
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& lhs,
                                                        const auto& rhs) {
      if (lhs.node != rhs.node) return lhs.node < rhs.node;
      return lhs.distance < rhs.distance;
    });
    candidates.erase(std::unique(candidates.begin(), candidates.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.node == rhs.node; }),
        candidates.end());
    if (candidates.empty()) {
      current.clear();
      local_timings.layer_routing_ns[depth] = elapsed_ns(layer_start);
      break;
    }
    const auto nearest = std::min_element(candidates.begin(), candidates.end(),
        [](const auto& lhs, const auto& rhs) {
          if (lhs.distance != rhs.distance) return lhs.distance < rhs.distance;
          return lhs.node < rhs.node;
        });
    new_path[depth] = nearest->node;
    current.clear();
    current.reserve(candidates.size());
    for (const auto candidate : candidates) current.push_back(candidate.node);
    local_timings.layer_routing_ns[depth] = elapsed_ns(layer_start);
  }

  const auto leaf_scan_start = Clock::now();
  std::vector<QueryHit> hits;
  std::unordered_set<SequenceId> verified;
  for (const auto leaf_id : current) {
    const auto& leaf = index_.nodes[leaf_id];
    if (!index_.is_terminal(leaf)) continue;
    std::vector<int> query_beacon_distances(leaf.beacon_count);
    for (std::uint32_t beacon = 0; beacon < leaf.beacon_count; ++beacon) {
      query_beacon_distances[beacon] =
          exact_to_reference(index_.beacons[leaf.first_beacon + beacon],
                             local_stats.beacon_edlib_calls,
                             local_timings.beacon_edlib_ns);
    }
    for (std::uint32_t ordinal = 0; ordinal < leaf.child_count; ++ordinal) {
#if defined(__GNUC__) || defined(__clang__)
      if (ordinal + 16 < leaf.child_count) {
        const auto future_slot = leaf.first_child + ordinal + 16;
        __builtin_prefetch(&index_.children[future_slot], 0, 1);
        __builtin_prefetch(
            &index_.child_beacon_distances[
                future_slot * index_.max_beacons], 0, 1);
      }
#endif
      ++local_stats.leaf_members_considered;
      const auto child_slot = leaf.first_child + ordinal;
      const auto sequence_id = index_.children[child_slot];
      bool envelope_compatible = true;
      for (std::uint32_t beacon = 0; beacon < leaf.beacon_count; ++beacon) {
        const auto reference_beacon_distance = index_.child_beacon_distances[
            child_slot * index_.max_beacons + beacon];
        if (std::abs(query_beacon_distances[beacon] -
                     static_cast<int>(reference_beacon_distance)) >
            static_cast<int>(config.tolerance)) {
          envelope_compatible = false;
          break;
        }
      }
      if (!envelope_compatible) {
        ++local_stats.leaf_members_mbb_pruned;
        continue;
      }
      if (!verified.insert(sequence_id).second) continue;
      ++local_stats.exact_verifications;
      const int d = bounded_to_reference(sequence_id, config.tolerance);
      if (d >= 0) hits.push_back({sequence_id, static_cast<std::uint32_t>(d)});
    }
  }
  std::sort(hits.begin(), hits.end(),
            [](const QueryHit& lhs, const QueryHit& rhs) {
              return lhs.sequence_id < rhs.sequence_id;
            });
  local_timings.leaf_scan_ns = elapsed_ns(leaf_scan_start);

  const auto cache_update_start = Clock::now();
  if (path_cache) {
    path_cache->previous_query.assign(sequence);
    path_cache->contained_path = std::move(new_path);
    path_cache->valid = true;
    if (!path_cache->root_anchor_query.empty()) {
      ++path_cache->root_anchor_age;
    }
  }
  local_timings.cache_update_ns = elapsed_ns(cache_update_start);
  const auto accounting_start = Clock::now();
  local_stats.edit_distance_calls = distance_.calls() - calls_at_start;
  const auto categorized_calls =
      local_stats.top_center_edlib_calls +
      local_stats.middle_center_edlib_calls +
      local_stats.leaf_center_edlib_calls +
      local_stats.beacon_edlib_calls +
      local_stats.path_pivot_query_edlib_calls +
      local_stats.path_pivot_row_edlib_calls +
      local_stats.query_anchor_edlib_calls +
      local_stats.leaf_verification_edlib_calls;
  if (categorized_calls > local_stats.edit_distance_calls) {
    throw std::logic_error("categorized edit-distance calls exceed total");
  }
  local_stats.uncategorized_edlib_calls =
      local_stats.edit_distance_calls - categorized_calls;
  if (stats) *stats = local_stats;
  local_timings.accounting_ns = elapsed_ns(accounting_start);
  local_timings.total_ns = elapsed_ns(total_start);
  if (timings) *timings = std::move(local_timings);
  return hits;
}

}  // namespace navigamer
