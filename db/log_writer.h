//
// Modernized for C++23.

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "db/log_format.h"
#include "leveldb/env.h"
#include "leveldb/status.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb::log {

// TypeCrc cache to speed up record generation.
inline std::array<uint32_t, kMaxRecordType + 1> ComputeTypeCrcs() {
  std::array<uint32_t, kMaxRecordType + 1> type_crc{};
  for (int i = 0; i <= kMaxRecordType; i++) {
    char t = static_cast<char>(i);
    type_crc[i] = crc32c::Value(std::string_view(&t, 1));
  }
  return type_crc;
}

inline const auto kTypeCrcs = ComputeTypeCrcs();

template <CWritableFile DestFile>
class Writer {
 public:
  // Create a writer that will append data to "*dest".
  // "*dest" must be initially empty.
  // "*dest" must remain live while this Writer is in use.
  explicit Writer(DestFile* dest) : dest_(dest), block_offset_(0) {}

  // Create a writer that will append data to "*dest".
  // "*dest" must have initial length "dest_length".
  // "*dest" must remain live while this Writer is in use.
  Writer(DestFile* dest, uint64_t dest_length)
      : dest_(dest), block_offset_(dest_length % kBlockSize) {}

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  ~Writer() = default;

  Result<void> AddRecord(std::string_view slice) {
    const char* ptr = slice.data();
    size_t left = slice.size();

    // Fragment the record if necessary and emit it. Note that if slice
    // is empty, we still want to iterate once to emit a single
    // zero-length record
    bool begin = true;
    do {
      const int leftover = kBlockSize - block_offset_;
      // invariant: leftover >= 0

      if (leftover < kHeaderSize) {
        // Switch to a new block
        if (leftover > 0) {
          // Fill the trailer (literal below relies on kHeaderSize being 7)
          static_assert(kHeaderSize == 7, "");
          if (auto s = dest_->Append(std::string_view("\x00\x00\x00\x00\x00\x00", leftover)); !s) {
            return s;
          }
        }
        block_offset_ = 0;
      }

      // Invariant: we never leave < kHeaderSize bytes in a block.
      const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
      const size_t fragment_length = (left < avail) ? left : avail;

      RecordType type;
      const bool end = (left == fragment_length);
      if (begin && end) {
        type = RecordType::kFullType;
      } else if (begin) {
        type = RecordType::kFirstType;
      } else if (end) {
        type = RecordType::kLastType;
      } else {
        type = RecordType::kMiddleType;
      }

      if (auto s = EmitPhysicalRecord(type, ptr, fragment_length); !s) {
        return s;
      }

      ptr += fragment_length;
      left -= fragment_length;
      begin = false;
    } while (left > 0);

    return {};
  }

 private:
  Result<void> EmitPhysicalRecord(RecordType t, const char* ptr, size_t length) {
    // Format the header
    char buf[kHeaderSize];
    buf[4] = static_cast<char>(length & 0xff);
    buf[5] = static_cast<char>(length >> 8);
    buf[6] = static_cast<char>(t);

    // Compute the crc of the record type and the payload.
    uint32_t crc = crc32c::Extend(kTypeCrcs[static_cast<int>(t)], std::string_view(ptr, length));
    crc = crc32c::Mask(crc);  // Adjust for storage
    EncodeFixed32(buf, crc);

    // Write the header and the payload
    if (auto s = dest_->Append(std::string_view(buf, kHeaderSize)); !s) {
      return s;
    }
    if (auto s = dest_->Append(std::string_view(ptr, length)); !s) {
      return s;
    }
    if (auto s = dest_->Flush(); !s) {
      return s;
    }

    block_offset_ += kHeaderSize + length;
    return {};
  }

  DestFile* dest_;
  int block_offset_;  // Current offset in block
};

}  // namespace leveldb::log
