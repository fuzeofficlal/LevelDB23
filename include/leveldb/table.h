//
// Modernized for C++23.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include "leveldb/env.h"
#include "leveldb/iterator.h"
#include "leveldb/options.h"
#include "leveldb/status.h"

namespace leveldb {

struct TableOptions {
  const Comparator* comparator = nullptr;
  Cache* block_cache = nullptr;
  const FilterPolicy* filter_policy = nullptr;
  bool paranoid_checks = false;

  // Implicitly convert from DB Options
  template <CFileSystem FS>
  TableOptions(const Options<FS>& options)
      : comparator(options.comparator),
        block_cache(options.block_cache),
        filter_policy(options.filter_policy),
        paranoid_checks(options.paranoid_checks) {}

  TableOptions() = default;
};

// A Table is a sorted map from strings to strings. Tables are
// immutable and persistent. A Table may be safely accessed from
// multiple threads without external synchronization.
template <CRandomAccessFile SrcFile>
class Table {
 public:
  // Attempt to open the table that is stored in bytes [0..file_size)
  // of "file", and read the metadata entries necessary to allow
  // retrieving data from the table.
  //
  // If successful, returns the newly opened table via Result.
  // The client must ensure that "file" remains live for the duration 
  // of the returned table's lifetime.
  static Result<std::unique_ptr<Table<SrcFile>>> Open(
      const TableOptions& options, SrcFile* file, uint64_t file_size);

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  ~Table();

  // Returns a new iterator over the table contents.
  // The result of NewIterator() is initially invalid (caller must
  // call one of the Seek methods on the iterator before using it).
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) const;

  // Given a key, return an approximate byte offset in the file where
  // the data for that key begins (or would begin if the key were
  // present in the file).
  uint64_t ApproximateOffsetOf(std::string_view key) const;

  // Calls handle_result with the entry found after a call to Seek(key).
  Result<void> InternalGet(
      const ReadOptions& options, std::string_view key,
      std::move_only_function<void(std::string_view, std::string_view)> handle_result);

 private:
  struct Rep;

  explicit Table(Rep* rep) : rep_(rep) {}

  static std::unique_ptr<Iterator> BlockReader(
      const Table<SrcFile>* table, const ReadOptions& options, std::string_view index_value);

  void ReadMeta(const class Footer& footer);
  void ReadFilter(std::string_view filter_handle_value);

  Rep* const rep_;
};

}  // namespace leveldb

#include "table/table.cc"
