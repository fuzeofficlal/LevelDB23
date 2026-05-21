//
// Modernized for C++23.
//
// Internal logging / string formatting utilities.
// Must not be included from any .h files to avoid polluting the namespace.
//
// C++23 changes:
//   - Slice → std::string_view
//   - std::string* output params → std::string& references
//   - ConsumeDecimalNumber: bool + output params → std::optional<uint64_t>
//   - snprintf → std::to_string
//   - Removed port/port.h dependency
//   - Added [[nodiscard]]

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace leveldb {

// Append a human-readable printout of "num" to dst.
void AppendNumberTo(std::string& dst, uint64_t num);

// Append a human-readable printout of "value" to dst.
// Escapes any non-printable characters found in "value".
void AppendEscapedStringTo(std::string& dst, std::string_view value);

// Return a human-readable printout of "num".
[[nodiscard]] std::string NumberToString(uint64_t num);

// Return a human-readable version of "value".
// Escapes any non-printable characters found in "value".
[[nodiscard]] std::string EscapeString(std::string_view value);

// Parse a human-readable decimal number from the front of input.
// On success: returns the parsed value and advances input past consumed digits.
// On failure: returns std::nullopt (no digits found, or overflow).
[[nodiscard]] std::optional<uint64_t>
ConsumeDecimalNumber(std::string_view& input) noexcept;

}  // namespace leveldb
