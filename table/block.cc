//
// Modernized for C++23.

#include "table/block.h"

#include <algorithm>
#include <cassert>
#include <cstdint>

#include "leveldb/comparator.h"
#include "util/coding.h"

namespace leveldb {

inline uint32_t Block::NumRestarts() const {
  assert(size_ >= sizeof(uint32_t));
  return DecodeFixed32(data_.data() + size_ - sizeof(uint32_t));
}

Block::Block(BlockContents contents)
    : data_(contents.data),
      heap_data_(std::move(contents.heap_data)),
      size_(contents.data.size()) {
  if (size_ < sizeof(uint32_t)) {
    size_ = 0;  // Error marker
  } else {
    size_t max_restarts_allowed = (size_ - sizeof(uint32_t)) / sizeof(uint32_t);
    if (NumRestarts() > max_restarts_allowed) {
      size_ = 0;
    } else {
      restart_offset_ = static_cast<uint32_t>(size_ - (1 + NumRestarts()) * sizeof(uint32_t));
    }
  }
}

static inline const char* DecodeEntry(const char* p, const char* limit,
                                      uint32_t* shared, uint32_t* non_shared,
                                      uint32_t* value_length) {
  if (limit - p < 3) return nullptr;
  *shared = reinterpret_cast<const uint8_t*>(p)[0];
  *non_shared = reinterpret_cast<const uint8_t*>(p)[1];
  *value_length = reinterpret_cast<const uint8_t*>(p)[2];
  if ((*shared | *non_shared | *value_length) < 128) {
    p += 3;
  } else {
    if ((p = detail::GetVarint32Ptr(p, limit, *shared)) == nullptr) return nullptr;
    if ((p = detail::GetVarint32Ptr(p, limit, *non_shared)) == nullptr) return nullptr;
    if ((p = detail::GetVarint32Ptr(p, limit, *value_length)) == nullptr) return nullptr;
  }

  if (static_cast<uint32_t>(limit - p) < (*non_shared + *value_length)) {
    return nullptr;
  }
  return p;
}

class Block::Iter final : public Iterator {
 private:
  const Comparator* const comparator_;
  const char* const data_;
  uint32_t const restarts_;
  uint32_t const num_restarts_;

  uint32_t current_;
  uint32_t restart_index_;
  std::string key_;
  std::string_view value_;
  Result<void> status_;

  inline int Compare(std::string_view a, std::string_view b) const {
    auto cmp = comparator_->Compare(a, b);
    if (cmp < 0) return -1;
    if (cmp > 0) return 1;
    return 0;
  }

  inline uint32_t NextEntryOffset() const {
    return static_cast<uint32_t>((value_.data() + value_.size()) - data_);
  }

  uint32_t GetRestartPoint(uint32_t index) {
    assert(index < num_restarts_);
    return DecodeFixed32(data_ + restarts_ + index * sizeof(uint32_t));
  }

  void SeekToRestartPoint(uint32_t index) {
    key_.clear();
    restart_index_ = index;
    uint32_t offset = GetRestartPoint(index);
    value_ = std::string_view(data_ + offset, 0);
  }

 public:
  Iter(const Comparator* comparator, const char* data, uint32_t restarts,
       uint32_t num_restarts)
      : comparator_(comparator),
        data_(data),
        restarts_(restarts),
        num_restarts_(num_restarts),
        current_(restarts_),
        restart_index_(num_restarts_) {
    assert(num_restarts_ > 0);
  }

  [[nodiscard]] bool Valid() const noexcept override { return current_ < restarts_; }
  [[nodiscard]] Result<void> status() const override { return status_; }
  [[nodiscard]] std::string_view key() const override {
    assert(Valid());
    return key_;
  }
  [[nodiscard]] std::string_view value() const override {
    assert(Valid());
    return value_;
  }

  void Next() override {
    assert(Valid());
    ParseNextKey();
  }

  void Prev() override {
    assert(Valid());
    const uint32_t original = current_;
    while (GetRestartPoint(restart_index_) >= original) {
      if (restart_index_ == 0) {
        current_ = restarts_;
        restart_index_ = num_restarts_;
        return;
      }
      restart_index_--;
    }

    SeekToRestartPoint(restart_index_);
    do {
    } while (ParseNextKey() && NextEntryOffset() < original);
  }

  void Seek(std::string_view target) override {
    uint32_t left = 0;
    uint32_t right = num_restarts_ - 1;
    int current_key_compare = 0;

    if (Valid()) {
      current_key_compare = Compare(key_, target);
      if (current_key_compare < 0) {
        left = restart_index_;
      } else if (current_key_compare > 0) {
        right = restart_index_;
      } else {
        return;
      }
    }

    while (left < right) {
      uint32_t mid = (left + right + 1) / 2;
      uint32_t region_offset = GetRestartPoint(mid);
      uint32_t shared, non_shared, value_length;
      const char* key_ptr =
          DecodeEntry(data_ + region_offset, data_ + restarts_, &shared,
                      &non_shared, &value_length);
      if (key_ptr == nullptr || (shared != 0)) {
        CorruptionError();
        return;
      }
      std::string_view mid_key(key_ptr, non_shared);
      if (Compare(mid_key, target) < 0) {
        left = mid;
      } else {
        right = mid - 1;
      }
    }

    assert(current_key_compare == 0 || Valid());
    bool skip_seek = left == restart_index_ && current_key_compare < 0;
    if (!skip_seek) {
      SeekToRestartPoint(left);
    }
    while (true) {
      if (!ParseNextKey()) {
        return;
      }
      if (Compare(key_, target) >= 0) {
        return;
      }
    }
  }

  void SeekToFirst() override {
    SeekToRestartPoint(0);
    ParseNextKey();
  }

  void SeekToLast() override {
    SeekToRestartPoint(num_restarts_ - 1);
    while (ParseNextKey() && NextEntryOffset() < restarts_) {
    }
  }

 private:
  void CorruptionError() {
    current_ = restarts_;
    restart_index_ = num_restarts_;
    status_ = Status::CorruptionErr("bad entry in block");
    key_.clear();
    value_ = {};
  }

  bool ParseNextKey() {
    current_ = NextEntryOffset();
    const char* p = data_ + current_;
    const char* limit = data_ + restarts_;
    if (p >= limit) {
      current_ = restarts_;
      restart_index_ = num_restarts_;
      return false;
    }

    uint32_t shared, non_shared, value_length;
    p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
    if (p == nullptr || key_.size() < shared) {
      CorruptionError();
      return false;
    } else {
      key_.resize(shared);
      key_.append(p, non_shared);
      value_ = std::string_view(p + non_shared, value_length);
      while (restart_index_ + 1 < num_restarts_ &&
             GetRestartPoint(restart_index_ + 1) < current_) {
        ++restart_index_;
      }
      return true;
    }
  }
};

std::unique_ptr<Iterator> Block::NewIterator(const Comparator* comparator) const {
  if (size_ < sizeof(uint32_t)) {
    return std::unique_ptr<Iterator>(NewErrorIterator(Status::CorruptionErr("bad block contents")));
  }
  const uint32_t num_restarts = NumRestarts();
  if (num_restarts == 0) {
    return std::unique_ptr<Iterator>(NewEmptyIterator());
  } else {
    return std::make_unique<Iter>(comparator, data_.data(), restart_offset_, num_restarts);
  }
}

}  // namespace leveldb
