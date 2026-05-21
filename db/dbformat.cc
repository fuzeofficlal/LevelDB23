//
// Modernized for C++23.

#include "db/dbformat.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

namespace leveldb {

static uint64_t PackSequenceAndType(uint64_t seq, ValueType t) {
  assert(seq <= kMaxSequenceNumber);
  assert(t <= kValueTypeForSeek);
  return (seq << 8) | t;
}

void AppendInternalKey(std::string& result, const ParsedInternalKey& key) {
  result.append(key.user_key.data(), key.user_key.size());
  PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
}

std::string ParsedInternalKey::DebugString() const {
  std::ostringstream ss;
  ss << "'" << user_key << "' @ " << sequence << " : "
     << (static_cast<int>(type) == kTypeValue ? "=>" : "DEL");
  return ss.str();
}

std::string InternalKey::DebugString() const {
  auto parsed = ParseInternalKey(rep_);
  if (parsed) {
    return parsed->DebugString();
  }
  return "(bad)";
}

const char* InternalKeyComparator::Name() const noexcept {
  return "leveldb.InternalKeyComparator";
}

std::strong_ordering InternalKeyComparator::Compare(std::string_view akey, std::string_view bkey) const noexcept {
  std::strong_ordering r = user_comparator_->Compare(ExtractUserKey(akey), ExtractUserKey(bkey));
  if (r == std::strong_ordering::equal) {
    const uint64_t anum = DecodeFixed64(akey.data() + akey.size() - 8);
    const uint64_t bnum = DecodeFixed64(bkey.data() + bkey.size() - 8);
    if (anum > bnum) {
      r = std::strong_ordering::less;
    } else if (anum < bnum) {
      r = std::strong_ordering::greater;
    }
  }
  return r;
}

void InternalKeyComparator::FindShortestSeparator(std::string& start, std::string_view limit) const {
  // Attempt to shorten the user portion of the key
  std::string_view user_start = ExtractUserKey(start);
  std::string_view user_limit = ExtractUserKey(limit);
  std::string tmp(user_start);
  user_comparator_->FindShortestSeparator(tmp, user_limit);
  if (tmp.size() < user_start.size() &&
      user_comparator_->Compare(user_start, tmp) < 0) {
    // User key has become shorter physically, but larger logically.
    // Tack on the earliest possible number to the shortened user key.
    PutFixed64(tmp, PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    assert(this->Compare(start, tmp) < 0);
    assert(this->Compare(tmp, limit) < 0);
    start.swap(tmp);
  }
}

void InternalKeyComparator::FindShortSuccessor(std::string& key) const {
  std::string_view user_key = ExtractUserKey(key);
  std::string tmp(user_key);
  user_comparator_->FindShortSuccessor(tmp);
  if (tmp.size() < user_key.size() &&
      user_comparator_->Compare(user_key, tmp) < 0) {
    // User key has become shorter physically, but larger logically.
    // Tack on the earliest possible number to the shortened user key.
    PutFixed64(tmp, PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    assert(this->Compare(key, tmp) < 0);
    key.swap(tmp);
  }
}

const char* InternalFilterPolicy::Name() const noexcept { return user_policy_->Name(); }

void InternalFilterPolicy::CreateFilter(std::span<const std::string_view> keys, std::string& dst) const {
  std::vector<std::string_view> user_keys(keys.size());
  for (size_t i = 0; i < keys.size(); i++) {
    user_keys[i] = ExtractUserKey(keys[i]);
  }
  user_policy_->CreateFilter(user_keys, dst);
}

bool InternalFilterPolicy::KeyMayMatch(std::string_view key, std::string_view filter) const {
  return user_policy_->KeyMayMatch(ExtractUserKey(key), filter);
}

LookupKey::LookupKey(std::string_view user_key, SequenceNumber s) {
  size_t usize = user_key.size();
  size_t needed = usize + 13;  // A conservative estimate
  char* dst;
  if (needed <= sizeof(space_)) {
    dst = space_;
  } else {
    dst = new char[needed];
  }
  start_ = dst;
  dst = EncodeVarint32(dst, usize + 8);
  kstart_ = dst;
  std::memcpy(dst, user_key.data(), usize);
  dst += usize;
  EncodeFixed64(dst, PackSequenceAndType(s, kValueTypeForSeek));
  dst += 8;
  end_ = dst;
}

}  // namespace leveldb
