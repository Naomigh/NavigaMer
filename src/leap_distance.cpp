#include "leap_distance.hpp"

#include "navigamer/edit_distance.hpp"

#include "SIMD_ED.h"

#include <array>
#include <memory>

namespace navigamer::detail {
namespace {

class LeapEngines {
 public:
  SIMD_ED& get(int max_distance) {
    auto& engine = engines_[max_distance];
    if (!engine) {
      engine = std::make_unique<SIMD_ED>();
      engine->init_levenshtein(max_distance, ED_GLOBAL, false);
    }
    return *engine;
  }

 private:
  std::array<std::unique_ptr<SIMD_ED>, EditDistance::kLeapMaxDistance + 1>
      engines_;
};

}  // namespace

int leap_distance(const DnaBits& lhs, const DnaBits& rhs,
                  int length, int max_distance) {
  thread_local LeapEngines engines;
  auto& engine = engines.get(max_distance);
  engine.load_reads(
      _mm256_load_si256(reinterpret_cast<const __m256i*>(lhs.low.data())),
      _mm256_load_si256(reinterpret_cast<const __m256i*>(lhs.high.data())),
      _mm256_load_si256(reinterpret_cast<const __m256i*>(rhs.low.data())),
      _mm256_load_si256(reinterpret_cast<const __m256i*>(rhs.high.data())), length);
  engine.calculate_masks();
  engine.reset();
  engine.run();
  return engine.check_pass() ? engine.get_ED() : -1;
}

}  // namespace navigamer::detail
