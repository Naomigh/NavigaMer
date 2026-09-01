#include "navigamer/edit_distance.hpp"

#include <edlib.h>

#include <limits>
#include <stdexcept>

namespace navigamer {

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
