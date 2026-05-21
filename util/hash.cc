#include "util/hash.h"

#include "util/coding.h"  // DecodeFixed32 — already modernized (inline, std::endian)

namespace leveldb {

uint32_t Hash(std::string_view data, uint32_t seed) noexcept {
  // Similar to murmur hash
  constexpr uint32_t m = 0xc6a4a793;
  constexpr uint32_t r = 24;

  const char* p = data.data();
  size_t n = data.size();
  uint32_t h = seed ^ (static_cast<uint32_t>(n) * m);

  // Pick up four bytes at a time
  while (n >= 4) {
    uint32_t w = DecodeFixed32(p);
    p += 4;
    n -= 4;
    h += w;
    h *= m;
    h ^= (h >> 16);
  }

  // Pick up remaining bytes
  switch (n) {
    case 3:
      h += static_cast<uint8_t>(p[2]) << 16;
      [[fallthrough]];
    case 2:
      h += static_cast<uint8_t>(p[1]) << 8;
      [[fallthrough]];
    case 1:
      h += static_cast<uint8_t>(p[0]);
      h *= m;
      h ^= (h >> r);
      break;
  }

  return h;
}

}  // namespace leveldb
