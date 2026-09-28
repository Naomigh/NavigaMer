#pragma once

#include "navigamer/edit_distance.hpp"

namespace navigamer::detail {

[[nodiscard]] int leap_distance(const DnaBits& lhs, const DnaBits& rhs,
                                int length, int max_distance);

}  // namespace navigamer::detail
