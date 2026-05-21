//
// Modernized for C++23.

#pragma once

#include <string>
#include <string_view>

#include "leveldb/status.h"

namespace leveldb {

class WriteBatch {
 public:
  class Handler {
   public:
    virtual ~Handler() = default;
    virtual void Put(std::string_view key, std::string_view value) = 0;
    virtual void Delete(std::string_view key) = 0;
  };

  WriteBatch();
  ~WriteBatch() = default;

  // Move-only, or copyable? std::string is copyable.
  WriteBatch(const WriteBatch&) = default;
  WriteBatch& operator=(const WriteBatch&) = default;
  WriteBatch(WriteBatch&&) noexcept = default;
  WriteBatch& operator=(WriteBatch&&) noexcept = default;

  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);
  void Clear();
  size_t ApproximateSize() const;
  void Append(const WriteBatch& source);
  Result<void> Iterate(Handler* handler) const;

 private:
  friend class WriteBatchInternal;

  std::string rep_;
};

}  // namespace leveldb
