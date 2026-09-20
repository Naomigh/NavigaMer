#pragma once

#include <string_view>

namespace navigamer {

class EditDistance {
 public:
  [[nodiscard]] static int exact(std::string_view lhs, std::string_view rhs);
  [[nodiscard]] static int bounded(std::string_view lhs, std::string_view rhs,
                                   int max_distance);
};

}  // namespace navigamer
