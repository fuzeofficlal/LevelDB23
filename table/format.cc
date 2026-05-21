//
// Modernized for C++23.

#include "table/format.h"

#include <cassert>

#include "util/coding.h"

namespace leveldb {

void BlockHandle::EncodeTo(std::string& dst) const {
  // Sanity check that all fields have been set
  assert(offset_ != ~static_cast<uint64_t>(0));
  assert(size_ != ~static_cast<uint64_t>(0));
  PutVarint64(dst, offset_);
  PutVarint64(dst, size_);
}

Result<void> BlockHandle::DecodeFrom(std::string_view& input) {
  auto o = GetVarint64(input);
  if (!o) return Status::CorruptionErr("bad block handle (offset)");
  offset_ = *o;

  auto s = GetVarint64(input);
  if (!s) return Status::CorruptionErr("bad block handle (size)");
  size_ = *s;

  return {};
}

void Footer::EncodeTo(std::string& dst) const {
  const size_t original_size = dst.size();
  metaindex_handle_.EncodeTo(dst);
  index_handle_.EncodeTo(dst);
  dst.resize(original_size + 2 * BlockHandle::kMaxEncodedLength);  // Padding
  PutFixed32(dst, static_cast<uint32_t>(kTableMagicNumber & 0xffffffffu));
  PutFixed32(dst, static_cast<uint32_t>(kTableMagicNumber >> 32));
  assert(dst.size() == original_size + kEncodedLength);
}

Result<void> Footer::DecodeFrom(std::string_view& input) {
  if (input.size() < kEncodedLength) {
    return Status::CorruptionErr("not an sstable (footer too short)");
  }

  const char* magic_ptr = input.data() + kEncodedLength - 8;
  const uint32_t magic_lo = DecodeFixed32(magic_ptr);
  const uint32_t magic_hi = DecodeFixed32(magic_ptr + 4);
  const uint64_t magic = ((static_cast<uint64_t>(magic_hi) << 32) |
                          (static_cast<uint64_t>(magic_lo)));
  if (magic != kTableMagicNumber) {
    return Status::CorruptionErr("not an sstable (bad magic number)");
  }

  if (auto res = metaindex_handle_.DecodeFrom(input); !res) {
    return res;
  }
  if (auto res = index_handle_.DecodeFrom(input); !res) {
    return res;
  }

  // We skip over any leftover data (just padding for now) in "input"
  const char* end = magic_ptr + 8;
  input = std::string_view(end, (input.data() + input.size()) - end);

  return {};
}

}  // namespace leveldb
