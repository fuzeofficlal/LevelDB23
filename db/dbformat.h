//
// Modernized for C++23.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "leveldb/comparator.h"
#include "leveldb/filter_policy.h"
#include "util/coding.h"

namespace leveldb {

namespace config {
static constexpr int kNumLevels = 7;
static constexpr int kL0_CompactionTrigger = 4;
static constexpr int kL0_SlowdownWritesTrigger = 8;
static constexpr int kL0_StopWritesTrigger = 12;
static constexpr int kMaxMemCompactLevel = 2;
static constexpr int kReadBytesPeriod = 1048576;
}  // namespace config

class InternalKey;

enum ValueType : uint8_t { kTypeDeletion = 0x0, kTypeValue = 0x1 };
static constexpr ValueType kValueTypeForSeek = kTypeValue;

using SequenceNumber = uint64_t;
static constexpr SequenceNumber kMaxSequenceNumber = ((0x1ull << 56) - 1);

struct ParsedInternalKey {
  std::string_view user_key;
  SequenceNumber sequence;
  ValueType type;

  ParsedInternalKey() = default;
  ParsedInternalKey(std::string_view u, SequenceNumber seq, ValueType t)
      : user_key(u), sequence(seq), type(t) {}
  std::string DebugString() const;
};

inline size_t InternalKeyEncodingLength(const ParsedInternalKey& key) {
  return key.user_key.size() + 8;
}

void AppendInternalKey(std::string& result, const ParsedInternalKey& key);

std::optional<ParsedInternalKey> ParseInternalKey(std::string_view internal_key);

inline std::string_view ExtractUserKey(std::string_view internal_key) {
  assert(internal_key.size() >= 8);
  return std::string_view(internal_key.data(), internal_key.size() - 8);
}

class InternalKeyComparator : public Comparator {
 public:
  explicit InternalKeyComparator(const Comparator* c) : user_comparator_(c) {}
  
  [[nodiscard]] const char* Name() const noexcept override;
  [[nodiscard]] std::strong_ordering Compare(std::string_view a, std::string_view b) const noexcept override;
  void FindShortestSeparator(std::string& start, std::string_view limit) const override;
  void FindShortSuccessor(std::string& key) const override;

  [[nodiscard]] const Comparator* user_comparator() const { return user_comparator_; }

  [[nodiscard]] std::strong_ordering Compare(const InternalKey& a, const InternalKey& b) const noexcept;

 private:
  const Comparator* user_comparator_;
};

class InternalFilterPolicy : public FilterPolicy {
 public:
  explicit InternalFilterPolicy(const FilterPolicy* p) : user_policy_(p) {}
  
  [[nodiscard]] const char* Name() const noexcept override;
  void CreateFilter(std::span<const std::string_view> keys, std::string& dst) const override;
  [[nodiscard]] bool KeyMayMatch(std::string_view key, std::string_view filter) const override;

 private:
  const FilterPolicy* const user_policy_;
};

class InternalKey {
 public:
  InternalKey() = default;
  InternalKey(std::string_view user_key, SequenceNumber s, ValueType t) {
    AppendInternalKey(rep_, ParsedInternalKey(user_key, s, t));
  }

  bool DecodeFrom(std::string_view s) {
    rep_.assign(s.data(), s.size());
    return !rep_.empty();
  }

  [[nodiscard]] std::string_view Encode() const {
    assert(!rep_.empty());
    return rep_;
  }

  [[nodiscard]] std::string_view user_key() const { return ExtractUserKey(rep_); }

  void SetFrom(const ParsedInternalKey& p) {
    rep_.clear();
    AppendInternalKey(rep_, p);
  }

  void Clear() { rep_.clear(); }

  std::string DebugString() const;

 private:
  std::string rep_;
};

inline std::strong_ordering InternalKeyComparator::Compare(const InternalKey& a, const InternalKey& b) const noexcept {
  return Compare(a.Encode(), b.Encode());
}

inline std::optional<ParsedInternalKey> ParseInternalKey(std::string_view internal_key) {
  const size_t n = internal_key.size();
  if (n < 8) return std::nullopt;
  uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
  uint8_t c = num & 0xff;
  if (c > static_cast<uint8_t>(kTypeValue)) return std::nullopt;
  return ParsedInternalKey(
      std::string_view(internal_key.data(), n - 8),
      num >> 8,
      static_cast<ValueType>(c));
}

class LookupKey {
 public:
  LookupKey(std::string_view user_key, SequenceNumber sequence);

  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;

  ~LookupKey();

  [[nodiscard]] std::string_view memtable_key() const {
    return std::string_view(start_, end_ - start_);
  }
  [[nodiscard]] std::string_view internal_key() const {
    return std::string_view(kstart_, end_ - kstart_);
  }
  [[nodiscard]] std::string_view user_key() const {
    return std::string_view(kstart_, end_ - kstart_ - 8);
  }

 private:
  const char* start_;
  const char* kstart_;
  const char* end_;
  char space_[200];
};

inline LookupKey::~LookupKey() {
  if (start_ != space_) delete[] start_;
}

}  // namespace leveldb
