#pragma once

#include <atomic>
#include <mutex>
#include <vector>
#include <cstdlib>
#include <cstdint>
#include <new>

namespace leveldb {

struct CoroAllocHeader {
  uint32_t magic;
  uint32_t size_class;
  void* pool_ptr;
};

constexpr uint32_t kCoroAllocMagic = 0xDEADC0F0;

extern thread_local bool tls_coro_allocator_enabled;

struct CoroAllocGuard {
  bool prev;
  explicit CoroAllocGuard(bool enable) noexcept {
    prev = tls_coro_allocator_enabled;
    tls_coro_allocator_enabled = enable;
  }
  ~CoroAllocGuard() noexcept {
    tls_coro_allocator_enabled = prev;
  }
};

class CoroAllocator {
 public:
  static void* Allocate(size_t size);
  static void Deallocate(void* ptr, size_t size) noexcept;
};

} // namespace leveldb
