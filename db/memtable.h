//
// Modernized for C++23.

#pragma once

#include <memory>
#include <string_view>
#include <optional>

#include "db/dbformat.h"
#include "db/skiplist.h"
#include "leveldb/iterator.h"
#include "util/arena.h"

namespace leveldb {

class InternalKeyComparator;
class MemTableIterator;

class MemTable {
 public:
  explicit MemTable(const InternalKeyComparator& comparator);

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  ~MemTable() = default;

  [[nodiscard]] size_t ApproximateMemoryUsage();

  [[nodiscard]] std::unique_ptr<Iterator> NewIterator();

  void Add(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value);

  [[nodiscard]] Result<std::optional<std::string_view>> Get(const LookupKey& key);

 private:
  friend class MemTableIterator;

  struct KeyComparator {
    const InternalKeyComparator comparator;
    explicit KeyComparator(const InternalKeyComparator& c) : comparator(c) {}
    std::strong_ordering operator()(const char* a, const char* b) const;
  };

  using Table = SkipList<const char*, KeyComparator>;

  KeyComparator comparator_;
  Arena arena_;
  Table table_;
};

}  // namespace leveldb
