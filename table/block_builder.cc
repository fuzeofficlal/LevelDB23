//
// Modernized for C++23.

#include "table/block_builder.h"

#include <algorithm>
#include <cassert>

#include "leveldb/comparator.h"
#include "leveldb/options.h"
#include "util/coding.h"

namespace leveldb {

BlockBuilder::BlockBuilder(const BlockBuilderOptions& options)
    : options_(options), restarts_(), counter_(0), finished_(false) {
  assert(options_.block_restart_interval >= 1);
  restarts_.push_back(0);  // First restart point is at offset 0
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);  // First restart point is at offset 0
  counter_ = 0;
  finished_ = false;
  last_key_.clear();
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  return (buffer_.size() +                       // Raw data buffer
          restarts_.size() * sizeof(uint32_t) +  // Restart array
          sizeof(uint32_t));                     // Restart array length
}

std::string_view BlockBuilder::Finish() {
  // Append restart array
  for (size_t i = 0; i < restarts_.size(); i++) {
    PutFixed32(buffer_, restarts_[i]);
  }
  PutFixed32(buffer_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return std::string_view(buffer_);
}

void BlockBuilder::Add(std::string_view key, std::string_view value) {
  std::string_view last_key_piece(last_key_);
  assert(!finished_);
  assert(counter_ <= options_.block_restart_interval);
  assert(buffer_.empty()  // No values yet?
         || options_.comparator->Compare(key, last_key_piece) > 0);
  size_t shared = 0;
  if (counter_ < options_.block_restart_interval) {
    // See how much sharing to do with previous string
    const size_t min_length = std::min(last_key_piece.size(), key.size());
    while ((shared < min_length) && (last_key_piece[shared] == key[shared])) {
      shared++;
    }
  } else {
    // Restart compression
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }
  const size_t non_shared = key.size() - shared;

  // Add "<shared><non_shared><value_size>" to buffer_
  PutVarint32(buffer_, static_cast<uint32_t>(shared));
  PutVarint32(buffer_, static_cast<uint32_t>(non_shared));
  PutVarint32(buffer_, static_cast<uint32_t>(value.size()));

  // Add string delta to buffer_ followed by value
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());

  // Update state
  last_key_.resize(shared);
  last_key_.append(key.data() + shared, non_shared);
  assert(std::string_view(last_key_) == key);
  counter_++;
}

}  // namespace leveldb
