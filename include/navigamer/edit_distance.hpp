#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>
#include <vector>

namespace navigamer {

class EditDistance {
 public:
  // Reuses the Myers query profile and its tiny block workspace across many
  // targets. This is the same exact global Levenshtein recurrence used by
  // Edlib, specialized for byte equality and distance-only queries.
  class Prepared {
   public:
    [[nodiscard]] int operator()(std::string_view target) const;

   private:
    friend class EditDistance;
    struct Block {
      std::uint64_t positive{0};
      std::uint64_t negative{0};
      int score{0};
    };

    Prepared(const EditDistance& owner, std::string_view query);

    const EditDistance* owner_{nullptr};
    std::size_t query_length_{0};
    std::size_t block_count_{0};
    std::size_t padding_{0};
    std::vector<std::uint64_t> equality_masks_;
    mutable std::vector<Block> blocks_;
  };

  // Exact global Levenshtein distance. If max_distance >= 0, -1 means the
  // exact distance is greater than max_distance.
  [[nodiscard]] int operator()(std::string_view lhs, std::string_view rhs,
                               int max_distance = -1) const;

  [[nodiscard]] Prepared prepare(std::string_view query) const {
    return Prepared(*this, query);
  }

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
