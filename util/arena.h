//
// Modernized for C++23.
//
// Arena is a simple memory allocator that allocates from large blocks and
// never frees individual allocations. Memory is released when the Arena
// is destroyed. Used for MemTable, SkipList nodes, etc.
//
// C++23 changes:
//   - Old: std::vector<char*> + manual new[]/delete[]
//   - New: std::vector<std::unique_ptr<char[]>> — RAII automatic cleanup
//   - Destructor becomes = default (no manual delete loop)
//   - Block size constant is constexpr
//   - Added [[nodiscard]], noexcept where appropriate

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace leveldb {

class Arena {
 public:
  Arena();

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // unique_ptr handles cleanup — no manual delete loop needed.
  ~Arena() = default;

  // Return a pointer to a newly allocated memory block of "bytes" bytes.
  [[nodiscard]] char* Allocate(size_t bytes);

  // Allocate memory with the normal alignment guarantees provided by malloc.
  [[nodiscard]] char* AllocateAligned(size_t bytes);

  // Returns an estimate of the total memory usage of data allocated
  // by the arena.
  [[nodiscard]] size_t MemoryUsage() const noexcept {
    return memory_usage_.load(std::memory_order_relaxed);
  }

 private:
  static constexpr size_t kBlockSize = 4096;

  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t block_bytes);

  // Allocation state
  char* alloc_ptr_ = nullptr;
  size_t alloc_bytes_remaining_ = 0;

  // Owned memory blocks — unique_ptr ensures automatic cleanup.
  std::vector<std::unique_ptr<char[]>> blocks_;

  // Total memory usage of the arena.
  std::atomic<size_t> memory_usage_{0};
};

inline char* Arena::Allocate(size_t bytes) {
  // The semantics of what to return are a bit messy if we allow
  // 0-byte allocations, so we disallow them here (we don't need
  // them for our internal use).
  assert(bytes > 0);
  if (bytes <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  return AllocateFallback(bytes);
}

}  // namespace leveldb
