//
// Modernized for C++23.
//
// C++23 changes:
//   - Slice -> std::string_view
//   - int Compare() -> std::strong_ordering Compare()
//   - FindShortestSeparator(std::string* start, const Slice& limit) ->
//     FindShortestSeparator(std::string& start, std::string_view limit)
//   - FindShortSuccessor(std::string* key) -> FindShortSuccessor(std::string& key)
//   - Added [[nodiscard]] and noexcept where applicable.

#pragma once

#include <compare>
#include <string>
#include <string_view>

namespace leveldb {

// A Comparator object provides a total order across strings that are
// used as keys in an sstable or a database. A Comparator implementation
// must be thread-safe since leveldb may invoke its methods concurrently
// from multiple threads.
class Comparator {
 public:
  virtual ~Comparator() = default;

  // Three-way comparison. Returns value:
  //   < 0 iff "a" < "b",
  //   == 0 iff "a" == "b",
  //   > 0 iff "a" > "b"
  // In C++23, we use std::strong_ordering for explicit three-way comparison.
  [[nodiscard]] virtual std::strong_ordering Compare(
      std::string_view a, std::string_view b) const noexcept = 0;

  // The name of the comparator. Used to check for comparator
  // mismatches (i.e., a DB created with one comparator is
  // accessed using a different comparator.
  //
  // The client of this package should switch to a new name whenever
  // the comparator implementation changes in a way that will cause
  // the relative ordering of any two keys to change.
  //
  // Names starting with "leveldb." are reserved and should not be used
  // by any clients of this package.
  [[nodiscard]] virtual const char* Name() const noexcept = 0;

  // Advanced functions: these are used to reduce the space requirements
  // for internal data structures like index blocks.

  // If start < limit, changes start to a short string in [start,limit).
  // Simple comparator implementations may leave start unchanged,
  // i.e., an implementation of this method that does nothing is correct.
  virtual void FindShortestSeparator(std::string& start,
                                     std::string_view limit) const = 0;

  // Changes key to a short string >= key.
  // Simple comparator implementations may leave key unchanged,
  // i.e., an implementation of this method that does nothing is correct.
  virtual void FindShortSuccessor(std::string& key) const = 0;
};

// Return a builtin comparator that uses lexicographic byte-wise
// ordering. The result remains the property of this module and
// must not be deleted.
[[nodiscard]] const Comparator* BytewiseComparator() noexcept;

}  // namespace leveldb
