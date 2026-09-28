#include "navigamer/edit_distance.hpp"
#include "navigamer/packed_centers.hpp"

#include "edlib.h"
#ifdef NAVIGAMER_HAVE_LEAP
#include "leap_distance.hpp"
#endif

#include <algorithm>
#include <stdexcept>

namespace navigamer {
namespace {

int edlib_distance(std::string_view lhs, std::string_view rhs, int k) {
  const auto config = edlibNewAlignConfig(k, EDLIB_MODE_NW,
                                          EDLIB_TASK_DISTANCE, nullptr, 0);
  auto result = edlibAlign(lhs.data(), static_cast<int>(lhs.size()), rhs.data(),
                           static_cast<int>(rhs.size()), config);
  const int status = result.status;
  const int distance = result.editDistance;
  edlibFreeAlignResult(result);
  if (status != EDLIB_STATUS_OK) {
    throw std::runtime_error("Edlib failed to calculate edit distance");
  }
  return k >= 0 && distance > k ? -1 : distance;
}

bool only_atgc(std::string_view sequence) {
  return std::all_of(sequence.begin(), sequence.end(), [](char base) {
    return base == 'A' || base == 'T' || base == 'G' || base == 'C';
  });
}

}  // namespace

DnaBits::DnaBits(std::string_view sequence) {
  for (std::size_t i = 0; i < sequence.size(); ++i) {
    const auto base = sequence[i];
    low[i / 8] |= std::uint8_t(base == 'C' || base == 'T') << (i % 8);
    high[i / 8] |= std::uint8_t(base == 'G' || base == 'T') << (i % 8);
  }
}

PreparedQuery::PreparedQuery(std::string_view sequence)
    : sequence_(sequence), dna_(only_atgc(sequence)) {
  if (dna_ && sequence.size() <= EditDistance::kLeapMaxLength &&
      EditDistance::leap_available()) bits_ = DnaBits(sequence);
}

void PreparedQuery::ProfileDeleter::operator()(EdlibDnaPrepared* profile) const {
  edlibDnaPreparedFree(profile);
}

bool PreparedQuery::use_leap(std::size_t target_length, int k) const {
  return dna_ && k > 0 && k <= EditDistance::kLeapMaxDistance &&
      sequence_.size() == target_length && sequence_.size() <= EditDistance::kLeapMaxLength &&
      static_cast<std::size_t>(k) < sequence_.size() && EditDistance::leap_available();
}

int PreparedQuery::edlib(std::string_view target, int k) {
  if (dna_) {
    if (!profile_) profile_.reset(edlibDnaPrepare(sequence_.data(), sequence_.size()));
    if (profile_) {
      const int result = k < 0
          ? edlibDnaPreparedDistance(profile_.get(), target.data(), target.size())
          : edlibDnaPreparedBoundedDistance(profile_.get(), target.data(), target.size(), k);
      if (result != -2) return result;
    }
  }
  return edlib_distance(sequence_, target, k);
}

int PreparedQuery::distance(std::string_view target, int k) {
  if (k == 0) return sequence_ == target ? 0 : -1;
#ifdef NAVIGAMER_HAVE_LEAP
  if (use_leap(target.size(), k) && only_atgc(target)) {
    return detail::leap_distance(bits_, DnaBits(target), sequence_.size(), k);
  }
#endif
  return edlib(target, k);
}

int PreparedQuery::distance(const PackedCenters& centers, std::size_t id,
                            std::string& scratch, int k) {
#ifdef NAVIGAMER_HAVE_LEAP
  if (use_leap(centers.length(), k)) {
    if (const auto* target = centers.bitplanes(id)) {
      return detail::leap_distance(bits_, *target, sequence_.size(), k);
    }
  }
#endif
  return distance(centers.decode(id, scratch), k);
}

int EditDistance::exact(std::string_view lhs, std::string_view rhs) {
  PreparedQuery query(lhs);
  return query.distance(rhs);
}

int EditDistance::bounded(std::string_view lhs, std::string_view rhs,
                          int max_distance) {
  PreparedQuery query(lhs);
  return query.distance(rhs, max_distance);
}

EditDistanceBackend EditDistance::route(
    std::string_view lhs, std::string_view rhs, int max_distance) {
  if (max_distance <= 0 || max_distance > kLeapMaxDistance || lhs.empty() ||
      lhs.size() != rhs.size() || lhs.size() > kLeapMaxLength ||
      static_cast<std::size_t>(max_distance) >= lhs.size() ||
      !leap_available() || !only_atgc(lhs) || !only_atgc(rhs)) {
    return EditDistanceBackend::kEdlib;
  }
  return EditDistanceBackend::kLeap;
}

bool EditDistance::leap_available() noexcept {
#if defined(NAVIGAMER_HAVE_LEAP) && \
    (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__))
  static const bool available = [] {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") &&
           __builtin_cpu_supports("bmi") &&
           __builtin_cpu_supports("ssse3");
  }();
  return available;
#else
  return false;
#endif
}

}  // namespace navigamer
