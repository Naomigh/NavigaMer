#include "navigamer/member_classifier.hpp"

#include <algorithm>
#include <stdexcept>

namespace navigamer {

AdjacentClassifier::AdjacentClassifier(const SequenceStore& reference,
    const std::vector<SequenceId>& centers, const BatchDistance& profiles,
    std::uint32_t tolerance)
    : reference_(reference), centers_(centers), profiles_(profiles), tolerance_(tolerance),
      distances_(centers.size()), updated_(centers.size()), membership_(centers.size()),
      exclusive_(centers.size()) {
  if (centers.empty() || centers.size() != profiles.size() ||
      !std::is_sorted(centers.begin(), centers.end()) ||
      std::adjacent_find(centers.begin(), centers.end()) != centers.end() ||
      centers.back() >= reference.size()) {
    throw std::invalid_argument("classifier requires sorted unique centers and matching profiles");
  }
}

int AdjacentClassifier::lower(std::size_t center) const {
  const auto age = step_ - updated_[center];
  if (age >= (std::uint64_t{reference_.window_length()} + 1) / 2) return 0;
  return std::max(0, distances_[center] - 2 * static_cast<int>(age));
}

int AdjacentClassifier::upper(std::size_t center) const {
  const auto age = step_ - updated_[center];
  const auto length = reference_.window_length();
  if (age >= (std::uint64_t{length} + 1) / 2) return length;
  return std::min(static_cast<int>(length), distances_[center] + 2 * static_cast<int>(age));
}

std::span<const std::uint8_t> AdjacentClassifier::classify(SequenceId id) {
  const auto query = reference_.sequence(id);
  if (query.size() != reference_.window_length()) {
    throw std::invalid_argument("adjacent classification requires fixed-length windows");
  }
  const auto center = std::lower_bound(centers_.begin(), centers_.end(), id);
  if (center != centers_.end() && *center == id) {
    // Center exclusivity is by occurrence ID, not by sequence text. Do not
    // poison the ordinary membership cache: an identical non-center overlaps.
    std::fill(exclusive_.begin(), exclusive_.end(), 0);
    exclusive_[center - centers_.begin()] = 1;
    ++exclusive_members;
    return exclusive_;
  }
  const auto location = reference_.location(id);
  const bool same_contig = valid_ && location.first.data() == previous_location_.first.data();
  const auto shift = same_contig ? (location.second >= previous_location_.second
      ? location.second - previous_location_.second : previous_location_.second - location.second) : 0;
  const bool reuse = same_contig && shift < reference_.window_length();
  if (reuse && query == previous_) {
    previous_ = query;
    previous_location_ = location;
    ++repeated;
    return membership_;
  }

  int minimum = reference_.window_length();
  if (!reuse) {
    step_ = 0;
    std::fill(updated_.begin(), updated_.end(), 0);
    for (std::size_t c = 0; c < centers_.size(); c += BatchDistance::kLanes) {
      const auto values = profiles_.exact_group(query, c / BatchDistance::kLanes, workspace_);
      const auto count = std::min(BatchDistance::kLanes, centers_.size() - c);
      exact_pairs += count;
      if (BatchDistance::simd_available()) ++batch_groups;
      for (std::size_t lane = 0; lane < count; ++lane) {
        distances_[c + lane] = values[lane];
        if (values[lane] <= minimum) { minimum = values[lane]; nearest_ = c + lane; }
      }
    }
    const auto limit = static_cast<std::uint64_t>(minimum) + 2 * std::uint64_t{tolerance_};
    for (std::size_t c = 0; c < centers_.size(); ++c) membership_[c] = static_cast<std::uint64_t>(distances_[c]) <= limit;
  } else {
    // A filtered parent can skip windows. Charge the actual coordinate gap,
    // not one logical step; same-contig windows s bases apart differ by <=2s.
    step_ += shift;
    const std::array<std::size_t, 1> nearest{nearest_};
    minimum = profiles_.exact_selected(query, nearest, workspace_)[0];
    distances_[nearest_] = minimum;
    updated_[nearest_] = step_;
    ++exact_pairs;
    std::array<std::size_t, BatchDistance::kLanes> pending;
    std::size_t count = 0;
    auto evaluate = [&] {
      if (count == 0) return;
      const auto values = profiles_.exact_selected(query, std::span(pending.data(), count), workspace_);
      exact_pairs += count;
      if (count > 1 && BatchDistance::simd_available()) ++batch_groups;
      for (std::size_t lane = 0; lane < count; ++lane) {
        const auto c = pending[lane];
        distances_[c] = values[lane];
        updated_[c] = step_;
        if (values[lane] < minimum) { minimum = values[lane]; nearest_ = c; }
      }
      count = 0;
    };
    // Skips remain sound when the current upper bound on the minimum decreases.
    for (std::size_t c = 0; c < centers_.size(); ++c) {
      if (updated_[c] != step_ && lower(c) < minimum) {
        pending[count++] = c;
        if (count == pending.size()) evaluate();
      }
    }
    evaluate();
    const auto limit = static_cast<std::uint64_t>(minimum) + 2 * std::uint64_t{tolerance_};
    auto resolve = [&] {
      if (count == 0) return;
      const auto values = profiles_.exact_selected(query, std::span(pending.data(), count), workspace_);
      exact_pairs += count;
      if (count > 1 && BatchDistance::simd_available()) ++batch_groups;
      for (std::size_t lane = 0; lane < count; ++lane) {
        const auto c = pending[lane];
        distances_[c] = values[lane];
        updated_[c] = step_;
        membership_[c] = static_cast<std::uint64_t>(values[lane]) <= limit;
      }
      count = 0;
    };
    for (std::size_t c = 0; c < centers_.size(); ++c) {
      if (static_cast<std::uint64_t>(upper(c)) <= limit) membership_[c] = 1;
      else if (static_cast<std::uint64_t>(lower(c)) > limit) membership_[c] = 0;
      else {
        pending[count++] = c;
        if (count == pending.size()) resolve();
      }
    }
    resolve();
  }
  valid_ = true;
  previous_ = query;
  previous_location_ = location;
  return membership_;
}

}  // namespace navigamer
