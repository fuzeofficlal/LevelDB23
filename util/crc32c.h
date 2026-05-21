//
// Modernized for C++23.
//
// CRC32C (Castagnoli) checksum used for data integrity in LevelDB.
//
// C++23 changes:
//   - (const char*, size_t) → std::string_view
//   - Mask/Unmask/kMaskDelta upgraded to constexpr
//   - static const → inline constexpr
//   - Nested namespace leveldb::crc32c (C++17)
//   - [[nodiscard]], noexcept throughout
//   - #pragma once

#pragma once

#include <cstdint>
#include <string_view>

namespace leveldb::crc32c {

// Return the crc32c of concat(A, data) where init_crc is the
// crc32c of some string A.  Extend() is often used to maintain the
// crc32c of a stream of data.
[[nodiscard]] uint32_t Extend(uint32_t init_crc,
                              std::string_view data) noexcept;

// Return the crc32c of data.
[[nodiscard]] inline uint32_t Value(std::string_view data) noexcept {
  return Extend(0, data);
}

inline constexpr uint32_t kMaskDelta = 0xa282ead8u;

// Return a masked representation of crc.
//
// Motivation: it is problematic to compute the CRC of a string that
// contains embedded CRCs.  Therefore we recommend that CRCs stored
// somewhere (e.g., in files) should be masked before being stored.
[[nodiscard]] constexpr uint32_t Mask(uint32_t crc) noexcept {
  // Rotate right by 15 bits and add a constant.
  return ((crc >> 15) | (crc << 17)) + kMaskDelta;
}

// Return the crc whose masked representation is masked_crc.
[[nodiscard]] constexpr uint32_t Unmask(uint32_t masked_crc) noexcept {
  uint32_t rot = masked_crc - kMaskDelta;
  return (rot >> 17) | (rot << 15);
}

}  // namespace leveldb::crc32c
