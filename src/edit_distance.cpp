#include "navigamer/edit_distance.hpp"

#include "edlib.h"

#include <stdexcept>

namespace navigamer {
namespace {

int align_distance(std::string_view lhs, std::string_view rhs, int k) {
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

}  // namespace

int EditDistance::exact(std::string_view lhs, std::string_view rhs) {
  return align_distance(lhs, rhs, -1);
}

int EditDistance::bounded(std::string_view lhs, std::string_view rhs,
                          int max_distance) {
  return align_distance(lhs, rhs, max_distance);
}

}  // namespace navigamer
