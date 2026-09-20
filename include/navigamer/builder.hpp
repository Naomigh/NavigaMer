#pragma once

#include "navigamer/index.hpp"

#include <array>
#include <cstdint>

namespace navigamer {

struct BuildConfig {
  std::array<std::uint32_t, 3> center_radii{60, 35, 15};
  // Fractions of large, middle, and small centers selected as beacons.
  std::array<double, 3> beacon_ratios{0.1, 0.1, 0.1};
  std::uint32_t max_tolerance{5};
  std::uint32_t threads{1};
};

struct LayerBuildStats {
  double center_selection_seconds{0};
  double beacon_table_seconds{0};
  double classification_seconds{0};
  std::uint64_t center_edit_calls{0};
  std::uint64_t beacon_table_edit_calls{0};
  std::uint64_t member_beacon_edit_calls{0};
  std::uint64_t candidate_edit_calls{0};
  std::uint64_t classification_center_pairs{0};
  std::uint64_t pruned_center_pairs{0};
  std::uint64_t classified_members{0};
};

struct BuildStats {
  std::array<LayerBuildStats, 3> layers{};
};

class IndexBuilder {
 public:
  static NavigaMerIndex build(const SequenceStore& reference,
                              const BuildConfig& config,
                              BuildStats* stats = nullptr);
};

}  // namespace navigamer
