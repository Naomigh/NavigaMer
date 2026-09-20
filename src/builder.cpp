#include "navigamer/builder.hpp"

#include "navigamer/edit_distance.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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
  workers.reserve(worker_count - 1);
  for (std::size_t worker = 1; worker < worker_count; ++worker) {
    workers.emplace_back(function, worker, worker_count);
  }
  function(0, worker_count);
  for (auto& worker : workers) worker.join();
}

CenterSelection choose_centers(const SequenceStore& reference,
                               const std::vector<SequenceId>& members,
                               std::uint32_t radius,
                               LayerBuildStats& stats) {
  CenterSelection selected{{members[0]}, {0}};
  for (std::size_t i = 1; i < members.size(); ++i) {
    bool covered = false;
    for (const auto center : selected.ids) {
      ++stats.center_edit_calls;
      if (EditDistance::exact(reference.sequence(members[i]),
                              reference.sequence(center)) <=
          static_cast<int>(radius)) {
        covered = true;
        break;
      }
    }
    if (!covered) {
      selected.ids.push_back(members[i]);
      selected.member_ordinals.push_back(i);
    }
  }
  return selected;
}

TempBeaconTable make_beacon_table(const SequenceStore& reference,
                                  const std::vector<SequenceId>& centers,
                                  double ratio, std::uint32_t threads,
                                  LayerBuildStats& stats) {
  TempBeaconTable table;
  const auto beacon_count = static_cast<std::size_t>(
      std::ceil(ratio * static_cast<double>(centers.size())));
  if (beacon_count == 0) return table;
  table.ordinals.reserve(beacon_count);
  for (std::size_t b = 0; b < beacon_count; ++b) {
    table.ordinals.push_back(b * centers.size() / beacon_count);
  }
  table.distances.resize(centers.size() * beacon_count);
  parallel_workers(centers.size(), threads,
                   [&](std::size_t worker, std::size_t worker_count) {
    for (std::size_t row = worker; row < centers.size(); row += worker_count) {
      for (std::size_t b = 0; b < beacon_count; ++b) {
        table.distances[row * beacon_count + b] = EditDistance::exact(
            reference.sequence(centers[row]),
            reference.sequence(centers[table.ordinals[b]]));
      }
    }
  });
  stats.beacon_table_edit_calls += centers.size() * beacon_count;
  return table;
}

struct WorkerClassificationStats {
  std::uint64_t member_beacon_edit_calls{0};
  std::uint64_t candidate_edit_calls{0};
  std::uint64_t center_pairs{0};
  std::uint64_t pruned_pairs{0};
  std::uint64_t members{0};
};

std::vector<std::vector<std::size_t>> classify_members(
    const SequenceStore& reference, const std::vector<SequenceId>& members,
    const CenterSelection& centers, const TempBeaconTable& table,
    const BuildConfig& config, LayerBuildStats& stats) {
  const auto no_center = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> exclusive_world(members.size(), no_center);
  for (std::size_t c = 0; c < centers.ids.size(); ++c) {
    exclusive_world[centers.member_ordinals[c]] = c;
  }

  std::vector<std::vector<std::size_t>> assignments(members.size());
  const auto worker_count = active_workers(members.size(), config.threads);
  std::vector<WorkerClassificationStats> worker_stats(worker_count);
  parallel_workers(members.size(), config.threads,
                   [&](std::size_t worker, std::size_t workers) {
    auto& local = worker_stats[worker];
    std::vector<int> distances(centers.ids.size());
    std::vector<int> query_beacon_distances(table.ordinals.size());
    for (std::size_t i = worker; i < members.size(); i += workers) {
      if (exclusive_world[i] != no_center) {
        assignments[i].push_back(exclusive_world[i]);
        continue;
      }

      ++local.members;
      local.center_pairs += centers.ids.size();
      std::fill(distances.begin(), distances.end(), -1);
      const auto sequence = reference.sequence(members[i]);
      int upper_bound = std::numeric_limits<int>::max();
      for (std::size_t b = 0; b < table.ordinals.size(); ++b) {
        const auto ordinal = table.ordinals[b];
        const int distance = EditDistance::exact(
            sequence, reference.sequence(centers.ids[ordinal]));
        query_beacon_distances[b] = distance;
        distances[ordinal] = distance;
        upper_bound = std::min(upper_bound, distance);
        ++local.member_beacon_edit_calls;
      }

      for (std::size_t c = 0; c < centers.ids.size(); ++c) {
        if (distances[c] >= 0) continue;
        if (!table.ordinals.empty()) {
          int lower_bound = 0;
          for (std::size_t b = 0; b < table.ordinals.size(); ++b) {
            lower_bound = std::max(
                lower_bound,
                std::abs(query_beacon_distances[b] -
                         table.distances[c * table.ordinals.size() + b]));
          }
          if (lower_bound >
              upper_bound + 2 * static_cast<int>(config.max_tolerance)) {
            ++local.pruned_pairs;
            continue;
          }
        }
        distances[c] = EditDistance::exact(
            sequence, reference.sequence(centers.ids[c]));
        ++local.candidate_edit_calls;
      }

      const int nearest = *std::min_element(
          distances.begin(), distances.end(), [](int lhs, int rhs) {
            if (lhs < 0) return false;
            if (rhs < 0) return true;
            return lhs < rhs;
          });
      const int assignment_limit =
          nearest + 2 * static_cast<int>(config.max_tolerance);
      for (std::size_t c = 0; c < centers.ids.size(); ++c) {
        if (distances[c] >= 0 && distances[c] <= assignment_limit) {
          assignments[i].push_back(c);
        }
      }
    }
  });
  for (const auto& local : worker_stats) {
    stats.member_beacon_edit_calls += local.member_beacon_edit_calls;
    stats.candidate_edit_calls += local.candidate_edit_calls;
    stats.classification_center_pairs += local.center_pairs;
    stats.pruned_center_pairs += local.pruned_pairs;
    stats.classified_members += local.members;
  }
  return assignments;
}

TempSplit split_parent(const SequenceStore& reference,
                       const std::vector<SequenceId>& members,
                       std::uint32_t radius, double beacon_ratio,
                       const BuildConfig& config, LayerBuildStats& stats) {
  const auto center_start = Clock::now();
  auto centers = choose_centers(reference, members, radius, stats);
  stats.center_selection_seconds +=
      std::chrono::duration<double>(Clock::now() - center_start).count();

  const auto beacon_start = Clock::now();
  auto table = make_beacon_table(reference, centers.ids, beacon_ratio,
                                 config.threads, stats);
  stats.beacon_table_seconds +=
      std::chrono::duration<double>(Clock::now() - beacon_start).count();

  const auto classification_start = Clock::now();
  auto assignments = classify_members(reference, members, centers, table,
                                      config, stats);
  stats.classification_seconds +=
      std::chrono::duration<double>(Clock::now() - classification_start).count();

  TempSplit result;
  result.beacons = std::move(table);
  result.worlds.reserve(centers.ids.size());
  for (const auto center : centers.ids) {
    result.worlds.push_back({center, {}, {}, {}, 0});
  }
  for (std::size_t i = 0; i < members.size(); ++i) {
    for (const auto world : assignments[i]) {
      result.worlds[world].members.push_back(members[i]);
    }
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

}  // namespace

NavigaMerIndex IndexBuilder::build(const SequenceStore& reference,
                                   const BuildConfig& config,
                                   BuildStats* build_stats) {
  if (reference.size() == 0) {
    throw std::invalid_argument("cannot build an index from an empty reference");
  }
  if (config.threads == 0) {
    throw std::invalid_argument("build thread count must be positive");
  }
  for (const double ratio : config.beacon_ratios) {
    if (!std::isfinite(ratio) || ratio < 0.0 || ratio > 1.0) {
      throw std::invalid_argument("beacon ratios must be between 0 and 1");
    }
  }
  BuildStats local_stats;
  BuildStats& stats = build_stats ? *build_stats : local_stats;
  stats = {};

  std::vector<SequenceId> all(reference.size());
  std::iota(all.begin(), all.end(), 0);
  auto large_split = split_parent(reference, all, config.center_radii[0],
                                  config.beacon_ratios[0], config,
                                  stats.layers[0]);
  auto root_beacons = std::move(large_split.beacons);
  auto large = std::move(large_split.worlds);

  std::vector<TempWorld> middle;
  for (auto& parent : large) {
    auto split = split_parent(reference, parent.members,
                              config.center_radii[1], config.beacon_ratios[1],
                              config, stats.layers[1]);
    parent.child_beacons = std::move(split.beacons);
    for (auto& child : split.worlds) {
      parent.children.push_back(middle.size());
      middle.push_back(std::move(child));
    }
    std::vector<SequenceId>{}.swap(parent.members);
  }

  std::vector<TempWorld> small;
  for (auto& parent : middle) {
    auto split = split_parent(reference, parent.members,
                              config.center_radii[2], config.beacon_ratios[2],
                              config, stats.layers[2]);
    parent.child_beacons = std::move(split.beacons);
    for (auto& child : split.worlds) {
      parent.children.push_back(small.size());
      small.push_back(std::move(child));
    }
    std::vector<SequenceId>{}.swap(parent.members);
  }

  NavigaMerIndex index;
  index.window_length = reference.window_length();
  index.stride = reference.stride();
  index.max_tolerance = config.max_tolerance;
  index.reference_window_count = reference.size();
  index.layers[0] = {1, large.size(), config.center_radii[0]};
  index.layers[1] = {1 + large.size(), middle.size(), config.center_radii[1]};
  index.layers[2] = {1 + large.size() + middle.size(), small.size(),
                     config.center_radii[2]};
  index.nodes.resize(1 + large.size() + middle.size() + small.size());
  index.center_sequences.resize(index.nodes.size());
  index.nodes[0].member_count = reference.size();
  index.nodes[0].child_count = large.size();
  for (std::size_t i = 0; i < large.size(); ++i) index.children.push_back(1 + i);

  auto copy_world = [&](const TempWorld& world, NodeId id, std::uint8_t layer) {
    auto& node = index.nodes[id];
    node.center_sequence_id = world.center;
    node.member_count = world.member_count;
    node.layer = layer;
    index.center_sequences[id] = reference.sequence(world.center);
    if (layer == 2) {
      node.first_member = index.leaf_ids.size();
      for (const auto member : world.members) {
        index.leaf_ids.push_back(member);
        index.leaf_sequences.emplace_back(reference.sequence(member));
      }
    } else {
      node.first_child = index.children.size();
      node.child_count = world.children.size();
      for (const auto child : world.children) {
        index.children.push_back(index.layers[layer + 1].first_node + child);
      }
    }
  };
  for (std::size_t i = 0; i < large.size(); ++i) {
    copy_world(large[i], index.layers[0].first_node + i, 0);
  }
  for (std::size_t i = 0; i < middle.size(); ++i) {
    copy_world(middle[i], index.layers[1].first_node + i, 1);
  }
  for (std::size_t i = 0; i < small.size(); ++i) {
    copy_world(small[i], index.layers[2].first_node + i, 2);
  }

  persist_beacon_table(index, index.root, root_beacons);
  for (std::size_t i = 0; i < large.size(); ++i) {
    persist_beacon_table(index, index.layers[0].first_node + i,
                         large[i].child_beacons);
  }
  for (std::size_t i = 0; i < middle.size(); ++i) {
    persist_beacon_table(index, index.layers[1].first_node + i,
                         middle[i].child_beacons);
  }
  return index;
}

}  // namespace navigamer
