//
// Modernized for C++23.
//
// Simple hash function used for internal data structures (LRU cache sharding,
// Bloom filters, etc.).  The algorithm is similar to MurmurHash.
//
// C++23 changes:
//   - Old: Hash(const char* data, size_t n, uint32_t seed)
//   - New: Hash(std::string_view data, uint32_t seed)
//   - Added [[nodiscard]], noexcept

#pragma once

#include <cstdint>
#include <string_view>

namespace leveldb {

// MurmurHash-like hash for internal data structures.
[[nodiscard]] uint32_t Hash(std::string_view data, uint32_t seed) noexcept;

}  // namespace leveldb
