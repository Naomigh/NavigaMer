#pragma once

#include "navigamer/index.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace navigamer {

enum class ClassificationMode { kBeacon, kAdjacent };

struct BuildConfig {
  // One radius per layer, ordered from root children to leaves (1..255 layers).
  std::vector<std::uint32_t> center_radii{60, 35, 15};
  // Empty means 0.1 per layer; otherwise one fraction per layer.
  std::vector<double> beacon_ratios;
  std::uint32_t max_tolerance{5};
  std::uint32_t threads{1};
  ClassificationMode classification_mode{ClassificationMode::kBeacon};
  // When set, greedily cover each parent's child centers at these radii.
  // One entry per layer, in the same order as center_radii.
  std::optional<std::vector<std::uint32_t>> beacon_cover_radii;
};

struct LayerBuildStats {
  double center_selection_seconds{0};
  double beacon_table_seconds{0};
  double classification_seconds{0};
  std::uint64_t center_edit_calls{0};
  std::uint64_t center_bound_accepts{0};
  std::uint64_t center_hint_hits{0};
  std::uint64_t center_batch_groups{0};
  std::uint64_t beacon_selection_edit_calls{0};
  std::uint64_t selected_beacons{0};
  std::uint64_t beacon_table_edit_calls{0};
  std::uint64_t member_beacon_edit_calls{0};
  std::uint64_t candidate_edit_calls{0};
  std::uint64_t candidate_batch_groups{0};
  std::uint64_t classification_center_pairs{0};
  std::uint64_t pruned_center_pairs{0};
  std::uint64_t classified_members{0};
  std::uint64_t classification_repeated{0};
};

struct BuildStats {
  std::vector<LayerBuildStats> layers;
};

class IndexBuilder {
 public:
  static NavigaMerIndex build(const SequenceStore& reference,
                              const BuildConfig& config,
                              BuildStats* stats = nullptr);
  // Append the configured radii below every existing leaf; input is unchanged.
  static NavigaMerIndex extend(const SequenceStore& reference,
                               const NavigaMerIndex& input,
                               const BuildConfig& config,
                               BuildStats* stats = nullptr);
  // Preserve this world's center and members, but rebuild all its descendants.
  // The selected world becomes the root of a new, independently queryable index.
  static NavigaMerIndex rebuild_world(const SequenceStore& reference,
                                      const NavigaMerIndex& input, NodeId world,
                                      const BuildConfig& config,
                                      BuildStats* stats = nullptr);
};

}  // namespace navigamer
