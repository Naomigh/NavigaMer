#include "navigamer/edit_distance.hpp"

#include <edlib.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace navigamer {
namespace {

constexpr std::size_t kWordBits = 64;
constexpr std::uint64_t kHighBit = std::uint64_t{1} << 63;

int advance_block(std::uint64_t positive, std::uint64_t negative,
                  std::uint64_t equal, int carry_in,
                  std::uint64_t& positive_out,
                  std::uint64_t& negative_out) noexcept {
  const auto carry_is_negative =
      static_cast<std::uint64_t>(carry_in >> 2) & std::uint64_t{1};
  const auto vertical = equal | negative;
  equal |= carry_is_negative;
  const auto horizontal = (((equal & positive) + positive) ^ positive) | equal;
  auto positive_horizontal = negative | ~(horizontal | positive);
  auto negative_horizontal = positive & horizontal;
  int carry_out = static_cast<int>(
      (positive_horizontal & kHighBit) >> (kWordBits - 1));
  carry_out -= static_cast<int>(
      (negative_horizontal & kHighBit) >> (kWordBits - 1));
  positive_horizontal <<= 1;
  negative_horizontal <<= 1;
  negative_horizontal |= carry_is_negative;
  positive_horizontal |= static_cast<std::uint64_t>((carry_in + 1) >> 1);
  positive_out = negative_horizontal | ~(vertical | positive_horizontal);
  negative_out = positive_horizontal & vertical;
  return carry_out;
}

}  // namespace

EditDistance::Prepared::Prepared(const EditDistance& owner,
                                 std::string_view query)
    : owner_(&owner),
      query_length_(query.size()),
      block_count_((query.size() + kWordBits - 1) / kWordBits),
      padding_(block_count_ * kWordBits - query.size()),
      equality_masks_(256 * block_count_, 0),
      blocks_(block_count_) {
  if (query.empty()) return;
  if (query.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error("sequence is too long for prepared edit distance");
  }

  if (padding_ != 0) {
    const auto valid = kWordBits - padding_;
    const auto padding_mask = ~((std::uint64_t{1} << valid) - 1);
    const auto last = block_count_ - 1;
    for (std::size_t symbol = 0; symbol < 256; ++symbol) {
      equality_masks_[symbol * block_count_ + last] = padding_mask;
    }
  }
  for (std::size_t position = 0; position < query.size(); ++position) {
    const auto symbol = static_cast<unsigned char>(query[position]);
    equality_masks_[static_cast<std::size_t>(symbol) * block_count_ +
                    position / kWordBits] |=
        std::uint64_t{1} << (position % kWordBits);
  }
}

int EditDistance::Prepared::operator()(std::string_view target) const {
  if (target.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error("sequence is too long for prepared edit distance");
  }
  owner_->calls_.fetch_add(1, std::memory_order_relaxed);
  if (query_length_ == 0) return static_cast<int>(target.size());
  if (target.empty()) return static_cast<int>(query_length_);

  for (std::size_t block = 0; block < block_count_; ++block) {
    blocks_[block].positive = std::numeric_limits<std::uint64_t>::max();
    blocks_[block].negative = 0;
    blocks_[block].score = static_cast<int>((block + 1) * kWordBits);
  }
  for (const unsigned char symbol : target) {
    int carry = 1;
    const auto* equal = equality_masks_.data() +
        static_cast<std::size_t>(symbol) * block_count_;
    for (std::size_t block = 0; block < block_count_; ++block) {
      auto& state = blocks_[block];
      carry = advance_block(state.positive, state.negative, equal[block], carry,
                            state.positive, state.negative);
      state.score += carry;
    }
  }

  const auto& last = blocks_.back();
  int score = last.score;
  auto mask = kHighBit;
  for (std::size_t offset = 0; offset < padding_; ++offset) {
    if ((last.positive & mask) != 0) --score;
    if ((last.negative & mask) != 0) ++score;
    mask >>= 1;
  }
  return score;
}

int EditDistance::operator()(std::string_view lhs, std::string_view rhs,
                             int max_distance) const {
  if (lhs.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      rhs.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error("sequence is too long for Edlib");
  }
  calls_.fetch_add(1, std::memory_order_relaxed);
  const auto config = edlibNewAlignConfig(max_distance, EDLIB_MODE_NW,
                                          EDLIB_TASK_DISTANCE, nullptr, 0);
  auto result = edlibAlign(lhs.data(), static_cast<int>(lhs.size()), rhs.data(),
                           static_cast<int>(rhs.size()), config);
  if (result.status != EDLIB_STATUS_OK) {
    edlibFreeAlignResult(result);
    throw std::runtime_error("Edlib failed to calculate edit distance");
  }
  const int edit_distance = result.editDistance;
  edlibFreeAlignResult(result);
  return edit_distance;
}

}  // namespace navigamer
