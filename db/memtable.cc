//
// Modernized for C++23.

#include "db/memtable.h"
#include "db/dbformat.h"
#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"
#include "util/coding.h"
#include <cstring>

namespace leveldb {

static std::string_view GetLengthPrefixedSlice(const char* data) {
  uint32_t len;
  const char* p = data;
  p = detail::GetVarint32Ptr(p, p + 5, len);  // +5: we assume "p" is not corrupted
  return std::string_view(p, len);
}

MemTable::MemTable(const InternalKeyComparator& comparator)
    : comparator_(comparator), table_(comparator_, &arena_) {}

size_t MemTable::ApproximateMemoryUsage() { return arena_.MemoryUsage(); }

std::strong_ordering MemTable::KeyComparator::operator()(const char* aptr,
                                        const char* bptr) const {
  std::string_view a = GetLengthPrefixedSlice(aptr);
  std::string_view b = GetLengthPrefixedSlice(bptr);
  return comparator.Compare(a, b);
}

static const char* EncodeKey(std::string* scratch, std::string_view target) {
  scratch->clear();
  PutVarint32(*scratch, target.size());
  scratch->append(target.data(), target.size());
  return scratch->data();
}

class MemTableIterator : public Iterator {
 public:
  explicit MemTableIterator(MemTable::Table* table) : iter_(table) {}

  MemTableIterator(const MemTableIterator&) = delete;
  MemTableIterator& operator=(const MemTableIterator&) = delete;

  ~MemTableIterator() override = default;

  [[nodiscard]] bool Valid() const noexcept override { return iter_.Valid(); }
  void Seek(std::string_view k) override { iter_.Seek(EncodeKey(&tmp_, k)); }
  void SeekToFirst() override { iter_.SeekToFirst(); }
  void SeekToLast() override { iter_.SeekToLast(); }
  void Next() override { iter_.Next(); }
  void Prev() override { iter_.Prev(); }
  [[nodiscard]] std::string_view key() const override { return GetLengthPrefixedSlice(iter_.key()); }
  [[nodiscard]] std::string_view value() const override {
    std::string_view key_slice = GetLengthPrefixedSlice(iter_.key());
    return GetLengthPrefixedSlice(key_slice.data() + key_slice.size());
  }

  [[nodiscard]] Result<void> status() const override { return {}; }

 private:
  MemTable::Table::Iterator iter_;
  std::string tmp_;  // For passing to EncodeKey
};

std::unique_ptr<Iterator> MemTable::NewIterator() { 
  return std::make_unique<MemTableIterator>(&table_); 
}

void MemTable::Add(SequenceNumber s, ValueType type, std::string_view key,
                   std::string_view value) {
  size_t key_size = key.size();
  size_t val_size = value.size();
  size_t internal_key_size = key_size + 8;
  const size_t encoded_len = VarintLength(internal_key_size) +
                             internal_key_size + VarintLength(val_size) +
                             val_size;
  char* buf = arena_.Allocate(encoded_len);
  char* p = EncodeVarint32(buf, internal_key_size);
  std::memcpy(p, key.data(), key_size);
  p += key_size;
  EncodeFixed64(p, (s << 8) | type);
  p += 8;
  p = EncodeVarint32(p, val_size);
  std::memcpy(p, value.data(), val_size);
  assert(p + val_size == buf + encoded_len);
  table_.Insert(buf);
}

Result<std::optional<std::string_view>> MemTable::Get(const LookupKey& key) {
  std::string_view memkey = key.memtable_key();
  Table::Iterator iter(&table_);
  iter.Seek(memkey.data());
  if (iter.Valid()) {
    const char* entry = iter.key();
    uint32_t key_length;
    const char* key_ptr = detail::GetVarint32Ptr(entry, entry + 5, key_length);
    if (comparator_.comparator.user_comparator()->Compare(
            std::string_view(key_ptr, key_length - 8), key.user_key()) == std::strong_ordering::equal) {
      const uint64_t tag = DecodeFixed64(key_ptr + key_length - 8);
      switch (static_cast<ValueType>(tag & 0xff)) {
        case kTypeValue: {
          std::string_view v = GetLengthPrefixedSlice(key_ptr + key_length);
          return std::make_optional(v);
        }
        case kTypeDeletion:
          return std::unexpected(Status::NotFoundErr("deleted in memtable"));
      }
    }
  }
  return std::nullopt; // Not found
}

}  // namespace leveldb
