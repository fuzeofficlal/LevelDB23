#include "db/coro_allocator.h"

namespace leveldb {

thread_local bool tls_coro_allocator_enabled = false;

struct FreeNode {
  FreeNode* next;
};

struct SizeClassPool {
  std::atomic<FreeNode*> shared_list{nullptr};
  FreeNode* private_list = nullptr;
};

struct CoroPool {
  std::atomic<bool> active{true};
  SizeClassPool classes[6];
};

class CoroAllocatorImpl {
 public:
  static inline std::mutex mutex_;
  static inline std::vector<CoroPool*> pools_;
  static inline thread_local CoroPool* my_pool_ = nullptr;

  static CoroPool* GetMyPool() {
    if (my_pool_ != nullptr) return my_pool_;
    std::lock_guard<std::mutex> lock(mutex_);
    for (CoroPool* p : pools_) {
      if (!p->active.load(std::memory_order_relaxed)) {
        p->active.store(true, std::memory_order_relaxed);
        my_pool_ = p;
        return p;
      }
    }
    CoroPool* p = new CoroPool();
    pools_.push_back(p);
    my_pool_ = p;
    return p;
  }

  static void ReleaseMyPool() {
    if (my_pool_ != nullptr) {
      my_pool_->active.store(false, std::memory_order_relaxed);
      my_pool_ = nullptr;
    }
  }
};

struct ThreadExitHelper {
  ~ThreadExitHelper() {
    CoroAllocatorImpl::ReleaseMyPool();
  }
};
inline thread_local ThreadExitHelper thread_exit_helper;

class CoroAllocatorCleanup {
 public:
  ~CoroAllocatorCleanup() {
    std::lock_guard<std::mutex> lock(CoroAllocatorImpl::mutex_);
    for (auto* p : CoroAllocatorImpl::pools_) {
      for (int sc = 0; sc < 6; ++sc) {
        FreeNode* n = p->classes[sc].private_list;
        while (n) {
          FreeNode* next = n->next;
          std::free(n);
          n = next;
        }
        FreeNode* s = p->classes[sc].shared_list.load();
        while (s) {
          FreeNode* next = s->next;
          std::free(s);
          s = next;
        }
      }
      delete p;
    }
    CoroAllocatorImpl::pools_.clear();
  }
};
inline CoroAllocatorCleanup coro_allocator_cleanup;

void* CoroAllocator::Allocate(size_t size) {
  size_t needed = size + sizeof(CoroAllocHeader);
  
  if (!tls_coro_allocator_enabled) {
    void* raw = std::malloc(needed);
    if (!raw) throw std::bad_alloc();
    CoroAllocHeader* header = reinterpret_cast<CoroAllocHeader*>(raw);
    header->magic = 0; // Not pool-allocated
    header->size_class = 0;
    header->pool_ptr = nullptr;
    return static_cast<char*>(raw) + sizeof(CoroAllocHeader);
  }

  int sc = -1;
  size_t class_size = 0;
  if (needed <= 128) { sc = 0; class_size = 128; }
  else if (needed <= 256) { sc = 1; class_size = 256; }
  else if (needed <= 512) { sc = 2; class_size = 512; }
  else if (needed <= 1024) { sc = 3; class_size = 1024; }
  else if (needed <= 2048) { sc = 4; class_size = 2048; }
  else if (needed <= 4096) { sc = 5; class_size = 4096; }

  if (sc == -1) {
    void* raw = std::malloc(needed);
    if (!raw) throw std::bad_alloc();
    CoroAllocHeader* header = reinterpret_cast<CoroAllocHeader*>(raw);
    header->magic = 0;
    header->size_class = 0;
    header->pool_ptr = nullptr;
    return static_cast<char*>(raw) + sizeof(CoroAllocHeader);
  }

  (void)thread_exit_helper; // Ensure helper instantiation
  CoroPool* pool = CoroAllocatorImpl::GetMyPool();
  auto& sc_pool = pool->classes[sc];

  FreeNode* node = sc_pool.private_list;
  if (node != nullptr) {
    sc_pool.private_list = node->next;
  } else {
    FreeNode* shared = sc_pool.shared_list.exchange(nullptr, std::memory_order_acquire);
    if (shared != nullptr) {
      sc_pool.private_list = shared->next;
      node = shared;
    } else {
      void* raw = std::malloc(class_size);
      if (!raw) throw std::bad_alloc();
      CoroAllocHeader* header = reinterpret_cast<CoroAllocHeader*>(raw);
      header->magic = kCoroAllocMagic;
      header->size_class = sc;
      header->pool_ptr = pool;
      return static_cast<char*>(raw) + sizeof(CoroAllocHeader);
    }
  }

  CoroAllocHeader* header = reinterpret_cast<CoroAllocHeader*>(node);
  header->magic = kCoroAllocMagic;
  header->size_class = sc;
  header->pool_ptr = pool;
  return reinterpret_cast<char*>(node) + sizeof(CoroAllocHeader);
}

void CoroAllocator::Deallocate(void* ptr, size_t size) noexcept {
  if (ptr == nullptr) return;
  CoroAllocHeader* header = reinterpret_cast<CoroAllocHeader*>(static_cast<char*>(ptr) - sizeof(CoroAllocHeader));
  if (header->magic != kCoroAllocMagic) {
    std::free(header);
    return;
  }

  auto* pool = reinterpret_cast<CoroPool*>(header->pool_ptr);
  int sc = header->size_class;
  FreeNode* node = reinterpret_cast<FreeNode*>(header);

  if (pool == CoroAllocatorImpl::my_pool_) {
    node->next = pool->classes[sc].private_list;
    pool->classes[sc].private_list = node;
  } else {
    FreeNode* old_head = pool->classes[sc].shared_list.load(std::memory_order_relaxed);
    do {
      node->next = old_head;
    } while (!pool->classes[sc].shared_list.compare_exchange_weak(
        old_head, node, std::memory_order_release, std::memory_order_relaxed));
  }
}

} // namespace leveldb
