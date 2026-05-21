//
// Modernized for C++23.
//
// Defines C++20 Concepts for the LevelDB FileSystem abstraction.
// This replaces the old virtual base class `Env`, enabling zero-overhead
// static polymorphism while retaining testability (e.g. via MemEnv).

#pragma once

#include <concepts>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

#include "leveldb/status.h"

namespace leveldb {

// ===========================================================================
// File abstractions (Concepts)
// ===========================================================================

// Sequential reading.
template <typename T>
concept CSequentialFile = requires(T f, size_t n, char* scratch, uint64_t skip) {
  // Read up to n bytes. Returns the data read (may be shorter than n).
  // scratch may be used as a temporary buffer.
  { f.Read(n, scratch) } -> std::same_as<Result<std::string_view>>;

  // Skip n bytes.
  { f.Skip(skip) } -> std::same_as<Result<void>>;
};

// Random-access reading (thread-safe).
template <typename T>
concept CRandomAccessFile = requires(const T f, uint64_t offset, size_t n, char* scratch) {
  // Read up to n bytes starting at offset.
  { f.Read(offset, n, scratch) } -> std::same_as<Result<std::string_view>>;
};

// Sequential writing (must provide buffering).
template <typename T>
concept CWritableFile = requires(T f, std::string_view data) {
  { f.Append(data) } -> std::same_as<Result<void>>;
  { f.Close() } -> std::same_as<Result<void>>;
  { f.Flush() } -> std::same_as<Result<void>>;
  { f.Sync() } -> std::same_as<Result<void>>;
};

// Identifies a locked file.
template <typename T>
concept CFileLock = std::destructible<T>;

// ===========================================================================
// FileSystem abstraction (Concept)
// ===========================================================================

template <typename T>
concept CFileSystem = requires(T fs, const std::filesystem::path& path) {
  // Associated types
  typename T::SequentialFile;
  requires CSequentialFile<typename T::SequentialFile>;

  typename T::RandomAccessFile;
  requires CRandomAccessFile<typename T::RandomAccessFile>;

  typename T::WritableFile;
  requires CWritableFile<typename T::WritableFile>;

  typename T::FileLock;
  requires CFileLock<typename T::FileLock>;

  // --- File creation ---

  { fs.NewSequentialFile(path) } -> std::same_as<Result<typename T::SequentialFile>>;
  { fs.NewRandomAccessFile(path) } -> std::same_as<Result<typename T::RandomAccessFile>>;
  { fs.NewWritableFile(path) } -> std::same_as<Result<typename T::WritableFile>>;
  { fs.NewAppendableFile(path) } -> std::same_as<Result<typename T::WritableFile>>;

  // --- Filesystem queries ---

  { fs.FileExists(path) } -> std::same_as<bool>;
  { fs.GetChildren(path) } -> std::same_as<Result<std::vector<std::string>>>;
  { fs.RemoveFile(path) } -> std::same_as<Result<void>>;
  { fs.CreateDir(path) } -> std::same_as<Result<void>>;
  { fs.RemoveDir(path) } -> std::same_as<Result<void>>;
  { fs.GetFileSize(path) } -> std::same_as<Result<uint64_t>>;
  { fs.RenameFile(path, path) } -> std::same_as<Result<void>>;

  // --- Locking ---

  { fs.LockFile(path) } -> std::same_as<Result<typename T::FileLock>>;
  { fs.UnlockFile(std::declval<typename T::FileLock>()) } -> std::same_as<Result<void>>;

  // --- Misc ---

  { fs.GetTestDirectory() } -> std::same_as<Result<std::filesystem::path>>;
};

}  // namespace leveldb
