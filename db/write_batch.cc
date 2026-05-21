//
// Modernized for C++23.

#include "leveldb/write_batch.h"
#include "db/write_batch_internal.h"
#include "db/memtable.h"
#include "util/coding.h"

namespace leveldb {

// WriteBatch header has an 8-byte sequence number followed by a 4-byte count.
static const size_t kHeader = 12;

WriteBatch::WriteBatch() { Clear(); }

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kHeader);
}

size_t WriteBatch::ApproximateSize() const { return rep_.size(); }

Result<void> WriteBatch::Iterate(Handler* handler) const {
  std::string_view input(rep_);
  if (input.size() < kHeader) {
    return std::unexpected(Status::CorruptionErr("malformed WriteBatch (too small)"));
  }

  input.remove_prefix(kHeader);
  std::string_view key, value;
  int found = 0;

  while (!input.empty()) {
    found++;
    char tag = input[0];
    input.remove_prefix(1);
    switch (tag) {
      case static_cast<char>(kTypeValue): {
        auto key_opt = GetLengthPrefixedSlice(input);
        if (!key_opt) {
          return std::unexpected(Status::CorruptionErr("bad WriteBatch Put"));
        }
        auto value_opt = GetLengthPrefixedSlice(input);
        if (!value_opt) {
          return std::unexpected(Status::CorruptionErr("bad WriteBatch Put"));
        }
        handler->Put(*key_opt, *value_opt);
        break;
      }
      case static_cast<char>(kTypeDeletion): {
        auto key_opt = GetLengthPrefixedSlice(input);
        if (!key_opt) {
          return std::unexpected(Status::CorruptionErr("bad WriteBatch Delete"));
        }
        handler->Delete(*key_opt);
        break;
      }
      default:
        return std::unexpected(Status::CorruptionErr("unknown WriteBatch tag"));
    }
  }

  if (found != WriteBatchInternal::Count(this)) {
    return std::unexpected(Status::CorruptionErr("WriteBatch has wrong count"));
  }

  return {};
}

int WriteBatchInternal::Count(const WriteBatch* b) {
  return DecodeFixed32(b->rep_.data() + 8);
}

void WriteBatchInternal::SetCount(WriteBatch* b, int n) {
  EncodeFixed32(&b->rep_[8], n);
}

SequenceNumber WriteBatchInternal::Sequence(const WriteBatch* b) {
  return SequenceNumber(DecodeFixed64(b->rep_.data()));
}

void WriteBatchInternal::SetSequence(WriteBatch* b, SequenceNumber seq) {
  EncodeFixed64(&b->rep_[0], seq);
}

void WriteBatch::Put(std::string_view key, std::string_view value) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixedSlice(rep_, key);
  PutLengthPrefixedSlice(rep_, value);
}

void WriteBatch::Delete(std::string_view key) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixedSlice(rep_, key);
}

void WriteBatch::Append(const WriteBatch& source) {
  WriteBatchInternal::Append(this, &source);
}

namespace {
class MemTableInserter : public WriteBatch::Handler {
 public:
  SequenceNumber sequence_;
  MemTable* mem_;

  void Put(std::string_view key, std::string_view value) override {
    mem_->Add(sequence_, kTypeValue, key, value);
    sequence_++;
  }
  void Delete(std::string_view key) override {
    mem_->Add(sequence_, kTypeDeletion, key, std::string_view());
    sequence_++;
  }
};
}  // namespace

Result<void> WriteBatchInternal::InsertInto(const WriteBatch* b, MemTable* memtable) {
  MemTableInserter inserter;
  inserter.sequence_ = WriteBatchInternal::Sequence(b);
  inserter.mem_ = memtable;
  return b->Iterate(&inserter);
}

void WriteBatchInternal::SetContents(WriteBatch* b, std::string_view contents) {
  assert(contents.size() >= kHeader);
  b->rep_.assign(contents.data(), contents.size());
}

void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch* src) {
  SetCount(dst, Count(dst) + Count(src));
  assert(src->rep_.size() >= kHeader);
  dst->rep_.append(src->rep_.data() + kHeader, src->rep_.size() - kHeader);
}

}  // namespace leveldb
