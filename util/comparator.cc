#include "leveldb/comparator.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>

#include "util/logging.h"
#include "util/no_destructor.h"

namespace leveldb {

namespace {

class BytewiseComparatorImpl : public Comparator {
 public:
  BytewiseComparatorImpl() = default;

  [[nodiscard]] const char* Name() const noexcept override {
    return "leveldb.BytewiseComparator";
  }

  [[nodiscard]] std::strong_ordering Compare(
      std::string_view a, std::string_view b) const noexcept override {
    int r = a.compare(b);
    if (r < 0) return std::strong_ordering::less;
    if (r > 0) return std::strong_ordering::greater;
    return std::strong_ordering::equal;
  }

  void FindShortestSeparator(std::string& start,
                             std::string_view limit) const override {
    // Find length of common prefix
    size_t min_length = std::min(start.size(), limit.size());
    size_t diff_index = 0;
    while ((diff_index < min_length) &&
           (start[diff_index] == limit[diff_index])) {
      diff_index++;
    }

    if (diff_index >= min_length) {
      // Do not shorten if one string is a prefix of the other
    } else {
      auto diff_byte = static_cast<uint8_t>(start[diff_index]);
      if (diff_byte < static_cast<uint8_t>(0xff) &&
          diff_byte + 1 < static_cast<uint8_t>(limit[diff_index])) {
        start[diff_index]++;
        start.resize(diff_index + 1);
        assert(Compare(start, limit) == std::strong_ordering::less);
      }
    }
  }

  void FindShortSuccessor(std::string& key) const override {
    // Find first character that can be incremented
    size_t n = key.size();
    for (size_t i = 0; i < n; i++) {
      const auto byte = static_cast<uint8_t>(key[i]);
      if (byte != static_cast<uint8_t>(0xff)) {
        key[i] = static_cast<char>(byte + 1);
        key.resize(i + 1);
        return;
      }
    }
    // key is a run of 0xffs. Leave it alone.
  }
};

}  // namespace

const Comparator* BytewiseComparator() noexcept {
  static NoDestructor<BytewiseComparatorImpl> singleton;
  return singleton.get();
}

}  // namespace leveldb
