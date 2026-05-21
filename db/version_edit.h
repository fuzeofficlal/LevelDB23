//
// Modernized for C++23.

#pragma once

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "db/dbformat.h"
#include "leveldb/status.h"

namespace leveldb {

class VersionSet;

struct FileMetaData {
  int allowed_seeks = 1 << 30;  // Seeks allowed until compaction
  uint64_t number = 0;
  uint64_t file_size = 0;       // File size in bytes
  InternalKey smallest;         // Smallest internal key served by table
  InternalKey largest;          // Largest internal key served by table
};

class VersionEdit {
 public:
  VersionEdit() = default;
  ~VersionEdit() = default;

  void Clear();

  void SetComparatorName(std::string_view name) {
    comparator_ = std::string(name);
  }
  void SetLogNumber(uint64_t num) {
    log_number_ = num;
  }
  void SetPrevLogNumber(uint64_t num) {
    prev_log_number_ = num;
  }
  void SetNextFile(uint64_t num) {
    next_file_number_ = num;
  }
  void SetLastSequence(SequenceNumber seq) {
    last_sequence_ = seq;
  }
  void SetCompactPointer(int level, const InternalKey& key) {
    compact_pointers_.emplace_back(level, key);
  }

  void AddFile(int level, uint64_t file, uint64_t file_size,
               const InternalKey& smallest, const InternalKey& largest) {
    auto f = std::make_shared<FileMetaData>();
    f->number = file;
    f->file_size = file_size;
    f->smallest = smallest;
    f->largest = largest;
    new_files_.emplace_back(level, std::move(f));
  }

  void RemoveFile(int level, uint64_t file) {
    deleted_files_.emplace(level, file);
  }

  void EncodeTo(std::string& dst) const;
  Result<void> DecodeFrom(std::string_view src);

  [[nodiscard]] std::string DebugString() const;

 private:
  friend class VersionSet;

  using DeletedFileSet = std::set<std::pair<int, uint64_t>>;

  std::optional<std::string> comparator_;
  std::optional<uint64_t> log_number_;
  std::optional<uint64_t> prev_log_number_;
  std::optional<uint64_t> next_file_number_;
  std::optional<SequenceNumber> last_sequence_;

  std::vector<std::pair<int, InternalKey>> compact_pointers_;
  DeletedFileSet deleted_files_;
  std::vector<std::pair<int, std::shared_ptr<FileMetaData>>> new_files_;
};

}  // namespace leveldb
