//
// Modernized for C++23.
//
// Endian-neutral encoding utilities for LevelDB's on-disk / on-wire formats:
//   * Fixed-length integers are encoded in little-endian byte order
//   * Variable-length integers use "varint" encoding (7 bits per byte,
//     MSB = continuation flag, identical to Protocol Buffers)
//   * Strings are encoded as a varint length prefix followed by raw bytes
//
// Design philosophy (C++23 modernization):
//   - Old: `Slice` for string parameters
//   - New: `std::string_view` (standard library replacement)
//   - Old: output parameters (`bool Get*(Slice*, uint32_t*)`)
//   - New: return `std::optional<T>`, eliminating output parameters
//   - Old: manual byte shuffling for all platforms
//   - New: `std::endian` compile-time LE detection for fast memcpy paths
//   - Pointer parameters replaced with references where appropriate
//   - `constexpr`, `noexcept`, `[[nodiscard]]` applied throughout

#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace leveldb {

// ===========================================================================
// Fixed-length encoding (little-endian)
// ===========================================================================

// Append a fixed-length integer to dst.
void PutFixed32(std::string& dst, uint32_t value);
void PutFixed64(std::string& dst, uint64_t value);

// Write a fixed-length integer directly into a character buffer.
// REQUIRES: dst has at least 4 / 8 bytes of space.
inline void EncodeFixed32(char* dst, uint32_t value) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    std::memcpy(dst, &value, sizeof(value));
  } else {
    auto* buf = reinterpret_cast<uint8_t*>(dst);
    buf[0] = static_cast<uint8_t>(value);
    buf[1] = static_cast<uint8_t>(value >> 8);
    buf[2] = static_cast<uint8_t>(value >> 16);
    buf[3] = static_cast<uint8_t>(value >> 24);
  }
}

inline void EncodeFixed64(char* dst, uint64_t value) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    std::memcpy(dst, &value, sizeof(value));
  } else {
    auto* buf = reinterpret_cast<uint8_t*>(dst);
    buf[0] = static_cast<uint8_t>(value);
    buf[1] = static_cast<uint8_t>(value >> 8);
    buf[2] = static_cast<uint8_t>(value >> 16);
    buf[3] = static_cast<uint8_t>(value >> 24);
    buf[4] = static_cast<uint8_t>(value >> 32);
    buf[5] = static_cast<uint8_t>(value >> 40);
    buf[6] = static_cast<uint8_t>(value >> 48);
    buf[7] = static_cast<uint8_t>(value >> 56);
  }
}

// Read a fixed-length integer from ptr. No bounds checking.
[[nodiscard]] inline uint32_t DecodeFixed32(const char* ptr) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    uint32_t result;
    std::memcpy(&result, ptr, sizeof(result));
    return result;
  } else {
    const auto* buf = reinterpret_cast<const uint8_t*>(ptr);
    return static_cast<uint32_t>(buf[0]) |
           (static_cast<uint32_t>(buf[1]) << 8) |
           (static_cast<uint32_t>(buf[2]) << 16) |
           (static_cast<uint32_t>(buf[3]) << 24);
  }
}

[[nodiscard]] inline uint64_t DecodeFixed64(const char* ptr) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    uint64_t result;
    std::memcpy(&result, ptr, sizeof(result));
    return result;
  } else {
    const auto* buf = reinterpret_cast<const uint8_t*>(ptr);
    return static_cast<uint64_t>(buf[0]) |
           (static_cast<uint64_t>(buf[1]) << 8) |
           (static_cast<uint64_t>(buf[2]) << 16) |
           (static_cast<uint64_t>(buf[3]) << 24) |
           (static_cast<uint64_t>(buf[4]) << 32) |
           (static_cast<uint64_t>(buf[5]) << 40) |
           (static_cast<uint64_t>(buf[6]) << 48) |
           (static_cast<uint64_t>(buf[7]) << 56);
  }
}

// ===========================================================================
// Variable-length encoding (varint)
// ===========================================================================

// Append a varint-encoded integer to dst.
void PutVarint32(std::string& dst, uint32_t value);
void PutVarint64(std::string& dst, uint64_t value);

// Write a varint-encoded integer directly into a character buffer.
// REQUIRES: dst has at least 5 / 10 bytes of space.
// Returns a pointer just past the last byte written.
char* EncodeVarint32(char* dst, uint32_t value) noexcept;
char* EncodeVarint64(char* dst, uint64_t value) noexcept;

// Decode a varint from the front of input, advancing past consumed bytes.
// Returns std::nullopt if the encoding is invalid or input is too short.
[[nodiscard]] std::optional<uint32_t>
GetVarint32(std::string_view& input) noexcept;

[[nodiscard]] std::optional<uint64_t>
GetVarint64(std::string_view& input) noexcept;

// Returns the number of bytes needed to varint-encode v.
[[nodiscard]] constexpr int VarintLength(uint64_t v) noexcept {
  int len = 1;
  while (v >= 128) {
    v >>= 7;
    ++len;
  }
  return len;
}

// ===========================================================================
// Length-prefixed strings
// ===========================================================================

// Encode: varint length prefix + raw bytes.
void PutLengthPrefixedSlice(std::string& dst, std::string_view value);

// Decode: read varint length, then extract that many bytes.
// Advances input past the consumed bytes on success.
[[nodiscard]] std::optional<std::string_view>
GetLengthPrefixedSlice(std::string_view& input) noexcept;

// ===========================================================================
// Internal detail — low-level pointer-based varint decoding
// ===========================================================================
// These are exposed for performance-critical internal code paths that need
// to avoid the overhead of std::optional and string_view bookkeeping.
// Prefer the public GetVarint* functions in normal code.

namespace detail {

// Fallback path for multi-byte varint32 decoding.
const char* GetVarint32PtrFallback(const char* p, const char* limit,
                                   uint32_t& value) noexcept;

// Fast-path varint32 decode from [p, limit).
// On success: stores decoded value, returns pointer past parsed bytes.
// On failure: returns nullptr.
inline const char* GetVarint32Ptr(const char* p, const char* limit,
                                  uint32_t& value) noexcept {
  if (p < limit) {
    uint32_t result = *reinterpret_cast<const uint8_t*>(p);
    if ((result & 128) == 0) {
      value = result;
      return p + 1;
    }
  }
  return GetVarint32PtrFallback(p, limit, value);
}

// Decode varint64 from [p, limit).
const char* GetVarint64Ptr(const char* p, const char* limit,
                           uint64_t& value) noexcept;

}  // namespace detail

}  // namespace leveldb
