//
// Modernized for C++23.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "leveldb/env.h"
#include "leveldb/options.h"
#include "leveldb/status.h"
#include "leveldb/iterator.h"
#include "leveldb/cache.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/compression.h"
#include "db/co_task.h"
#include "db/async_executor.h"

namespace leveldb {

class Block;

struct BlockReaderResult {
  std::unique_ptr<Iterator> iter;
  std::shared_ptr<Cache::CacheHandle> cache_handle = nullptr;
  std::shared_ptr<char[]> heap_data = nullptr;
};

// BlockHandle is a pointer to the extent of a file that stores a data
// block or a meta block.
class BlockHandle {
 public:
  // Maximum encoding length of a BlockHandle
  static constexpr size_t kMaxEncodedLength = 10 + 10;

  BlockHandle();

  // The offset of the block in the file.
  uint64_t offset() const { return offset_; }
  void set_offset(uint64_t offset) { offset_ = offset; }

  // The size of the stored block
  uint64_t size() const { return size_; }
  void set_size(uint64_t size) { size_ = size; }

  void EncodeTo(std::string& dst) const;
  Result<void> DecodeFrom(std::string_view& input);

 private:
  uint64_t offset_;
  uint64_t size_;
};

// Footer encapsulates the fixed information stored at the tail
// end of every table file.
class Footer {
 public:
  // Encoded length of a Footer.  Note that the serialization of a
  // Footer will always occupy exactly this many bytes.  It consists
  // of two block handles and a magic number.
  static constexpr size_t kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8;

  Footer() = default;

  // The block handle for the metaindex block of the table
  const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
  void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }

  // The block handle for the index block of the table
  const BlockHandle& index_handle() const { return index_handle_; }
  void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

  void EncodeTo(std::string& dst) const;
  Result<void> DecodeFrom(std::string_view& input);

 private:
  BlockHandle metaindex_handle_;
  BlockHandle index_handle_;
};

// kTableMagicNumber was picked by running
//    echo http://code.google.com/p/leveldb/ | sha1sum
// and taking the leading 64 bits.
inline constexpr uint64_t kTableMagicNumber = 0xdb4775248b80fb57ull;

// 1-byte type + 32-bit crc
inline constexpr size_t kBlockTrailerSize = 5;

struct BlockContents {
  std::string_view data;         // Actual contents of data
  bool cachable = false;         // True iff data can be cached
  std::shared_ptr<char[]> heap_data = nullptr; // Shared ownership of data.data()
};

// Read the block identified by "handle" from "file".
// On failure return non-OK.  On success return the BlockContents.
template <CRandomAccessFile File>
Result<BlockContents> ReadBlock(const File* file, const ReadOptions& options,
                                const BlockHandle& handle) {
  // Read the block contents as well as the type/crc footer.
  // See table_builder.cc for the code that built this structure.
  size_t n = static_cast<size_t>(handle.size());
  auto buf = std::make_shared<char[]>(n + kBlockTrailerSize);
  
  auto read_res = file->Read(handle.offset(), n + kBlockTrailerSize, buf.get());
  if (!read_res) {
    return std::unexpected(read_res.error());
  }
  
  std::string_view contents = *read_res;
  if (contents.size() != n + kBlockTrailerSize) {
    return Status::CorruptionErr("truncated block read");
  }

  // Check the crc of the type and the block contents
  const char* data = contents.data();  // Pointer to where Read put the data
  if (options.verify_checksums) {
    const uint32_t crc = crc32c::Unmask(DecodeFixed32(data + n + 1));
    const uint32_t actual = crc32c::Value(std::string_view(data, n + 1));
    if (actual != crc) {
      return Status::CorruptionErr("block checksum mismatch");
    }
  }

  BlockContents result;
  switch (static_cast<CompressionType>(data[n])) {
    case CompressionType::kNoCompression:
      if (data != buf.get()) {
        // File implementation gave us pointer to some other data (e.g., mmap).
        // Use it directly under the assumption that it will be live
        // while the file is open.
        result.data = std::string_view(data, n);
        result.heap_data = nullptr;
        result.cachable = false;  // Do not double-cache
      } else {
        result.data = std::string_view(buf.get(), n);
        result.heap_data = buf;
        result.cachable = true;
      }
      break;
      
    case CompressionType::kSnappyCompression: {
      auto ulength = Snappy_GetUncompressedLength(std::string_view(data, n));
      if (!ulength) {
        return Status::CorruptionErr("corrupted snappy compressed block length");
      }
      auto ubuf = std::make_shared<char[]>(*ulength);
      if (!Snappy_Uncompress(std::string_view(data, n), ubuf.get())) {
        return Status::CorruptionErr("corrupted snappy compressed block contents");
      }
      result.data = std::string_view(ubuf.get(), *ulength);
      result.heap_data = ubuf;
      result.cachable = true;
      break;
    }
    
    case CompressionType::kZstdCompression: {
      auto ulength = Zstd_GetUncompressedLength(std::string_view(data, n));
      if (!ulength) {
        return Status::CorruptionErr("corrupted zstd compressed block length");
      }
      auto ubuf = std::make_shared<char[]>(*ulength);
      if (!Zstd_Uncompress(std::string_view(data, n), ubuf.get())) {
        return Status::CorruptionErr("corrupted zstd compressed block contents");
      }
      result.data = std::string_view(ubuf.get(), *ulength);
      result.heap_data = ubuf;
      result.cachable = true;
      break;
    }
    
    default:
      return Status::CorruptionErr("bad block type");
  }

  return result;
}

// Read block asynchronously using io_uring if enabled, otherwise falls back to executor pool.
template <CRandomAccessFile File>
Task<Result<BlockContents>> ReadBlockAsync(const File* file, const ReadOptions& options,
                                           const BlockHandle& handle, AsyncExecutor* executor) {
  size_t n = static_cast<size_t>(handle.size());
  auto buf = std::make_shared<char[]>(n + kBlockTrailerSize);
  
  std::string_view contents;
  if (options.io_uring && executor->is_io_uring_enabled()) {
    int fd = -1;
    if constexpr (requires(const File& f) { { f.handle() } -> std::convertible_to<int>; }) {
      fd = file->handle();
    }
    if (fd != -1) {
      auto read_res = co_await executor->ReadAsync(fd, handle.offset(), n + kBlockTrailerSize, buf.get());
      if (!read_res) {
        co_return std::unexpected(read_res.error());
      }
      contents = *read_res;
    } else {
      auto read_res = co_await executor->submit([file, &handle, n, buf_ptr = buf.get()]() {
        return file->Read(handle.offset(), n + kBlockTrailerSize, buf_ptr);
      });
      if (!read_res) {
        co_return std::unexpected(read_res.error());
      }
      contents = *read_res;
    }
  } else {
    auto read_res = co_await executor->submit([file, &handle, n, buf_ptr = buf.get()]() {
      return file->Read(handle.offset(), n + kBlockTrailerSize, buf_ptr);
    });
    if (!read_res) {
      co_return std::unexpected(read_res.error());
    }
    contents = *read_res;
  }

  if (contents.size() != n + kBlockTrailerSize) {
    co_return std::unexpected(Status::CorruptionErr("truncated block read"));
  }

  // Check the crc of the type and the block contents
  const char* data = contents.data();  // Pointer to where Read put the data
  if (options.verify_checksums) {
    const uint32_t crc = crc32c::Unmask(DecodeFixed32(data + n + 1));
    const uint32_t actual = crc32c::Value(std::string_view(data, n + 1));
    if (actual != crc) {
      co_return std::unexpected(Status::CorruptionErr("block checksum mismatch"));
    }
  }

  BlockContents result;
  switch (static_cast<CompressionType>(data[n])) {
    case CompressionType::kNoCompression:
      if (data != buf.get()) {
        result.data = std::string_view(data, n);
        result.heap_data = nullptr;
        result.cachable = false;
      } else {
        result.data = std::string_view(buf.get(), n);
        result.heap_data = buf;
        result.cachable = true;
      }
      break;
      
    case CompressionType::kSnappyCompression: {
      auto ulength = Snappy_GetUncompressedLength(std::string_view(data, n));
      if (!ulength) {
        co_return std::unexpected(Status::CorruptionErr("corrupted snappy compressed block length"));
      }
      auto ubuf = std::make_shared<char[]>(*ulength);
      if (!Snappy_Uncompress(std::string_view(data, n), ubuf.get())) {
        co_return std::unexpected(Status::CorruptionErr("corrupted snappy compressed block contents"));
      }
      result.data = std::string_view(ubuf.get(), *ulength);
      result.heap_data = ubuf;
      result.cachable = true;
      break;
    }
    
    case CompressionType::kZstdCompression: {
      auto ulength = Zstd_GetUncompressedLength(std::string_view(data, n));
      if (!ulength) {
        co_return std::unexpected(Status::CorruptionErr("corrupted zstd compressed block length"));
      }
      auto ubuf = std::make_shared<char[]>(*ulength);
      if (!Zstd_Uncompress(std::string_view(data, n), ubuf.get())) {
        co_return std::unexpected(Status::CorruptionErr("corrupted zstd compressed block contents"));
      }
      result.data = std::string_view(ubuf.get(), *ulength);
      result.heap_data = ubuf;
      result.cachable = true;
      break;
    }
    
    default:
      co_return std::unexpected(Status::CorruptionErr("bad block type"));
  }

  co_return result;
}

// Implementation details follow.  Clients should ignore,

inline BlockHandle::BlockHandle()
    : offset_(~static_cast<uint64_t>(0)), size_(~static_cast<uint64_t>(0)) {}

}  // namespace leveldb
