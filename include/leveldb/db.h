//
// Modernized for C++23.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "leveldb/iterator.h"
#include "leveldb/options.h"
#include "leveldb/status.h"
#include "leveldb/std_file_system.h" // Default FS

namespace leveldb {

static const int kMajorVersion = 1;
static const int kMinorVersion = 23;

struct ReadOptions;
struct WriteOptions;
class WriteBatch;

// Abstract handle to particular state of a DB.
// A Snapshot is an immutable object and can therefore be safely
// accessed from multiple threads without any external synchronization.
class Snapshot {
 public:
  virtual ~Snapshot() = default;
};

// A range of keys
struct Range {
  Range() = default;
  Range(std::string_view s, std::string_view l) : start(s), limit(l) {}

  std::string_view start;  // Included in the range
  std::string_view limit;  // Not included in the range
};

// A DB is a persistent ordered map from keys to values.
// A DB is safe for concurrent access from multiple threads without
// any external synchronization.
class DB {
 public:
  // Open the database with the specified "name".
  static Result<std::unique_ptr<DB>> Open(const Options<StdFileSystem>& options,
                                          std::string_view name);

  DB() = default;

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  virtual ~DB() = default;

  virtual Result<void> Put(const WriteOptions& options, std::string_view key,
                           std::string_view value) = 0;

  virtual Result<void> Delete(const WriteOptions& options, std::string_view key) = 0;

  virtual Result<void> Write(const WriteOptions& options, WriteBatch* updates) = 0;

  // If there is no entry for "key", returns std::nullopt.
  virtual Result<std::optional<std::string>> Get(const ReadOptions& options,
                                                 std::string_view key) = 0;

  virtual std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) = 0;

  // Return a handle to the current DB state.
  // The returned snapshot is owned by the user and must be kept alive.
  // We use std::shared_ptr so ReleaseSnapshot is no longer necessary!
  virtual std::shared_ptr<const Snapshot> GetSnapshot() = 0;

  // DB implementations can export properties about their state
  virtual Result<std::optional<std::string>> GetProperty(std::string_view property) = 0;

  virtual void GetApproximateSizes(const Range* range, int n,
                                   uint64_t* sizes) = 0;

  virtual void CompactRange(const std::string_view* begin, const std::string_view* end) = 0;
};

// Destroy the contents of the specified database.
Result<void> DestroyDB(std::string_view name, const Options<StdFileSystem>& options);

// If a DB cannot be opened, you may attempt to call this method to
// resurrect as much of the contents of the database as possible.
Result<void> RepairDB(std::string_view dbname, const Options<StdFileSystem>& options);

}  // namespace leveldb
