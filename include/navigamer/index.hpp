#pragma once

#include "navigamer/sequence_store.hpp"
#include "navigamer/packed_centers.hpp"
#include "navigamer/types.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace navigamer {

inline constexpr std::uint32_t kIndexFormatVersion = 5;

class NavigaMerIndex {
 public:
  std::uint32_t window_length{0};
  std::uint32_t stride{1};
  std::uint32_t max_tolerance{0};
  std::uint64_t reference_window_count{0};
  std::uint64_t reference_fingerprint{0};
  bool uses_shared_reference{true};
  NodeId root{0};
  std::vector<LayerInfo> layers;
  std::vector<WorldNode> nodes;
  PackedCenters center_sequences;
  std::vector<NodeId> children;
  std::vector<SequenceId> leaf_ids;
  // Only populated when reading legacy v3/v4 indexes. New builds store IDs only.
  std::vector<std::string> legacy_leaf_sequences;
  std::vector<std::uint64_t> beacon_ordinals;
  std::vector<int> beacon_distances;

  void save(const std::filesystem::path& path) const;
  static NavigaMerIndex load(const std::filesystem::path& path);
  [[nodiscard]] bool is_leaf(NodeId node) const noexcept {
    return nodes[node].layer != kRootLayer && nodes[node].layer + 1u == layers.size();
  }
  [[nodiscard]] std::span<const SequenceId> members(NodeId leaf) const {
    const auto& node = nodes[leaf];
    return {leaf_ids.data() + node.first_member, node.member_count};
  }
  [[nodiscard]] std::uint64_t bytes() const noexcept;
  // Sorted union of descendant leaf memberships, deduplicated by occurrence ID.
  [[nodiscard]] std::vector<SequenceId> world_members(NodeId world) const;
  [[nodiscard]] bool is_scoped() const noexcept {
    return nodes[root].center_sequence_id != kNoSequence;
  }
  [[nodiscard]] std::string summary() const;
  [[nodiscard]] std::string_view member_sequence(
      std::uint64_t member, const SequenceStore& reference) const {
    return uses_shared_reference ? reference.sequence(leaf_ids[member])
                                 : std::string_view(legacy_leaf_sequences[member]);
  }
};

}  // namespace navigamer
