#pragma once

#include "navigamer/index.hpp"

#include <array>
#include <cstdint>

namespace navigamer {

struct BuildConfig {
  std::array<std::uint32_t, 3> center_radii{60, 35, 15};
  // Fractions of middle centers per large world and small centers per middle world.
  std::array<double, 2> beacon_ratios{0.1, 0.1};
  std::uint32_t max_tolerance{5};
  std::uint32_t threads{1};
};

class IndexBuilder {
 public:
  static NavigaMerIndex build(const SequenceStore& reference,
                              const BuildConfig& config);
};

}  // namespace navigamer
