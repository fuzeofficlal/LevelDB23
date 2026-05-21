//
// Modernized for C++23.

#include "table/two_level_iterator.h"

#include "table/block.h"
#include "table/format.h"
#include "table/iterator_wrapper.h"

namespace leveldb {

namespace {

class TwoLevelIterator : public Iterator {
 public:
  TwoLevelIterator(std::unique_ptr<Iterator> index_iter,
                   BlockFunction block_function, const ReadOptions& options);

  ~TwoLevelIterator() override = default;

  void Seek(std::string_view target) override;
  void SeekToFirst() override;
  void SeekToLast() override;
  void Next() override;
  void Prev() override;

  [[nodiscard]] bool Valid() const noexcept override {
    return data_iter_.Valid();
  }
  [[nodiscard]] std::string_view key() const override {
    assert(Valid());
    return data_iter_.key();
  }
  [[nodiscard]] std::string_view value() const override {
    assert(Valid());
    return data_iter_.value();
  }
  [[nodiscard]] Result<void> status() const override {
    if (!status_) return status_;
    if (!index_iter_.status()) return index_iter_.status();
    if (data_iter_.iter() != nullptr && !data_iter_.status()) {
      return data_iter_.status();
    }
    return {};
  }

 private:
  void SaveError(const Result<void>& s) {
    if (status_ && !s) status_ = s;
  }
  void SkipEmptyDataBlocksForward();
  void SkipEmptyDataBlocksBackward();
  void SetDataIterator(std::unique_ptr<Iterator> data_iter);
  void InitDataBlock();

  BlockFunction block_function_;
  const ReadOptions options_;
  Result<void> status_;
  IteratorWrapper index_iter_;
  IteratorWrapper data_iter_;
  std::string data_block_handle_;
};

TwoLevelIterator::TwoLevelIterator(std::unique_ptr<Iterator> index_iter,
                                   BlockFunction block_function,
                                   const ReadOptions& options)
    : block_function_(std::move(block_function)),
      options_(options),
      index_iter_(std::move(index_iter)) {}

void TwoLevelIterator::Seek(std::string_view target) {
  index_iter_.Seek(target);
  InitDataBlock();
  if (data_iter_.iter() != nullptr) data_iter_.Seek(target);
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::SeekToFirst() {
  index_iter_.SeekToFirst();
  InitDataBlock();
  if (data_iter_.iter() != nullptr) data_iter_.SeekToFirst();
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::SeekToLast() {
  index_iter_.SeekToLast();
  InitDataBlock();
  if (data_iter_.iter() != nullptr) data_iter_.SeekToLast();
  SkipEmptyDataBlocksBackward();
}

void TwoLevelIterator::Next() {
  assert(Valid());
  data_iter_.Next();
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::Prev() {
  assert(Valid());
  data_iter_.Prev();
  SkipEmptyDataBlocksBackward();
}

void TwoLevelIterator::SkipEmptyDataBlocksForward() {
  while (data_iter_.iter() == nullptr || !data_iter_.Valid()) {
    // Move to next block
    if (!index_iter_.Valid()) {
      SetDataIterator(nullptr);
      return;
    }
    index_iter_.Next();
    InitDataBlock();
    if (data_iter_.iter() != nullptr) data_iter_.SeekToFirst();
  }
}

void TwoLevelIterator::SkipEmptyDataBlocksBackward() {
  while (data_iter_.iter() == nullptr || !data_iter_.Valid()) {
    // Move to next block
    if (!index_iter_.Valid()) {
      SetDataIterator(nullptr);
      return;
    }
    index_iter_.Prev();
    InitDataBlock();
    if (data_iter_.iter() != nullptr) data_iter_.SeekToLast();
  }
}

void TwoLevelIterator::SetDataIterator(std::unique_ptr<Iterator> data_iter) {
  if (data_iter_.iter() != nullptr) SaveError(data_iter_.status());
  data_iter_.Set(std::move(data_iter));
}

void TwoLevelIterator::InitDataBlock() {
  if (!index_iter_.Valid()) {
    SetDataIterator(nullptr);
  } else {
    std::string_view handle = index_iter_.value();
    if (data_iter_.iter() != nullptr && handle == data_block_handle_) {
      // data_iter_ is already constructed with this iterator, so
      // no need to change anything
    } else {
      std::unique_ptr<Iterator> iter = block_function_(options_, handle);
      data_block_handle_.assign(handle.data(), handle.size());
      SetDataIterator(std::move(iter));
    }
  }
}

}  // namespace

std::unique_ptr<Iterator> NewTwoLevelIterator(
    std::unique_ptr<Iterator> index_iter,
    BlockFunction block_function,
    const ReadOptions& options) {
  return std::make_unique<TwoLevelIterator>(std::move(index_iter),
                                            std::move(block_function), options);
}

}  // namespace leveldb
