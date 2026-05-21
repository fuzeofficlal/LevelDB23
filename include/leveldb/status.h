//
// Modernized Status for C++23.
//
// Status encapsulates the result of an operation. It may indicate success,
// or it may indicate an error with an associated error message.
//
// Design philosophy (C++23 modernization):
//   - Old: raw `new[] char` buffer with manual memory management
//   - New: `std::unique_ptr<State>` for automatic, exception-safe ownership
//   - Old: `Slice` for message passing
//   - New: `std::string_view` (standard library replacement)
//   - Old: private enum `Code` hidden inside the class
//   - New: `enum class StatusCode : uint8_t` as a first-class public type
//   - New: `Result<T>` type alias using `std::expected<T, Status>` (C++23)
//     for composable error handling without exceptions
//
// Return convention:
//   - All fallible functions return `Result<T>`.
//   - Side-effect-only functions return `Result<void>` (not bare `Status`).
//   - `Status` itself is never used directly as a return type; it lives
//     solely as the error payload inside `Result<T>`.
//
// Zero-cost OK optimization is preserved: when status is OK, state_ is
// nullptr — no heap allocation occurs.
//
// Multiple threads can invoke const methods on a Status without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same Status must use
// external synchronization.

#pragma once

#include "./export.h"

#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace leveldb {

// ---------------------------------------------------------------------------
// StatusCode — strongly typed error code
// ---------------------------------------------------------------------------
// Pulled out of the old `Status::Code` private enum so that:
//   1. External code can pattern-match on error codes without friend access
//   2. It can be used as the error type in `std::expected`
//   3. `enum class` prevents implicit int conversions (type safety)
// ---------------------------------------------------------------------------
enum class StatusCode : uint8_t {
  kOk = 0,
  kNotFound = 1,
  kCorruption = 2,
  kNotSupported = 3,
  kInvalidArgument = 4,
  kIOError = 5,
};

// Convert a StatusCode to a human-readable string_view.
[[nodiscard]] constexpr std::string_view
StatusCodeToString(StatusCode code) noexcept {
  switch (code) {
  case StatusCode::kOk:
    return "OK";
  case StatusCode::kNotFound:
    return "NotFound";
  case StatusCode::kCorruption:
    return "Corruption";
  case StatusCode::kNotSupported:
    return "Not Supported";
  case StatusCode::kInvalidArgument:
    return "Invalid Argument";
  case StatusCode::kIOError:
    return "IO Error";
  }
  return "Unknown";
}

// ---------------------------------------------------------------------------
// Status — encapsulates the result of an operation
// ---------------------------------------------------------------------------
class LEVELDB_EXPORT Status {
public:
  // ---- Constructors / Rule-of-Five ----------------------------------------

  // Create a success status (zero-cost: no heap allocation).
  Status() noexcept = default;

  // Move operations: trivially cheap (pointer swap).
  Status(Status &&) noexcept = default;
  Status &operator=(Status &&) noexcept = default;

  // Copy: deep-copies the State if present.
  Status(const Status &rhs)
      : state_(rhs.state_ ? std::make_unique<State>(*rhs.state_) : nullptr) {}

  Status &operator=(const Status &rhs) {
    if (this != &rhs) {
      state_ = rhs.state_ ? std::make_unique<State>(*rhs.state_) : nullptr;
    }
    return *this;
  }

  // Destructor: unique_ptr handles cleanup automatically.
  ~Status() noexcept = default;

  // ---- Convenience factory methods ----------------------------------------

  // Create an error Status of the appropriate type.
  [[nodiscard]] static Status NotFound(std::string_view msg,
                                       std::string_view msg2 = {}) {
    return Status(StatusCode::kNotFound, msg, msg2);
  }

  [[nodiscard]] static Status Corruption(std::string_view msg,
                                         std::string_view msg2 = {}) {
    return Status(StatusCode::kCorruption, msg, msg2);
  }

  [[nodiscard]] static Status NotSupported(std::string_view msg,
                                           std::string_view msg2 = {}) {
    return Status(StatusCode::kNotSupported, msg, msg2);
  }

  [[nodiscard]] static Status InvalidArgument(std::string_view msg,
                                              std::string_view msg2 = {}) {
    return Status(StatusCode::kInvalidArgument, msg, msg2);
  }

  [[nodiscard]] static Status IOError(std::string_view msg,
                                      std::string_view msg2 = {}) {
    return Status(StatusCode::kIOError, msg, msg2);
  }

  // ---- Result<T> error constructors ----------------------------------------
  // Return std::unexpected<Status> directly for use in Result<T>-returning
  // functions. Eliminates the need to wrap factory calls in std::unexpected().
  //
  // Usage:
  //   Result<Block> ReadBlock(...) {
  //     if (error) return Status::NotFoundErr("missing block");
  //     return block;
  //   }

  [[nodiscard]] static std::unexpected<Status>
  NotFoundErr(std::string_view msg, std::string_view msg2 = {}) {
    return std::unexpected(NotFound(msg, msg2));
  }

  [[nodiscard]] static std::unexpected<Status>
  CorruptionErr(std::string_view msg, std::string_view msg2 = {}) {
    return std::unexpected(Corruption(msg, msg2));
  }

  [[nodiscard]] static std::unexpected<Status>
  NotSupportedErr(std::string_view msg, std::string_view msg2 = {}) {
    return std::unexpected(NotSupported(msg, msg2));
  }

  [[nodiscard]] static std::unexpected<Status>
  InvalidArgumentErr(std::string_view msg, std::string_view msg2 = {}) {
    return std::unexpected(InvalidArgument(msg, msg2));
  }

  [[nodiscard]] static std::unexpected<Status>
  IOErrorErr(std::string_view msg, std::string_view msg2 = {}) {
    return std::unexpected(IOError(msg, msg2));
  }

  // ---- Observers -----------------------------------------------------------

  // Returns true iff the status indicates success.
  [[nodiscard]] bool ok() const noexcept { return !state_; }

  // Returns the error code (kOk if success).
  [[nodiscard]] StatusCode code() const noexcept {
    return state_ ? state_->code : StatusCode::kOk;
  }

  // Returns the error message (empty if success).
  [[nodiscard]] std::string_view message() const noexcept {
    return state_ ? std::string_view(state_->msg) : std::string_view{};
  }

  // Returns true iff the status indicates a NotFound error.
  [[nodiscard]] bool IsNotFound() const noexcept {
    return code() == StatusCode::kNotFound;
  }

  // Returns true iff the status indicates a Corruption error.
  [[nodiscard]] bool IsCorruption() const noexcept {
    return code() == StatusCode::kCorruption;
  }

  // Returns true iff the status indicates an IOError.
  [[nodiscard]] bool IsIOError() const noexcept {
    return code() == StatusCode::kIOError;
  }

  // Returns true iff the status indicates a NotSupportedError.
  [[nodiscard]] bool IsNotSupportedError() const noexcept {
    return code() == StatusCode::kNotSupported;
  }

  // Returns true iff the status indicates an InvalidArgument.
  [[nodiscard]] bool IsInvalidArgument() const noexcept {
    return code() == StatusCode::kInvalidArgument;
  }

  // Return a string representation of this status suitable for printing.
  // Returns the string "OK" for success.
  [[nodiscard]] std::string ToString() const {
    if (ok())
      return "OK";
    std::string result{StatusCodeToString(code())};
    if (auto msg = message(); !msg.empty()) {
      result += ": ";
      result += msg;
    }
    return result;
  }

  // ---- Conversion helpers --------------------------------------------------

  // Allow contextual bool conversion: `if (status) { ... }` means success.
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

private:
  // Internal state — only allocated on error (OK == nullptr).
  struct State {
    StatusCode code;
    std::string msg;
  };

  std::unique_ptr<State> state_{nullptr};

  // Private constructor for error states.
  Status(StatusCode code, std::string_view msg, std::string_view msg2)
      : state_(std::make_unique<State>(
            code, msg2.empty() ? std::string(msg)
                               : std::format("{}: {}", msg, msg2))) {}
};

// ---------------------------------------------------------------------------
// Result<T> — the unified return type for all fallible operations
// ---------------------------------------------------------------------------
// Old pattern:
//   Status ReadBlock(const ReadOptions&, Block** result);  // output param
//   Status Flush();                                        // bare Status
//
// New pattern (C++23):
//   Result<Block> ReadBlock(const ReadOptions&);   // has a value
//   Result<void>  Flush();                         // side-effect only
//
// Usage:
//   auto result = ReadBlock(options);
//   if (result) {
//     DoSomething(*result);
//   } else {
//     HandleError(result.error());
//   }
//
// Or with monadic chaining (C++23 std::expected):
//   auto final = ReadBlock(options)
//       .and_then(DecodeBlock)
//       .transform(ExtractData)
//       .or_else(LogAndRecover);
//
// Side-effect chaining:
//   auto ok = Validate()
//       .and_then([]() { return Flush(); })
//       .and_then([]() { return Sync(); });
// ---------------------------------------------------------------------------
template <typename T> using Result = std::expected<T, Status>;

} // namespace leveldb
