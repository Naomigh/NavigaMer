#pragma once

#include "navigamer/edit_distance.hpp"
#include "navigamer/sequence_store.hpp"
#include "navigamer/types.hpp"

#include <cstdint>
#include <vector>

namespace navigamer {

// Exact BK metric tree used only while constructing worlds. It is itself
// pointer-free: node and edge links are indices into flat vectors. Its pruning
// is solely the edit-distance triangle inequality, never a sequence seed.
class MetricTree {
 public:
  struct NearestResult {
    std::uint32_t payload{0};
    std::uint32_t distance{0};
  };

  MetricTree(const SequenceStore& sequences, const EditDistance& distance)
      : sequences_(sequences), distance_(distance) {}

  void insert(SequenceId sequence_id, std::uint32_t payload);
  [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

  [[nodiscard]] bool any_within(SequenceId query, std::uint32_t radius,
                                std::uint32_t* payload = nullptr) const;
  [[nodiscard]] std::vector<std::uint32_t> range(
      SequenceId query, std::uint32_t radius) const;
  [[nodiscard]] NearestResult nearest(
      SequenceId query, std::uint32_t initial_payload,
      std::uint32_t initial_distance) const;

 private:
  struct Node {
    SequenceId sequence_id{0};
    std::uint32_t payload{0};
    std::uint32_t first_edge{std::numeric_limits<std::uint32_t>::max()};
  };
  struct Edge {
    std::uint32_t child{0};
    std::uint32_t next{std::numeric_limits<std::uint32_t>::max()};
    std::uint16_t distance{0};
    std::uint16_t reserved{0};
  };

  const SequenceStore& sequences_;
  const EditDistance& distance_;
  std::vector<Node> nodes_;
  std::vector<Edge> edges_;
};

}  // namespace navigamer
