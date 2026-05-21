//
// Modernized for C++23.

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>
#include <mutex>
#include <set>
#include <string>

#include "db/dbformat.h"
#include "db/log_writer.h"
#include "leveldb/db.h"
#include "leveldb/std_file_system.h"
#include "db/snapshot.h"

namespace leveldb {

class MemTable;
class TableCache;
class VersionSet;
class VersionEdit;
class Compaction;

class DBImpl : public DB {
 public:
  DBImpl(const Options<StdFileSystem>& options, std::string dbname);

  DBImpl(const DBImpl&) = delete;
  DBImpl& operator=(const DBImpl&) = delete;

  ~DBImpl() override;

  Result<void> Put(const WriteOptions& options, std::string_view key,
                   std::string_view value) override;
  Result<void> Delete(const WriteOptions& options, std::string_view key) override;
  Result<void> Write(const WriteOptions& options, WriteBatch* updates) override;
  Result<std::optional<std::string>> Get(const ReadOptions& options,
                                         std::string_view key) override;
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override;
  std::shared_ptr<const Snapshot> GetSnapshot() override;
  Result<std::optional<std::string>> GetProperty(std::string_view property) override;
  void GetApproximateSizes(const Range* range, int n, uint64_t* sizes) override;
  void CompactRange(const std::string_view* begin, const std::string_view* end) override;

  Result<void> Recover();
  void TEST_CompactRange(int level, const std::string_view* begin, const std::string_view* end);
  Result<void> TEST_CompactMemTable();
  void RecordReadSample(std::string_view key);

 private:
  friend class DB;
  struct Writer;
  struct ManualCompaction {
    int level;
    bool done;
    const InternalKey* begin;
    const InternalKey* end;
    InternalKey tmp_storage;
  };

  Result<void> MakeRoomForWrite(bool force, std::unique_lock<std::mutex>& lk);
  WriteBatch* BuildBatchGroup(Writer** last_writer);
  void RecordBackgroundError(const Status& s);

  void MaybeScheduleCompaction();
  void BackgroundCall();
  void BackgroundCompaction();
  Result<void> DoCompactionWork(Compaction* c);

  StdFileSystem* const env_;
  const InternalKeyComparator internal_comparator_;
  const std::unique_ptr<const FilterPolicy> internal_filter_policy_;
  const Options<StdFileSystem> options_;
  const std::string dbname_;

  std::mutex mutex_;
  std::condition_variable background_work_finished_signal_;
  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;
  std::atomic<bool> has_imm_{false};
  std::atomic<bool> shutting_down_{false};
  bool bg_compaction_scheduled_ = false;
  std::unique_ptr<std::thread> bg_thread_;
  
  std::unique_ptr<StdFileSystem::WritableFile> logfile_;
  uint64_t logfile_number_ = 0;
  std::unique_ptr<log::Writer<StdFileSystem::WritableFile>> log_;

  std::deque<Writer*> writers_;
  WriteBatch* tmp_batch_;

  std::unique_ptr<TableCache> table_cache_;
  std::unique_ptr<VersionSet> versions_;

  SnapshotList snapshots_;
  std::atomic<uint32_t> seed_{0};

  Status bg_error_;
  ManualCompaction* manual_compaction_ = nullptr;
};

}  // namespace leveldb
