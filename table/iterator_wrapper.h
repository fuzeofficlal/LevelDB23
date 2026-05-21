//
// Modernized for C++23.

#pragma once

#include <cassert>
#include <memory>
#include <string_view>

#include "leveldb/iterator.h"

namespace leveldb {

class IteratorWrapper {
 public:
  IteratorWrapper() : valid_(false) {}
  explicit IteratorWrapper(std::unique_ptr<Iterator> iter) { Set(std::move(iter)); }

  Iterator* iter() const { return iter_.get(); }

  void Set(std::unique_ptr<Iterator> iter) {
    iter_ = std::move(iter);
    if (iter_ == nullptr) {
      valid_ = false;
    } else {
      Update();
    }
  }

  [[nodiscard]] bool Valid() const { return valid_; }
  [[nodiscard]] std::string_view key() const {
    assert(Valid());
    return key_;
  }
  [[nodiscard]] std::string_view value() const {
    assert(Valid());
    return iter_->value();
  }
  [[nodiscard]] Result<void> status() const {
    assert(iter_);
    return iter_->status();
  }
  void Next() {
    assert(iter_);
    iter_->Next();
    Update();
  }
  void Prev() {
    assert(iter_);
    iter_->Prev();
    Update();
  }
  void Seek(std::string_view k) {
    assert(iter_);
    iter_->Seek(k);
    Update();
  }
  void SeekToFirst() {
    assert(iter_);
    iter_->SeekToFirst();
    Update();
  }
  void SeekToLast() {
    assert(iter_);
    iter_->SeekToLast();
    Update();
  }

 private:
  void Update() {
    valid_ = iter_->Valid();
    if (valid_) {
      key_ = iter_->key();
    }
  }

  std::unique_ptr<Iterator> iter_;
  bool valid_;
  std::string_view key_;
};

}  // namespace leveldb
