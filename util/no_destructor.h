//
// Modernized for C++23.
//
// Wraps an instance whose destructor is never called.
// Intended for function-level static variables that must outlive all other
// objects (e.g., global comparator singletons).
//
// C++23 changes:
//   - Old: alignas(T) char[sizeof(T)] + placement new + manual static_asserts
//   - New: anonymous union member — compiler handles size and alignment
//   - Added [[nodiscard]], noexcept

#pragma once

#include <type_traits>
#include <utility>

namespace leveldb {

template <typename T>
class NoDestructor {
 public:
  template <typename... Args>
  explicit NoDestructor(Args&&... args) {
    new (&storage_) T(std::forward<Args>(args)...);
  }

  // Intentionally leaks — that is the entire purpose of this class.
  // Cannot use = default here because when T has a non-trivial destructor,
  // the union's destructor would be implicitly deleted.
  ~NoDestructor() {}

  NoDestructor(const NoDestructor&) = delete;
  NoDestructor& operator=(const NoDestructor&) = delete;

  [[nodiscard]] T* get() noexcept {
    return reinterpret_cast<T*>(&storage_);
  }

  [[nodiscard]] const T* get() const noexcept {
    return reinterpret_cast<const T*>(&storage_);
  }

 private:
  // Anonymous union: the compiler automatically ensures correct size and
  // alignment for T, eliminating the need for manual alignas + char[].
  union {
    T storage_;
  };
};

}  // namespace leveldb
