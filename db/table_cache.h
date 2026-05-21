//
// Modernized for C++23.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <mutex>

#include "leveldb/db.h"
#include "leveldb/table.h"
#include "leveldb/std_file_system.h"
#include "leveldb/cache.h"
#include "db/co_task.h"

namespace leveldb {

class AsyncExecutor;

class TableCache {
 public:
  TableCache(const std::string& dbname, const Options<StdFileSystem>* options, int entries);
  ~TableCache();

  TableCache(const TableCache&) = delete;
  TableCache& operator=(const TableCache&) = delete;

  // Return an iterator for the specified file number.
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options,
                                        uint64_t file_number,
                                        uint64_t file_size);

  // If a seek to internal key "k" in specified file finds an entry,
  // call (*handle_result)(k, v).
  Result<void> Get(const ReadOptions& options, uint64_t file_number,
                   uint64_t file_size, std::string_view k,
                   std::move_only_function<void(std::string_view, std::string_view)> handle_result);

  Task<Result<void>> GetAsync(const ReadOptions& options, uint64_t file_number,
                              uint64_t file_size, std::string_view k,
                              std::move_only_function<void(std::string_view, std::string_view)> handle_result,
                              AsyncExecutor* executor);

  // Evict any entry for the specified file number
  void Evict(uint64_t file_number);

  // Return the approximate offset in the table of the data for "k"
  uint64_t ApproximateOffsetOf(uint64_t file_number, uint64_t file_size,
                               std::string_view k);

 private:
  struct Entry {
    std::unique_ptr<StdFileSystem::RandomAccessFile> file;
    std::unique_ptr<Table<StdFileSystem::RandomAccessFile>> table;
  };

  Result<Entry*> FindTable(uint64_t file_number, uint64_t file_size);

  StdFileSystem* const env_;
  const std::string dbname_;
  const Options<StdFileSystem>* const options_;
  
  // Since util/cache.h is not fully modernized or tested in LevelDB_23 yet,
  // we use a simple std::unordered_map with a mutex for the cache.
  // LRU eviction logic can be added later.
  std::mutex mutex_;
  std::unordered_map<uint64_t, std::unique_ptr<Entry>> cache_;
};

}  // namespace leveldb
