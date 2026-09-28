#include "navigamer/batch_distance.hpp"
#include "navigamer/edit_distance.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

#if defined(NAVIGAMER_HAVE_BATCH_MYERS) && \
    (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__))
#define NAVIGAMER_BATCH_AVX2 1
#include <immintrin.h>
#endif

namespace navigamer {

bool BatchDistance::simd_available() noexcept {
#ifdef NAVIGAMER_BATCH_AVX2
  static const bool available = [] {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
  }();
  return available;
#else
  return false;
#endif
}

BatchDistance::BatchDistance(std::size_t length)
    : length_(length), block_count_(length / 64 + (length % 64 != 0)),
      padding_mask_(length % 64 == 0 ? 0 : ~((std::uint64_t{1} << (length % 64)) - 1)) {
  if (length > static_cast<std::size_t>(std::numeric_limits<int>::max() - 64)) {
    throw std::length_error("sequence too long for batch distance");
  }
}

void BatchDistance::append(std::string_view center) {
  if (center.size() != length_) {
    throw std::invalid_argument("batch centers must have equal length");
  }
  if (simd_available() && length_ != 0) {
    const auto lane = size() % kLanes;
    if (lane == 0) groups_.emplace_back();
    auto& group = groups_.back();
    for (std::size_t position = 0; position < length_; ++position) {
      auto& symbol = group.symbols[static_cast<unsigned char>(center[position])];
      if (symbol == 256) {
        symbol = group.alphabet_size++;
        group.masks.resize(group.alphabet_size * block_count_ * kLanes, 0);
        const auto last = ((symbol + 1) * block_count_ - 1) * kLanes;
        std::fill_n(group.masks.begin() + last, kLanes, padding_mask_);
      }
      group.masks[(symbol * block_count_ + position / 64) * kLanes + lane] |=
          std::uint64_t{1} << (position % 64);
    }
  }
  centers_.emplace_back(center);
}

std::array<int, BatchDistance::kLanes> BatchDistance::exact_group(
    std::string_view target, std::size_t group, Workspace& workspace) const {
  if (group >= size() / kLanes + (size() % kLanes != 0)) {
    throw std::out_of_range("invalid batch distance group");
  }
  if (target.size() > static_cast<std::size_t>(std::numeric_limits<int>::max() - 64)) {
    throw std::length_error("target too long for batch distance");
  }
  std::array<int, kLanes> result{};
  if (length_ == 0) {
    result.fill(static_cast<int>(target.size()));
    return result;
  }
#ifdef NAVIGAMER_BATCH_AVX2
  if (simd_available()) return avx2_group(target, group, workspace);
#else
  (void)workspace;
#endif
  for (std::size_t lane = 0; lane < kLanes && group * kLanes + lane < size(); ++lane) {
    result[lane] = EditDistance::exact(centers_[group * kLanes + lane], target);
  }
  return result;
}

#ifdef NAVIGAMER_BATCH_AVX2
__attribute__((target("avx2")))
std::array<int, BatchDistance::kLanes> BatchDistance::avx2_group(
    std::string_view target, std::size_t group_id, Workspace& workspace) const {
  const auto& group = groups_[group_id];
  workspace.blocks.resize(block_count_);
  const auto ones = _mm256_set1_epi64x(-1);
  const auto one = _mm256_set1_epi64x(1);
  for (auto& block : workspace.blocks) {
    _mm256_store_si256(reinterpret_cast<__m256i*>(block.positive.data()), ones);
    _mm256_store_si256(reinterpret_cast<__m256i*>(block.negative.data()), _mm256_setzero_si256());
  }
  auto score = _mm256_set1_epi64x(static_cast<long long>(block_count_ * 64));
  for (const unsigned char symbol : target) {
    auto carry = one;
    const auto slot = group.symbols[symbol];
    for (std::size_t b = 0; b < block_count_; ++b) {
      auto& block = workspace.blocks[b];
      const auto positive = _mm256_load_si256(reinterpret_cast<const __m256i*>(block.positive.data()));
      const auto negative = _mm256_load_si256(reinterpret_cast<const __m256i*>(block.negative.data()));
      const auto equal = slot == 256
          ? _mm256_set1_epi64x(b + 1 == block_count_ ? static_cast<long long>(padding_mask_) : 0)
          : _mm256_loadu_si256(reinterpret_cast<const __m256i*>(
                group.masks.data() + (slot * block_count_ + b) * kLanes));
      // carry is -1, 0 or 1. Logical shift suffices to extract its sign bit.
      const auto carry_negative = _mm256_and_si256(_mm256_srli_epi64(carry, 2), one);
      const auto vertical = _mm256_or_si256(equal, negative);
      const auto eq_carry = _mm256_or_si256(equal, carry_negative);
      const auto horizontal = _mm256_or_si256(
          _mm256_xor_si256(_mm256_add_epi64(_mm256_and_si256(eq_carry, positive), positive), positive),
          eq_carry);
      auto ph = _mm256_or_si256(negative, _mm256_xor_si256(_mm256_or_si256(horizontal, positive), ones));
      auto mh = _mm256_and_si256(positive, horizontal);
      const auto next = _mm256_sub_epi64(_mm256_srli_epi64(ph, 63), _mm256_srli_epi64(mh, 63));
      ph = _mm256_or_si256(_mm256_slli_epi64(ph, 1), _mm256_srli_epi64(_mm256_add_epi64(carry, one), 1));
      mh = _mm256_or_si256(_mm256_slli_epi64(mh, 1), carry_negative);
      _mm256_store_si256(reinterpret_cast<__m256i*>(block.positive.data()),
          _mm256_or_si256(mh, _mm256_xor_si256(_mm256_or_si256(vertical, ph), ones)));
      _mm256_store_si256(reinterpret_cast<__m256i*>(block.negative.data()), _mm256_and_si256(ph, vertical));
      carry = next;
    }
    score = _mm256_add_epi64(score, carry);
  }
  alignas(32) std::array<std::int64_t, kLanes> scores;
  _mm256_store_si256(reinterpret_cast<__m256i*>(scores.data()), score);
  std::array<int, kLanes> result;
  const auto& last = workspace.blocks.back();
  for (std::size_t lane = 0; lane < kLanes; ++lane) {
    result[lane] = static_cast<int>(scores[lane])
        - std::popcount(last.positive[lane] & padding_mask_)
        + std::popcount(last.negative[lane] & padding_mask_);
  }
  return result;
}
#endif

}  // namespace navigamer
