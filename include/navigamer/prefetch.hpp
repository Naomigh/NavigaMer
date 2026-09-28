#pragma once

#include <cstddef>

namespace navigamer {

template <int Locality>
inline void prefetch_read(const void* data, std::size_t bytes) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  const auto* address = static_cast<const char*>(data);
  for (std::size_t offset = 0; offset < bytes; offset += 64) {
    __builtin_prefetch(address + offset, 0, Locality);
  }
  if (bytes != 0) __builtin_prefetch(address + bytes - 1, 0, Locality);
#else
  (void)data;
  (void)bytes;
#endif
}

}  // namespace navigamer
