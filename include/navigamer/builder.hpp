#pragma once

#include "navigamer/edit_distance.hpp"
#include "navigamer/index.hpp"
#include "navigamer/sequence_store.hpp"

#include <cstdint>
#include <vector>

namespace navigamer {

enum class BuildMode : std::uint32_t {
  kNearestOwner = 0,
  kNestedBalls = 1,
  kTopDownNested = 2,
};

struct BuildConfig {
  // Coarse to fine. Every radius must be positive and strictly decreasing.
  std::vector<std::uint32_t> radii{90, 55, 30};
  std::uint32_t max_beacons{4};
  std::uint32_t threads{1};
  std::uint32_t hot_cache_size{16};
  // Zero fills top worlds up to radii[0]. A smaller positive value keeps each
  // top world's actual occupied ball tighter, improving exact query bounds.
  std::uint32_t top_fill_radius{0};
  // Centers in the terminal layer cover reference points within
  // radius-2*containment_tolerance. This guard band guarantees that every
  // query with a hit within this tolerance has a terminal world that fully
  // contains the query ball.
  std::uint32_t containment_tolerance{3};
  // Nearest-owner mode may try a future item at R/2 as the center. Nested-ball
  // mode deliberately uses the current uncovered item: pushing leaf centers
  // apart consumes the parent containment slack and degenerates the hierarchy.
  bool delayed_centers{true};
  // Nested construction normally reuses any prior world found by an exact BK
  // lookup. Disabling this keeps only the hot local history and may create
  // redundant worlds, but cannot affect coverage or query exactness.
  bool exact_global_reuse{true};
  // Top-down nested mode builds a unique skeleton, expands terminal worlds to
  // complete metric balls, then packs the frozen balls upward. It never
  // performs the nearest-owner correction passes.
  BuildMode mode{BuildMode::kTopDownNested};
};

struct BuildStats {
  std::vector<std::uint64_t> worlds_per_layer;
  std::uint64_t world_edges{0};
  std::uint64_t terminal_memberships{0};
  std::uint64_t unique_terminal_memberships{0};
  std::uint64_t edit_distance_calls{0};
  double center_seconds{0.0};
  double owner_seconds{0.0};
  double membership_seconds{0.0};
  double packing_seconds{0.0};
  double topology_seconds{0.0};
  double mbb_seconds{0.0};
};

class IndexBuilder {
 public:
  explicit IndexBuilder(const SequenceStore& sequences)
      : sequences_(sequences) {}

  NavigaMerIndex build(const BuildConfig& config,
                       BuildStats* stats = nullptr) const;

 private:
  const SequenceStore& sequences_;
  mutable EditDistance distance_;
};

}  // namespace navigamer
