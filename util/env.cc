#include "leveldb/env.h"

#include <cstdarg>
#include <cstdio>

namespace leveldb {

// ===========================================================================
// Virtual destructor definitions
// ===========================================================================

Env::Env() = default;
Env::~Env() = default;
SequentialFile::~SequentialFile() = default;
RandomAccessFile::~RandomAccessFile() = default;
WritableFile::~WritableFile() = default;
Logger::~Logger() = default;
FileLock::~FileLock() = default;

// ===========================================================================
// Default virtual method implementations
// ===========================================================================

Result<std::unique_ptr<WritableFile>>
Env::NewAppendableFile(const std::filesystem::path& fname) {
  return Status::NotSupportedErr(
      "NewAppendableFile not supported by this Env: " + fname.string());
}

// ===========================================================================
// Free-standing utility functions
// ===========================================================================

void Log(Logger* info_log, const char* format, ...) {
  if (info_log != nullptr) {
    std::va_list ap;
    va_start(ap, format);
    info_log->Logv(format, ap);
    va_end(ap);
  }
}

Result<void> WriteStringToFile(Env* env, std::string_view data,
                               const std::filesystem::path& fname) {
  auto file_result = env->NewWritableFile(fname);
  if (!file_result) return std::unexpected(file_result.error());

  auto& file = *file_result;
  auto s = file->Append(data);
  if (s) {
    s = file->Close();
  }
  if (!s) {
    env->RemoveFile(fname);  // Best-effort cleanup
  }
  return s;
}

Result<std::string> ReadFileToString(Env* env,
                                     const std::filesystem::path& fname) {
  auto file_result = env->NewSequentialFile(fname);
  if (!file_result) return std::unexpected(file_result.error());

  auto& file = *file_result;
  std::string result;
  constexpr size_t kBufferSize = 8192;
  char scratch[kBufferSize];

  while (true) {
    auto read_result = file->Read(kBufferSize, scratch);
    if (!read_result) return std::unexpected(read_result.error());

    auto fragment = *read_result;
    result.append(fragment.data(), fragment.size());
    if (fragment.size() < kBufferSize) break;  // EOF
  }

  return result;
}

}  // namespace leveldb
