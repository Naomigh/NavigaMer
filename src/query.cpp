#include "navigamer/query.hpp"
#include "navigamer/prefetch.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <utility>

namespace navigamer {

QueryEngine::QueryEngine(const NavigaMerIndex& index, const SequenceStore& reference)
    : index_(index), reference_(reference) {
  if (index.uses_shared_reference &&
      (reference.window_length() != index.window_length ||
       reference.stride() != index.stride ||
       reference.size() != index.reference_window_count ||
       reference.fingerprint() != index.reference_fingerprint)) {
    throw std::invalid_argument("reference does not match the index (content, contigs or window parameters)");
  }
}

std::vector<QueryHit> QueryEngine::query(
    std::string_view sequence, const QueryConfig& config, PathCache* cache,
    QueryStats* stats) const {
  QueryStats total(index_.layers.size());
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

NodeId QueryEngine::route(
    NodeId parent_id, std::size_t depth, PreparedQuery& query,
    NodeId cached_child, const QueryConfig& config, QueryStats& local,
    RoutingScratch& scratch) const {
  const auto& parent = index_.nodes[parent_id];
  auto& distances = scratch.distances;
  constexpr int kUnknownDistance = -1;
  constexpr int kRejectedDistance = -2;
  distances.assign(parent.child_count, kUnknownDistance);
  auto evaluate_exact = [&](std::uint64_t ordinal) {
    auto& distance = distances[ordinal];
    if (distance < 0) {
      const auto child = index_.children[parent.first_child + ordinal];
      distance = query.distance(index_.center_sequences, child, scratch.center);
      ++local.route_edit_calls[depth];
      ++local.route_candidates[depth];
    }
    return distance;
  };
  std::uint64_t initial = 0;
  if (config.route_mode != RouteMode::kScan && parent.beacon_count != 0) {
    initial = index_.beacon_ordinals[parent.first_beacon];
  }
  if (config.use_cache && cached_child != kNoNode) {
    const auto first = index_.children[parent.first_child];
    if (cached_child >= first && cached_child - first < parent.child_count) {
      initial = cached_child - first;
      ++local.cache_candidates;
    }
  }
  NodeId best_child = index_.children[parent.first_child + initial];
  int best_distance = evaluate_exact(initial);
  auto update = [&](std::uint64_t ordinal, bool exact_required = false) {
    const auto child = index_.children[parent.first_child + ordinal];
    auto& distance = distances[ordinal];
    if (exact_required && distance < 0) {
      distance = evaluate_exact(ordinal);
    } else if (!exact_required && distance == kUnknownDistance) {
      const int max_distance = best_distance - (child < best_child ? 0 : 1);
      if (max_distance < 0) {
        distance = kRejectedDistance;
      } else {
        distance = query.distance(index_.center_sequences, child, scratch.center, max_distance);
        ++local.route_edit_calls[depth];
        ++local.route_candidates[depth];
        if (distance < 0) distance = kRejectedDistance;
      }
    }
    if (distance < 0) return;
    if (distance < best_distance || (distance == best_distance && child < best_child)) {
      best_distance = distance;
      best_child = child;
    }
  };
  if (config.route_mode == RouteMode::kScan) {
    for (std::uint64_t ordinal = 0; ordinal < parent.child_count; ++ordinal) update(ordinal);
    return best_child;
  }
  auto& query_beacon_distances = scratch.beacon_distances;
  query_beacon_distances.resize(parent.beacon_count);
  for (std::uint64_t b = 0; b < parent.beacon_count; ++b) {
    const auto ordinal = index_.beacon_ordinals[parent.first_beacon + b];
    update(ordinal, true);
    query_beacon_distances[b] = distances[ordinal];
  }
  auto cannot_improve = [&](int lower_bound, std::uint64_t ordinal) {
    return lower_bound > best_distance ||
        (lower_bound == best_distance &&
         index_.children[parent.first_child + ordinal] >= best_child);
  };
  auto& candidates = scratch.candidates;
  candidates.clear();
  for (std::uint64_t ordinal = 0; ordinal < parent.child_count; ++ordinal) {
    if (distances[ordinal] != kUnknownDistance) continue;
    int lower_bound = 0;
    for (std::uint64_t b = 0; b < parent.beacon_count; ++b) {
      const auto center_distance = index_.beacon_distances[
          parent.first_distance + ordinal * parent.beacon_count + b];
      lower_bound = std::max(lower_bound,
                             std::abs(query_beacon_distances[b] - center_distance));
    }
    if (cannot_improve(lower_bound, ordinal)) ++local.route_pruned[depth];
    else candidates.push_back({lower_bound, ordinal});
  }
  std::sort(candidates.begin(), candidates.end(), [&](const auto& lhs, const auto& rhs) {
    if (lhs.first != rhs.first) return lhs.first < rhs.first;
    return index_.children[parent.first_child + lhs.second] <
           index_.children[parent.first_child + rhs.second];
  });
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const auto [lower_bound, ordinal] = candidates[i];
    if (cannot_improve(lower_bound, ordinal)) {
      local.route_pruned[depth] += candidates.size() - i;
      break;
    }
    update(ordinal);
  }
  return best_child;
}

std::vector<QueryHit> QueryEngine::query_one(
    std::string_view sequence, bool reverse, const QueryConfig& config,
    PathCache* cache, QueryStats* stats) const {
  QueryStats local(index_.layers.size());
  RoutingScratch scratch;
  PreparedQuery query(sequence);
  NodeId parent = index_.root;
  std::vector<NodeId> path(index_.layers.size());
  for (std::size_t depth = 0; depth < path.size(); ++depth) {
    ++local.route_worlds[depth];
    const auto cached = cache && cache->valid && depth < cache->path.size()
        ? cache->path[depth] : kNoNode;
    const auto& world = index_.nodes[parent];
    if (config.enable_prefetch) {
      index_.center_sequences.prefetch(index_.children[world.first_child], world.child_count);
    }
    path[depth] = route(parent, depth, query, cached, config, local, scratch);
    parent = path[depth];
  }
  auto hits = verify_leaf(parent, sequence, reverse, config, local);
  if (cache) {
    cache->path = path;
    cache->valid = true;
  }
  if (stats) *stats = local;
  return hits;
}

std::vector<QueryHit> QueryEngine::verify_leaf(
    NodeId leaf_id, std::string_view sequence, bool reverse,
    const QueryConfig& config, QueryStats& local) const {
  const auto& leaf = index_.nodes[leaf_id];
  local.leaf_members += leaf.member_count;
  std::vector<QueryHit> hits;
  PreparedQuery query(sequence);
  for (std::uint64_t i = 0; i < leaf.member_count; ++i) {
    const auto member = leaf.first_member + i;
    if (config.enable_prefetch && i + 4 < leaf.member_count) {
      const auto future = index_.member_sequence(member + 4, reference_);
      prefetch_read<1>(future.data(), future.size());
    }
    const int distance = query.distance(index_.member_sequence(member, reference_), config.tolerance);
    ++local.exact_verifications;
    if (distance >= 0) {
      const auto id = index_.leaf_ids[member];
      hits.push_back({id, id, static_cast<std::uint32_t>(distance), reverse});
    }
  }
  return hits;
}

std::vector<std::vector<QueryHit>> QueryEngine::query_batch(
    std::span<const std::string_view> sequences, const QueryConfig& config,
    PathCache* cache, QueryStats* stats, const BatchQueryConfig& batch_config) const {
  if (batch_config.threads == 0 || batch_config.min_queries_per_thread == 0) {
    throw std::invalid_argument("query thread count and minimum queries per thread must be positive");
  }
  struct WorkItem {
    std::string_view sequence;
    std::size_t input;
    bool reverse;
  };
  struct WorldBatch {
    NodeId parent;
    std::vector<std::size_t> items;
  };
  QueryStats local(index_.layers.size());
  if (sequences.empty()) {
    if (stats) *stats = local;
    return {};
  }
  std::vector<std::string> reversed;
  reversed.reserve(config.both_strands ? sequences.size() : 0);
  std::vector<WorkItem> work;
  work.reserve(sequences.size() * (config.both_strands ? 2 : 1));
  for (std::size_t i = 0; i < sequences.size(); ++i) {
    work.push_back({sequences[i], i, false});
    if (config.both_strands) {
      reversed.push_back(reverse_complement(sequences[i]));
      work.push_back({reversed.back(), i, true});
    }
  }
  std::vector<WorldBatch> current{{index_.root, {}}};
  for (std::size_t i = 0; i < work.size(); ++i) current[0].items.push_back(i);
  const PathCache previous = cache ? *cache : PathCache{};
  PathCache final_path;
  final_path.path.resize(index_.layers.size(), kNoNode);
  // Each item has one writer, including separate slots for the two strands.
  std::vector<NodeId> assignments(work.size());
  std::vector<std::vector<QueryHit>> item_hits(batch_config.retain_hits ? work.size() : 0);
  auto participants = [&](std::size_t count) {
    return std::min<std::size_t>(batch_config.threads,
        std::max<std::size_t>(1, count / batch_config.min_queries_per_thread));
  };
  const auto worker_count = participants(work.size());
  struct alignas(64) WorkerState {
    RoutingScratch scratch;
    QueryStats stats;
    std::binary_semaphore start{0};
    std::binary_semaphore finished{0};
  };
  auto states = std::make_unique<WorkerState[]>(worker_count);
  for (std::size_t i = 0; i < worker_count; ++i) {
    states[i].stats = QueryStats(index_.layers.size());
  }
  auto process_range = [&](std::size_t worker_id, const WorldBatch& batch,
                           std::size_t depth, std::size_t part, std::size_t parts) {
    const auto count = batch.items.size();
    const auto per_part = count / parts;
    const auto remainder = count % parts;
    const auto begin = part * per_part + std::min(part, remainder);
    const auto end = begin + per_part + (part < remainder ? 1 : 0);
    auto& scratch = states[worker_id].scratch;
    auto& worker_stats = states[worker_id].stats;
    const auto& parent = index_.nodes[batch.parent];
    const bool routing = depth < index_.layers.size();
    if (routing && config.enable_prefetch) {
      index_.center_sequences.prefetch(index_.children[parent.first_child], parent.child_count);
    }
    NodeId cached_child = routing && previous.valid && depth < previous.path.size()
        ? previous.path[depth] : kNoNode;
    for (auto i = begin; i < end; ++i) {
      if (config.enable_prefetch && i + 4 < end) {
        const auto future = work[batch.items[i + 4]].sequence;
        prefetch_read<1>(future.data(), future.size());
      }
      const auto item = batch.items[i];
      const auto& input = work[item];
      if (routing) {
        PreparedQuery query(input.sequence);
        const auto child = route(batch.parent, depth, query,
                                 cached_child, config, worker_stats, scratch);
        assignments[item] = child;
        cached_child = child;
        if (item == work.size() - 1) final_path.path[depth] = child;
      } else {
        auto hits = verify_leaf(batch.parent, input.sequence, input.reverse, config, worker_stats);
        if (batch_config.retain_hits) item_hits[item] = std::move(hits);
      }
    }
  };
  struct Task {
    std::size_t world;
    std::size_t part;
    std::size_t parts;
  };
  std::vector<Task> tasks;
  std::atomic<std::size_t> next_task{0};
  std::size_t dispatched_depth = 0;
  auto process_tasks = [&](std::size_t worker_id) {
    while (true) {
      const auto ordinal = next_task.fetch_add(1, std::memory_order_relaxed);
      if (ordinal >= tasks.size()) break;
      const auto& task = tasks[ordinal];
      process_range(worker_id, current[task.world], dispatched_depth, task.part, task.parts);
    }
  };
  // Publish a whole layer at once. Small worlds stay on one worker; workers
  // never wait for another small world before claiming their next task.
  // The frontier and task list remain immutable until all workers finish.
  bool stop = false;
  std::vector<std::thread> workers;
  for (std::size_t i = 1; i < worker_count; ++i) {
    workers.emplace_back([&, i] {
      while (true) {
        states[i].start.acquire();
        if (stop) break;
        process_tasks(i);
        states[i].finished.release();
      }
    });
  }
  const auto target_queries = 1 + (work.size() - 1) / worker_count;
  auto run_layer = [&](std::size_t depth) {
    tasks.clear();
    for (std::size_t w = 0; w < current.size(); ++w) {
      const auto count = current[w].items.size();
      const auto parts = std::min(participants(count), 1 + (count - 1) / target_queries);
      for (std::size_t part = 0; part < parts; ++part) tasks.push_back({w, part, parts});
    }
    const auto active = std::min(worker_count, tasks.size());
    dispatched_depth = depth;
    next_task.store(0, std::memory_order_relaxed);
    for (std::size_t i = 1; i < active; ++i) states[i].start.release();
    process_tasks(0);
    for (std::size_t i = 1; i < active; ++i) states[i].finished.acquire();
  };
  for (std::size_t depth = 0; depth < index_.layers.size(); ++depth) {
    states[0].stats.route_worlds[depth] += current.size();
    run_layer(depth);
    std::vector<WorldBatch> next;
    for (const auto& batch : current) {
      const auto& parent = index_.nodes[batch.parent];
      std::vector<std::vector<std::size_t>> buckets(parent.child_count);
      for (const auto item : batch.items) {
        const auto ordinal = assignments[item] - index_.children[parent.first_child];
        buckets[ordinal].push_back(item);
      }
      for (std::size_t c = 0; c < buckets.size(); ++c) {
        if (!buckets[c].empty()) {
          next.push_back({index_.children[parent.first_child + c], std::move(buckets[c])});
        }
      }
    }
    current = std::move(next);
  }
  run_layer(index_.layers.size());
  stop = true;
  for (std::size_t i = 1; i < worker_count; ++i) states[i].start.release();
  for (auto& thread : workers) thread.join();
  for (std::size_t i = 0; i < worker_count; ++i) local += states[i].stats;

  std::vector<std::vector<QueryHit>> results(batch_config.retain_hits ? sequences.size() : 0);
  for (std::size_t item = 0; item < item_hits.size(); ++item) {
    auto& output = results[work[item].input];
    if (!work[item].reverse) output = std::move(item_hits[item]);
    else output.insert(output.end(), item_hits[item].begin(), item_hits[item].end());
  }
  for (auto& hits : results) {
    std::sort(hits.begin(), hits.end(), [](const QueryHit& lhs, const QueryHit& rhs) {
      if (lhs.occurrence_id != rhs.occurrence_id) return lhs.occurrence_id < rhs.occurrence_id;
      if (lhs.reverse != rhs.reverse) return lhs.reverse < rhs.reverse;
      if (lhs.distance != rhs.distance) return lhs.distance < rhs.distance;
      return lhs.unique_id < rhs.unique_id;
    });
  }
  if (cache) {
    final_path.valid = true;
    *cache = final_path;
  }
  if (stats) *stats = local;
  return results;
}

std::vector<QueryHit> brute_force_query(const NavigaMerIndex& index,
                                      const SequenceStore& reference,
                                      std::string_view query,
                                      std::uint32_t tolerance, bool reverse) {
  std::vector<QueryHit> hits;
  auto check = [&](SequenceId id) {
    const int distance = EditDistance::bounded(query, reference.sequence(id),
                                               static_cast<int>(tolerance));
    if (distance >= 0) hits.push_back({id, id, static_cast<std::uint32_t>(distance), reverse});
  };
  if (index.is_scoped()) {
    for (const auto id : index.world_members(index.root)) check(id);
  } else {
    for (SequenceId id = 0; id < index.reference_window_count; ++id) check(id);
  }
  return hits;
}

}  // namespace navigamer
