//
// Modernized for C++23.

#include "db/builder.h"
#include "db/db_impl.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "db/write_batch_internal.h"
#include "leveldb/comparator.h"
#include "leveldb/db.h"
#include "leveldb/std_file_system.h"
#include "leveldb/table_builder.h"
#include "util/logging.h"

#include <string>
#include <vector>

namespace leveldb {

template<typename... Args>
void Log(const std::function<void(std::string_view)>& info_log, const char* fmt, Args... args) {
  if (info_log) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), fmt, args...);
    info_log(buf);
  }
}

namespace {

Options<StdFileSystem> SanitizeOptions(const Options<StdFileSystem>& src, const InternalKeyComparator* icmp) {
  Options<StdFileSystem> result = src;
  result.comparator = icmp;
  return result;
}

class Repairer {
 public:
  Repairer(std::string_view dbname, const Options<StdFileSystem>& options)
      : dbname_(dbname),
        env_(options.env),
        icmp_(options.comparator ? options.comparator : BytewiseComparator()),
        ipolicy_(options.filter_policy),
        options_(SanitizeOptions(options, &icmp_)),
        next_file_number_(1) {

    table_cache_ = new TableCache(dbname_, &options_, 10);
  }


  ~Repairer() {
    delete table_cache_;
  }

  Result<void> Run() {
    auto status = FindFiles();
    if (status) {
      ConvertLogFilesToTables();
      ExtractMetaData();
      status = WriteDescriptor();
    }
    if (status) {
      unsigned long long bytes = 0;
      for (size_t i = 0; i < tables_.size(); i++) {
        bytes += tables_[i].meta.file_size;
      }
      Log(options_.info_log,
          "**** Repaired leveldb %s; "
          "recovered %d files; %llu bytes. "
          "Some data may have been lost. "
          "****",
          dbname_.c_str(), static_cast<int>(tables_.size()), bytes);
    }
    return status;
  }

 private:
  struct TableInfo {
    FileMetaData meta;
    SequenceNumber max_sequence;
  };

  Result<void> FindFiles() {
    auto children_res = env_->GetChildren(dbname_);
    if (!children_res) return std::unexpected(children_res.error());
    auto filenames = std::move(*children_res);

    if (filenames.empty()) {
      return std::unexpected(Status::IOError(dbname_, "repair found no files"));
    }

    for (size_t i = 0; i < filenames.size(); i++) {
      auto parsed = ParseFileName(filenames[i]);
      if (parsed) {
        if (parsed->type == kDescriptorFile) {
          manifests_.push_back(filenames[i]);
        } else {
          if (parsed->number + 1 > next_file_number_) {
            next_file_number_ = parsed->number + 1;
          }
          if (parsed->type == kLogFile) {
            logs_.push_back(parsed->number);
          } else if (parsed->type == kTableFile) {
            table_numbers_.push_back(parsed->number);
          }
        }
      }
    }
    return {};
  }

  void ConvertLogFilesToTables() {
    for (size_t i = 0; i < logs_.size(); i++) {
      std::string logname = LogFileName(dbname_, logs_[i]);
      auto status = ConvertLogToTable(logs_[i]);
      if (!status) {
        Log(options_.info_log, "Log #%llu: ignoring conversion error: %s",
            (unsigned long long)logs_[i], status.error().ToString().c_str());
      }
      ArchiveFile(logname);
    }
  }

  Result<void> ConvertLogToTable(uint64_t lognum) {
    std::string logname = LogFileName(dbname_, lognum);
    auto lfile_res = env_->NewSequentialFile(logname);
    if (!lfile_res) return std::unexpected(lfile_res.error());
    auto lfile = std::make_unique<StdFileSystem::SequentialFile>(std::move(*lfile_res));

    auto reporter = [&](uint64_t bytes, const Status& s) {
      Log(options_.info_log, "Log #%llu: dropping %llu bytes; %s",
          (unsigned long long)lognum, (unsigned long long)bytes, s.ToString().c_str());
    };

    log::Reader<StdFileSystem::SequentialFile> reader(lfile.get(), reporter, false, 0);

    std::string scratch;
    WriteBatch batch;
    auto mem = std::make_shared<MemTable>(icmp_);
    int counter = 0;
    while (auto record_opt = reader.ReadRecord(scratch)) {
      auto record = *record_opt;
      if (record.size() < 12) {
        reporter(record.size(), Status::Corruption("log record too small"));
        continue;
      }
      WriteBatchInternal::SetContents(&batch, record);
      auto status = WriteBatchInternal::InsertInto(&batch, mem.get());
      if (status) {
        counter += WriteBatchInternal::Count(&batch);
      } else {
        Log(options_.info_log, "Log #%llu: ignoring %s",
            (unsigned long long)lognum, status.error().ToString().c_str());
      }
    }

    FileMetaData meta;
    meta.number = next_file_number_++;
    auto iter = mem->NewIterator();
    auto status = BuildTable(dbname_, env_, options_, table_cache_, iter.get(), &meta);
    if (status && meta.file_size > 0) {
      table_numbers_.push_back(meta.number);
    }
    Log(options_.info_log, "Log #%llu: %d ops saved to Table #%llu %s",
        (unsigned long long)lognum, counter, (unsigned long long)meta.number,
        status ? "OK" : status.error().ToString().c_str());
    return status;
  }

  void ExtractMetaData() {
    for (size_t i = 0; i < table_numbers_.size(); i++) {
      ScanTable(table_numbers_[i]);
    }
  }

  std::unique_ptr<Iterator> NewTableIterator(const FileMetaData& meta) {
    ReadOptions r;
    r.verify_checksums = options_.paranoid_checks;
    return table_cache_->NewIterator(r, meta.number, meta.file_size);
  }

  void ScanTable(uint64_t number) {
    TableInfo t;
    t.meta.number = number;
    std::string fname = TableFileName(dbname_, number);
    auto size_res = env_->GetFileSize(fname);
    if (!size_res) {
      fname = SSTTableFileName(dbname_, number);
      size_res = env_->GetFileSize(fname);
    }
    if (!size_res) {
      ArchiveFile(TableFileName(dbname_, number));
      ArchiveFile(SSTTableFileName(dbname_, number));
      Log(options_.info_log, "Table #%llu: dropped: %s",
          (unsigned long long)t.meta.number, size_res.error().ToString().c_str());
      return;
    }
    t.meta.file_size = *size_res;

    int counter = 0;
    auto iter = NewTableIterator(t.meta);
    bool empty = true;
    t.max_sequence = 0;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      auto key = iter->key();
      auto parsed_opt = ParseInternalKey(key);
      if (!parsed_opt) {
        Log(options_.info_log, "Table #%llu: unparsable key %s",
            (unsigned long long)t.meta.number, EscapeString(key).c_str());
        continue;
      }

      counter++;
      if (empty) {
        empty = false;
        t.meta.smallest.DecodeFrom(key);
      }
      t.meta.largest.DecodeFrom(key);
      if (parsed_opt->sequence > t.max_sequence) {
        t.max_sequence = parsed_opt->sequence;
      }
    }

    Status iter_status;
    if (!iter->status()) iter_status = iter->status().error();

    Log(options_.info_log, "Table #%llu: %d entries %s",
        (unsigned long long)t.meta.number, counter, iter_status.ok() ? "OK" : iter_status.ToString().c_str());

    if (iter_status.ok()) {
      tables_.push_back(t);
    } else {
      RepairTable(fname, t);
    }
  }

  void RepairTable(const std::string& src, TableInfo t) {
    std::string copy = TableFileName(dbname_, next_file_number_++);
    auto s_res = env_->NewWritableFile(copy);
    if (!s_res) return;
    auto file = std::make_unique<StdFileSystem::WritableFile>(std::move(*s_res));
    auto builder = std::make_unique<TableBuilder<StdFileSystem::WritableFile>>(options_, file.get());

    auto iter = NewTableIterator(t.meta);
    int counter = 0;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      builder->Add(iter->key(), iter->value());
      counter++;
    }

    ArchiveFile(src);
    Result<void> s;
    if (counter == 0) {
      builder->Abandon();
    } else {
      s = builder->Finish();
      if (s) {
        t.meta.file_size = builder->FileSize();
      }
    }
    builder.reset();

    if (s) s = file->Close();
    file.reset();

    if (counter > 0 && s) {
      std::string orig = TableFileName(dbname_, t.meta.number);
      s = env_->RenameFile(copy, orig);
      if (s) {
        Log(options_.info_log, "Table #%llu: %d entries repaired",
            (unsigned long long)t.meta.number, counter);
        tables_.push_back(t);
      }
    }
    if (!s) {
      auto _ = env_->RemoveFile(copy);
    }
  }

  Result<void> WriteDescriptor() {
    std::string tmp = TempFileName(dbname_, 1);
    auto file_res = env_->NewWritableFile(tmp);
    if (!file_res) return std::unexpected(file_res.error());
    auto file = std::make_unique<StdFileSystem::WritableFile>(std::move(*file_res));

    SequenceNumber max_sequence = 0;
    for (size_t i = 0; i < tables_.size(); i++) {
      if (max_sequence < tables_[i].max_sequence) {
        max_sequence = tables_[i].max_sequence;
      }
    }

    edit_.SetComparatorName(icmp_.user_comparator()->Name());
    edit_.SetLogNumber(0);
    edit_.SetNextFile(next_file_number_);
    edit_.SetLastSequence(max_sequence);

    for (size_t i = 0; i < tables_.size(); i++) {
      const TableInfo& t = tables_[i];
      edit_.AddFile(0, t.meta.number, t.meta.file_size, t.meta.smallest, t.meta.largest);
    }

    {
      log::Writer<StdFileSystem::WritableFile> log(file.get());
      std::string record;
      edit_.EncodeTo(record);
      auto status = log.AddRecord(record);
      if (!status) return status;
    }
    auto status = file->Close();
    file.reset();

    if (!status) {
      auto _ = env_->RemoveFile(tmp);
    } else {
      for (size_t i = 0; i < manifests_.size(); i++) {
        ArchiveFile(dbname_ + "/" + manifests_[i]);
      }
      status = env_->RenameFile(tmp, DescriptorFileName(dbname_, 1));
      if (status) {
        status = SetCurrentFile(env_, dbname_, 1);
      } else {
        auto _ = env_->RemoveFile(tmp);
      }
    }
    return status;
  }

  void ArchiveFile(const std::string& fname) {
    const char* slash = strrchr(fname.c_str(), '/');
    std::string new_dir;
    if (slash != nullptr) {
      new_dir.assign(fname.data(), slash - fname.data());
    }
    new_dir.append("/lost");
    auto _ = env_->CreateDir(new_dir);
    std::string new_file = new_dir;
    new_file.append("/");
    new_file.append((slash == nullptr) ? fname.c_str() : slash + 1);
    auto s = env_->RenameFile(fname, new_file);
    Log(options_.info_log, "Archiving %s: %s\n", fname.c_str(),
        s ? "OK" : s.error().ToString().c_str());
  }

  const std::string dbname_;
  StdFileSystem* const env_;
  InternalKeyComparator const icmp_;
  InternalFilterPolicy const ipolicy_;
  const Options<StdFileSystem> options_;
  TableCache* table_cache_;
  VersionEdit edit_;

  std::vector<std::string> manifests_;
  std::vector<uint64_t> table_numbers_;
  std::vector<uint64_t> logs_;
  std::vector<TableInfo> tables_;
  uint64_t next_file_number_;
};
}  // namespace

Result<void> RepairDB(std::string_view dbname, const Options<StdFileSystem>& options) {
  Repairer repairer(dbname, options);
  return repairer.Run();
}

}  // namespace leveldb
