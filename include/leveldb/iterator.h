//
// Modernized for C++23.
//
// An iterator yields a sequence of key/value pairs from a source.
//
// Multiple threads can invoke const methods on an Iterator without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same Iterator must use
// external synchronization.
//
// C++23 changes:
//   - Slice -> std::string_view
//   - Status status() -> Result<void> status()
//   - RegisterCleanup replaced with a vector of std::move_only_function<void()>
//   - Removed LEVELDB_EXPORT.
//   - Added [[nodiscard]] where appropriate.

#pragma once

#include <functional>
#include <string_view>
#include <vector>

#include "leveldb/status.h"

namespace leveldb {

class Iterator {
 public:
  Iterator() = default;

  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;

  virtual ~Iterator() {
    // Run cleanup functions in reverse order of registration
    for (auto it = cleanup_functions_.rbegin(); it != cleanup_functions_.rend();
         ++it) {
      if (*it) {
        (*it)();
      }
    }
  }

  // An iterator is either positioned at a key/value pair, or
  // not valid. This method returns true iff the iterator is valid.
  [[nodiscard]] virtual bool Valid() const noexcept = 0;

  // Position at the first key in the source. The iterator is Valid()
  // after this call iff the source is not empty.
  virtual void SeekToFirst() = 0;

  // Position at the last key in the source. The iterator is
  // Valid() after this call iff the source is not empty.
  virtual void SeekToLast() = 0;

  // Position at the first key in the source that is at or past target.
  // The iterator is Valid() after this call iff the source contains
  // an entry that comes at or past target.
  virtual void Seek(std::string_view target) = 0;

  // Moves to the next entry in the source. After this call, Valid() is
  // true iff the iterator was not positioned at the last entry in the source.
  // REQUIRES: Valid()
  virtual void Next() = 0;

  // Moves to the previous entry in the source. After this call, Valid() is
  // true iff the iterator was not positioned at the first entry in source.
  // REQUIRES: Valid()
  virtual void Prev() = 0;

  // Return the key for the current entry. The underlying storage for
  // the returned string_view is valid only until the next modification of
  // the iterator.
  // REQUIRES: Valid()
  [[nodiscard]] virtual std::string_view key() const = 0;

  // Return the value for the current entry. The underlying storage for
  // the returned string_view is valid only until the next modification of
  // the iterator.
  // REQUIRES: Valid()
  [[nodiscard]] virtual std::string_view value() const = 0;

  // If an error has occurred, return it. Else return an ok Result<void>.
  [[nodiscard]] virtual Result<void> status() const = 0;

  // Clients are allowed to register a function that
  // will be invoked when this iterator is destroyed.
  // This replaces the old RegisterCleanup(void*, void*) with a modern
  // C++ closure.
  void RegisterCleanup(std::move_only_function<void()> func) {
    if (func) {
      cleanup_functions_.push_back(std::move(func));
    }
  }

 private:
  std::vector<std::move_only_function<void()>> cleanup_functions_;
};

// Return an empty iterator (yields nothing).
[[nodiscard]] Iterator* NewEmptyIterator();

// Return an empty iterator with the specified status.
[[nodiscard]] Iterator* NewErrorIterator(const Result<void>& status);

}  // namespace leveldb
