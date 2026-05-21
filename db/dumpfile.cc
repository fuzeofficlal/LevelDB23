//
// Modernized for C++23.

#include "leveldb/dumpfile.h"

#include <cstdio>
#include <format>
#include <string>

#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/version_edit.h"
#include "db/write_batch_internal.h"
#include "leveldb/std_file_system.h"
#include "leveldb/iterator.h"
#include "leveldb/options.h"
#include "leveldb/status.h"
#include "leveldb/table.h"
#include "leveldb/write_batch.h"
#include "util/logging.h"

namespace leveldb {

namespace {

std::optional<FileType> GuessType(const std::filesystem::path& fname) {
  auto parsed = ParseFileName(fname.filename().string());
  if (parsed) {
    return parsed->type;
  }
  return std::nullopt;
}

// Print contents of a log file. (*func)() is called on every record.
Result<void> PrintLogContents(StdFileSystem* env, const std::filesystem::path& fname,
                              void (*func)(uint64_t, std::string_view, std::function<void(std::string_view)>),
                              std::function<void(std::string_view)> append_cb) {
  auto file_res = env->NewSequentialFile(fname);
  if (!file_res) {
    return std::unexpected(file_res.error());
  }

  auto reporter = [append_cb](size_t bytes, const Status& status) {
    std::string r = "corruption: ";
    AppendNumberTo(r, bytes);
    r += " bytes; ";
    r += status.ToString();
    r.push_back('\n');
    append_cb(r);
  };

  log::Reader<StdFileSystem::SequentialFile> reader(&file_res.value(), reporter, true, 0);
  std::string scratch;
  
  while (true) {
    auto record = reader.ReadRecord(scratch);
    if (!record) {
      break;
    }
    (*func)(reader.LastRecordOffset(), *record, append_cb);
  }
  return {};
}

// Called on every item found in a WriteBatch.
class WriteBatchItemPrinter : public WriteBatch::Handler {
 public:
  void Put(std::string_view key, std::string_view value) override {
    std::string r = "  put '";
    AppendEscapedStringTo(r, key);
    r += "' '";
    AppendEscapedStringTo(r, value);
    r += "'\n";
    append_cb_(r);
  }
  void Delete(std::string_view key) override {
    std::string r = "  del '";
    AppendEscapedStringTo(r, key);
    r += "'\n";
    append_cb_(r);
  }

  std::function<void(std::string_view)> append_cb_;
};

// Called on every log record (each one of which is a WriteBatch)
// found in a kLogFile.
static void WriteBatchPrinter(uint64_t pos, std::string_view record, std::function<void(std::string_view)> append_cb) {
  std::string r = "--- offset ";
  AppendNumberTo(r, pos);
  r += "; ";
  if (record.size() < 12) {
    r += "log record length ";
    AppendNumberTo(r, record.size());
    r += " is too small\n";
    append_cb(r);
    return;
  }
  WriteBatch batch;
  WriteBatchInternal::SetContents(&batch, record);
  r += "sequence ";
  AppendNumberTo(r, WriteBatchInternal::Sequence(&batch));
  r.push_back('\n');
  append_cb(r);
  
  WriteBatchItemPrinter batch_item_printer;
  batch_item_printer.append_cb_ = append_cb;
  auto s = batch.Iterate(&batch_item_printer);
  if (!s) {
    append_cb("  error: " + s.error().ToString() + "\n");
  }
}

Result<void> DumpLog(StdFileSystem* env, const std::filesystem::path& fname, std::function<void(std::string_view)> append_cb) {
  return PrintLogContents(env, fname, WriteBatchPrinter, append_cb);
}

// Called on every log record (each one of which is a WriteBatch)
// found in a kDescriptorFile.
static void VersionEditPrinter(uint64_t pos, std::string_view record, std::function<void(std::string_view)> append_cb) {
  std::string r = "--- offset ";
  AppendNumberTo(r, pos);
  r += "; ";
  VersionEdit edit;
  auto s = edit.DecodeFrom(record);
  if (!s) {
    r += s.error().ToString();
    r.push_back('\n');
  } else {
    r += edit.DebugString();
  }
  append_cb(r);
}

Result<void> DumpDescriptor(StdFileSystem* env, const std::filesystem::path& fname, std::function<void(std::string_view)> append_cb) {
  return PrintLogContents(env, fname, VersionEditPrinter, append_cb);
}

Result<void> DumpTable(StdFileSystem* env, const std::filesystem::path& fname, std::function<void(std::string_view)> append_cb) {
  auto size_res = env->GetFileSize(fname);
  if (!size_res) return std::unexpected(size_res.error());
  
  auto file_res = env->NewRandomAccessFile(fname);
  if (!file_res) return std::unexpected(file_res.error());
  
  Options<StdFileSystem> options;
  auto table_res = Table<StdFileSystem::RandomAccessFile>::Open(options, &file_res.value(), size_res.value());
  
  if (!table_res) return std::unexpected(table_res.error());
  
  auto table = std::move(table_res.value());

  ReadOptions ro;
  ro.fill_cache = false;
  auto iter = table->NewIterator(ro);
  std::string r;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    r.clear();
    auto parsed_key = ParseInternalKey(iter->key());
    if (!parsed_key) {
      r = "badkey '";
      AppendEscapedStringTo(r, iter->key());
      r += "' => '";
      AppendEscapedStringTo(r, iter->value());
      r += "'\n";
      append_cb(r);
    } else {
      ParsedInternalKey key = *parsed_key;
      r = "'";
      AppendEscapedStringTo(r, key.user_key);
      r += "' @ ";
      AppendNumberTo(r, key.sequence);
      r += " : ";
      if (key.type == kTypeDeletion) {
        r += "del";
      } else if (key.type == kTypeValue) {
        r += "val";
      } else {
        AppendNumberTo(r, static_cast<uint64_t>(key.type));
      }
      r += " => '";
      AppendEscapedStringTo(r, iter->value());
      r += "'\n";
      append_cb(r);
    }
  }
  auto s = iter->status();
  if (!s) {
    append_cb("iterator error: " + s.error().ToString() + "\n");
  }

  return {};
}

}  // namespace

Result<void> DumpFile(StdFileSystem* env, const std::filesystem::path& fname, std::function<void(std::string_view)> append_cb) {
  auto ftype = GuessType(fname);
  if (!ftype) {
    return std::unexpected(Status::InvalidArgument(fname.string() + ": unknown file type"));
  }
  switch (*ftype) {
    case kLogFile:
      return DumpLog(env, fname, append_cb);
    case kDescriptorFile:
      return DumpDescriptor(env, fname, append_cb);
    case kTableFile:
      return DumpTable(env, fname, append_cb);
    default:
      break;
  }
  return std::unexpected(Status::InvalidArgument(fname.string() + ": not a dump-able file type"));
}

}  // namespace leveldb
