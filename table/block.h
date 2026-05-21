//
// Modernized for C++23.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "leveldb/iterator.h"
#include "table/format.h"

namespace leveldb {

class Comparator;

class Block {
 public:
  // Initialize the block with the specified contents.
  // The block takes ownership of the memory via contents.heap_data.
  explicit Block(BlockContents contents);

  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  ~Block() = default;

  size_t size() const { return size_; }
  
  // Creates an iterator over the block contents.
  std::unique_ptr<Iterator> NewIterator(const Comparator* comparator) const;

 private:
  class Iter;

  uint32_t NumRestarts() const;

  std::string_view data_;
  std::shared_ptr<char[]> heap_data_;
  size_t size_;
  uint32_t restart_offset_;  // Offset in data_ of restart array
};

}  // namespace leveldb
