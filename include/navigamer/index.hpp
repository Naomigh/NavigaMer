#pragma once

#include "navigamer/sequence_store.hpp"
#include "navigamer/types.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace navigamer {

class NavigaMerIndex {
 public:
  std::uint32_t window_length{0};
  std::uint32_t stride{1};
  std::uint32_t max_beacons{0};
  RoutingMode routing_mode{RoutingMode::kNearestOwner};
  std::uint64_t reference_count{0};
  std::uint64_t reference_checksum{0};
  NodeId root{0};

  std::vector<LayerInfo> layers;
  std::vector<WorldNode> nodes;
  std::vector<std::uint64_t> children;
  std::vector<SequenceId> beacons;

  // One root per world node; terminal worlds use kNoNode. The directory is an
  // exact triangle-inequality index over each world's child centers.
  std::vector<NodeId> metric_roots;
  std::vector<MetricIndexNode> metric_nodes;
  std::vector<MetricIndexEdge> metric_edges;

  // Small/medium sibling sets additionally store a row-major exact center
  // distance matrix. The previous query path can then act as a dynamic local
  // beacon. Offsets are kNoNode when no dense matrix is stored.
  std::vector<NodeId> dense_pair_offsets;
  std::vector<std::uint8_t> dense_pair_distances;

  // Fixed-width matrix indexed by child-array slot, then beacon ordinal.
  // For a world child this stores d(child.center, beacon); for a terminal
  // sequence child it stores d(sequence, beacon). The world radius expands
  // this point distance into a conservative metric bounding interval.
  std::vector<std::uint16_t> child_beacon_distances;

  void save(const std::filesystem::path& path) const;
  static NavigaMerIndex load(const std::filesystem::path& path);
  void validate(const SequenceStore* reference = nullptr) const;

  [[nodiscard]] bool is_terminal(const WorldNode& node) const noexcept {
    return node.layer != kSyntheticLayer &&
           static_cast<std::size_t>(node.layer + 1) == layers.size();
  }
  [[nodiscard]] std::uint32_t child_radius(
      const WorldNode& parent) const noexcept;
  [[nodiscard]] std::uint64_t bytes() const noexcept;
  [[nodiscard]] std::string summary() const;
};

}  // namespace navigamer
