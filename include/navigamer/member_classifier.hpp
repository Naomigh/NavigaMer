#pragma once

#include "navigamer/batch_distance.hpp"
#include "navigamer/sequence_store.hpp"

#include <span>

namespace navigamer {

// Exact streaming classification, adapted from the production R75 driver.
// Share immutable centers/profiles; each worker owns its classifier and cache.
// The reference and center IDs must outlive this object. IDs are sorted, unique.
class AdjacentClassifier {
 public:
  AdjacentClassifier(const SequenceStore& reference, const std::vector<SequenceId>& centers,
                     const BatchDistance& profiles, std::uint32_t tolerance);
  [[nodiscard]] std::span<const std::uint8_t> classify(SequenceId id);
  std::uint64_t exact_pairs{0}, batch_groups{0}, repeated{0}, exclusive_members{0};

 private:
  [[nodiscard]] int lower(std::size_t center) const;
  [[nodiscard]] int upper(std::size_t center) const;
  const SequenceStore& reference_;
  const std::vector<SequenceId>& centers_;
  const BatchDistance& profiles_;
  BatchDistance::Workspace workspace_;
  std::uint32_t tolerance_;
  std::vector<int> distances_;
  std::vector<std::uint64_t> updated_;
  std::vector<std::uint8_t> membership_, exclusive_;
  std::string_view previous_;
  std::pair<std::string_view, std::uint64_t> previous_location_;
  std::uint64_t step_{0};
  std::size_t nearest_{0};
  bool valid_{false};
};

}  // namespace navigamer
