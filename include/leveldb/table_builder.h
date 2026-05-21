//
// Modernized for C++23.

#pragma once

#include <cstdint>
#include <string_view>

#include "leveldb/env.h"
#include "leveldb/options.h"
#include "leveldb/status.h"

namespace leveldb {

class BlockBuilder;
class BlockHandle;

struct TableBuilderOptions {
  const Comparator* comparator = nullptr;
  size_t block_size = 4 * 1024;
  int block_restart_interval = 16;
  CompressionType compression = CompressionType::kSnappyCompression;
  int zstd_compression_level = 1;
  const FilterPolicy* filter_policy = nullptr;

  // Implicitly convert from DB Options
  template <CFileSystem FS>
  TableBuilderOptions(const Options<FS>& options)
      : comparator(options.comparator),
        block_size(options.block_size),
        block_restart_interval(options.block_restart_interval),
        compression(options.compression),
        zstd_compression_level(options.zstd_compression_level),
        filter_policy(options.filter_policy) {}

  TableBuilderOptions() = default;
};

// TableBuilder provides the interface used to build a Table
// (an immutable and sorted map from keys to values).
//
// Multiple threads can invoke const methods on a TableBuilder without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same TableBuilder must use
// external synchronization.
template <CWritableFile DestFile>
class TableBuilder {
 public:
  TableBuilder(const TableBuilderOptions& options, DestFile* file);

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  ~TableBuilder();

  // Change the options used by this builder.
  Result<void> ChangeOptions(const TableBuilderOptions& options);

  // Add key,value to the table being constructed.
  // REQUIRES: key is after any previously added key according to comparator.
  // REQUIRES: Finish(), Abandon() have not been called
  void Add(std::string_view key, std::string_view value);

  // Advanced operation: flush any buffered key/value pairs to file.
  void Flush();

  // Return ok iff no error has been detected.
  Result<void> status() const;

  // Finish building the table. Stops using the file passed to the
  // constructor after this function returns.
  // REQUIRES: Finish(), Abandon() have not been called
  Result<void> Finish();

  // Indicate that the contents of this builder should be abandoned.
  void Abandon();

  // Number of calls to Add() so far.
  uint64_t NumEntries() const;

  // Size of the file generated so far.
  uint64_t FileSize() const;

 private:
  [[nodiscard]] bool ok() const { return status().has_value(); }
  void WriteBlock(BlockBuilder* block, BlockHandle* handle);
  void WriteRawBlock(std::string_view block_contents, CompressionType type, BlockHandle* handle);

  struct Rep;
  Rep* rep_;
};

}  // namespace leveldb

// Include the template implementation
#include "table/table_builder.cc"
