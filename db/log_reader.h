//
// Modernized for C++23.

#pragma once

#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "db/log_format.h"
#include "leveldb/env.h"
#include "leveldb/status.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb::log {

// Type for reporting corruption.
using Reporter = std::function<void(size_t bytes, const Status& status)>;

template <CSequentialFile SeqFile>
class Reader {
 public:
  // Create a reader that will return log records from "*file".
  // "*file" must remain live while this Reader is in use.
  //
  // If "reporter" is valid, it is notified whenever some data is
  // dropped due to a detected corruption.
  //
  // If "checksum" is true, verify checksums if available.
  //
  // The Reader will start reading at the first record located at physical
  // position >= initial_offset within the file.
  Reader(SeqFile* file, Reporter reporter, bool checksum, uint64_t initial_offset)
      : file_(file),
        reporter_(std::move(reporter)),
        checksum_(checksum),
        backing_store_(std::make_unique<char[]>(kBlockSize)),
        buffer_(),
        eof_(false),
        last_record_offset_(0),
        end_of_buffer_offset_(0),
        initial_offset_(initial_offset),
        resyncing_(initial_offset > 0) {}

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  ~Reader() = default;

  // Read the next record. Returns the record data if read successfully,
  // or std::nullopt if we hit the end of the input.
  // May use "scratch" as temporary storage for fragmented records.
  // The returned string_view may point into "scratch" or the internal buffer.
  std::optional<std::string_view> ReadRecord(std::string& scratch) {
    if (last_record_offset_ < initial_offset_) {
      if (!SkipToInitialBlock()) {
        return std::nullopt;
      }
    }

    scratch.clear();
    bool in_fragmented_record = false;
    uint64_t prospective_record_offset = 0;

    std::string_view fragment;
    while (true) {
      const unsigned int record_type = ReadPhysicalRecord(fragment);

      uint64_t physical_record_offset =
          end_of_buffer_offset_ - buffer_.size() - kHeaderSize - fragment.size();

      if (resyncing_) {
        if (record_type == static_cast<unsigned int>(RecordType::kMiddleType)) {
          continue;
        } else if (record_type == static_cast<unsigned int>(RecordType::kLastType)) {
          resyncing_ = false;
          continue;
        } else {
          resyncing_ = false;
        }
      }

      switch (record_type) {
        case static_cast<unsigned int>(RecordType::kFullType):
          if (in_fragmented_record) {
            if (!scratch.empty()) {
              ReportCorruption(scratch.size(), "partial record without end(1)");
            }
          }
          prospective_record_offset = physical_record_offset;
          scratch.clear();
          last_record_offset_ = prospective_record_offset;
          return fragment;

        case static_cast<unsigned int>(RecordType::kFirstType):
          if (in_fragmented_record) {
            if (!scratch.empty()) {
              ReportCorruption(scratch.size(), "partial record without end(2)");
            }
          }
          prospective_record_offset = physical_record_offset;
          scratch.assign(fragment.data(), fragment.size());
          in_fragmented_record = true;
          break;

        case static_cast<unsigned int>(RecordType::kMiddleType):
          if (!in_fragmented_record) {
            ReportCorruption(fragment.size(), "missing start of fragmented record(1)");
          } else {
            scratch.append(fragment.data(), fragment.size());
          }
          break;

        case static_cast<unsigned int>(RecordType::kLastType):
          if (!in_fragmented_record) {
            ReportCorruption(fragment.size(), "missing start of fragmented record(2)");
          } else {
            scratch.append(fragment.data(), fragment.size());
            last_record_offset_ = prospective_record_offset;
            return std::string_view(scratch);
          }
          break;

        case kEof:
          if (in_fragmented_record) {
            scratch.clear();
          }
          return std::nullopt;

        case kBadRecord:
          if (in_fragmented_record) {
            ReportCorruption(scratch.size(), "error in middle of record");
            in_fragmented_record = false;
            scratch.clear();
          }
          break;

        default: {
          std::string msg = std::format("unknown record type {}", record_type);
          ReportCorruption(
              (fragment.size() + (in_fragmented_record ? scratch.size() : 0)),
              Status::Corruption(msg));
          in_fragmented_record = false;
          scratch.clear();
          break;
        }
      }
    }
  }

  // Returns the physical offset of the last record returned by ReadRecord.
  uint64_t LastRecordOffset() const { return last_record_offset_; }

 private:
  static constexpr unsigned int kEof = kMaxRecordType + 1;
  static constexpr unsigned int kBadRecord = kMaxRecordType + 2;

  bool SkipToInitialBlock() {
    const size_t offset_in_block = initial_offset_ % kBlockSize;
    uint64_t block_start_location = initial_offset_ - offset_in_block;

    // Don't search a block if we'd be in the trailer
    if (offset_in_block > kBlockSize - 6) {
      block_start_location += kBlockSize;
    }

    end_of_buffer_offset_ = block_start_location;

    if (block_start_location > 0) {
      auto skip_result = file_->Skip(block_start_location);
      if (!skip_result) {
        ReportDrop(block_start_location, skip_result.error());
        return false;
      }
    }

    return true;
  }

  unsigned int ReadPhysicalRecord(std::string_view& result) {
    while (true) {
      if (buffer_.size() < kHeaderSize) {
        if (!eof_) {
          // Last read was a full read, so this is a trailer to skip
          buffer_ = {};
          auto read_res = file_->Read(kBlockSize, backing_store_.get());
          if (!read_res) {
            buffer_ = {};
            ReportDrop(kBlockSize, read_res.error());
            eof_ = true;
            return kEof;
          }
          
          buffer_ = *read_res;
          end_of_buffer_offset_ += buffer_.size();
          
          if (buffer_.size() < kBlockSize) {
            eof_ = true;
          }
          continue;
        } else {
          buffer_ = {};
          return kEof;
        }
      }

      // Parse the header
      const char* header = buffer_.data();
      const uint32_t a = static_cast<uint32_t>(header[4]) & 0xff;
      const uint32_t b = static_cast<uint32_t>(header[5]) & 0xff;
      const unsigned int type = header[6];
      const uint32_t length = a | (b << 8);
      
      if (kHeaderSize + length > buffer_.size()) {
        size_t drop_size = buffer_.size();
        buffer_ = {};
        if (!eof_) {
          ReportCorruption(drop_size, "bad record length");
          return kBadRecord;
        }
        return kEof;
      }

      if (type == static_cast<unsigned int>(RecordType::kZeroType) && length == 0) {
        buffer_ = {};
        return kBadRecord;
      }

      // Check crc
      if (checksum_) {
        uint32_t expected_crc = crc32c::Unmask(DecodeFixed32(header));
        uint32_t actual_crc = crc32c::Value(std::string_view(header + 6, 1 + length));
        if (actual_crc != expected_crc) {
          size_t drop_size = buffer_.size();
          buffer_ = {};
          ReportCorruption(drop_size, "checksum mismatch");
          return kBadRecord;
        }
      }

      buffer_.remove_prefix(kHeaderSize + length);

      // Skip physical record that started before initial_offset_
      if (end_of_buffer_offset_ - buffer_.size() - kHeaderSize - length < initial_offset_) {
        result = {};
        return kBadRecord;
      }

      result = std::string_view(header + kHeaderSize, length);
      return type;
    }
  }

  void ReportCorruption(uint64_t bytes, std::string_view reason) {
    ReportDrop(bytes, Status::Corruption(reason));
  }
  
  void ReportCorruption(uint64_t bytes, const Status& status) {
    ReportDrop(bytes, status);
  }

  void ReportDrop(uint64_t bytes, const Status& reason) {
    if (reporter_ && end_of_buffer_offset_ - buffer_.size() - bytes >= initial_offset_) {
      reporter_(static_cast<size_t>(bytes), reason);
    }
  }

  SeqFile* const file_;
  Reporter const reporter_;
  bool const checksum_;
  std::unique_ptr<char[]> backing_store_;
  std::string_view buffer_;
  bool eof_;

  uint64_t last_record_offset_;
  uint64_t end_of_buffer_offset_;
  uint64_t const initial_offset_;
  bool resyncing_;
};

}  // namespace leveldb::log
