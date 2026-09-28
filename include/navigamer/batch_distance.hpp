#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace navigamer {

// Center-prepared Myers profiles, adapted from Minghao's AVX-512 kernel.
// Four independent 64-bit lanes on AVX2; exact scalar fallback otherwise.
// Append only between scans. Concurrent readers each need their own workspace.
class BatchDistance {
 public:
  static constexpr std::size_t kLanes = 4;
  struct alignas(32) Block {
    std::array<std::uint64_t, kLanes> positive;
    std::array<std::uint64_t, kLanes> negative;
  };
  struct Workspace {
    std::vector<Block> blocks;
    std::array<std::uint16_t, 256> gathered_symbols;
    std::vector<std::uint64_t> gathered_masks;
  };

  explicit BatchDistance(std::size_t length);
  void append(std::string_view center);
  // Only the first min(kLanes, size() - group*kLanes) results are valid.
  [[nodiscard]] std::array<int, kLanes> exact_group(
      std::string_view target, std::size_t group, Workspace& workspace) const;
  // Gather up to kLanes arbitrary centers into one prepared SIMD profile.
  [[nodiscard]] std::array<int, kLanes> exact_selected(
      std::string_view target, std::span<const std::size_t> selected, Workspace& workspace) const;
  [[nodiscard]] std::size_t size() const noexcept { return centers_.size(); }
  [[nodiscard]] std::uint64_t bytes() const noexcept;
  [[nodiscard]] static bool simd_available() noexcept;

 private:
  struct Group {
    Group() { symbols.fill(256); }
    std::array<std::uint16_t, 256> symbols;
    std::size_t alphabet_size = 0;
    std::vector<std::uint64_t> masks;
  };
  [[nodiscard]] std::array<int, kLanes> avx2_profile(
      std::string_view target, const std::array<std::uint16_t, 256>& symbols,
      const std::vector<std::uint64_t>& masks, Workspace& workspace) const;
  std::size_t length_, block_count_;
  std::uint64_t padding_mask_;
  std::uint64_t mask_bytes_{0}, string_bytes_{0};
  std::vector<std::string> centers_;
  std::vector<Group> groups_;
};

}  // namespace navigamer
