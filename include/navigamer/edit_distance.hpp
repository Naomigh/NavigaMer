#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

struct EdlibDnaPrepared;

namespace navigamer {

enum class EditDistanceBackend { kEdlib, kLeap };

// At most 256 bases. LEAP compares these planes by XOR; one bit per position.
struct alignas(32) DnaBits {
  std::array<std::uint8_t, 32> low{};
  std::array<std::uint8_t, 32> high{};
  DnaBits() = default;
  explicit DnaBits(std::string_view sequence);
};

class PackedCenters;

// Non-owning query text must outlive this object. Preparation is reused across
// comparisons; the Edlib profile is created only when an Edlib call needs it.
class PreparedQuery {
 public:
  explicit PreparedQuery(std::string_view sequence);
  [[nodiscard]] int distance(std::string_view target, int k = -1);
  [[nodiscard]] int distance(const PackedCenters& centers, std::size_t id,
                             std::string& scratch, int k = -1);

 private:
  struct ProfileDeleter { void operator()(EdlibDnaPrepared*) const; };
  [[nodiscard]] bool use_leap(std::size_t target_length, int k) const;
  [[nodiscard]] int edlib(std::string_view target, int k);
  std::string_view sequence_;
  bool dna_;
  DnaBits bits_;
  std::unique_ptr<EdlibDnaPrepared, ProfileDeleter> profile_;
};

class EditDistance {
 public:
  static constexpr int kLeapMaxDistance = 15;
  static constexpr std::size_t kLeapMaxLength = 256;

  [[nodiscard]] static int exact(std::string_view lhs, std::string_view rhs);
  [[nodiscard]] static int bounded(std::string_view lhs, std::string_view rhs,
                                   int max_distance);
  [[nodiscard]] static EditDistanceBackend route(
      std::string_view lhs, std::string_view rhs, int max_distance);
  [[nodiscard]] static bool leap_available() noexcept;
};

}  // namespace navigamer
