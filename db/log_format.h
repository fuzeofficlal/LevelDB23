//
// Modernized for C++23.
// Log format information shared by reader and writer.

#pragma once

#include <cstdint>

namespace leveldb::log {

enum class RecordType : uint8_t {
  // Zero is reserved for preallocated files
  kZeroType = 0,

  kFullType = 1,

  // For fragments
  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4
};

constexpr int kMaxRecordType = static_cast<int>(RecordType::kLastType);

constexpr int kBlockSize = 32768;

// Header is checksum (4 bytes), length (2 bytes), type (1 byte).
constexpr int kHeaderSize = 4 + 2 + 1;

}  // namespace leveldb::log
