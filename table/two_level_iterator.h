//
// Modernized for C++23.

#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include "leveldb/iterator.h"
#include "leveldb/options.h"

namespace leveldb {

using BlockFunction = std::move_only_function<std::unique_ptr<Iterator>(
    const ReadOptions&, std::string_view)>;

// Return a new two level iterator.  A two-level iterator contains an
// index iterator whose values point to a sequence of blocks where
// each block is itself a sequence of key,value pairs.  The returned
// two-level iterator yields the concatenation of all key/value pairs
// in the sequence of blocks.  Takes ownership of "index_iter".
//
// Uses a supplied function to convert an index_iter value into
// an iterator over the contents of the corresponding block.
std::unique_ptr<Iterator> NewTwoLevelIterator(
    std::unique_ptr<Iterator> index_iter,
    BlockFunction block_function,
    const ReadOptions& options);

}  // namespace leveldb
