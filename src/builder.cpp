#include "navigamer/builder.hpp"

#include "navigamer/batch_distance.hpp"
#include "navigamer/edit_distance.hpp"
#include "navigamer/member_classifier.hpp"
#include "navigamer/prefetch.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <thread>

namespace navigamer {
namespace {

using Clock = std::chrono::steady_clock;

struct TempBeaconTable {
  std::vector<std::uint64_t> ordinals;
  std::vector<int> distances;
};

struct TempWorld {
  SequenceId center;
  std::vector<SequenceId> members;
  std::vector<std::size_t> children;
  TempBeaconTable child_beacons;
  std::uint64_t member_count;
};

struct TempSplit {
  std::vector<TempWorld> worlds;
  TempBeaconTable beacons;
};

struct CenterSelection {
  std::vector<SequenceId> ids;
  std::vector<std::size_t> member_ordinals;
  PackedCenters sequences;
};

std::size_t active_workers(std::size_t count, std::uint32_t requested) {
  return std::min<std::size_t>(count, requested);
}

template <typename Function>
void parallel_workers(std::size_t count, std::uint32_t requested,
                      Function function) {
  const auto worker_count = active_workers(count, requested);
  if (worker_count == 0) return;
  std::vector<std::thread> workers;
  std::vector<std::exception_ptr> errors(worker_count);
  auto run = [&](std::size_t worker) {
    try { function(worker, worker_count); }
    catch (...) { errors[worker] = std::current_exception(); }
  };
  workers.reserve(worker_count - 1);
  for (std::size_t worker = 1; worker < worker_count; ++worker) {
    workers.emplace_back(run, worker);
  }
  run(0);
  for (auto& worker : workers) worker.join();
  for (const auto& error : errors) if (error) std::rethrow_exception(error);
}

CenterSelection choose_centers(const SequenceStore& reference,
                               const std::vector<SequenceId>& members,
                               std::uint32_t radius,
                               LayerBuildStats& stats, std::uint32_t threads = 1) {
  CenterSelection selected{{members[0]}, {0}, PackedCenters(reference.window_length())};
  selected.sequences.append(reference.sequence(members[0]));
  if (radius >= reference.window_length()) return selected;
  const bool use_batch = radius > EditDistance::kLeapMaxDistance && BatchDistance::simd_available();
  BatchDistance batch(reference.window_length());
  if (use_batch) batch.append(reference.sequence(members[0]));
  struct Hint {
    std::size_t center = std::numeric_limits<std::size_t>::max();
    std::uint32_t upper = 0;
    std::pair<std::string_view, std::uint64_t> location;
  };
  auto covered = [&](std::size_t i, std::size_t first, std::size_t last,
                     Hint& hint, std::string& scratch, BatchDistance::Workspace& workspace,
                     LayerBuildStats& local) {
    if (first == last) return false;
    const bool valid_hint = hint.center >= first && hint.center < last;
    const auto location = reference.location(members[i]);
    // Same-contig windows separated by s bases differ by at most 2s edits.
    // Compare name addresses, not text: distinct contigs can share a name.
    if (valid_hint && location.first.data() == hint.location.first.data()) {
      const auto shift = location.second >= hint.location.second
          ? location.second - hint.location.second : hint.location.second - location.second;
      if (shift <= (radius - hint.upper) / 2) {
        hint.upper += 2 * shift;
        hint.location = location;
        ++local.center_bound_accepts;
        return true;
      }
    }
    const auto sequence = reference.sequence(members[i]);
    PreparedQuery query(sequence);
    if (valid_hint) {
      ++local.center_edit_calls;
      const auto d = query.distance(selected.sequences, hint.center, scratch, radius);
      if (d >= 0) {
        hint.upper = d;
        hint.location = location;
        ++local.center_hint_hits;
        return true;
      }
    }
    for (std::size_t c = first; c < last; ++c) {
      if (use_batch && c % BatchDistance::kLanes == 0 && last - c >= BatchDistance::kLanes) {
        const auto distances = batch.exact_group(sequence, c / BatchDistance::kLanes, workspace);
        local.center_edit_calls += BatchDistance::kLanes;
        ++local.center_batch_groups;
        for (std::size_t lane = 0; lane < BatchDistance::kLanes; ++lane) {
          if (distances[lane] <= static_cast<int>(radius)) {
            hint = {c + lane, static_cast<std::uint32_t>(distances[lane]), location};
            return true;
          }
        }
        c += BatchDistance::kLanes - 1;
        continue;
      }
      if (valid_hint && c == hint.center) continue;
      ++local.center_edit_calls;
      const auto d = query.distance(selected.sequences, c, scratch, radius);
      if (d >= 0) {
        hint = {c, static_cast<std::uint32_t>(d), location};
        return true;
      }
    }
    return false;
  };
  auto append = [&](std::size_t i, Hint& hint) {
    selected.ids.push_back(members[i]);
    selected.member_ordinals.push_back(i);
    selected.sequences.append(reference.sequence(members[i]));
    if (use_batch) batch.append(reference.sequence(members[i]));
    hint = {selected.ids.size() - 1, 0, reference.location(members[i])};
  };
  Hint serial_hint{0, 0, reference.location(members[0])};
  std::string serial_scratch;
  BatchDistance::Workspace serial_workspace;
  constexpr std::size_t batch_size = 4096;
  if (threads == 1 || members.size() < batch_size) {
    for (std::size_t i = 1; i < members.size(); ++i) {
      if (!covered(i, 0, selected.ids.size(), serial_hint, serial_scratch, serial_workspace, stats)) {
        append(i, serial_hint);
      }
    }
    return selected;
  }

  // Workers check an immutable center snapshot. Only the serial merge adds
  // centers, in input order, after checking survivors against newly added ones.
  const auto worker_count = active_workers(batch_size, threads);
  std::vector<Hint> hints(worker_count);
  std::vector<std::string> scratch(worker_count);
  std::vector<BatchDistance::Workspace> workspaces(worker_count);
  std::vector<LayerBuildStats> worker_stats(worker_count);
  std::vector<std::uint8_t> accepted(batch_size);
  std::barrier sync(static_cast<std::ptrdiff_t>(worker_count));
  std::size_t begin = 1, end = 1, snapshot_count = 0;
  bool stop = false;
  auto scan = [&](std::size_t worker) {
    const auto first = begin + (end - begin) * worker / worker_count;
    const auto last = begin + (end - begin) * (worker + 1) / worker_count;
    for (auto i = first; i < last; ++i) {
      accepted[i - begin] = covered(i, 0, snapshot_count, hints[worker],
                                    scratch[worker], workspaces[worker], worker_stats[worker]);
    }
  };
  std::vector<std::thread> workers;
  for (std::size_t worker = 1; worker < worker_count; ++worker) {
    workers.emplace_back([&, worker] {
      for (;;) {
        sync.arrive_and_wait();
        if (stop) return;
        scan(worker);
        sync.arrive_and_wait();
      }
    });
  }
  for (; begin < members.size(); begin = end) {
    end = begin + std::min(batch_size, members.size() - begin);
    snapshot_count = selected.ids.size();
    sync.arrive_and_wait();
    scan(0);
    sync.arrive_and_wait();
    for (auto i = begin; i < end; ++i) {
      if (!accepted[i - begin] &&
          !covered(i, snapshot_count, selected.ids.size(), serial_hint, serial_scratch, serial_workspace, stats)) {
        append(i, serial_hint);
      }
    }
  }
  stop = true;
  sync.arrive_and_wait();
  for (auto& worker : workers) worker.join();
  for (const auto& local : worker_stats) {
    stats.center_edit_calls += local.center_edit_calls;
    stats.center_bound_accepts += local.center_bound_accepts;
    stats.center_hint_hits += local.center_hint_hits;
    stats.center_batch_groups += local.center_batch_groups;
  }
  return selected;
}

TempBeaconTable make_beacon_table(const PackedCenters& centers,
                                  double ratio, std::uint32_t threads,
                                  LayerBuildStats& stats,
                                  std::optional<std::uint32_t> cover_radius) {
  TempBeaconTable table;
  if (cover_radius) {
    std::string center_scratch, anchor_scratch;
    for (std::size_t c = 0; c < centers.size(); ++c) {
      const auto sequence = centers.decode(c, center_scratch);
      PreparedQuery query(sequence);
      bool covered = false;
      for (const auto anchor : table.ordinals) {
        ++stats.beacon_selection_edit_calls;
        if (query.distance(centers, anchor, anchor_scratch, *cover_radius) >= 0) {
          covered = true;
          break;
        }
      }
      if (!covered) table.ordinals.push_back(c);
    }
  } else {
    const auto count = static_cast<std::size_t>(
        std::ceil(ratio * static_cast<double>(centers.size())));
    table.ordinals.reserve(count);
    for (std::size_t b = 0; b < count; ++b) {
      table.ordinals.push_back(b * centers.size() / count);
    }
  }
  const auto beacon_count = table.ordinals.size();
  stats.selected_beacons += beacon_count;
  if (beacon_count == 0) return table;
  table.distances.resize(centers.size() * beacon_count);
  parallel_workers(centers.size(), threads,
                   [&](std::size_t worker, std::size_t worker_count) {
    std::string row_scratch, beacon_scratch;
    centers.prefetch(0, centers.size());
    const auto begin = centers.size() * worker / worker_count;
    const auto end = centers.size() * (worker + 1) / worker_count;
    for (std::size_t row = begin; row < end; ++row) {
      const auto sequence = centers.decode(row, row_scratch);
      PreparedQuery query(sequence);
      for (std::size_t b = 0; b < beacon_count; ++b) {
        table.distances[row * beacon_count + b] =
            query.distance(centers, table.ordinals[b], beacon_scratch);
      }
    }
  });
  stats.beacon_table_edit_calls += centers.size() * beacon_count;
  return table;
}

struct WorkerClassificationStats {
  std::uint64_t member_beacon_edit_calls{0};
  std::uint64_t candidate_edit_calls{0};
  std::uint64_t candidate_batch_groups{0};
  std::uint64_t center_pairs{0};
  std::uint64_t pruned_pairs{0};
  std::uint64_t members{0};
  std::uint64_t repeated{0};
};

std::vector<std::vector<SequenceId>> classify_members(
    const SequenceStore& reference, const std::vector<SequenceId>& members,
    const CenterSelection& centers, const TempBeaconTable& table,
    const BuildConfig& config, LayerBuildStats& stats) {
  const bool use_batch = BatchDistance::simd_available();
  BatchDistance batch(reference.window_length());
  if (use_batch || config.classification_mode == ClassificationMode::kAdjacent) {
    for (const auto id : centers.ids) batch.append(reference.sequence(id));
  }
  const auto no_center = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> exclusive_world;
  if (config.classification_mode == ClassificationMode::kBeacon) {
    exclusive_world.assign(members.size(), no_center);
    for (std::size_t c = 0; c < centers.ids.size(); ++c) {
      exclusive_world[centers.member_ordinals[c]] = c;
    }
  }

  const auto worker_count = active_workers(members.size(), config.threads);
  // Each worker streams one contiguous member range into its own child buckets.
  // Merging ranges in worker order preserves the original membership order.
  std::vector<std::vector<std::vector<SequenceId>>> worker_buckets(
      worker_count, std::vector<std::vector<SequenceId>>(centers.ids.size()));
  std::vector<WorkerClassificationStats> worker_stats(worker_count);
  parallel_workers(members.size(), config.threads,
                   [&](std::size_t worker, std::size_t workers) {
    auto& local = worker_stats[worker];
    auto& buckets = worker_buckets[worker];
    BatchDistance::Workspace workspace;
    std::string center_scratch;
    const auto begin = members.size() * worker / workers;
    const auto end = members.size() * (worker + 1) / workers;
    const auto nc = centers.ids.size(), nb = table.ordinals.size();
    if (config.classification_mode == ClassificationMode::kAdjacent) {
      AdjacentClassifier classifier(reference, centers.ids, batch, config.max_tolerance);
      for (auto i = begin; i < end; ++i) {
        const auto before = classifier.exact_pairs;
        const auto exclusive_before = classifier.exclusive_members;
        const auto assignment = classifier.classify(members[i]);
        for (std::size_t c = 0; c < nc; ++c) {
          if (assignment[c]) buckets[c].push_back(members[i]);
        }
        if (classifier.exclusive_members == exclusive_before) {
          ++local.members;
          local.center_pairs += nc;
          local.pruned_pairs += nc - (classifier.exact_pairs - before);
        }
      }
      local.candidate_edit_calls = classifier.exact_pairs;
      local.candidate_batch_groups = classifier.batch_groups;
      local.repeated = classifier.repeated;
      return;
    }
    constexpr std::size_t query_tile = 8, center_tile = 32;
    std::array<std::string_view, query_tile> sequences;
    std::vector<int> distances(query_tile * nc), anchors(query_tile * nb);
    std::vector<int> upper(query_tile);
    std::vector<PreparedQuery> queries;
    queries.reserve(query_tile);
    for (auto start = begin; start < end; start += query_tile) {
      const auto count = std::min(query_tile, end - start);
      queries.clear();
      std::fill(distances.begin(), distances.end(), -1);
      std::fill(upper.begin(), upper.end(), std::numeric_limits<int>::max());
      for (std::size_t q = 0; q < count; ++q) {
        sequences[q] = reference.sequence(members[start + q]);
        queries.emplace_back(sequences[q]);
        if (exclusive_world[start + q] != no_center) continue;
        ++local.members;
        local.center_pairs += nc;
        for (std::size_t b = 0; b < nb; ++b) {
          const auto c = table.ordinals[b];
          const auto d = queries[q].distance(centers.sequences, c, center_scratch);
          anchors[q * nb + b] = distances[q * nc + c] = d;
          upper[q] = std::min(upper[q], d);
          ++local.member_beacon_edit_calls;
        }
      }
      // A small center block is reused by all queries before moving on.
      for (std::size_t first = 0; first < nc; first += center_tile) {
        const auto last = std::min(nc, first + center_tile);
        centers.sequences.prefetch(first, last - first);
        for (std::size_t q = 0; q < count; ++q) {
          if (exclusive_world[start + q] != no_center) continue;
          for (auto c = first; c < last; ++c) {
            // Batch only complete, uncomputed groups with no pruned lanes.
            // Small bounds keep LEAP; isolated candidates keep scalar Edlib.
            const int group_limit = upper[q] == std::numeric_limits<int>::max()
                ? -1 : upper[q] + 2 * static_cast<int>(config.max_tolerance);
            if (use_batch && c % BatchDistance::kLanes == 0 &&
                last - c >= BatchDistance::kLanes &&
                (group_limit < 0 || group_limit > EditDistance::kLeapMaxDistance)) {
              bool eligible = true;
              for (std::size_t lane = 0; lane < BatchDistance::kLanes && eligible; ++lane) {
                eligible = distances[q * nc + c + lane] < 0;
                for (std::size_t b = 0; b < nb && eligible && group_limit >= 0; ++b) {
                  eligible = std::abs(anchors[q * nb + b] - table.distances[(c + lane) * nb + b]) <= group_limit;
                }
              }
              if (eligible) {
                const auto values = batch.exact_group(sequences[q], c / BatchDistance::kLanes, workspace);
                for (std::size_t lane = 0; lane < BatchDistance::kLanes; ++lane) {
                  distances[q * nc + c + lane] = values[lane];
                  upper[q] = std::min(upper[q], values[lane]);
                }
                local.candidate_edit_calls += BatchDistance::kLanes;
                ++local.candidate_batch_groups;
                c += BatchDistance::kLanes - 1;
                continue;
              }
            }
            auto& d = distances[q * nc + c];
            if (d >= 0) continue;
            int lower = 0;
            for (std::size_t b = 0; b < nb; ++b) {
              lower = std::max(lower, std::abs(anchors[q * nb + b] - table.distances[c * nb + b]));
            }
            const int limit = upper[q] == std::numeric_limits<int>::max()
                ? -1 : upper[q] + 2 * static_cast<int>(config.max_tolerance);
            if (limit >= 0 && lower > limit) { ++local.pruned_pairs; continue; }
            d = queries[q].distance(centers.sequences, c, center_scratch, limit);
            if (d >= 0) upper[q] = std::min(upper[q], d);
            ++local.candidate_edit_calls;
          }
        }
      }
      for (std::size_t q = 0; q < count; ++q) {
        const auto i = start + q;
        if (exclusive_world[i] != no_center) {
          buckets[exclusive_world[i]].push_back(members[i]);
          continue;
        }
        const int limit = upper[q] + 2 * static_cast<int>(config.max_tolerance);
        for (std::size_t c = 0; c < nc; ++c) {
          const int d = distances[q * nc + c];
          if (d >= 0 && d <= limit) buckets[c].push_back(members[i]);
        }
      }
    }
  });
  for (const auto& local : worker_stats) {
    stats.member_beacon_edit_calls += local.member_beacon_edit_calls;
    stats.candidate_edit_calls += local.candidate_edit_calls;
    stats.candidate_batch_groups += local.candidate_batch_groups;
    stats.classification_center_pairs += local.center_pairs;
    stats.pruned_center_pairs += local.pruned_pairs;
    stats.classified_members += local.members;
    stats.classification_repeated += local.repeated;
  }
  std::vector<std::vector<SequenceId>> buckets(centers.ids.size());
  for (std::size_t c = 0; c < centers.ids.size(); ++c) {
    std::size_t count = 0;
    for (const auto& local : worker_buckets) count += local[c].size();
    buckets[c].reserve(count);
    for (const auto& local : worker_buckets) {
      buckets[c].insert(buckets[c].end(), local[c].begin(), local[c].end());
    }
  }
  return buckets;
}

TempSplit split_parent(const SequenceStore& reference,
                       const std::vector<SequenceId>& members,
                       std::size_t layer,
                       const BuildConfig& config, LayerBuildStats& stats) {
  const auto center_start = Clock::now();
  auto centers = choose_centers(reference, members, config.center_radii[layer], stats, config.threads);
  stats.center_selection_seconds +=
      std::chrono::duration<double>(Clock::now() - center_start).count();

  const auto beacon_start = Clock::now();
  const auto cover_radius = config.beacon_cover_radii
      ? std::optional<std::uint32_t>((*config.beacon_cover_radii)[layer]) : std::nullopt;
  const auto ratio = config.beacon_ratios.empty() ? 0.1 : config.beacon_ratios[layer];
  auto table = make_beacon_table(centers.sequences, ratio,
                                 config.threads, stats, cover_radius);
  stats.beacon_table_seconds +=
      std::chrono::duration<double>(Clock::now() - beacon_start).count();

  const auto classification_start = Clock::now();
  auto buckets = classify_members(reference, members, centers, table, config, stats);
  stats.classification_seconds +=
      std::chrono::duration<double>(Clock::now() - classification_start).count();

  TempSplit result;
  result.beacons = std::move(table);
  result.worlds.reserve(centers.ids.size());
  for (std::size_t c = 0; c < centers.ids.size(); ++c) {
    result.worlds.push_back({centers.ids[c], std::move(buckets[c]), {}, {}, 0});
  }
  for (auto& world : result.worlds) world.member_count = world.members.size();
  return result;
}

void persist_beacon_table(NavigaMerIndex& index, NodeId parent_id,
                          const TempBeaconTable& table) {
  auto& parent = index.nodes[parent_id];
  parent.first_beacon = index.beacon_ordinals.size();
  parent.beacon_count = table.ordinals.size();
  index.beacon_ordinals.insert(index.beacon_ordinals.end(),
                               table.ordinals.begin(), table.ordinals.end());
  parent.first_distance = index.beacon_distances.size();
  index.beacon_distances.insert(index.beacon_distances.end(),
                                table.distances.begin(), table.distances.end());
}

void validate_build_config(const SequenceStore& reference, const BuildConfig& config) {
  if (reference.size() == 0) {
    throw std::invalid_argument("cannot build an index from an empty reference");
  }
  if (config.threads == 0) {
    throw std::invalid_argument("build thread count must be positive");
  }
  const auto layer_count = config.center_radii.size();
  if (layer_count == 0 || layer_count > kRootLayer) {
    throw std::invalid_argument("layer count must be between 1 and 255");
  }
  if (!config.beacon_ratios.empty() && config.beacon_ratios.size() != layer_count) {
    throw std::invalid_argument("beacon ratios must contain one value per layer");
  }
  if (config.beacon_cover_radii && config.beacon_cover_radii->size() != layer_count) {
    throw std::invalid_argument("beacon cover radii must contain one value per layer");
  }
  for (const double ratio : config.beacon_ratios) {
    if (!std::isfinite(ratio) || ratio < 0.0 || ratio > 1.0) {
      throw std::invalid_argument("beacon ratios must be between 0 and 1");
    }
  }
}

NavigaMerIndex build_scope(const SequenceStore& reference,
                           const std::vector<SequenceId>& members,
                           SequenceId root_center, const BuildConfig& config,
                           BuildStats* build_stats) {
  validate_build_config(reference, config);
  const auto layer_count = config.center_radii.size();
  BuildStats local_stats;
  BuildStats& stats = build_stats ? *build_stats : local_stats;
  stats = {};
  stats.layers.resize(layer_count);

  if (members.empty()) throw std::invalid_argument("cannot build an empty world");
  auto large_split = split_parent(reference, members, 0, config, stats.layers[0]);
  auto root_beacons = std::move(large_split.beacons);
  std::vector<std::vector<TempWorld>> worlds(layer_count);
  worlds[0] = std::move(large_split.worlds);

  auto split_layer = [&](std::vector<TempWorld>& parents, std::size_t layer) {
    std::vector<TempSplit> splits(parents.size());
    std::vector<LayerBuildStats> parent_stats(parents.size());
    std::atomic<std::size_t> next{0};
    auto inner_config = config;
    inner_config.threads = config.threads / active_workers(parents.size(), config.threads);
    parallel_workers(parents.size(), config.threads, [&](std::size_t, std::size_t) {
      for (;;) {
        const auto i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= parents.size()) break;
        splits[i] = split_parent(reference, parents[i].members, layer,
                                 inner_config, parent_stats[i]);
        std::vector<SequenceId>{}.swap(parents[i].members);
      }
    });
    std::vector<TempWorld> children;
    auto& total = stats.layers[layer];
    for (std::size_t i = 0; i < parents.size(); ++i) {
      auto& parent = parents[i];
      parent.child_beacons = std::move(splits[i].beacons);
      for (auto& child : splits[i].worlds) {
        parent.children.push_back(children.size());
        children.push_back(std::move(child));
      }
      const auto& s = parent_stats[i];
      total.center_selection_seconds += s.center_selection_seconds;
      total.beacon_table_seconds += s.beacon_table_seconds;
      total.classification_seconds += s.classification_seconds;
      total.center_edit_calls += s.center_edit_calls;
      total.center_bound_accepts += s.center_bound_accepts;
      total.center_hint_hits += s.center_hint_hits;
      total.center_batch_groups += s.center_batch_groups;
      total.beacon_selection_edit_calls += s.beacon_selection_edit_calls;
      total.selected_beacons += s.selected_beacons;
      total.beacon_table_edit_calls += s.beacon_table_edit_calls;
      total.member_beacon_edit_calls += s.member_beacon_edit_calls;
      total.candidate_edit_calls += s.candidate_edit_calls;
      total.candidate_batch_groups += s.candidate_batch_groups;
      total.classification_center_pairs += s.classification_center_pairs;
      total.pruned_center_pairs += s.pruned_center_pairs;
      total.classified_members += s.classified_members;
      total.classification_repeated += s.classification_repeated;
    }
    return children;
  };
  for (std::size_t layer = 1; layer < layer_count; ++layer) {
    worlds[layer] = split_layer(worlds[layer - 1], layer);
  }

  NavigaMerIndex index;
  index.window_length = reference.window_length();
  index.stride = reference.stride();
  index.max_tolerance = config.max_tolerance;
  index.reference_window_count = reference.size();
  index.reference_fingerprint = reference.fingerprint();
  std::size_t node_count = 1;
  for (std::size_t layer = 0; layer < layer_count; ++layer) {
    index.layers.push_back({node_count, worlds[layer].size(), config.center_radii[layer]});
    node_count += worlds[layer].size();
  }
  index.nodes.resize(node_count);
  index.center_sequences = PackedCenters(reference.window_length());
  // Retain a root slot so packed center IDs equal node IDs. Scoped roots keep
  // the selected world's center; it does not seed greedy child selection.
  index.center_sequences.append(root_center == kNoSequence
      ? std::string(reference.window_length(), 'A') : std::string(reference.sequence(root_center)));
  index.nodes[0].center_sequence_id = root_center;
  index.nodes[0].member_count = members.size();
  index.nodes[0].child_count = worlds[0].size();
  for (std::size_t i = 0; i < worlds[0].size(); ++i) index.children.push_back(1 + i);

  auto copy_world = [&](const TempWorld& world, NodeId id, std::uint8_t layer) {
    auto& node = index.nodes[id];
    node.center_sequence_id = world.center;
    node.member_count = world.member_count;
    node.layer = layer;
    index.center_sequences.append(reference.sequence(world.center));
    if (layer + 1u == layer_count) {
      node.first_member = index.leaf_ids.size();
      for (const auto member : world.members) {
        index.leaf_ids.push_back(member);
      }
    } else {
      node.first_child = index.children.size();
      node.child_count = world.children.size();
      for (const auto child : world.children) {
        index.children.push_back(index.layers[layer + 1].first_node + child);
      }
    }
  };
  for (std::size_t layer = 0; layer < layer_count; ++layer) {
    for (std::size_t i = 0; i < worlds[layer].size(); ++i) {
      copy_world(worlds[layer][i], index.layers[layer].first_node + i,
                 static_cast<std::uint8_t>(layer));
    }
  }

  persist_beacon_table(index, index.root, root_beacons);
  for (std::size_t layer = 0; layer + 1 < layer_count; ++layer) {
    for (std::size_t i = 0; i < worlds[layer].size(); ++i) {
      persist_beacon_table(index, index.layers[layer].first_node + i,
                           worlds[layer][i].child_beacons);
    }
  }
  return index;
}

void validate_input(const SequenceStore& reference, const NavigaMerIndex& input,
                    const BuildConfig& config) {
  if (!input.uses_shared_reference) {
    throw std::invalid_argument("continuation requires a reference-backed v5 index");
  }
  if (reference.size() != input.reference_window_count ||
      reference.fingerprint() != input.reference_fingerprint ||
      reference.window_length() != input.window_length || reference.stride() != input.stride) {
    throw std::invalid_argument("reference does not match the input index");
  }
  if (config.max_tolerance != input.max_tolerance) {
    throw std::invalid_argument("continuation must retain the input build tolerance");
  }
  if (input.layers.empty() || input.root >= input.nodes.size()) {
    throw std::invalid_argument("input index has no world hierarchy");
  }
}

void accumulate_stats(LayerBuildStats& total, const LayerBuildStats& value) {
  total.center_selection_seconds += value.center_selection_seconds;
  total.beacon_table_seconds += value.beacon_table_seconds;
  total.classification_seconds += value.classification_seconds;
  total.center_edit_calls += value.center_edit_calls;
  total.center_bound_accepts += value.center_bound_accepts;
  total.center_hint_hits += value.center_hint_hits;
  total.center_batch_groups += value.center_batch_groups;
  total.beacon_selection_edit_calls += value.beacon_selection_edit_calls;
  total.selected_beacons += value.selected_beacons;
  total.beacon_table_edit_calls += value.beacon_table_edit_calls;
  total.member_beacon_edit_calls += value.member_beacon_edit_calls;
  total.candidate_edit_calls += value.candidate_edit_calls;
  total.candidate_batch_groups += value.candidate_batch_groups;
  total.classification_center_pairs += value.classification_center_pairs;
  total.pruned_center_pairs += value.pruned_center_pairs;
  total.classified_members += value.classified_members;
  total.classification_repeated += value.classification_repeated;
}

}  // namespace

NavigaMerIndex IndexBuilder::build(const SequenceStore& reference,
                                   const BuildConfig& config, BuildStats* stats) {
  validate_build_config(reference, config);
  std::vector<SequenceId> members(reference.size());
  std::iota(members.begin(), members.end(), 0);
  return build_scope(reference, members, kNoSequence, config, stats);
}

NavigaMerIndex IndexBuilder::rebuild_world(const SequenceStore& reference,
                                           const NavigaMerIndex& input, NodeId world,
                                           const BuildConfig& config, BuildStats* stats) {
  validate_input(reference, input, config);
  validate_build_config(reference, config);
  const auto members = input.world_members(world);
  return build_scope(reference, members, input.nodes[world].center_sequence_id, config, stats);
}

NavigaMerIndex IndexBuilder::extend(const SequenceStore& reference,
                                    const NavigaMerIndex& input,
                                    const BuildConfig& config, BuildStats* build_stats) {
  validate_input(reference, input, config);
  validate_build_config(reference, config);
  if (config.center_radii.empty() || input.layers.size() + config.center_radii.size() > kRootLayer) {
    throw std::invalid_argument("extended layer count must be between 1 and 255");
  }
  const auto& leaves = input.layers.back();
  if (leaves.node_count == 0) throw std::invalid_argument("input index has no leaf worlds");
  std::vector<NavigaMerIndex> subtrees(leaves.node_count);
  std::vector<BuildStats> parent_stats(leaves.node_count);
  auto inner_config = config;
  inner_config.threads = config.threads / active_workers(leaves.node_count, config.threads);
  std::atomic<std::size_t> next{0};
  parallel_workers(leaves.node_count, config.threads, [&](std::size_t, std::size_t) {
    for (;;) {
      const auto parent = next.fetch_add(1, std::memory_order_relaxed);
      if (parent >= leaves.node_count) break;
      subtrees[parent] = rebuild_world(reference, input, leaves.first_node + parent,
                                       inner_config, &parent_stats[parent]);
    }
  });
  BuildStats local_stats;
  auto& stats = build_stats ? *build_stats : local_stats;
  stats = {};
  stats.layers.resize(config.center_radii.size());
  for (const auto& value : parent_stats) {
    for (std::size_t layer = 0; layer < stats.layers.size(); ++layer) {
      accumulate_stats(stats.layers[layer], value.layers[layer]);
    }
  }

  NavigaMerIndex result;
  result.window_length = input.window_length;
  result.stride = input.stride;
  result.max_tolerance = input.max_tolerance;
  result.reference_window_count = input.reference_window_count;
  result.reference_fingerprint = input.reference_fingerprint;
  result.root = input.root;
  result.layers = input.layers;
  result.nodes = input.nodes;
  result.center_sequences = input.center_sequences;
  result.children = input.children;
  result.beacon_ordinals = input.beacon_ordinals;
  result.beacon_distances = input.beacon_distances;
  // Canonical BFS node/storage order makes staged and one-shot builds identical.
  std::vector<std::vector<NodeId>> mapping(subtrees.size());
  for (std::size_t p = 0; p < subtrees.size(); ++p) {
    mapping[p].resize(subtrees[p].nodes.size());
    mapping[p][0] = leaves.first_node + p;
    result.nodes[leaves.first_node + p].first_member = 0;
  }
  for (std::size_t layer = 0; layer < config.center_radii.size(); ++layer) {
    const auto first = result.nodes.size();
    for (std::size_t p = 0; p < subtrees.size(); ++p) {
      const auto& sub = subtrees[p];
      const auto& info = sub.layers[layer];
      for (NodeId id = info.first_node; id < info.first_node + info.node_count; ++id) {
        mapping[p][id] = result.nodes.size();
        auto node = sub.nodes[id];
        node.layer = input.layers.size() + layer;
        result.nodes.push_back(node);
        result.center_sequences.append(reference.sequence(node.center_sequence_id));
      }
    }
    result.layers.push_back({first, result.nodes.size() - first, config.center_radii[layer]});
  }
  auto copy_parent = [&](std::size_t p, NodeId id) {
    const auto& sub = subtrees[p];
    const auto& source = sub.nodes[id];
    auto& target = result.nodes[mapping[p][id]];
    if (sub.is_leaf(id)) {
      target.first_member = result.leaf_ids.size();
      const auto members = sub.members(id);
      result.leaf_ids.insert(result.leaf_ids.end(), members.begin(), members.end());
      return;
    }
    target.first_child = result.children.size();
    target.child_count = source.child_count;
    for (std::uint64_t c = 0; c < source.child_count; ++c) {
      result.children.push_back(mapping[p][sub.children[source.first_child + c]]);
    }
    TempBeaconTable table;
    table.ordinals.assign(sub.beacon_ordinals.begin() + source.first_beacon,
                          sub.beacon_ordinals.begin() + source.first_beacon + source.beacon_count);
    table.distances.assign(sub.beacon_distances.begin() + source.first_distance,
        sub.beacon_distances.begin() + source.first_distance + source.child_count * source.beacon_count);
    persist_beacon_table(result, mapping[p][id], table);
  };
  for (std::size_t p = 0; p < subtrees.size(); ++p) copy_parent(p, 0);
  for (std::size_t layer = 0; layer < config.center_radii.size(); ++layer) {
    for (std::size_t p = 0; p < subtrees.size(); ++p) {
      const auto& info = subtrees[p].layers[layer];
      for (NodeId id = info.first_node; id < info.first_node + info.node_count; ++id) copy_parent(p, id);
    }
  }
  return result;
}

}  // namespace navigamer
