#pragma once

#include "navigamer/sequence_store.hpp"
#include "navigamer/types.hpp"

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace navigamer {

class NavigaMerIndex {
 public:
  std::uint32_t window_length{0};
  std::uint32_t stride{1};
  std::uint32_t max_tolerance{0};
  std::uint64_t reference_window_count{0};
  NodeId root{0};
  std::array<LayerInfo, 3> layers{};
  std::vector<WorldNode> nodes;
  std::vector<std::string> center_sequences;
  std::vector<NodeId> children;
  std::vector<SequenceId> leaf_ids;
  // Every leaf owns the actual bases of each member, without compression or sharing.
  std::vector<std::string> leaf_sequences;
  std::vector<std::uint64_t> beacon_ordinals;
  std::vector<int> beacon_distances;

  void save(const std::filesystem::path& path) const;
  static NavigaMerIndex load(const std::filesystem::path& path);
  [[nodiscard]] bool is_leaf(NodeId node) const noexcept {
    return nodes[node].layer == 2;
  }
  [[nodiscard]] std::span<const SequenceId> members(NodeId leaf) const {
    const auto& node = nodes[leaf];
    return {leaf_ids.data() + node.first_member, node.member_count};
  }
  [[nodiscard]] std::uint64_t bytes() const noexcept;
  [[nodiscard]] std::string summary() const;
};

}  // namespace navigamer
