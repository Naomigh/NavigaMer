#include "navigamer/edit_distance.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

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
  int carry_out = static_cast<int>((positive_horizontal & kHighBit) >> 63);
  carry_out -= static_cast<int>((negative_horizontal & kHighBit) >> 63);
  positive_horizontal <<= 1;
  negative_horizontal <<= 1;
  negative_horizontal |= carry_is_negative;
  positive_horizontal |= static_cast<std::uint64_t>((carry_in + 1) >> 1);
  positive_out = negative_horizontal | ~(vertical | positive_horizontal);
  negative_out = positive_horizontal & vertical;
  return carry_out;
}

}  // namespace

EditDistance::Prepared::Prepared(std::string_view query) { reset(query); }

void EditDistance::Prepared::reset(std::string_view query) {
  for (const auto symbol : active_symbols_) {
    if (block_count_ != 0) {
      std::fill_n(equality_masks_.data() +
                      static_cast<std::size_t>(symbol) * block_count_,
                  block_count_, 0);
    }
    symbol_active_[symbol] = 0;
  }
  active_symbols_.clear();
  query_length_ = query.size();
  const auto new_block_count = (query.size() + kWordBits - 1) / kWordBits;
  if (new_block_count != block_count_) {
    block_count_ = new_block_count;
    equality_masks_.assign(256 * block_count_, 0);
    blocks_.resize(block_count_);
  }
  if (symbol_active_.empty()) symbol_active_.assign(256, 0);
  padding_ = block_count_ * kWordBits - query.size();
  padding_mask_ = 0;
  if (padding_ != 0) {
    padding_mask_ = ~((std::uint64_t{1} << (kWordBits - padding_)) - 1);
  }
  for (std::size_t position = 0; position < query.size(); ++position) {
    const auto symbol = static_cast<unsigned char>(query[position]);
    if (symbol_active_[symbol] == 0) {
      symbol_active_[symbol] = 1;
      active_symbols_.push_back(symbol);
    }
    equality_masks_[static_cast<std::size_t>(symbol) * block_count_ +
                    position / kWordBits] |=
        std::uint64_t{1} << (position % kWordBits);
  }
}

int EditDistance::Prepared::exact(std::string_view target) const {
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
      auto equality = symbol_active_[symbol] == 0 ? 0 : equal[block];
      if (block + 1 == block_count_) equality |= padding_mask_;
      carry = advance_block(state.positive, state.negative, equality, carry,
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

int EditDistance::exact(std::string_view lhs, std::string_view rhs) {
  return Prepared(lhs).exact(rhs);
}

int EditDistance::bounded(std::string_view lhs, std::string_view rhs,
                          int max_distance) {
  if (std::abs(static_cast<long long>(lhs.size()) -
               static_cast<long long>(rhs.size())) > max_distance) return -1;
  const int inf = max_distance + 1;
  std::vector<int> previous(rhs.size() + 1, inf), current(rhs.size() + 1, inf);
  for (std::size_t j = 0;
       j <= std::min(rhs.size(), static_cast<std::size_t>(max_distance)); ++j) {
    previous[j] = static_cast<int>(j);
  }
  for (std::size_t i = 1; i <= lhs.size(); ++i) {
    std::fill(current.begin(), current.end(), inf);
    const auto begin = i > static_cast<std::size_t>(max_distance)
        ? i - max_distance : 0;
    const auto end = std::min(rhs.size(), i + max_distance);
    if (begin == 0) current[0] = static_cast<int>(i);
    for (auto j = std::max<std::size_t>(1, begin); j <= end; ++j) {
      current[j] = std::min({previous[j] + 1, current[j - 1] + 1,
                            previous[j - 1] + (lhs[i - 1] != rhs[j - 1])});
    }
    previous.swap(current);
  }
  return previous[rhs.size()] <= max_distance ? previous[rhs.size()] : -1;
}

}  // namespace navigamer
