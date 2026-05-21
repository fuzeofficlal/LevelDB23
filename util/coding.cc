#include "util/coding.h"

namespace leveldb {

// ===========================================================================
// Fixed-length encoding
// ===========================================================================

void PutFixed32(std::string& dst, uint32_t value) {
  char buf[sizeof(value)];
  EncodeFixed32(buf, value);
  dst.append(buf, sizeof(buf));
}

void PutFixed64(std::string& dst, uint64_t value) {
  char buf[sizeof(value)];
  EncodeFixed64(buf, value);
  dst.append(buf, sizeof(buf));
}

// ===========================================================================
// Variable-length encoding
// ===========================================================================

char* EncodeVarint32(char* dst, uint32_t v) noexcept {
  auto* ptr = reinterpret_cast<uint8_t*>(dst);
  static constexpr uint32_t B = 128;

  if (v < (1 << 7)) {
    *(ptr++) = static_cast<uint8_t>(v);
  } else if (v < (1 << 14)) {
    *(ptr++) = static_cast<uint8_t>(v | B);
    *(ptr++) = static_cast<uint8_t>(v >> 7);
  } else if (v < (1 << 21)) {
    *(ptr++) = static_cast<uint8_t>(v | B);
    *(ptr++) = static_cast<uint8_t>((v >> 7) | B);
    *(ptr++) = static_cast<uint8_t>(v >> 14);
  } else if (v < (1 << 28)) {
    *(ptr++) = static_cast<uint8_t>(v | B);
    *(ptr++) = static_cast<uint8_t>((v >> 7) | B);
    *(ptr++) = static_cast<uint8_t>((v >> 14) | B);
    *(ptr++) = static_cast<uint8_t>(v >> 21);
  } else {
    *(ptr++) = static_cast<uint8_t>(v | B);
    *(ptr++) = static_cast<uint8_t>((v >> 7) | B);
    *(ptr++) = static_cast<uint8_t>((v >> 14) | B);
    *(ptr++) = static_cast<uint8_t>((v >> 21) | B);
    *(ptr++) = static_cast<uint8_t>(v >> 28);
  }
  return reinterpret_cast<char*>(ptr);
}

void PutVarint32(std::string& dst, uint32_t v) {
  char buf[5];
  char* ptr = EncodeVarint32(buf, v);
  dst.append(buf, static_cast<size_t>(ptr - buf));
}

char* EncodeVarint64(char* dst, uint64_t v) noexcept {
  static constexpr uint64_t B = 128;
  auto* ptr = reinterpret_cast<uint8_t*>(dst);
  while (v >= B) {
    *(ptr++) = static_cast<uint8_t>(v | B);
    v >>= 7;
  }
  *(ptr++) = static_cast<uint8_t>(v);
  return reinterpret_cast<char*>(ptr);
}

void PutVarint64(std::string& dst, uint64_t v) {
  char buf[10];
  char* ptr = EncodeVarint64(buf, v);
  dst.append(buf, static_cast<size_t>(ptr - buf));
}

// ===========================================================================
// Length-prefixed strings
// ===========================================================================

void PutLengthPrefixedSlice(std::string& dst, std::string_view value) {
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  dst.append(value.data(), value.size());
}

// ===========================================================================
// Internal detail — varint decoding
// ===========================================================================

namespace detail {

const char* GetVarint32PtrFallback(const char* p, const char* limit,
                                   uint32_t& value) noexcept {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    uint32_t byte = *reinterpret_cast<const uint8_t*>(p);
    ++p;
    if (byte & 128) {
      // Continuation bit set — more bytes follow.
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      value = result;
      return p;
    }
  }
  return nullptr;
}

const char* GetVarint64Ptr(const char* p, const char* limit,
                           uint64_t& value) noexcept {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    uint64_t byte = *reinterpret_cast<const uint8_t*>(p);
    ++p;
    if (byte & 128) {
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      value = result;
      return p;
    }
  }
  return nullptr;
}

}  // namespace detail

// ===========================================================================
// Public Get* — return std::optional, no output parameters
// ===========================================================================

std::optional<uint32_t> GetVarint32(std::string_view& input) noexcept {
  const char* p = input.data();
  const char* limit = p + input.size();
  uint32_t value;
  const char* q = detail::GetVarint32Ptr(p, limit, value);
  if (q == nullptr) {
    return std::nullopt;
  }
  input.remove_prefix(static_cast<size_t>(q - p));
  return value;
}

std::optional<uint64_t> GetVarint64(std::string_view& input) noexcept {
  const char* p = input.data();
  const char* limit = p + input.size();
  uint64_t value;
  const char* q = detail::GetVarint64Ptr(p, limit, value);
  if (q == nullptr) {
    return std::nullopt;
  }
  input.remove_prefix(static_cast<size_t>(q - p));
  return value;
}

std::optional<std::string_view>
GetLengthPrefixedSlice(std::string_view& input) noexcept {
  // Decode the varint length prefix (this advances input past the length).
  auto len = GetVarint32(input);
  if (!len || input.size() < *len) {
    return std::nullopt;
  }
  std::string_view result = input.substr(0, *len);
  input.remove_prefix(*len);
  return result;
}

}  // namespace leveldb
