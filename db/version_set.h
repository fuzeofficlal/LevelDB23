//
// Modernized for C++23.

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "db/dbformat.h"
#include "db/version_edit.h"
#include "leveldb/options.h"
#include "leveldb/std_file_system.h"
#include "leveldb/iterator.h"

#include "db/log_writer.h"

namespace leveldb {

class Compaction;
class Iterator;
class MemTable;
class VersionSet;
class TableCache;
class Version;
class VersionSet;

// Return the smallest index i such that files[i]->largest >= key.
// Return files.size() if there is no such file.
int FindFile(const InternalKeyComparator& icmp,
             const std::vector<std::shared_ptr<FileMetaData>>& files,
             std::string_view key);

bool SomeFileOverlapsRange(const InternalKeyComparator& icmp,
                           bool disjoint_sorted_files,
                           const std::vector<std::shared_ptr<FileMetaData>>& files,
                           const std::string_view* smallest_user_key,
                           const std::string_view* largest_user_key);

class Version : public std::enable_shared_from_this<Version> {
 public:
  explicit Version(VersionSet* vset);
  ~Version();

  struct GetStats {
    std::shared_ptr<FileMetaData> seek_file;
    int seek_file_level = -1;
  };

  void AddIterators(const ReadOptions&, std::vector<std::unique_ptr<Iterator>>& iters);

  // Lookup the value for key.  If found, returns the value.
  // Fills *stats for potential compaction trigger.
  [[nodiscard]] Result<std::optional<std::string>> Get(
      const ReadOptions&, const LookupKey& key, GetStats* stats);

  bool UpdateStats(const GetStats& stats);
  bool RecordReadSample(std::string_view key);

  void GetOverlappingInputs(
      int level,
      const InternalKey* begin,  // nullptr means before all keys
      const InternalKey* end,    // nullptr means after all keys
      std::vector<std::shared_ptr<FileMetaData>>& inputs);

  bool OverlapInLevel(int level, const std::string_view* smallest_user_key,
                      const std::string_view* largest_user_key);

  int PickLevelForMemTableOutput(std::string_view smallest_user_key,
                                 std::string_view largest_user_key);

  int NumFiles(int level) const { return files_[level].size(); }

  [[nodiscard]] std::string DebugString() const;

 private:
  friend class Compaction;
  friend class VersionSet;

  class LevelFileNumIterator;

  Version(const Version&) = delete;
  Version& operator=(const Version&) = delete;

  std::unique_ptr<Iterator> NewConcatenatingIterator(const ReadOptions&, int level) const;

  // Modernized ForEachOverlapping using move_only_function or a template callable
  template <typename Func>
  void ForEachOverlapping(std::string_view user_key, std::string_view internal_key, Func func);

  VersionSet* vset_;  // VersionSet to which this Version belongs
  
  // Double-linked list of versions maintained by VersionSet.
  // Using raw pointers here since VersionSet owns the list.
  Version* next_;
  Version* prev_;

  // List of files per level
  std::vector<std::shared_ptr<FileMetaData>> files_[config::kNumLevels];

  // Next file to compact based on seek stats.
  std::shared_ptr<FileMetaData> file_to_compact_;
  int file_to_compact_level_ = -1;

  double compaction_score_ = -1.0;
  int compaction_level_ = -1;
};

class VersionSet {
 public:
  VersionSet(std::string dbname, const Options<StdFileSystem>* options,
             TableCache* table_cache, const InternalKeyComparator*);
  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  ~VersionSet();

  Result<void> LogAndApply(VersionEdit* edit);
  Result<bool> Recover();

  std::shared_ptr<Version> current() const { return current_; }

  uint64_t ManifestFileNumber() const { return manifest_file_number_; }
  uint64_t NewFileNumber() { return next_file_number_++; }

  void ReuseFileNumber(uint64_t file_number) {
    if (next_file_number_ == file_number + 1) {
      next_file_number_ = file_number;
    }
  }

  int NumLevelFiles(int level) const;
  int64_t NumLevelBytes(int level) const;

  uint64_t LastSequence() const { return last_sequence_; }
  void SetLastSequence(uint64_t s) {
    assert(s >= last_sequence_);
    last_sequence_ = s;
  }

  void MarkFileNumberUsed(uint64_t number);

  uint64_t LogNumber() const { return log_number_; }
  uint64_t PrevLogNumber() const { return prev_log_number_; }

  std::unique_ptr<Compaction> PickCompaction();
  std::unique_ptr<Compaction> CompactRange(int level, const InternalKey* begin,
                                           const InternalKey* end);

  int64_t MaxNextLevelOverlappingBytes();

  std::unique_ptr<Iterator> MakeInputIterator(Compaction* c);

  bool NeedsCompaction() const {
    auto v = current_;
    return (v->compaction_score_ >= 1) || (v->file_to_compact_ != nullptr);
  }

  void AddLiveFiles(std::set<uint64_t>* live);

  uint64_t ApproximateOffsetOf(std::shared_ptr<Version> v, const InternalKey& key);

 private:
  class Builder;

  friend class Compaction;
  friend class Version;

  bool ReuseManifest(std::string_view dscname, std::string_view dscbase);

  void Finalize(std::shared_ptr<Version> v);

  void GetRange(const std::vector<std::shared_ptr<FileMetaData>>& inputs,
                InternalKey* smallest, InternalKey* largest);

  void GetRange2(const std::vector<std::shared_ptr<FileMetaData>>& inputs1,
                 const std::vector<std::shared_ptr<FileMetaData>>& inputs2,
                 InternalKey* smallest, InternalKey* largest);

  void SetupOtherInputs(Compaction* c);
  Result<void> WriteSnapshot(log::Writer<StdFileSystem::WritableFile>* log);

  void AppendVersion(std::shared_ptr<Version> v);

  StdFileSystem* const env_;
  const std::string dbname_;
  const Options<StdFileSystem>* const options_;
  TableCache* const table_cache_;
  const InternalKeyComparator icmp_;
  uint64_t next_file_number_ = 2;
  uint64_t manifest_file_number_ = 0;
  uint64_t last_sequence_ = 0;
  uint64_t log_number_ = 0;
  uint64_t prev_log_number_ = 0;

  std::unique_ptr<StdFileSystem::WritableFile> descriptor_file_;
  std::unique_ptr<log::Writer<StdFileSystem::WritableFile>> descriptor_log_;
  
  // A dummy version used as the head of the circular doubly-linked list.
  std::shared_ptr<Version> dummy_versions_;
  std::shared_ptr<Version> current_;

  std::string compact_pointer_[config::kNumLevels];
};

class Compaction {
 public:
  ~Compaction() = default;

  int level() const { return level_; }
  VersionEdit* edit() { return &edit_; }
  int num_input_files(int which) const { return inputs_[which].size(); }
  std::shared_ptr<FileMetaData> input(int which, int i) const { return inputs_[which][i]; }
  uint64_t MaxOutputFileSize() const { return max_output_file_size_; }

  bool IsTrivialMove() const;
  void AddInputDeletions(VersionEdit* edit);
  bool IsBaseLevelForKey(std::string_view user_key);
  bool ShouldStopBefore(std::string_view internal_key);

  void ReleaseInputs();

 private:
  friend class Version;
  friend class VersionSet;

  Compaction(const Options<StdFileSystem>* options, int level);

  int level_;
  uint64_t max_output_file_size_;
  std::shared_ptr<Version> input_version_;
  VersionEdit edit_;

  std::vector<std::shared_ptr<FileMetaData>> inputs_[2];
  std::vector<std::shared_ptr<FileMetaData>> grandparents_;
  size_t grandparent_index_ = 0;
  bool seen_key_ = false;
  int64_t overlapped_bytes_ = 0;

  size_t level_ptrs_[config::kNumLevels];
};

}  // namespace leveldb
