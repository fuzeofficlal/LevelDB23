//
// Modernized for C++23.

#include "db/table_cache.h"
#include "db/filename.h"
#include "leveldb/env.h"
#include "leveldb/table.h"

namespace leveldb {

TableCache::TableCache(const std::string& dbname, const Options<StdFileSystem>* options, int entries)
    : env_(options->env), dbname_(dbname), options_(options) {}

TableCache::~TableCache() = default;

Result<TableCache::Entry*> TableCache::FindTable(uint64_t file_number, uint64_t file_size) {
  {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = cache_.find(file_number);
    if (it != cache_.end()) {
      return it->second.get();
    }
  }

  std::filesystem::path fname = TableFileName(dbname_, file_number);
  auto file_res = env_->NewRandomAccessFile(fname);
  if (!file_res) return std::unexpected(file_res.error());

  TableOptions table_options(*options_);
  
  // Create a unique pointer for the file so we can move it
  auto file_ptr = std::make_unique<StdFileSystem::RandomAccessFile>(std::move(*file_res));

  auto table_res = Table<StdFileSystem::RandomAccessFile>::Open(table_options, file_ptr.get(), file_size);
  if (!table_res) return std::unexpected(table_res.error());

  auto entry = std::make_unique<Entry>();
  entry->table = std::move(*table_res);
  entry->file = std::move(file_ptr);
  
  auto* ptr = entry.get();
  {
    std::lock_guard<std::mutex> lk(mutex_);
    cache_[file_number] = std::move(entry);
  }
  return ptr;
}

std::unique_ptr<Iterator> TableCache::NewIterator(const ReadOptions& options,
                                                  uint64_t file_number,
                                                  uint64_t file_size) {
  auto entry_res = FindTable(file_number, file_size);
  if (!entry_res) {
    return std::unique_ptr<Iterator>(NewErrorIterator(std::unexpected(entry_res.error())));
  }
  return (*entry_res)->table->NewIterator(options);
}

Result<void> TableCache::Get(const ReadOptions& options, uint64_t file_number,
                             uint64_t file_size, std::string_view k,
                             std::move_only_function<void(std::string_view, std::string_view)> handle_result) {
  auto entry_res = FindTable(file_number, file_size);
  if (!entry_res) return std::unexpected(entry_res.error());
  
  return (*entry_res)->table->InternalGet(options, k, std::move(handle_result));
}

void TableCache::Evict(uint64_t file_number) {
  std::lock_guard<std::mutex> lk(mutex_);
  cache_.erase(file_number);
}

uint64_t TableCache::ApproximateOffsetOf(uint64_t file_number, uint64_t file_size,
                                         std::string_view k) {
  auto entry_res = FindTable(file_number, file_size);
  if (!entry_res) {
    return 0; // Return 0 on error, matching original behavior somewhat
  }
  return (*entry_res)->table->ApproximateOffsetOf(k);
}

}  // namespace leveldb
