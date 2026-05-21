//
// Modernized for C++23.

#include "db/db_impl.h"
#include "db/builder.h"
#include "db/filename.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "db/db_iter.h"
#include "table/merger.h"
#include "leveldb/table_builder.h"
#include "leveldb/comparator.h"
#include "db/log_reader.h"
#include "db/version_edit.h"

namespace leveldb {

struct DBImpl::Writer {
  Status status;
  WriteBatch* batch;
  bool sync;
  bool done;
  std::condition_variable cv;
};

namespace {
template <typename T, typename V>
void ClipToRange(T* ptr, V minvalue, V maxvalue) {
  if (static_cast<V>(*ptr) > maxvalue) *ptr = maxvalue;
  if (static_cast<V>(*ptr) < minvalue) *ptr = minvalue;
}

Options<StdFileSystem> SanitizeOptions(const Options<StdFileSystem>& src,
                                       const InternalKeyComparator* icmp,
                                       const FilterPolicy* ipolicy) {
  Options<StdFileSystem> result = src;
  result.comparator = icmp;
  result.filter_policy = (src.filter_policy != nullptr) ? ipolicy : nullptr;
  ClipToRange(&result.max_open_files, 64 + 10, 50000);
  ClipToRange(&result.write_buffer_size, 64 << 10, 1 << 30);
  ClipToRange(&result.max_file_size, 1 << 20, 1 << 30);
  ClipToRange(&result.block_size, 1 << 10, 4 << 20);
  return result;
}
} // namespace

DBImpl::DBImpl(const Options<StdFileSystem>& options, std::string dbname)
    : env_(options.env),
      internal_comparator_(options.comparator ? options.comparator : BytewiseComparator()),
      internal_filter_policy_(options.filter_policy ? std::make_unique<const InternalFilterPolicy>(options.filter_policy) : nullptr),
      options_(SanitizeOptions(options, &internal_comparator_, internal_filter_policy_.get())),
      dbname_(std::move(dbname)),
      tmp_batch_(new WriteBatch),
      table_cache_(std::make_unique<TableCache>(dbname_, &options_, options_.max_open_files)),
      versions_(std::make_unique<VersionSet>(dbname_, &options_, table_cache_.get(), &internal_comparator_)) {}

DBImpl::~DBImpl() {
  shutting_down_ = true;
  background_work_finished_signal_.notify_all();
  if (bg_thread_ && bg_thread_->joinable()) {
    bg_thread_->join();
  }
  
  std::lock_guard<std::mutex> lk(mutex_);
  delete tmp_batch_;
}

Result<std::unique_ptr<DB>> DB::Open(const Options<StdFileSystem>& options,
                                     std::string_view name) {
  auto impl = std::make_unique<DBImpl>(options, std::string(name));
  
  std::unique_lock<std::mutex> lk(impl->mutex_);
  auto recover_res = impl->Recover();
  if (!recover_res) return std::unexpected(recover_res.error());

  if (impl->mem_ == nullptr) {
    impl->logfile_number_ = impl->versions_->NewFileNumber();
    auto lfile_res = options.env->NewWritableFile(LogFileName(impl->dbname_, impl->logfile_number_));
    if (!lfile_res) return std::unexpected(lfile_res.error());
    
    impl->logfile_ = std::make_unique<StdFileSystem::WritableFile>(std::move(*lfile_res));
    impl->log_ = std::make_unique<log::Writer<StdFileSystem::WritableFile>>(impl->logfile_.get());
    impl->mem_ = std::make_shared<MemTable>(impl->internal_comparator_);
  }

  impl->bg_thread_ = std::make_unique<std::thread>(&DBImpl::BackgroundCall, impl.get());

  return std::unique_ptr<DB>(impl.release());
}

Result<void> DBImpl::Recover() {
  env_->CreateDir(dbname_);
  auto env_res = env_->FileExists(CurrentFileName(dbname_));
  if (!env_res && options_.create_if_missing) {
    auto new_db_res = versions_->LogAndApply(new VersionEdit());
    if (!new_db_res) return new_db_res;
  } else if (!env_res) {
    return std::unexpected(Status::InvalidArgument("db does not exist"));
  }

  auto rec_res = versions_->Recover();
  if (!rec_res) return std::unexpected(rec_res.error());

  // List all files in the directory
  auto children_res = env_->GetChildren(dbname_);
  if (!children_res) return std::unexpected(children_res.error());
  auto filenames = std::move(*children_res);

  // Find all log files
  std::vector<uint64_t> logs;
  for (const auto& fname : filenames) {
    auto parsed = ParseFileName(fname);
    if (parsed && parsed->type == kLogFile) {
      if (parsed->number >= versions_->LogNumber() ||
          (versions_->PrevLogNumber() != 0 && parsed->number == versions_->PrevLogNumber())) {
        logs.push_back(parsed->number);
      }
    }
  }
  std::sort(logs.begin(), logs.end());

  SequenceNumber max_sequence = 0;
  VersionEdit edit;
  bool save_manifest = false;
  for (size_t i = 0; i < logs.size(); i++) {
    auto status = RecoverLogFile(logs[i], (i == logs.size() - 1), &save_manifest, &edit, &max_sequence);
    if (!status) return status;
    versions_->MarkFileNumberUsed(logs[i]);
  }

  if (versions_->LastSequence() < max_sequence) {
    versions_->SetLastSequence(max_sequence);
  }

  if (save_manifest) {
    uint64_t new_log_number = versions_->NewFileNumber();
    edit.SetLogNumber(new_log_number);
    auto log_res = versions_->LogAndApply(&edit);
    if (!log_res) return log_res;
  }

  return {};
}

Result<void> DBImpl::RecoverLogFile(uint64_t log_number, bool edit_save, bool* save_manifest, VersionEdit* edit, SequenceNumber* max_sequence) {
  std::string logname = LogFileName(dbname_, log_number);
  auto file_res = env_->NewSequentialFile(logname);
  if (!file_res) return {}; // Log file may have been deleted or empty, ignore
  auto file = std::make_unique<StdFileSystem::SequentialFile>(std::move(*file_res));

  auto reporter = [](uint64_t bytes, const Status& s) {};
  log::Reader<StdFileSystem::SequentialFile> reader(file.get(), reporter, true, 0);

  std::string scratch;
  auto mem = std::make_shared<MemTable>(internal_comparator_);
  int count = 0;
  while (auto record_opt = reader.ReadRecord(scratch)) {
    std::string_view record = *record_opt;
    if (record.size() < 12) continue;
    WriteBatch batch;
    WriteBatchInternal::SetContents(&batch, record);
    auto ins_res = WriteBatchInternal::InsertInto(&batch, mem.get());
    if (ins_res) {
      count += WriteBatchInternal::Count(&batch);
      auto seq = WriteBatchInternal::Sequence(&batch);
      auto last_seq = seq + WriteBatchInternal::Count(&batch) - 1;
      if (last_seq > *max_sequence) {
        *max_sequence = last_seq;
      }
    }
  }

  if (count > 0) {
    FileMetaData meta;
    meta.number = versions_->NewFileNumber();
    auto iter = mem->NewIterator();
    auto build_res = BuildTable(dbname_, env_, options_, table_cache_.get(), iter.get(), &meta);
    if (!build_res) return build_res;
    edit->AddFile(0, meta.number, meta.file_size, meta.smallest, meta.largest);
    *save_manifest = true;
    versions_->MarkFileNumberUsed(meta.number);
  }
  return {};
}

Result<void> DBImpl::Put(const WriteOptions& options, std::string_view key,
                         std::string_view value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(options, &batch);
}

Result<void> DBImpl::Delete(const WriteOptions& options, std::string_view key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(options, &batch);
}

Result<void> DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
  Writer w;
  w.batch = updates;
  w.sync = options.sync;
  w.done = false;

  std::unique_lock<std::mutex> lk(mutex_);
  writers_.push_back(&w);
  
  while (!w.done && &w != writers_.front()) {
    w.cv.wait(lk);
  }
  if (w.done) return w.status.ok() ? Result<void>() : std::unexpected(w.status);

  auto room_res = MakeRoomForWrite(updates == nullptr, lk);
  if (!room_res) {
    w.status = room_res.error();
    w.done = true;
    writers_.pop_front();
    if (!writers_.empty()) writers_.front()->cv.notify_one();
    return std::unexpected(w.status);
  }

  uint64_t last_sequence = versions_->LastSequence();
  Writer* last_writer = &w;
  if (updates != nullptr) {
    WriteBatch* write_batch = BuildBatchGroup(&last_writer);
    WriteBatchInternal::SetSequence(write_batch, last_sequence + 1);
    last_sequence += WriteBatchInternal::Count(write_batch);

    {
      lk.unlock();
      
      std::string record(WriteBatchInternal::Contents(write_batch));
      auto log_res = log_->AddRecord(record);
      if (log_res && options.sync) {
        log_res = logfile_->Sync();
      }
      
      Status s;
      if (!log_res) {
        s = log_res.error();
      } else {
        auto insert_res = WriteBatchInternal::InsertInto(write_batch, mem_.get());
        if (!insert_res) s = insert_res.error();
      }

      lk.lock();

      if (write_batch == tmp_batch_) tmp_batch_->Clear();
      
      versions_->SetLastSequence(last_sequence);
      
      w.status = s; // Simplified: assign to all in group
    }
  }

  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &w) {
      ready->status = w.status;
      ready->done = true;
      ready->cv.notify_one();
    }
    if (ready == last_writer) break;
  }

  if (!writers_.empty()) writers_.front()->cv.notify_one();
  
  return w.status.ok() ? Result<void>() : std::unexpected(w.status);
}

Result<void> DBImpl::MakeRoomForWrite(bool force, std::unique_lock<std::mutex>& lk) {
  bool allow_delay = !force;
  while (true) {
    if (!bg_error_.ok()) {
      return std::unexpected(bg_error_);
    } else if (allow_delay && versions_->NumLevelFiles(0) >= config::kL0_SlowdownWritesTrigger) {
      mutex_.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      mutex_.lock();
      allow_delay = false;
    } else if (!force && (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size)) {
      return {};
    } else if (imm_ != nullptr) {
      background_work_finished_signal_.wait(lk);
    } else if (versions_->NumLevelFiles(0) >= config::kL0_StopWritesTrigger) {
      background_work_finished_signal_.wait(lk);
    } else {
      auto new_log_num = versions_->NewFileNumber();
      auto lfile_res = options_.env->NewWritableFile(LogFileName(dbname_, new_log_num));
      if (!lfile_res) {
        versions_->ReuseFileNumber(new_log_num);
        return std::unexpected(lfile_res.error());
      }
      logfile_number_ = new_log_num;
      logfile_ = std::make_unique<StdFileSystem::WritableFile>(std::move(*lfile_res));
      log_ = std::make_unique<log::Writer<StdFileSystem::WritableFile>>(logfile_.get());
      imm_ = mem_;
      has_imm_.store(true, std::memory_order_release);
      mem_ = std::make_shared<MemTable>(internal_comparator_);
      force = false;
      MaybeScheduleCompaction();
    }
  }
}

WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer) {
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;
  assert(result != nullptr);
  
  size_t size = WriteBatchInternal::ByteSize(first->batch);
  size_t max_size = 1 << 20;
  if (size <= (128 << 10)) max_size = size + (128 << 10);
  
  *last_writer = first;
  auto iter = writers_.begin();
  ++iter;
  for (; iter != writers_.end(); ++iter) {
    Writer* w = *iter;
    if (w->sync && !first->sync) break;
    
    if (w->batch != nullptr) {
      size += WriteBatchInternal::ByteSize(w->batch);
      if (size > max_size) break;

      if (result == first->batch) {
        result = tmp_batch_;
        WriteBatchInternal::Append(result, first->batch);
      }
      WriteBatchInternal::Append(result, w->batch);
    }
    *last_writer = w;
  }
  return result;
}

Result<std::optional<std::string>> DBImpl::Get(const ReadOptions& options,
                                               std::string_view key) {
  std::unique_lock<std::mutex> lk(mutex_);
  
  uint64_t seq = versions_->LastSequence();
  if (options.snapshot != nullptr) {
    seq = static_cast<const SnapshotImpl*>(options.snapshot)->sequence_number();
  }
  LookupKey lkey(key, seq);
  
  auto mem = mem_;
  auto imm = imm_;
  auto current = versions_->current();
  
  lk.unlock();
  
  if (mem) {
    auto mem_res = mem->Get(lkey);
    if (!mem_res) return std::unexpected(mem_res.error());
    if (*mem_res) return std::string(**mem_res);
  }
  if (imm) {
    auto imm_res = imm->Get(lkey);
    if (!imm_res) return std::unexpected(imm_res.error());
    if (*imm_res) return std::string(**imm_res);
  }
  
  Version::GetStats stats;
  auto v_res = current->Get(options, lkey, &stats);
  
  bool have_stat_update = false;
  if (current->UpdateStats(stats)) {
    have_stat_update = true;
  }
  
  if (have_stat_update) {
    lk.lock();
    MaybeScheduleCompaction();
  }
  
  return v_res;
}

void DBImpl::MaybeScheduleCompaction() {
  if (bg_compaction_scheduled_ || shutting_down_.load(std::memory_order_acquire)) {
    return;
  }
  bg_compaction_scheduled_ = true;
  background_work_finished_signal_.notify_one();
}

void DBImpl::BackgroundCall() {
  while (!shutting_down_.load(std::memory_order_acquire)) {
    std::unique_lock<std::mutex> lk(mutex_);
    background_work_finished_signal_.wait(lk, [this]() {
      return shutting_down_.load(std::memory_order_acquire) || bg_compaction_scheduled_;
    });
    
    if (shutting_down_.load(std::memory_order_acquire)) break;

    BackgroundCompaction();
    bg_compaction_scheduled_ = false;
    MaybeScheduleCompaction();
    background_work_finished_signal_.notify_all();
  }
}

void DBImpl::BackgroundCompaction() {
  if (imm_ != nullptr) {
    // Compact memtable
    FileMetaData meta;
    meta.number = versions_->NewFileNumber();
    
    mutex_.unlock();
    auto iter = imm_->NewIterator();
    auto build_res = BuildTable(dbname_, env_, options_, table_cache_.get(), iter.get(), &meta);
    mutex_.lock();
    
    if (build_res && meta.file_size > 0) {
      VersionEdit edit;
      int level = versions_->current()->PickLevelForMemTableOutput(meta.smallest.user_key(), meta.largest.user_key());
      edit.AddFile(level, meta.number, meta.file_size, meta.smallest, meta.largest);
      edit.SetLogNumber(logfile_number_);
      auto log_res = versions_->LogAndApply(&edit);
      if (!log_res) RecordBackgroundError(log_res.error());
    } else if (!build_res) {
      RecordBackgroundError(build_res.error());
    }
    
    imm_.reset();
    has_imm_.store(false, std::memory_order_release);
    return;
  }
  
  std::unique_ptr<Compaction> c;
  bool is_manual = (manual_compaction_ != nullptr);
  InternalKey manual_end;
  if (is_manual) {
    ManualCompaction* m = manual_compaction_;
    c = versions_->CompactRange(m->level, m->begin, m->end);
    m->done = (c == nullptr);
    if (c != nullptr) {
      manual_end = c->input(0, c->num_input_files(0) - 1)->largest;
    }
  } else {
    c = versions_->PickCompaction();
  }
  
  if (!c) {
    if (is_manual) manual_compaction_ = nullptr;
    return;
  }
  
  if (!is_manual && c->IsTrivialMove()) {
    auto f = c->input(0, 0);
    c->edit()->RemoveFile(c->level(), f->number);
    c->edit()->AddFile(c->level() + 1, f->number, f->file_size, f->smallest, f->largest);
    auto log_res = versions_->LogAndApply(c->edit());
    if (!log_res) RecordBackgroundError(log_res.error());
    return;
  }
  
  auto status = DoCompactionWork(c.get());
  if (!status) {
    RecordBackgroundError(status.error());
  } else {
    for (int which = 0; which < 2; which++) {
      for (int i = 0; i < c->num_input_files(which); i++) {
        c->edit()->RemoveFile(c->level() + which, c->input(which, i)->number);
      }
    }
    auto log_res = versions_->LogAndApply(c->edit());
    if (!log_res) RecordBackgroundError(log_res.error());
  }

  if (is_manual) {
    ManualCompaction* m = manual_compaction_;
    if (!status) {
      m->done = true;
    }
    if (!m->done) {
      m->tmp_storage = manual_end;
      m->begin = &m->tmp_storage;
    }
    manual_compaction_ = nullptr;
  }
}

void DBImpl::RecordBackgroundError(const Status& s) {
  if (bg_error_.ok()) {
    bg_error_ = s;
    background_work_finished_signal_.notify_all();
  }
}

Result<void> DBImpl::DoCompactionWork(Compaction* c) {
  auto input = versions_->MakeInputIterator(c);
  input->SeekToFirst();
  
  if (!input->Valid()) return {};
  
  SequenceNumber smallest_snapshot;
  if (snapshots_.empty()) {
    smallest_snapshot = versions_->LastSequence();
  } else {
    smallest_snapshot = snapshots_.oldest()->sequence_number();
  }
  
  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  meta.smallest.Clear();
  meta.largest.Clear();
  
  mutex_.unlock();
  
  std::unique_ptr<StdFileSystem::WritableFile> outfile;
  std::string fname = TableFileName(dbname_, meta.number);
  auto f_res = env_->NewWritableFile(fname);
  if (!f_res) {
    mutex_.lock();
    return std::unexpected(f_res.error());
  }
  outfile = std::make_unique<StdFileSystem::WritableFile>(std::move(*f_res));
  
  TableBuilderOptions tb_options(options_);
  TableBuilder<StdFileSystem::WritableFile> builder(tb_options, outfile.get());
  
  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
  
  ParsedInternalKey ikey;
  while (input->Valid() && !shutting_down_.load(std::memory_order_acquire)) {
    std::string_view key = input->key();
    
    bool drop = false;
    auto opt_ikey = ParseInternalKey(key);
    if (!opt_ikey) {
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    } else {
      ikey = *opt_ikey;
      if (!has_current_user_key || internal_comparator_.user_comparator()->Compare(ikey.user_key, current_user_key) != 0) {
        current_user_key = ikey.user_key;
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
      }
      
      if (last_sequence_for_key <= smallest_snapshot) {
        drop = true;
      } else if (ikey.type == kTypeDeletion && ikey.sequence <= smallest_snapshot && c->IsBaseLevelForKey(ikey.user_key)) {
        drop = true;
      }
      last_sequence_for_key = ikey.sequence;
    }
    
    if (!drop) {
      if (meta.file_size == 0) { // we can use file_size as indicator of empty
        meta.smallest.DecodeFrom(key);
      }
      meta.largest.DecodeFrom(key);
      builder.Add(key, input->value());
      meta.file_size = 1; // Mark as not empty temporarily
    }
    input->Next();
  }
  
  auto s = builder.Finish();
  if (s) {
    meta.file_size = builder.FileSize();
    s = outfile->Sync();
  }
  if (s) {
    s = outfile->Close();
  }
  
  mutex_.lock();
  
  if (!s) return std::unexpected(s.error());
  
  if (meta.file_size > 0) {
    c->edit()->AddFile(c->level() + 1, meta.number, meta.file_size, meta.smallest, meta.largest);
  }
  
  return {};
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& options) {
  SequenceNumber latest_snapshot;
  uint32_t seed;
  std::vector<std::unique_ptr<Iterator>> list;
  std::shared_ptr<MemTable> mem;
  std::shared_ptr<MemTable> imm;
  std::shared_ptr<Version> current;
  {
    std::unique_lock<std::mutex> lk(mutex_);
    latest_snapshot = versions_->LastSequence();
    mem = mem_;
    imm = imm_;
    current = versions_->current();
    
    list.push_back(mem->NewIterator());
    if (imm != nullptr) {
      list.push_back(imm->NewIterator());
    }
    current->AddIterators(options, list);
    seed = ++seed_;
  }
  
  auto internal_iter = NewMergingIterator(&internal_comparator_, std::move(list));
  
  SequenceNumber sequence = options.snapshot != nullptr ? 
      static_cast<const SnapshotImpl*>(options.snapshot)->sequence_number() : latest_snapshot;
  
  auto db_iter = NewDBIterator(this, internal_comparator_.user_comparator(), std::move(internal_iter), sequence, seed);
  db_iter->RegisterCleanup([mem = std::move(mem), imm = std::move(imm), current = std::move(current)]() {
    // Keep mem, imm, and current version alive until the iterator is destroyed.
  });
  return db_iter;
}

std::shared_ptr<const Snapshot> DBImpl::GetSnapshot() {
  std::unique_lock<std::mutex> lk(mutex_);
  auto sn = snapshots_.New(versions_->LastSequence());
  return std::shared_ptr<const Snapshot>(sn, [this](const Snapshot* s) {
    std::unique_lock<std::mutex> lk(mutex_);
    snapshots_.Delete(static_cast<const SnapshotImpl*>(s));
  });
}
Result<std::optional<std::string>> DBImpl::GetProperty(std::string_view property) {
  std::unique_lock<std::mutex> lk(mutex_);
  std::string_view in = property;
  std::string_view prefix("leveldb.");
  if (!in.starts_with(prefix)) return std::nullopt;
  in.remove_prefix(prefix.size());

  if (in.starts_with("num-files-at-level")) {
    in.remove_prefix(std::string_view("num-files-at-level").size());
    uint64_t level;
    auto [ptr, ec] = std::from_chars(in.data(), in.data() + in.size(), level);
    bool ok = (ec == std::errc()) && (ptr == in.data() + in.size());
    if (!ok || level >= config::kNumLevels) {
      return std::nullopt;
    } else {
      char buf[100];
      std::snprintf(buf, sizeof(buf), "%d",
                    versions_->NumLevelFiles(static_cast<int>(level)));
      return std::string(buf);
    }
  } else if (in == "sstables") {
    return versions_->current()->DebugString();
  } else if (in == "approximate-memory-usage") {
    size_t total_usage = 0;
    if (options_.block_cache) {
      total_usage += options_.block_cache->TotalCharge();
    }
    if (mem_) {
      total_usage += mem_->ApproximateMemoryUsage();
    }
    if (imm_) {
      total_usage += imm_->ApproximateMemoryUsage();
    }
    char buf[50];
    std::snprintf(buf, sizeof(buf), "%llu",
                  static_cast<unsigned long long>(total_usage));
    return std::string(buf);
  } else if (in == "stats") {
    char buf[200];
    std::string value;
    std::snprintf(buf, sizeof(buf),
                  "                               Compactions\n"
                  "Level  Files Size(MB) Time(sec) Read(MB) Write(MB)\n"
                  "--------------------------------------------------\n");
    value.append(buf);
    for (int level = 0; level < config::kNumLevels; level++) {
      int files = versions_->NumLevelFiles(level);
      if (files > 0) {
        std::snprintf(buf, sizeof(buf), "%3d %8d %8.0f %9.0f %8.0f %9.0f\n",
                      level, files, versions_->NumLevelBytes(level) / 1048576.0,
                      0.0,
                      0.0,
                      0.0);
        value.append(buf);
      }
    }
    return value;
  }

  return std::nullopt;
}

void DBImpl::GetApproximateSizes(const Range* range, int n, uint64_t* sizes) {
  std::unique_lock<std::mutex> lk(mutex_);
  auto current = versions_->current();

  for (int i = 0; i < n; i++) {
    InternalKey k1(range[i].start, kMaxSequenceNumber, kValueTypeForSeek);
    InternalKey k2(range[i].limit, kMaxSequenceNumber, kValueTypeForSeek);
    uint64_t start = versions_->ApproximateOffsetOf(current, k1);
    uint64_t limit = versions_->ApproximateOffsetOf(current, k2);
    sizes[i] = (limit >= start ? limit - start : 0);
  }
}
void DBImpl::CompactRange(const std::string_view* begin, const std::string_view* end) {
  int max_level_with_files = 1;
  {
    std::unique_lock<std::mutex> lk(mutex_);
    auto base = versions_->current();
    for (int level = 1; level < config::kNumLevels; level++) {
      if (base->OverlapInLevel(level, begin, end)) {
        max_level_with_files = level;
      }
    }
  }
  TEST_CompactMemTable();
  for (int level = 0; level < max_level_with_files; level++) {
    TEST_CompactRange(level, begin, end);
  }
}

void DBImpl::TEST_CompactRange(int level, const std::string_view* begin, const std::string_view* end) {
  assert(level >= 0);
  assert(level + 1 < config::kNumLevels);

  InternalKey begin_storage, end_storage;
  ManualCompaction manual;
  manual.level = level;
  manual.done = false;
  if (begin == nullptr) {
    manual.begin = nullptr;
  } else {
    begin_storage = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
    manual.begin = &begin_storage;
  }
  if (end == nullptr) {
    manual.end = nullptr;
  } else {
    end_storage = InternalKey(*end, 0, static_cast<ValueType>(0));
    manual.end = &end_storage;
  }

  std::unique_lock<std::mutex> lk(mutex_);
  while (!manual.done && !shutting_down_.load(std::memory_order_acquire) && bg_error_.ok()) {
    if (manual_compaction_ == nullptr) {
      manual_compaction_ = &manual;
      MaybeScheduleCompaction();
    } else {
      background_work_finished_signal_.wait(lk);
    }
  }
  if (manual_compaction_ == &manual) {
    manual_compaction_ = nullptr;
  }
}

Result<void> DBImpl::TEST_CompactMemTable() {
  auto s = Write(WriteOptions(), nullptr);
  if (s) {
    std::unique_lock<std::mutex> lk(mutex_);
    while (imm_ != nullptr && bg_error_.ok() && !shutting_down_.load(std::memory_order_acquire)) {
      background_work_finished_signal_.wait(lk);
    }
    if (imm_ != nullptr) {
      s = std::unexpected(bg_error_);
    }
  }
  return s;
}

void DBImpl::RecordReadSample(std::string_view key) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (versions_->current()->RecordReadSample(key)) {
    MaybeScheduleCompaction();
  }
}

Result<void> DestroyDB(std::string_view name, const Options<StdFileSystem>& options) {
  auto env = options.env;
  std::filesystem::path dbname(name);
  auto children_res = env->GetChildren(dbname);
  if (!children_res) {
    return {}; // Ignore error in case directory does not exist
  }
  
  auto filenames = std::move(*children_res);
  std::filesystem::path lockname = dbname / "LOCK";
  auto lock_res = env->LockFile(lockname);
  if (lock_res) {
    uint64_t number;
    FileType type;
    for (const auto& fname : filenames) {
      auto parsed = ParseFileName(fname);
      if (parsed && parsed->type != kDBLockFile) {
        auto del_res = env->RemoveFile(dbname / fname);
        if (!del_res) {
          // Keep returning error if deletion fails
          return del_res;
        }
      }
    }
    env->UnlockFile(std::move(*lock_res));
    env->RemoveFile(lockname);
    env->RemoveDir(dbname);
  }
  return {};
}
}  // namespace leveldb
