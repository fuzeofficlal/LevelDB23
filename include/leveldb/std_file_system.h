//
// Modernized for C++23.
//
// Provides a standard implementation of the CFileSystem concept, utilizing
// C++23 <filesystem>, <fstream>, and OS-specific APIs for thread-safe
// random access and file locking.

#pragma once

#include "leveldb/env.h"
#include <filesystem>
#include <fstream>

// Forward declare OS-specific handle types to avoid exposing windows.h
#if defined(_WIN32)
using FileHandle = void*;
#else
using FileHandle = int;
#endif

namespace leveldb {

class StdSequentialFile {
 public:
  explicit StdSequentialFile(const std::filesystem::path& path);
  ~StdSequentialFile() = default;

  // Movable but not copyable
  StdSequentialFile(StdSequentialFile&&) noexcept = default;
  StdSequentialFile& operator=(StdSequentialFile&&) noexcept = default;

  Result<std::string_view> Read(size_t n, char* scratch);
  Result<void> Skip(uint64_t n);

 private:
  std::ifstream file_;
};

class StdRandomAccessFile {
 public:
  explicit StdRandomAccessFile(const std::filesystem::path& path);
  ~StdRandomAccessFile();

  // Movable but not copyable
  StdRandomAccessFile(StdRandomAccessFile&&) noexcept;
  StdRandomAccessFile& operator=(StdRandomAccessFile&&) noexcept;

  Result<std::string_view> Read(uint64_t offset, size_t n, char* scratch) const;

  FileHandle handle() const { return handle_; }

 private:
  FileHandle handle_;
};

class StdWritableFile {
 public:
  StdWritableFile(const std::filesystem::path& path, bool append);
  ~StdWritableFile();

  // Movable but not copyable
  StdWritableFile(StdWritableFile&&) noexcept = default;
  StdWritableFile& operator=(StdWritableFile&&) noexcept = default;

  Result<void> Append(std::string_view data);
  Result<void> Close();
  Result<void> Flush();
  Result<void> Sync();

 private:
  std::ofstream file_;
};

struct StdFileLock {
  FileHandle handle_;
  std::filesystem::path path_;
};

class StdFileSystem {
 public:
  using SequentialFile = StdSequentialFile;
  using RandomAccessFile = StdRandomAccessFile;
  using WritableFile = StdWritableFile;
  using FileLock = StdFileLock;

  Result<SequentialFile> NewSequentialFile(const std::filesystem::path& path) const;
  Result<RandomAccessFile> NewRandomAccessFile(const std::filesystem::path& path) const;
  Result<WritableFile> NewWritableFile(const std::filesystem::path& path) const;
  Result<WritableFile> NewAppendableFile(const std::filesystem::path& path) const;

  bool FileExists(const std::filesystem::path& path) const;
  Result<std::vector<std::string>> GetChildren(const std::filesystem::path& dir) const;
  Result<void> RemoveFile(const std::filesystem::path& path) const;
  Result<void> CreateDir(const std::filesystem::path& dir) const;
  Result<void> RemoveDir(const std::filesystem::path& dir) const;
  Result<uint64_t> GetFileSize(const std::filesystem::path& path) const;
  Result<void> RenameFile(const std::filesystem::path& src, const std::filesystem::path& target) const;

  Result<FileLock> LockFile(const std::filesystem::path& path) const;
  Result<void> UnlockFile(FileLock lock) const;

  Result<std::filesystem::path> GetTestDirectory() const;
};

// Compile-time check to ensure StdFileSystem meets the CFileSystem concept.
static_assert(CFileSystem<StdFileSystem>);

}  // namespace leveldb
