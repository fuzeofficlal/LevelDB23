#include "util/logging.h"

#include <format>
#include <limits>
#include <string>

namespace leveldb {

void AppendNumberTo(std::string& dst, uint64_t num) {
  dst += std::to_string(num);
}

void AppendEscapedStringTo(std::string& dst, std::string_view value) {
  for (char c : value) {
    if (c >= ' ' && c <= '~') {
      dst.push_back(c);
    } else {
      dst += std::format("\\x{:02x}",
                         static_cast<unsigned int>(c) & 0xff);
    }
  }
}

std::string NumberToString(uint64_t num) {
  return std::to_string(num);
}

std::string EscapeString(std::string_view value) {
  std::string r;
  AppendEscapedStringTo(r, value);
  return r;
}

std::optional<uint64_t>
ConsumeDecimalNumber(std::string_view& input) noexcept {
  // Constants that will be optimized away.
  constexpr uint64_t kMaxUint64 = std::numeric_limits<uint64_t>::max();
  constexpr char kLastDigitOfMaxUint64 =
      '0' + static_cast<char>(kMaxUint64 % 10);

  uint64_t value = 0;
  size_t digits_consumed = 0;

  for (size_t i = 0; i < input.size(); ++i) {
    const auto ch = static_cast<uint8_t>(input[i]);
    if (ch < '0' || ch > '9') break;

    // Overflow check.
    if (value > kMaxUint64 / 10 ||
        (value == kMaxUint64 / 10 && ch > kLastDigitOfMaxUint64)) {
      return std::nullopt;
    }

    value = (value * 10) + (ch - '0');
    ++digits_consumed;
  }

  if (digits_consumed == 0) {
    return std::nullopt;
  }

  input.remove_prefix(digits_consumed);
  return value;
}

}  // namespace leveldb
