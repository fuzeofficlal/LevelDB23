//
// Modernized for C++23.

#include "leveldb/iterator.h"
#include <cassert>

namespace leveldb {

namespace {

class EmptyIterator : public Iterator {
 public:
  explicit EmptyIterator(Result<void> status) : status_(std::move(status)) {}
  ~EmptyIterator() override = default;
  
  [[nodiscard]] bool Valid() const noexcept override { return false; }
  void Seek(std::string_view target) override {}
  void SeekToFirst() override {}
  void SeekToLast() override {}
  void Next() override { assert(false); }
  void Prev() override { assert(false); }
  [[nodiscard]] std::string_view key() const override { assert(false); return {}; }
  [[nodiscard]] std::string_view value() const override { assert(false); return {}; }
  [[nodiscard]] Result<void> status() const override { return status_; }

 private:
  Result<void> status_;
};

}  // namespace

Iterator* NewEmptyIterator() {
  return new EmptyIterator({});
}

Iterator* NewErrorIterator(const Result<void>& status) {
  return new EmptyIterator(status);
}

}  // namespace leveldb
