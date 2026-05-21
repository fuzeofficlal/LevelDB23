//
// Modernized for C++23.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>

#include "leveldb/status.h"
#include "leveldb/std_file_system.h"

namespace leveldb {

enum FileType {
  kLogFile,
  kDBLockFile,
  kTableFile,
  kDescriptorFile,
  kCurrentFile,
  kTempFile,
  kInfoLogFile
};

std::string LogFileName(std::string_view dbname, uint64_t number);
std::string TableFileName(std::string_view dbname, uint64_t number);
std::string SSTTableFileName(std::string_view dbname, uint64_t number);
std::string DescriptorFileName(std::string_view dbname, uint64_t number);
std::string CurrentFileName(std::string_view dbname);
std::string LockFileName(std::string_view dbname);
std::string TempFileName(std::string_view dbname, uint64_t number);
std::string InfoLogFileName(std::string_view dbname);
std::string OldInfoLogFileName(std::string_view dbname);

struct ParsedFile {
  uint64_t number;
  FileType type;
};

std::optional<ParsedFile> ParseFileName(std::string_view filename);

Result<void> SetCurrentFile(StdFileSystem* env, std::string_view dbname,
                            uint64_t descriptor_number);

}  // namespace leveldb
