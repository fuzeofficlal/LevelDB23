//
// Modernized for C++23.
//
// A Cache is an interface that maps keys to values. It has internal
// synchronization and may be safely accessed concurrently from
// multiple threads.
//
// C++23 changes:
//   - Slice -> std::string_view
//   - Introduced CacheHandle (RAII wrapper) to replace manual Handle* + Release()
//   - Deleter is now std::function<void(std::string_view, void*)>
//   - Removed LEVELDB_EXPORT.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace leveldb {

class Cache;

// Create a new cache with a fixed size capacity. This implementation
// of Cache uses a least-recently-used eviction policy.
[[nodiscard]] Cache* NewLRUCache(size_t capacity);

class Cache {
 public:
  Cache() = default;

  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;

  // Destroys all existing entries by calling the "deleter"
  // function that was passed to the constructor.
  virtual ~Cache() = default;

  // Opaque handle to an entry stored in the cache.
  struct Handle {};

  // RAII handle that automatically releases the underlying Handle*
  // when it goes out of scope.
  class CacheHandle {
   public:
    CacheHandle() noexcept = default;
    CacheHandle(Cache* cache, Handle* handle) noexcept
        : cache_(cache), handle_(handle) {}

    // Move-only semantics
    CacheHandle(CacheHandle&& other) noexcept
        : cache_(other.cache_), handle_(other.handle_) {
      other.cache_ = nullptr;
      other.handle_ = nullptr;
    }

    CacheHandle& operator=(CacheHandle&& other) noexcept {
      if (this != &other) {
        Reset();
        cache_ = other.cache_;
        handle_ = other.handle_;
        other.cache_ = nullptr;
        other.handle_ = nullptr;
      }
      return *this;
    }

    CacheHandle(const CacheHandle&) = delete;
    CacheHandle& operator=(const CacheHandle&) = delete;

    ~CacheHandle() { Reset(); }

    [[nodiscard]] explicit operator bool() const noexcept {
      return handle_ != nullptr;
    }
    [[nodiscard]] Handle* get() const noexcept { return handle_; }

    void Reset() {
      if (cache_ && handle_) {
        // We rely on a virtual Release method implemented by Cache.
        // To avoid circular dependencies, the Cache interface requires
        // a public Release method that we call here.
        cache_->Release(handle_);
      }
      cache_ = nullptr;
      handle_ = nullptr;
    }

   private:
    Cache* cache_ = nullptr;
    Handle* handle_ = nullptr;
  };

  using DeleterType = std::function<void(std::string_view, void*)>;

  // Insert a mapping from key->value into the cache and assign it
  // the specified charge against the total cache capacity.
  //
  // Returns a CacheHandle that manages the mapping. When the handle
  // is destroyed, it releases the mapping. When all handles are released
  // and it is evicted, the key and value will be passed to "deleter".
  [[nodiscard]] virtual CacheHandle Insert(std::string_view key, void* value,
                                           size_t charge,
                                           DeleterType deleter) = 0;

  // If the cache has no mapping for "key", returns an empty handle.
  // Else return a handle that corresponds to the mapping.
  [[nodiscard]] virtual CacheHandle Lookup(std::string_view key) = 0;

  // Manual release, though CacheHandle should be preferred.
  virtual void Release(Handle* handle) = 0;

  // Return the value encapsulated in a handle returned by a
  // successful Lookup().
  // REQUIRES: handle must not be null.
  [[nodiscard]] virtual void* Value(Handle* handle) = 0;

  // If the cache contains entry for key, erase it. Note that the
  // underlying entry will be kept around until all existing handles
  // to it have been released.
  virtual void Erase(std::string_view key) = 0;

  // Return a new numeric id. May be used by multiple clients who are
  // sharing the same cache to partition the key space.
  virtual uint64_t NewId() = 0;

  // Remove all cache entries that are not actively in use.
  virtual void Prune() {}

  // Return an estimate of the combined charges of all elements stored in the
  // cache.
  [[nodiscard]] virtual size_t TotalCharge() const = 0;
};

}  // namespace leveldb
