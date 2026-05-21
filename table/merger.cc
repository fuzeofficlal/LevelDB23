#include "table/merger.h"

#include "leveldb/comparator.h"
#include "leveldb/iterator.h"
#include "table/iterator_wrapper.h"

#include <vector>

namespace leveldb {

namespace {
class MergingIterator : public Iterator {
 public:
  MergingIterator(const Comparator* comparator, std::vector<std::unique_ptr<Iterator>> children)
      : comparator_(comparator),
        children_(children.size()),
        n_(children.size()),
        current_(nullptr),
        direction_(kForward) {
    for (size_t i = 0; i < n_; i++) {
      children_[i].Set(std::move(children[i]));
    }
  }

  ~MergingIterator() override = default;

  bool Valid() const noexcept override { return (current_ != nullptr); }

  void SeekToFirst() override {
    for (size_t i = 0; i < n_; i++) {
      children_[i].SeekToFirst();
    }
    FindSmallest();
    direction_ = kForward;
  }

  void SeekToLast() override {
    for (size_t i = 0; i < n_; i++) {
      children_[i].SeekToLast();
    }
    FindLargest();
    direction_ = kReverse;
  }

  void Seek(std::string_view target) override {
    for (size_t i = 0; i < n_; i++) {
      children_[i].Seek(target);
    }
    FindSmallest();
    direction_ = kForward;
  }

  void Next() override {
    assert(Valid());
    if (direction_ != kForward) {
      for (size_t i = 0; i < n_; i++) {
        IteratorWrapper* child = &children_[i];
        if (child != current_) {
          child->Seek(key());
          if (child->Valid() &&
              comparator_->Compare(key(), child->key()) == 0) {
            child->Next();
          }
        }
      }
      direction_ = kForward;
    }

    current_->Next();
    FindSmallest();
  }

  void Prev() override {
    assert(Valid());
    if (direction_ != kReverse) {
      for (size_t i = 0; i < n_; i++) {
        IteratorWrapper* child = &children_[i];
        if (child != current_) {
          child->Seek(key());
          if (child->Valid()) {
            child->Prev();
          } else {
            child->SeekToLast();
          }
        }
      }
      direction_ = kReverse;
    }

    current_->Prev();
    FindLargest();
  }

  std::string_view key() const override {
    assert(Valid());
    return current_->key();
  }

  std::string_view value() const override {
    assert(Valid());
    return current_->value();
  }

  Result<void> status() const override {
    Result<void> status;
    for (size_t i = 0; i < n_; i++) {
      status = children_[i].status();
      if (!status) {
        break;
      }
    }
    return status;
  }

 private:
  enum Direction { kForward, kReverse };

  void FindSmallest() {
    IteratorWrapper* smallest = nullptr;
    for (size_t i = 0; i < n_; i++) {
      IteratorWrapper* child = &children_[i];
      if (child->Valid()) {
        if (smallest == nullptr) {
          smallest = child;
        } else if (comparator_->Compare(child->key(), smallest->key()) < 0) {
          smallest = child;
        }
      }
    }
    current_ = smallest;
  }

  void FindLargest() {
    IteratorWrapper* largest = nullptr;
    // Walk backwards for stable merging
    for (int i = static_cast<int>(n_) - 1; i >= 0; i--) {
      IteratorWrapper* child = &children_[i];
      if (child->Valid()) {
        if (largest == nullptr) {
          largest = child;
        } else if (comparator_->Compare(child->key(), largest->key()) > 0) {
          largest = child;
        }
      }
    }
    current_ = largest;
  }

  const Comparator* comparator_;
  std::vector<IteratorWrapper> children_;
  size_t n_;
  IteratorWrapper* current_;
  Direction direction_;
};
}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(const Comparator* comparator,
                                             std::vector<std::unique_ptr<Iterator>> children) {
  if (children.empty()) {
    return std::unique_ptr<Iterator>(NewEmptyIterator());
  } else if (children.size() == 1) {
    return std::move(children[0]);
  } else {
    return std::make_unique<MergingIterator>(comparator, std::move(children));
  }
}

}  // namespace leveldb
