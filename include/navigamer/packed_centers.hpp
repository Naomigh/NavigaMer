#pragma once

#include "navigamer/edit_distance.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace navigamer {

// Fixed-length centers in one allocation, in sibling order. A/T/G/C = 0/1/2/3.
// Other symbols are preserved literally in a sparse side table (not wildcards).
class PackedCenters {
 public:
  explicit PackedCenters(std::uint32_t length = 0) : length_(length) {}
  void append(std::string_view sequence);
  [[nodiscard]] std::string_view decode(std::size_t id, std::string& scratch) const;
  [[nodiscard]] std::string decode(std::size_t id) const;
  void prefetch(std::size_t first, std::size_t count) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return exception_offsets_.size() - 1; }
  [[nodiscard]] std::size_t stride() const noexcept { return (std::size_t(length_) + 3) / 4; }
  [[nodiscard]] std::span<const std::uint8_t> data() const noexcept { return bases_; }
  [[nodiscard]] std::uint64_t bytes() const noexcept;
  [[nodiscard]] std::uint32_t length() const noexcept { return length_; }
  [[nodiscard]] const DnaBits* bitplanes(std::size_t id) const noexcept;

 private:
  friend class NavigaMerIndex;
  void prepare_bitplanes();
  std::uint32_t length_;
  std::vector<std::uint8_t> bases_;
  std::vector<std::uint64_t> exception_offsets_{0};
  std::vector<std::uint32_t> exception_positions_;
  std::vector<char> exception_symbols_;
  std::vector<DnaBits> bitplanes_;
};

}  // namespace navigamer
