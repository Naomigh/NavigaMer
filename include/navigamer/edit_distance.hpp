#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace navigamer {

class EditDistance {
 public:
  class Prepared {
   public:
    [[nodiscard]] int exact(std::string_view target) const;
    void reset(std::string_view query);

   private:
    friend class EditDistance;
    struct Block {
      std::uint64_t positive{0};
      std::uint64_t negative{0};
      int score{0};
    };
    explicit Prepared(std::string_view query);
    std::size_t query_length_{0};
    std::size_t block_count_{0};
    std::size_t padding_{0};
    std::uint64_t padding_mask_{0};
    std::vector<std::uint64_t> equality_masks_;
    std::vector<unsigned char> active_symbols_;
    std::vector<std::uint8_t> symbol_active_;
    mutable std::vector<Block> blocks_;
  };

  [[nodiscard]] Prepared prepare(std::string_view query) const {
    return Prepared(query);
  }
  [[nodiscard]] static int exact(std::string_view lhs, std::string_view rhs);
  [[nodiscard]] static int bounded(std::string_view lhs, std::string_view rhs,
                                   int max_distance);
};

}  // namespace navigamer
