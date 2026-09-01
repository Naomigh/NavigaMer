#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace navigamer {

class EditDistance {
 public:
  // Exact global Levenshtein distance. If max_distance >= 0, -1 means the
  // exact distance is greater than max_distance.
  [[nodiscard]] int operator()(std::string_view lhs, std::string_view rhs,
                               int max_distance = -1) const;

  [[nodiscard]] std::uint64_t calls() const noexcept {
    return calls_.load(std::memory_order_relaxed);
  }
  void reset_calls() const noexcept {
    calls_.store(0, std::memory_order_relaxed);
  }

 private:
  mutable std::atomic<std::uint64_t> calls_{0};
};

}  // namespace navigamer
