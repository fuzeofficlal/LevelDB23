//
// Modernized for C++23.

#include "db/filename.h"
#include <cstdio>
#include <charconv>

namespace leveldb {

static std::string MakeFileName(std::string_view dbname, uint64_t number, std::string_view suffix) {
  char buf[100];
  std::snprintf(buf, sizeof(buf), "/%06llu", static_cast<unsigned long long>(number));
  std::string result(dbname);
  result.append(buf);
  result.append(suffix);
  return result;
}

std::string LogFileName(std::string_view dbname, uint64_t number) {
  return MakeFileName(dbname, number, ".log");
}

std::string TableFileName(std::string_view dbname, uint64_t number) {
  return MakeFileName(dbname, number, ".ldb");
}

std::string SSTTableFileName(std::string_view dbname, uint64_t number) {
  return MakeFileName(dbname, number, ".sst");
}

std::string DescriptorFileName(std::string_view dbname, uint64_t number) {
  char buf[100];
  std::snprintf(buf, sizeof(buf), "/MANIFEST-%06llu", static_cast<unsigned long long>(number));
  std::string result(dbname);
  result.append(buf);
  return result;
}

std::string CurrentFileName(std::string_view dbname) {
  return std::string(dbname) + "/CURRENT";
}

std::string LockFileName(std::string_view dbname) {
  return std::string(dbname) + "/LOCK";
}

std::string TempFileName(std::string_view dbname, uint64_t number) {
  return MakeFileName(dbname, number, ".dbtmp");
}

std::string InfoLogFileName(std::string_view dbname) {
  return std::string(dbname) + "/LOG";
}

std::string OldInfoLogFileName(std::string_view dbname) {
  return std::string(dbname) + "/LOG.old";
}

std::optional<ParsedFile> ParseFileName(std::string_view filename) {
  if (filename == "CURRENT") return ParsedFile{0, kCurrentFile};
  if (filename == "LOCK") return ParsedFile{0, kDBLockFile};
  if (filename == "LOG" || filename == "LOG.old") return ParsedFile{0, kInfoLogFile};

  if (filename.starts_with("MANIFEST-")) {
    uint64_t num;
    auto [ptr, ec] = std::from_chars(filename.data() + 9, filename.data() + filename.size(), num);
    if (ec == std::errc() && ptr == filename.data() + filename.size()) {
      return ParsedFile{num, kDescriptorFile};
    }
  } else {
    // Expected format: "%06llu.suffix"
    size_t dot_pos = filename.find('.');
    if (dot_pos != std::string_view::npos) {
      uint64_t num;
      auto [ptr, ec] = std::from_chars(filename.data(), filename.data() + dot_pos, num);
      if (ec == std::errc() && ptr == filename.data() + dot_pos) {
        std::string_view suffix = filename.substr(dot_pos);
        if (suffix == ".log") return ParsedFile{num, kLogFile};
        if (suffix == ".sst" || suffix == ".ldb") return ParsedFile{num, kTableFile};
        if (suffix == ".dbtmp") return ParsedFile{num, kTempFile};
      }
    }
  }
  return std::nullopt;
}

Result<void> SetCurrentFile(StdFileSystem* env, std::string_view dbname,
                            uint64_t descriptor_number) {
  std::string manifest = DescriptorFileName(dbname, descriptor_number);
  std::string contents = manifest.substr(dbname.length() + 1);
  if (!contents.ends_with('\n')) {
    contents.push_back('\n');
  }

  std::string tmp = TempFileName(dbname, descriptor_number);
  auto f_res = env->NewWritableFile(tmp);
  if (!f_res) return std::unexpected(f_res.error());
  
  auto s = (*f_res).Append(contents);
  if (!s) {
    auto _ = env->RemoveFile(tmp);
    return s;
  }
  s = (*f_res).Sync();
  if (!s) {
    auto _ = env->RemoveFile(tmp);
    return s;
  }
  s = (*f_res).Close();
  if (!s) {
    auto _ = env->RemoveFile(tmp);
    return s;
  }
  
  s = env->RenameFile(tmp, CurrentFileName(dbname));
  if (!s) {
    auto _ = env->RemoveFile(tmp);
    return s;
  }
  return {};
}

}  // namespace leveldb
