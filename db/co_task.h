#pragma once

#include <coroutine>
#include <deque>
#include <mutex>
#include <expected>
#include <utility>
#include <exception>
#include <cassert>
#include <semaphore>

#include "leveldb/status.h"
#include "leveldb/write_batch.h"

namespace leveldb {

struct CoroutineWriter;

template <typename T>
class [[nodiscard]] Task {
 public:
  struct promise_type;
  using handle_type = std::coroutine_handle<promise_type>;
  
  struct promise_type {
    T result;
    std::coroutine_handle<> continuation = nullptr;

    Task get_return_object() {
      return Task(handle_type::from_promise(*this));
    }

    std::suspend_always initial_suspend() noexcept { return {}; }

    struct final_awaiter {
      bool await_ready() noexcept { return false; }
      std::coroutine_handle<> await_suspend(handle_type h) noexcept {
        if (h.promise().continuation) {
          return h.promise().continuation;
        }
        return std::noop_coroutine();
      }
      void await_resume() noexcept {}
    };

    final_awaiter final_suspend() noexcept { return {}; }

    template <typename U>
    void return_value(U&& val) {
      result = std::forward<U>(val);
    }

    void unhandled_exception() {
      std::terminate();
    }
  };

  explicit Task(handle_type h) : handle_(h) {}
  Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      if (handle_) handle_.destroy();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }
  ~Task() {
    if (handle_) handle_.destroy();
  }

  bool await_ready() const noexcept {
    return !handle_ || handle_.done();
  }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting_handle) noexcept {
    handle_.promise().continuation = awaiting_handle;
    return handle_;
  }

  T await_resume() {
    return std::move(handle_.promise().result);
  }

  void resume() {
    if (handle_ && !handle_.done()) {
      handle_.resume();
    }
  }

  bool done() const { return !handle_ || handle_.done(); }

 private:
  handle_type handle_;
};

template <>
class [[nodiscard]] Task<void> {
 public:
  struct promise_type;
  using handle_type = std::coroutine_handle<promise_type>;

  struct promise_type {
    Result<void> result;
    std::coroutine_handle<> continuation = nullptr;

    Task get_return_object() {
      return Task(handle_type::from_promise(*this));
    }

    std::suspend_always initial_suspend() noexcept { return {}; }

    struct final_awaiter {
      bool await_ready() noexcept { return false; }
      std::coroutine_handle<> await_suspend(handle_type h) noexcept {
        if (h.promise().continuation) {
          return h.promise().continuation;
        }
        return std::noop_coroutine();
      }
      void await_resume() noexcept {}
    };

    final_awaiter final_suspend() noexcept { return {}; }

    void return_void() {
      result = Result<void>();
    }

    void unhandled_exception() {
      try {
        std::rethrow_exception(std::current_exception());
      } catch (const std::exception& e) {
        result = std::unexpected(Status::Corruption(e.what()));
      } catch (...) {
        result = std::unexpected(Status::Corruption("Unknown exception in coroutine"));
      }
    }
  };

  explicit Task(handle_type h) : handle_(h) {}
  Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      if (handle_) handle_.destroy();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }
  ~Task() {
    if (handle_) handle_.destroy();
  }

  bool await_ready() const noexcept {
    return !handle_ || handle_.done();
  }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting_handle) noexcept {
    handle_.promise().continuation = awaiting_handle;
    return handle_;
  }

  Result<void> await_resume() {
    return std::move(handle_.promise().result);
  }

  void resume() {
    if (handle_ && !handle_.done()) {
      handle_.resume();
    }
  }

  bool done() const { return !handle_ || handle_.done(); }

 private:
  handle_type handle_;
};

struct CoroutineWriter {
  Status status;
  WriteBatch* batch;
  bool sync;
  bool done;
  std::coroutine_handle<> handle;
  std::binary_semaphore sem{0};
};

class CoWriteQueue {
 public:
  CoWriteQueue() = default;
  ~CoWriteQueue() = default;

  CoWriteQueue(const CoWriteQueue&) = delete;
  CoWriteQueue& operator=(const CoWriteQueue&) = delete;

  struct Awaiter {
    CoWriteQueue* queue;
    CoroutineWriter* writer;

    bool await_ready() noexcept {
      return false;
    }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
      std::lock_guard<std::mutex> lk(queue->mutex_);
      writer->handle = h;
      if (queue->writers_.empty()) {
        queue->writers_.push_back(writer);
        return false; // Resume immediately (no suspension)
      }
      queue->writers_.push_back(writer);
      return true; // Suspend
    }

    void await_resume() noexcept {}
  };

  Awaiter enqueue(CoroutineWriter* w) {
    return Awaiter{this, w};
  }

  std::mutex& mutex() { return mutex_; }
  std::deque<CoroutineWriter*>& writers() { return writers_; }

 private:
  std::mutex mutex_;
  std::deque<CoroutineWriter*> writers_;
};

} // namespace leveldb
