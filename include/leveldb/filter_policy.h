//
// Modernized for C++23.
//
// A database can be configured with a custom FilterPolicy object.
// This object is responsible for creating a small filter from a set
// of keys. These filters are stored in leveldb and are consulted
// automatically by leveldb to decide whether or not to read some
// information from disk. In many cases, a filter can cut down the
// number of disk seeks form a handful to a single disk seek per
// DB::Get() call.
//
// Most people will want to use the builtin bloom filter support (see
// NewBloomFilterPolicy() below).
//
// C++23 changes:
//   - Slice -> std::string_view
//   - CreateFilter(const Slice* keys, int n) -> CreateFilter(std::span<const std::string_view> keys)
//   - Removed LEVELDB_EXPORT.

#pragma once

#include <span>
#include <string>
#include <string_view>

namespace leveldb {

class FilterPolicy {
 public:
  virtual ~FilterPolicy() = default;

  // Return the name of this policy. Note that if the filter encoding
  // changes in an incompatible way, the name returned by this method
  // must be changed. Otherwise, old incompatible filters may be
  // passed to methods of this type.
  [[nodiscard]] virtual const char* Name() const noexcept = 0;

  // keys contains a list of keys (potentially with duplicates)
  // that are ordered according to the user supplied comparator.
  // Append a filter that summarizes keys to dst.
  //
  // Warning: do not change the initial contents of dst. Instead,
  // append the newly constructed filter to dst.
  virtual void CreateFilter(std::span<const std::string_view> keys,
                            std::string& dst) const = 0;

  // "filter" contains the data appended by a preceding call to
  // CreateFilter() on this class. This method must return true if
  // the key was in the list of keys passed to CreateFilter().
  // This method may return true or false if the key was not on the
  // list, but it should aim to return false with a high probability.
  [[nodiscard]] virtual bool KeyMayMatch(std::string_view key,
                                         std::string_view filter) const = 0;
};

// Return a new filter policy that uses a bloom filter with approximately
// the specified number of bits per key. A good value for bits_per_key
// is 10, which yields a filter with ~ 1% false positive rate.
//
// Callers must delete the result after any database that is using the
// result has been closed.
//
// Note: if you are using a custom comparator that ignores some parts
// of the keys being compared, you must not use NewBloomFilterPolicy()
// and must provide your own FilterPolicy that also ignores the
// corresponding parts of the keys. For example, if the comparator
// ignores trailing spaces, it would be incorrect to use a
// FilterPolicy (like NewBloomFilterPolicy) that does not ignore
// trailing spaces in keys.
[[nodiscard]] const FilterPolicy* NewBloomFilterPolicy(int bits_per_key);

}  // namespace leveldb
