#include "navigamer/packed_centers.hpp"

#include "navigamer/prefetch.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

namespace navigamer {
namespace {

constexpr auto kDecoded = [] {
  std::array<std::array<char, 4>, 256> table{};
  for (std::size_t byte = 0; byte < table.size(); ++byte) {
    for (std::size_t base = 0; base < 4; ++base) {
      table[byte][base] = "ATGC"[(byte >> (2 * base)) & 3];
    }
  }
  return table;
}();

}  // namespace

void PackedCenters::append(std::string_view sequence) {
  if (sequence.size() != length_) {
    throw std::invalid_argument("packed centers must have the configured length");
  }
  const auto start = bases_.size();
  bases_.resize(start + stride(), 0);
  for (std::size_t i = 0; i < sequence.size(); ++i) {
    std::uint8_t code = 0;
    switch (sequence[i]) {
      case 'A': break;
      case 'T': code = 1; break;
      case 'G': code = 2; break;
      case 'C': code = 3; break;
      default:
        exception_positions_.push_back(i);
        exception_symbols_.push_back(sequence[i]);
        break;
    }
    bases_[start + i / 4] |= code << (2 * (i % 4));
  }
  exception_offsets_.push_back(exception_positions_.size());
  if (length_ <= EditDistance::kLeapMaxLength && EditDistance::leap_available()) {
    bitplanes_.emplace_back(sequence);
  }
}

const DnaBits* PackedCenters::bitplanes(std::size_t id) const noexcept {
  return !bitplanes_.empty() && exception_offsets_[id] == exception_offsets_[id + 1]
      ? &bitplanes_[id] : nullptr;
}

void PackedCenters::prepare_bitplanes() {
  if (length_ > EditDistance::kLeapMaxLength || !EditDistance::leap_available()) return;
  bitplanes_.reserve(size());
  std::string scratch;
  for (std::size_t i = 0; i < size(); ++i) bitplanes_.emplace_back(decode(i, scratch));
}

std::string_view PackedCenters::decode(std::size_t id, std::string& scratch) const {
  scratch.resize(length_);
  const auto start = id * stride();
  std::size_t i = 0;
  for (; i + 4 <= length_; i += 4) {
    std::memcpy(scratch.data() + i, kDecoded[bases_[start + i / 4]].data(), 4);
  }
  if (i < length_) {
    std::memcpy(scratch.data() + i, kDecoded[bases_[start + i / 4]].data(), length_ - i);
  }
  for (auto e = exception_offsets_[id]; e < exception_offsets_[id + 1]; ++e) {
    scratch[exception_positions_[e]] = exception_symbols_[e];
  }
  return scratch;
}

std::string PackedCenters::decode(std::size_t id) const {
  std::string result;
  (void)decode(id, result);
  return result;
}

void PackedCenters::prefetch(std::size_t first, std::size_t count) const noexcept {
  if (count == 0) return;
  if (stride() != 0) prefetch_read<3>(bases_.data() + first * stride(), count * stride());
  if (!bitplanes_.empty()) prefetch_read<3>(bitplanes_.data() + first, count * sizeof(DnaBits));
  prefetch_read<3>(exception_offsets_.data() + first, (count + 1) * sizeof(std::uint64_t));
  const auto begin = exception_offsets_[first];
  const auto end = exception_offsets_[first + count];
  if (begin != end) {
    prefetch_read<3>(exception_positions_.data() + begin, (end - begin) * sizeof(std::uint32_t));
    prefetch_read<3>(exception_symbols_.data() + begin, end - begin);
  }
}

std::uint64_t PackedCenters::bytes() const noexcept {
  return bases_.size() + bitplanes_.size() * sizeof(DnaBits) +
      exception_offsets_.size() * sizeof(std::uint64_t) +
      exception_positions_.size() * sizeof(std::uint32_t) + exception_symbols_.size();
}

}  // namespace navigamer
