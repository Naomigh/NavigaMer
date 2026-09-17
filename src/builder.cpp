#include "navigamer/builder.hpp"

#include "navigamer/edit_distance.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <thread>

namespace navigamer {
namespace {

struct TempWorld {
  SequenceId center;
  std::vector<SequenceId> members;
  std::vector<std::size_t> children;
  std::uint64_t member_count;
};

std::vector<SequenceId> choose_centers(
    const SequenceStore& reference, const std::vector<SequenceId>& members,
    std::uint32_t radius) {
  std::vector<SequenceId> centers{members[0]};
  for (std::size_t i = 1; i < members.size(); ++i) {
    auto prepared = EditDistance{}.prepare(reference.sequence(members[i]));
    bool covered = false;
    for (const auto center : centers) {
      if (prepared.exact(reference.sequence(center)) <= static_cast<int>(radius)) {
        covered = true;
        break;
      }
    }
    if (!covered) centers.push_back(members[i]);
  }
  return centers;
}

std::vector<TempWorld> split_parent(
    const SequenceStore& reference, const std::vector<SequenceId>& members,
    std::uint32_t radius, const BuildConfig& config) {
  const auto centers = choose_centers(reference, members, radius);
  std::vector<std::vector<std::size_t>> assignments(members.size());
  auto classify = [&](std::uint32_t worker) {
    std::vector<int> distances(centers.size());
    for (std::size_t i = worker; i < members.size(); i += config.threads) {
      const auto center = std::find(centers.begin(), centers.end(), members[i]);
      if (center != centers.end()) {
        assignments[i].push_back(center - centers.begin());
        continue;
      }
      auto prepared = EditDistance{}.prepare(reference.sequence(members[i]));
      for (std::size_t c = 0; c < centers.size(); ++c) {
        distances[c] = prepared.exact(reference.sequence(centers[c]));
      }
      const int x = *std::min_element(distances.begin(), distances.end());
      const int limit = x + 2 * static_cast<int>(config.max_tolerance);
      for (std::size_t c = 0; c < centers.size(); ++c) {
        if (distances[c] <= limit) assignments[i].push_back(c);
      }
    }
  };
  std::vector<std::thread> workers;
  for (std::uint32_t worker = 1; worker < config.threads; ++worker) {
    workers.emplace_back(classify, worker);
  }
  classify(0);
  for (auto& worker : workers) worker.join();

  std::vector<TempWorld> worlds;
  for (const auto center : centers) worlds.push_back({center, {}, {}, 0});
  for (std::size_t i = 0; i < members.size(); ++i) {
    for (const auto world : assignments[i]) worlds[world].members.push_back(members[i]);
  }
  for (auto& world : worlds) world.member_count = world.members.size();
  return worlds;
}

void make_beacon_table(NavigaMerIndex& index, NodeId parent_id, double ratio) {
  auto& parent = index.nodes[parent_id];
  parent.first_beacon = index.beacon_ordinals.size();
  parent.beacon_count = static_cast<std::uint64_t>(
      std::ceil(ratio * parent.child_count));
  for (std::uint64_t b = 0; b < parent.beacon_count; ++b) {
    index.beacon_ordinals.push_back(b * parent.child_count / parent.beacon_count);
  }
  parent.first_distance = index.beacon_distances.size();
  index.beacon_distances.resize(parent.first_distance +
                                 parent.child_count * parent.beacon_count);
  for (std::uint64_t b = 0; b < parent.beacon_count; ++b) {
    const auto ordinal = index.beacon_ordinals[parent.first_beacon + b];
    const auto beacon = index.children[parent.first_child + ordinal];
    auto prepared = EditDistance{}.prepare(index.center_sequences[beacon]);
    for (std::uint64_t row = 0; row < parent.child_count; ++row) {
      const auto child = index.children[parent.first_child + row];
      index.beacon_distances[parent.first_distance + row * parent.beacon_count + b] =
          prepared.exact(index.center_sequences[child]);
    }
  }
}

}  // namespace

NavigaMerIndex IndexBuilder::build(const SequenceStore& reference,
                                   const BuildConfig& config) {
  std::vector<SequenceId> all(reference.size());
  std::iota(all.begin(), all.end(), 0);
  auto large = split_parent(reference, all, config.center_radii[0], config);
  std::vector<TempWorld> middle, small;
  for (auto& parent : large) {
    auto children = split_parent(reference, parent.members,
                                 config.center_radii[1], config);
    for (auto& child : children) {
      parent.children.push_back(middle.size());
      middle.push_back(std::move(child));
    }
    std::vector<SequenceId>{}.swap(parent.members);
  }
  for (auto& parent : middle) {
    auto children = split_parent(reference, parent.members,
                                 config.center_radii[2], config);
    for (auto& child : children) {
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
  for (std::size_t i = 0; i < large.size(); ++i) {
    make_beacon_table(index, index.layers[0].first_node + i, config.beacon_ratios[0]);
  }
  for (std::size_t i = 0; i < middle.size(); ++i) {
    make_beacon_table(index, index.layers[1].first_node + i, config.beacon_ratios[1]);
  }
  return index;
}

}  // namespace navigamer
