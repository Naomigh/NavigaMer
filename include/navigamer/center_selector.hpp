#pragma once

#include "navigamer/builder.hpp"

#include <memory>
#include <span>

namespace navigamer {

// Input-order greedy selection over replayable, sorted member-ID blocks.
// Existing centers allow resuming at an already committed member position.
class CenterSelector {
 public:
  CenterSelector(const SequenceStore& reference, std::uint32_t radius,
                 std::uint32_t threads, const std::vector<SequenceId>& existing = {},
                 std::uint64_t memory_limit = 0);
  ~CenterSelector();
  void consume(std::span<const SequenceId> members);
  [[nodiscard]] const std::vector<SequenceId>& centers() const;
  [[nodiscard]] const PackedCenters& sequences() const;
  [[nodiscard]] const LayerBuildStats& stats() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace navigamer
