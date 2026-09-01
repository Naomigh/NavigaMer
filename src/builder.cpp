#include "navigamer/builder.hpp"

#include "navigamer/metric_tree.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <future>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>

namespace navigamer {
namespace {

using Clock = std::chrono::steady_clock;

std::uint32_t effective_threads(std::uint32_t requested) {
  if (requested != 0) return requested;
  return std::max(1U, std::thread::hardware_concurrency());
}

template <class Fn>
void run_ranges(std::uint64_t count, std::uint32_t threads, Fn&& fn) {
  if (count == 0) return;
  threads = std::min<std::uint32_t>(threads, count);
  std::vector<std::future<void>> futures;
  futures.reserve(threads);
  for (std::uint32_t thread = 0; thread < threads; ++thread) {
    const auto begin = count * thread / threads;
    const auto end = count * (thread + 1) / threads;
    futures.push_back(std::async(std::launch::async, [&, thread, begin, end] {
      fn(thread, begin, end);
    }));
  }
  for (auto& future : futures) future.get();
}

struct CenterSelection {
  std::vector<SequenceId> centers;
  std::vector<std::uint32_t> covering_hint;
  std::unique_ptr<MetricTree> directory;
};

struct Partition {
  std::vector<SequenceId> centers;
  std::vector<std::vector<SequenceId>> members;
  std::vector<std::uint16_t> cover_radii;
};

struct TemporaryWorld {
  SequenceId center{kNoSequence};
  std::uint64_t member_count{0};
  std::uint32_t parent_local{0};
  std::uint16_t cover_radius{0};
  std::vector<SequenceId> members;
  std::vector<std::uint32_t> child_worlds;
};

CenterSelection select_centers(const SequenceStore& sequences,
                               const EditDistance& distance,
                               const std::vector<SequenceId>& items,
                               std::uint32_t radius,
                               std::uint32_t hot_cache_size,
                               bool delayed_centers,
                               bool exact_global_reuse = true) {
  CenterSelection result;
  result.covering_hint.resize(items.size());
  result.directory = std::make_unique<MetricTree>(sequences, distance);
  std::deque<std::uint32_t> hot;

  for (std::size_t ordinal = 0; ordinal < items.size(); ++ordinal) {
    const auto sequence_id = items[ordinal];
    bool covered = false;
    std::uint32_t covering = 0;
    for (const auto local_id : hot) {
      const int d = distance(sequences.sequence(sequence_id),
                             sequences.sequence(result.centers[local_id]),
                             static_cast<int>(radius));
      if (d >= 0) {
        covered = true;
        covering = local_id;
        break;
      }
    }
    if (!covered && exact_global_reuse && !result.directory->empty() &&
        result.directory->any_within(sequence_id, radius, &covering)) {
      covered = true;
    }

    if (!covered) {
      SequenceId center = sequence_id;
      if (delayed_centers && ordinal + 1 < items.size()) {
        const auto lookahead = std::max<std::uint32_t>(1, radius / 2);
        const auto candidate_ordinal =
            std::min<std::size_t>(items.size() - 1, ordinal + lookahead);
        const auto candidate = items[candidate_ordinal];
        const bool covers_trigger =
            distance(sequences.sequence(sequence_id),
                     sequences.sequence(candidate), static_cast<int>(radius)) >= 0;
        std::uint32_t ignored = 0;
        const bool separated = !exact_global_reuse || result.directory->empty() ||
            !result.directory->any_within(candidate, radius, &ignored);
        if (covers_trigger && separated) center = candidate;
      }
      if (result.centers.size() >=
          static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::length_error("one parent has more than 2^32 child worlds");
      }
      covering = static_cast<std::uint32_t>(result.centers.size());
      result.centers.push_back(center);
      if (exact_global_reuse) result.directory->insert(center, covering);
    }
    result.covering_hint[ordinal] = covering;
    if (hot_cache_size != 0) {
      const auto found = std::find(hot.begin(), hot.end(), covering);
      if (found != hot.end()) hot.erase(found);
      hot.push_front(covering);
      while (hot.size() > hot_cache_size) hot.pop_back();
    }
  }
  if (result.centers.empty()) throw std::logic_error("center selection is empty");
  return result;
}

Partition assign_nearest_owners(const SequenceStore& sequences,
                                const EditDistance& distance,
                                const std::vector<SequenceId>& items,
                                CenterSelection selection,
                                std::uint32_t radius,
                                std::uint32_t threads) {
  std::vector<std::uint32_t> owners(items.size());
  std::vector<std::uint16_t> owner_distances(items.size());
  run_ranges(items.size(), threads,
             [&](std::uint32_t, std::uint64_t begin, std::uint64_t end) {
    for (std::uint64_t ordinal = begin; ordinal < end; ++ordinal) {
      const auto sequence_id = items[ordinal];
      const auto hint = selection.covering_hint[ordinal];
      const int hint_distance = distance(
          sequences.sequence(sequence_id),
          sequences.sequence(selection.centers[hint]));
      if (hint_distance < 0 || hint_distance > static_cast<int>(radius)) {
        throw std::logic_error("online covering hint violates world radius");
      }
      const auto nearest = selection.directory->nearest(
          sequence_id, hint, static_cast<std::uint32_t>(hint_distance));
      if (nearest.distance > radius || nearest.distance >= kMissingDistance) {
        throw std::logic_error("nearest-owner compaction lost world coverage");
      }
      owners[ordinal] = nearest.payload;
      owner_distances[ordinal] = static_cast<std::uint16_t>(nearest.distance);
    }
  });

  Partition partition;
  partition.centers = std::move(selection.centers);
  partition.members.resize(partition.centers.size());
  partition.cover_radii.assign(partition.centers.size(), 0);
  std::vector<std::size_t> counts(partition.centers.size(), 0);
  for (const auto owner : owners) ++counts[owner];
  for (std::size_t owner = 0; owner < counts.size(); ++owner) {
    partition.members[owner].reserve(counts[owner]);
  }
  for (std::size_t ordinal = 0; ordinal < items.size(); ++ordinal) {
    const auto owner = owners[ordinal];
    partition.members[owner].push_back(items[ordinal]);
    partition.cover_radii[owner] =
        std::max(partition.cover_radii[owner], owner_distances[ordinal]);
  }
  for (std::size_t owner = 0; owner < partition.centers.size(); ++owner) {
    if (partition.members[owner].empty()) {
      throw std::logic_error("nearest-owner compaction produced an empty world");
    }
  }
  return partition;
}

Partition assign_online_owners(const SequenceStore& sequences,
                               const EditDistance& distance,
                               const std::vector<SequenceId>& items,
                               CenterSelection selection,
                               std::uint32_t radius,
                               std::uint32_t threads) {
  std::vector<std::uint16_t> owner_distances(items.size());
  run_ranges(items.size(), threads,
             [&](std::uint32_t, std::uint64_t begin, std::uint64_t end) {
    for (std::uint64_t ordinal = begin; ordinal < end; ++ordinal) {
      const auto owner = selection.covering_hint[ordinal];
      const int d = distance(sequences.sequence(items[ordinal]),
                             sequences.sequence(selection.centers[owner]),
                             static_cast<int>(radius));
      if (d < 0 || d >= kMissingDistance) {
        throw std::logic_error("online owner violates leaf-world radius");
      }
      owner_distances[ordinal] = static_cast<std::uint16_t>(d);
    }
  });

  Partition partition;
  partition.centers = std::move(selection.centers);
  partition.members.resize(partition.centers.size());
  partition.cover_radii.assign(partition.centers.size(), 0);
  std::vector<std::size_t> counts(partition.centers.size(), 0);
  for (const auto owner : selection.covering_hint) ++counts[owner];
  for (std::size_t owner = 0; owner < counts.size(); ++owner) {
    if (counts[owner] == 0) {
      throw std::logic_error("online construction produced an empty leaf world");
    }
    partition.members[owner].reserve(counts[owner]);
  }
  for (std::size_t ordinal = 0; ordinal < items.size(); ++ordinal) {
    const auto owner = selection.covering_hint[ordinal];
    partition.members[owner].push_back(items[ordinal]);
    partition.cover_radii[owner] =
        std::max(partition.cover_radii[owner], owner_distances[ordinal]);
  }
  return partition;
}

std::vector<TemporaryWorld> pack_frozen_worlds(
    const SequenceStore& sequences, const EditDistance& distance,
    std::vector<TemporaryWorld>& children, std::uint32_t parent_radius,
    std::uint32_t hot_cache_size, bool exact_global_reuse) {
  std::vector<TemporaryWorld> parents;
  MetricTree directory(sequences, distance);
  std::deque<std::uint32_t> hot;

  for (std::uint32_t child_local = 0; child_local < children.size();
       ++child_local) {
    auto& child = children[child_local];
    if (child.cover_radius > parent_radius) {
      throw std::logic_error("child occupied ball exceeds parent radius");
    }
    const auto center_slack = parent_radius - child.cover_radius;
    bool contained = false;
    std::uint32_t parent_local = 0;
    int center_distance = -1;
    for (const auto candidate : hot) {
      const int d = distance(sequences.sequence(child.center),
                             sequences.sequence(parents[candidate].center),
                             static_cast<int>(center_slack));
      if (d >= 0) {
        contained = true;
        parent_local = candidate;
        center_distance = d;
        break;
      }
    }
    if (!contained && exact_global_reuse && !directory.empty() &&
        directory.any_within(child.center, center_slack, &parent_local)) {
      center_distance = distance(
          sequences.sequence(child.center),
          sequences.sequence(parents[parent_local].center),
          static_cast<int>(center_slack));
      if (center_distance < 0) {
        throw std::logic_error("metric directory returned invalid containment");
      }
      contained = true;
    }
    if (!contained) {
      if (parents.size() >=
          static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::length_error("one packed layer exceeds 2^32 worlds");
      }
      parent_local = static_cast<std::uint32_t>(parents.size());
      TemporaryWorld parent;
      parent.center = child.center;
      parent.cover_radius = child.cover_radius;
      parents.push_back(std::move(parent));
      if (exact_global_reuse) directory.insert(child.center, parent_local);
      center_distance = 0;
    }

    auto& parent = parents[parent_local];
    const auto occupied_radius = static_cast<std::uint32_t>(center_distance) +
                                 child.cover_radius;
    if (occupied_radius > parent_radius || occupied_radius >= kMissingDistance) {
      throw std::logic_error("packed child ball is not contained by parent");
    }
    parent.cover_radius = std::max(
        parent.cover_radius, static_cast<std::uint16_t>(occupied_radius));
    parent.member_count += child.member_count;
    parent.child_worlds.push_back(child_local);
    child.parent_local = parent_local;

    if (hot_cache_size != 0) {
      const auto found = std::find(hot.begin(), hot.end(), parent_local);
      if (found != hot.end()) hot.erase(found);
      hot.push_front(parent_local);
      while (hot.size() > hot_cache_size) hot.pop_back();
    }
  }
  if (parents.empty()) throw std::logic_error("packed layer is empty");
  return parents;
}

struct LocalMetricNode {
  std::uint64_t child_slot{0};
  std::vector<std::pair<std::uint16_t, std::uint32_t>> edges;
  std::uint32_t subtree_cover_radius{0};
};

void build_persistent_metric_tree(NavigaMerIndex& index, NodeId parent_id,
                                  const SequenceStore& sequences,
                                  const EditDistance& distance) {
  const auto& parent = index.nodes[parent_id];
  if (index.is_terminal(parent) || parent.child_count == 0) return;

  std::vector<std::uint64_t> slots(parent.child_count);
  std::iota(slots.begin(), slots.end(), parent.first_child);
  // Deterministic shuffling avoids reference-coordinate insertion order. It
  // is not a sequence filter and cannot change the returned set.
  std::mt19937_64 random(0x4e61766967614d65ULL ^ parent_id);
  std::shuffle(slots.begin(), slots.end(), random);

  std::vector<LocalMetricNode> local;
  const auto root_child = index.children[slots.front()];
  local.push_back(
      {slots.front(), {}, index.nodes[root_child].cover_radius});
  for (std::size_t ordinal = 1; ordinal < slots.size(); ++ordinal) {
    const auto slot = slots[ordinal];
    const auto child = index.children[slot];
    const auto center = index.nodes[child].center_sequence_id;
    std::uint32_t current = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ancestors;
    while (true) {
      const auto current_child = index.children[local[current].child_slot];
      const auto current_center = index.nodes[current_child].center_sequence_id;
      const int d = distance(sequences.sequence(center),
                             sequences.sequence(current_center));
      if (d < 0 || d >= kMissingDistance) {
        throw std::length_error("metric-directory distance exceeds uint16");
      }
      ancestors.push_back(
          {current, static_cast<std::uint32_t>(d)});
      auto edge = std::find_if(local[current].edges.begin(),
          local[current].edges.end(), [&](const auto& value) {
            return value.first == static_cast<std::uint16_t>(d);
          });
      if (edge != local[current].edges.end()) {
        current = edge->second;
        continue;
      }
      if (local.size() >= std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("persistent metric directory exceeds 32-bit IDs");
      }
      const auto child_node = static_cast<std::uint32_t>(local.size());
      local.push_back({slot, {}, index.nodes[child].cover_radius});
      for (const auto& [ancestor, ancestor_distance] : ancestors) {
        local[ancestor].subtree_cover_radius = std::max(
            local[ancestor].subtree_cover_radius,
            ancestor_distance + index.nodes[child].cover_radius);
      }
      local[current].edges.push_back(
          {static_cast<std::uint16_t>(d), child_node});
      break;
    }
  }

  if (index.metric_nodes.size() + local.size() >=
      std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("global metric directory exceeds 32-bit IDs");
  }
  const auto base = static_cast<std::uint32_t>(index.metric_nodes.size());
  index.metric_roots[parent_id] = base;
  index.metric_nodes.resize(index.metric_nodes.size() + local.size());
  for (std::uint32_t local_id = 0; local_id < local.size(); ++local_id) {
    auto& output = index.metric_nodes[base + local_id];
    output.child_slot = local[local_id].child_slot;
    output.first_edge = index.metric_edges.size();
    std::sort(local[local_id].edges.begin(), local[local_id].edges.end());
    for (const auto& [edge_distance, child_node] : local[local_id].edges) {
      index.metric_edges.push_back(
          {static_cast<std::uint32_t>(base + child_node), edge_distance, 0});
    }
    output.edge_count = static_cast<std::uint32_t>(local[local_id].edges.size());
    output.subtree_cover_radius = local[local_id].subtree_cover_radius;
  }
}

void build_dense_pair_matrix(NavigaMerIndex& index, NodeId parent_id,
                             const SequenceStore& sequences,
                             const EditDistance& distance,
                             std::uint32_t threads) {
  constexpr std::uint32_t kDensePairLimit = 4096;
  const auto& parent = index.nodes[parent_id];
  if (index.is_terminal(parent) || parent.child_count == 0 ||
      parent.child_count > kDensePairLimit) {
    return;
  }
  const auto count = static_cast<std::uint64_t>(parent.child_count);
  const auto offset = index.dense_pair_distances.size();
  index.dense_pair_offsets[parent_id] = offset;
  index.dense_pair_distances.resize(offset + count * count);
  auto calculate_rows = [&](std::uint64_t begin, std::uint64_t end) {
    for (std::uint64_t row = begin; row < end; ++row) {
      const auto row_child = index.children[parent.first_child + row];
      const auto row_center = index.nodes[row_child].center_sequence_id;
      for (std::uint64_t column = 0; column < count; ++column) {
        const auto column_child = index.children[parent.first_child + column];
        const auto column_center = index.nodes[column_child].center_sequence_id;
        const int d = distance(sequences.sequence(row_center),
                               sequences.sequence(column_center));
        if (d < 0 || d >= std::numeric_limits<std::uint8_t>::max()) {
          throw std::length_error("dense center distance exceeds uint8 capacity");
        }
        index.dense_pair_distances[offset + row * count + column] =
            static_cast<std::uint8_t>(d);
      }
    }
  };
  if (count >= 256 && threads > 1) {
    run_ranges(count, threads,
               [&](std::uint32_t, std::uint64_t begin, std::uint64_t end) {
      calculate_rows(begin, end);
    });
  } else {
    calculate_rows(0, count);
  }
}

}  // namespace

NavigaMerIndex IndexBuilder::build(const BuildConfig& input_config,
                                   BuildStats* stats) const {
  BuildConfig config = input_config;
  config.threads = effective_threads(config.threads);
  if (sequences_.size() == 0) throw std::invalid_argument("reference has no windows");
  if (config.radii.empty() || config.max_beacons == 0) {
    throw std::invalid_argument("at least one radius and beacon are required");
  }
  if (config.radii.size() > kSyntheticLayer) {
    throw std::invalid_argument("too many hierarchy layers for disk layout");
  }
  for (std::size_t layer = 0; layer < config.radii.size(); ++layer) {
    if (config.radii[layer] == 0 || config.radii[layer] >= kMissingDistance) {
      throw std::invalid_argument("radii must fit positive uint16 ranges");
    }
    if (layer != 0 && config.radii[layer - 1] <= config.radii[layer]) {
      throw std::invalid_argument("radii must be strictly decreasing");
    }
  }
  if (config.max_beacons > 64) {
    throw std::invalid_argument("max_beacons above 64 is unsupported");
  }

  distance_.reset_calls();
  double center_seconds = 0.0;
  double owner_seconds = 0.0;
  double packing_seconds = 0.0;
  std::vector<std::vector<TemporaryWorld>> temporary(config.radii.size());
  std::vector<SequenceId> all_sequences(sequences_.size());
  std::iota(all_sequences.begin(), all_sequences.end(), SequenceId{0});

  if (config.mode == BuildMode::kNestedBalls) {
    const auto leaf = config.radii.size() - 1;
    const auto center_start = Clock::now();
    auto selection = select_centers(
        sequences_, distance_, all_sequences, config.radii[leaf],
        config.hot_cache_size, false, config.exact_global_reuse);
    center_seconds += std::chrono::duration<double>(
        Clock::now() - center_start).count();

    const auto owner_start = Clock::now();
    auto partition = assign_online_owners(
        sequences_, distance_, all_sequences, std::move(selection),
        config.radii[leaf], config.threads);
    owner_seconds += std::chrono::duration<double>(
        Clock::now() - owner_start).count();
    temporary[leaf].reserve(partition.centers.size());
    for (std::size_t child = 0; child < partition.centers.size(); ++child) {
      TemporaryWorld world;
      world.center = partition.centers[child];
      world.member_count = partition.members[child].size();
      world.cover_radius = partition.cover_radii[child];
      world.members = std::move(partition.members[child]);
      temporary[leaf].push_back(std::move(world));
    }

    const auto packing_start = Clock::now();
    for (std::size_t child_layer = leaf; child_layer > 0; --child_layer) {
      temporary[child_layer - 1] = pack_frozen_worlds(
          sequences_, distance_, temporary[child_layer],
          config.radii[child_layer - 1], config.hot_cache_size,
          config.exact_global_reuse);
    }
    packing_seconds = std::chrono::duration<double>(
        Clock::now() - packing_start).count();
  } else {
    for (std::size_t layer = 0; layer < config.radii.size(); ++layer) {
      const std::size_t parent_count =
          layer == 0 ? 1 : temporary[layer - 1].size();
      std::vector<CenterSelection> selections(parent_count);
      auto items_for_parent =
          [&](std::size_t parent) -> const std::vector<SequenceId>& {
        return layer == 0 ? all_sequences : temporary[layer - 1][parent].members;
      };

      const auto center_start = Clock::now();
      run_ranges(parent_count, layer == 0 ? 1 : config.threads,
                 [&](std::uint32_t, std::uint64_t begin, std::uint64_t end) {
        for (std::uint64_t parent = begin; parent < end; ++parent) {
          selections[parent] = select_centers(
              sequences_, distance_, items_for_parent(parent),
              config.radii[layer], config.hot_cache_size,
              config.delayed_centers);
        }
      });
      center_seconds += std::chrono::duration<double>(
          Clock::now() - center_start).count();

      const auto owner_start = Clock::now();
      std::vector<Partition> partitions(parent_count);
      if (parent_count == 1) {
        partitions[0] = assign_nearest_owners(
            sequences_, distance_, items_for_parent(0),
            std::move(selections[0]), config.radii[layer], config.threads);
      } else {
        run_ranges(parent_count, config.threads,
                   [&](std::uint32_t, std::uint64_t begin,
                       std::uint64_t end) {
          for (std::uint64_t parent = begin; parent < end; ++parent) {
            partitions[parent] = assign_nearest_owners(
                sequences_, distance_, items_for_parent(parent),
                std::move(selections[parent]), config.radii[layer], 1);
          }
        });
      }
      owner_seconds += std::chrono::duration<double>(
          Clock::now() - owner_start).count();

      std::size_t world_count = 0;
      for (const auto& part : partitions) world_count += part.centers.size();
      temporary[layer].reserve(world_count);
      for (std::uint32_t parent = 0; parent < partitions.size(); ++parent) {
        auto& part = partitions[parent];
        for (std::size_t child = 0; child < part.centers.size(); ++child) {
          TemporaryWorld world;
          world.center = part.centers[child];
          world.member_count = part.members[child].size();
          world.parent_local = parent;
          world.cover_radius = part.cover_radii[child];
          world.members = std::move(part.members[child]);
          temporary[layer].push_back(std::move(world));
        }
      }
      if (layer > 0) {
        for (auto& parent : temporary[layer - 1]) {
          std::vector<SequenceId>().swap(parent.members);
        }
      }
    }
  }

  if (config.mode == BuildMode::kNearestOwner) {
    for (std::size_t layer = 1; layer < temporary.size(); ++layer) {
      for (std::uint32_t child = 0; child < temporary[layer].size(); ++child) {
        const auto parent = temporary[layer][child].parent_local;
        if (parent >= temporary[layer - 1].size()) {
          throw std::logic_error("orphan nearest-owner world");
        }
        temporary[layer - 1][parent].child_worlds.push_back(child);
      }
    }
  }

  const auto topology_start = Clock::now();
  NavigaMerIndex index;
  index.window_length = sequences_.window_length();
  index.stride = sequences_.stride();
  index.max_beacons = config.max_beacons;
  index.routing_mode = config.mode == BuildMode::kNestedBalls
      ? RoutingMode::kNestedBalls
      : RoutingMode::kNearestOwner;
  index.reference_count = sequences_.size();
  index.reference_checksum = sequences_.checksum();
  index.root = 0;
  WorldNode root;
  root.bwt_interval_length = sequences_.size();
  index.nodes.push_back(root);
  for (std::size_t layer = 0; layer < temporary.size(); ++layer) {
    LayerInfo info;
    info.first_node = index.nodes.size();
    info.node_count = temporary[layer].size();
    info.radius = config.radii[layer];
    index.layers.push_back(info);
    for (const auto& source : temporary[layer]) {
      WorldNode node;
      node.center_sequence_id = source.center;
      node.bwt_interval_length = source.member_count;
      node.cover_radius = source.cover_radius;
      node.layer = static_cast<std::uint8_t>(layer);
      index.nodes.push_back(node);
    }
  }

  auto append_child = [&](WorldNode& parent, std::uint64_t child) {
    if (parent.child_count == std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("one world has more than 2^32 children");
    }
    index.children.push_back(child);
    ++parent.child_count;
  };
  index.nodes[index.root].first_child = index.children.size();
  for (std::uint64_t local = 0; local < temporary.front().size(); ++local) {
    append_child(index.nodes[index.root], index.layers.front().first_node + local);
  }
  for (std::size_t layer = 0; layer < temporary.size(); ++layer) {
    for (std::uint32_t local = 0; local < temporary[layer].size(); ++local) {
      auto& node = index.nodes[index.layers[layer].first_node + local];
      node.first_child = index.children.size();
      if (layer + 1 == temporary.size()) {
        for (const auto sequence_id : temporary[layer][local].members) {
          append_child(node, sequence_id);
        }
      } else {
        for (const auto child_local : temporary[layer][local].child_worlds) {
          if (child_local >= temporary[layer + 1].size() ||
              temporary[layer + 1][child_local].parent_local != local) {
            throw std::logic_error("invalid packed parent-child edge");
          }
          append_child(node,
                       index.layers[layer + 1].first_node + child_local);
        }
      }
    }
  }

  index.metric_roots.assign(index.nodes.size(), kNoNode);
  index.dense_pair_offsets.assign(index.nodes.size(), kNoNode);
  for (NodeId parent = 0; parent < index.nodes.size(); ++parent) {
    build_persistent_metric_tree(index, parent, sequences_, distance_);
    build_dense_pair_matrix(index, parent, sequences_, distance_, config.threads);
  }
  const auto topology_end = Clock::now();

  const auto mbb_start = Clock::now();
  for (auto& node : index.nodes) {
    node.first_beacon = index.beacons.size();
    if (node.child_count == 0) continue;
    const auto wanted = std::min(config.max_beacons, node.child_count);
    std::unordered_set<SequenceId> seen;
    for (std::uint32_t beacon_ordinal = 0; beacon_ordinal < wanted;
         ++beacon_ordinal) {
      const std::uint64_t ordinal = wanted == 1
          ? 0
          : static_cast<std::uint64_t>(beacon_ordinal) * (node.child_count - 1) /
                (wanted - 1);
      const auto child = index.children[node.first_child + ordinal];
      const SequenceId beacon = index.is_terminal(node)
          ? child
          : index.nodes[child].center_sequence_id;
      if (seen.insert(beacon).second) index.beacons.push_back(beacon);
    }
    node.beacon_count = static_cast<std::uint8_t>(
        index.beacons.size() - node.first_beacon);
  }

  index.child_beacon_distances.assign(
      index.children.size() * index.max_beacons, kMissingDistance);
  run_ranges(index.nodes.size(), config.threads,
             [&](std::uint32_t, std::uint64_t begin, std::uint64_t end) {
    for (NodeId node_id = begin; node_id < end; ++node_id) {
      const auto& node = index.nodes[node_id];
      for (std::uint32_t child_ordinal = 0; child_ordinal < node.child_count;
           ++child_ordinal) {
        const auto child_slot = node.first_child + child_ordinal;
        const auto child = index.children[child_slot];
        const SequenceId child_sequence = index.is_terminal(node)
            ? child
            : index.nodes[child].center_sequence_id;
        for (std::uint32_t beacon_ordinal = 0;
             beacon_ordinal < node.beacon_count; ++beacon_ordinal) {
          const auto beacon = index.beacons[node.first_beacon + beacon_ordinal];
          const int d = distance_(sequences_.sequence(child_sequence),
                                  sequences_.sequence(beacon));
          if (d < 0 || d >= kMissingDistance) {
            throw std::length_error("beacon distance exceeds uint16 capacity");
          }
          index.child_beacon_distances[
              child_slot * index.max_beacons + beacon_ordinal] =
              static_cast<std::uint16_t>(d);
        }
      }
    }
  });
  const auto mbb_end = Clock::now();

  index.validate(&sequences_);
  if (stats) {
    stats->worlds_per_layer.clear();
    for (const auto& layer : index.layers) {
      stats->worlds_per_layer.push_back(layer.node_count);
    }
    stats->world_edges = 0;
    for (const auto& node : index.nodes) {
      if (!index.is_terminal(node)) stats->world_edges += node.child_count;
    }
    stats->terminal_memberships = sequences_.size();
    stats->edit_distance_calls = distance_.calls();
    stats->center_seconds = center_seconds;
    stats->owner_seconds = owner_seconds;
    stats->packing_seconds = packing_seconds;
    stats->topology_seconds = std::chrono::duration<double>(
        topology_end - topology_start).count();
    stats->mbb_seconds = std::chrono::duration<double>(
        mbb_end - mbb_start).count();
  }
  return index;
}

}  // namespace navigamer
