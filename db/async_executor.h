#pragma once

#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <type_traits>
#include <coroutine>
#include <utility>
#include <atomic>
#include <memory>
#include <optional>
#include <semaphore>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>

#ifdef __linux__
#include <liburing.h>
#endif

#include "leveldb/status.h"

namespace leveldb {

inline thread_local int tls_worker_id = -1;

class AsyncExecutor;

struct URingAwaiter {
  AsyncExecutor* exec;
  int fd;
  uint64_t offset;
  size_t n;
  char* scratch;

  int result_res = 0;
  std::coroutine_handle<> handle;
  std::atomic<bool> completed{false};

  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h);
  Result<std::string_view> await_resume();
  void Complete(int res);
};

struct TaskWrapper {
  std::move_only_function<void()> task;
};

template <typename T>
class ChaseLevDeque {
 private:
  struct Array {
    int64_t capacity;
    std::atomic<T>* buffer;

    Array(int64_t cap) : capacity(cap) {
      buffer = new std::atomic<T>[cap];
      for (int64_t i = 0; i < cap; ++i) {
        buffer[i].store(nullptr, std::memory_order_relaxed);
      }
    }

    ~Array() {
      delete[] buffer;
    }

    int64_t size() const { return capacity; }

    T read(int64_t i) {
      return buffer[i % capacity].load(std::memory_order_relaxed);
    }

    void write(int64_t i, T val) {
      buffer[i % capacity].store(val, std::memory_order_relaxed);
    }

    Array* grow(int64_t b, int64_t t) {
      Array* n = new Array(capacity * 2);
      for (int64_t i = t; i < b; ++i) {
        n->write(i, read(i));
      }
      return n;
    }
  };

  std::atomic<Array*> array_;
  std::atomic<int64_t> top_;
  std::atomic<int64_t> bottom_;
  std::vector<Array*> obsolete_arrays_;
  std::mutex array_mutex_;

 public:
  ChaseLevDeque(int64_t initial_capacity = 1024) {
    array_.store(new Array(initial_capacity), std::memory_order_relaxed);
    top_.store(0, std::memory_order_relaxed);
    bottom_.store(0, std::memory_order_relaxed);
  }

  ~ChaseLevDeque() {
    Array* arr = array_.load(std::memory_order_relaxed);
    if (arr) {
      int64_t b = bottom_.load(std::memory_order_relaxed);
      int64_t t = top_.load(std::memory_order_relaxed);
      for (int64_t i = t; i < b; ++i) {
        T task = arr->read(i);
        if (task) {
          delete task;
        }
      }
      delete arr;
    }
    for (Array* old_arr : obsolete_arrays_) {
      delete old_arr;
    }
  }

  void push(T val) {
    int64_t b = bottom_.load(std::memory_order_relaxed);
    int64_t t = top_.load(std::memory_order_acquire);
    Array* a = array_.load(std::memory_order_relaxed);
    int64_t size = b - t;
    if (size >= a->size() - 1) {
      std::lock_guard<std::mutex> lock(array_mutex_);
      t = top_.load(std::memory_order_relaxed);
      a = array_.load(std::memory_order_relaxed);
      Array* new_a = a->grow(b, t);
      obsolete_arrays_.push_back(a);
      array_.store(new_a, std::memory_order_release);
      a = new_a;
    }
    a->write(b, val);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    bottom_.store(b + 1, std::memory_order_relaxed);
  }

  T pop() {
    int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
    Array* a = array_.load(std::memory_order_relaxed);
    bottom_.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    int64_t t = top_.load(std::memory_order_relaxed);
    if (t <= b) {
      T val = a->read(b);
      if (t == b) {
        if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
          val = nullptr;
        }
        bottom_.store(b + 1, std::memory_order_relaxed);
      }
      return val;
    } else {
      bottom_.store(b + 1, std::memory_order_relaxed);
      return nullptr;
    }
  }

  T steal() {
    while (true) {
      int64_t t = top_.load(std::memory_order_acquire);
      std::atomic_thread_fence(std::memory_order_seq_cst);
      int64_t b = bottom_.load(std::memory_order_acquire);
      if (t < b) {
        std::lock_guard<std::mutex> lock(array_mutex_);
        Array* a = array_.load(std::memory_order_acquire);
        T val = a->read(t);
        if (top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
          return val;
        }
        continue;
      }
      return nullptr;
    }
  }
};

template <typename T>
class MPSCQueue {
 private:
  struct Node {
    std::atomic<Node*> next;
    T value;
    Node() : next(nullptr) {}
    Node(T val) : next(nullptr), value(val) {}
  };

  std::atomic<Node*> head_;
  Node* tail_;

 public:
  MPSCQueue() {
    Node* sentinel = new Node();
    head_.store(sentinel, std::memory_order_relaxed);
    tail_ = sentinel;
  }

  ~MPSCQueue() {
    T val;
    while (dequeue(val)) {
      if constexpr (std::is_pointer_v<T>) {
        delete val;
      }
    }
    delete tail_;
  }

  void enqueue(T val) {
    Node* node = new Node(val);
    Node* prev = head_.exchange(node, std::memory_order_acq_rel);
    prev->next.store(node, std::memory_order_release);
  }

  bool dequeue(T& val) {
    Node* tail = tail_;
    Node* next = tail->next.load(std::memory_order_acquire);
    if (next != nullptr) {
      tail_ = next;
      val = next->value;
      delete tail;
      return true;
    }
    return false;
  }

  bool empty() const {
    return head_.load(std::memory_order_relaxed) == tail_ && tail_->next.load(std::memory_order_relaxed) == nullptr;
  }
};

class AsyncExecutor {
 public:
  explicit AsyncExecutor(size_t threads = 4, bool sharded = false, bool lock_free_queue = false, bool io_uring_opt = false, bool coro_allocator = false)
      : sharded_(sharded), lock_free_queue_(lock_free_queue), next_queue_(0), total_tasks_(0), shutdown_(false), io_uring_enabled_(false) {
    
#ifdef __linux__
    if (io_uring_opt) {
      int ret = io_uring_queue_init(256, &ring_, 0);
      if (ret == 0) {
        io_uring_enabled_ = true;
        poller_ = std::thread([this]() { PollerLoop(); });
      }
    }
#endif

    if (lock_free_queue_) {
      for (size_t i = 0; i < threads; ++i) {
        local_queues_.push_back(std::make_unique<ChaseLevDeque<TaskWrapper*>>());
        external_queues_.push_back(std::make_unique<MPSCQueue<TaskWrapper*>>());
      }
    } else if (sharded_) {
      for (size_t i = 0; i < threads; ++i) {
        queues_.push_back(std::make_unique<ThreadQueue>());
      }
    }

    for (size_t i = 0; i < threads; ++i) {
      workers_.emplace_back([this, i, threads, coro_allocator]() {
        tls_worker_id = i;
        CoroAllocGuard guard(coro_allocator);
        while (true) {
          if (lock_free_queue_) {
            TaskWrapper* task_ptr = nullptr;
            task_ptr = local_queues_[i]->pop();
            if (!task_ptr) {
              external_queues_[i]->dequeue(task_ptr);
            }
            if (!task_ptr) {
              for (size_t offset = 1; offset < threads; ++offset) {
                size_t victim = (i + offset) % threads;
                task_ptr = local_queues_[victim]->steal();
                if (task_ptr) break;
              }
            }

            if (task_ptr) {
              task_ptr->task();
              delete task_ptr;
              total_tasks_.fetch_sub(1, std::memory_order_relaxed);
            } else {
              if (shutdown_.load(std::memory_order_relaxed) && total_tasks_.load(std::memory_order_relaxed) == 0) {
                return;
              }
              std::unique_lock<std::mutex> lk(sleep_mutex_);
              sleep_cv_.wait(lk, [this]() {
                return shutdown_.load(std::memory_order_relaxed) || total_tasks_.load(std::memory_order_relaxed) > 0;
              });
            }
          } else {
            // Lock-based implementation
            std::move_only_function<void()> task;
            if (sharded_) {
              {
                std::unique_lock<std::mutex> lock(queues_[i]->mutex);
                queues_[i]->cv.wait(lock, [this, i]() {
                  return shutdown_.load(std::memory_order_relaxed) ||
                         !queues_[i]->tasks.empty() ||
                         total_tasks_.load(std::memory_order_relaxed) > 0;
                });
                if (!queues_[i]->tasks.empty()) {
                  task = std::move(queues_[i]->tasks.front());
                  queues_[i]->tasks.pop();
                  total_tasks_.fetch_sub(1, std::memory_order_relaxed);
                }
              }

              if (!task && total_tasks_.load(std::memory_order_relaxed) > 0) {
                for (size_t offset = 1; offset < queues_.size(); ++offset) {
                  size_t victim = (i + offset) % queues_.size();
                  std::unique_lock<std::mutex> lock(queues_[victim]->mutex, std::try_to_lock);
                  if (lock.owns_lock() && !queues_[victim]->tasks.empty()) {
                    task = std::move(queues_[victim]->tasks.front());
                    queues_[victim]->tasks.pop();
                    total_tasks_.fetch_sub(1, std::memory_order_relaxed);
                    break;
                  }
                }
              }

              if (!task && shutdown_.load(std::memory_order_relaxed)) {
                if (total_tasks_.load(std::memory_order_relaxed) == 0) {
                  return;
                }
              }
            } else {
              std::unique_lock<std::mutex> lock(mutex_);
              cv_.wait(lock, [this]() { return shutdown_.load(std::memory_order_relaxed) || !tasks_.empty(); });
              if (shutdown_.load(std::memory_order_relaxed) && tasks_.empty()) {
                return;
              }
              task = std::move(tasks_.front());
              tasks_.pop();
            }

            if (task) {
              task();
            } else {
              std::this_thread::yield();
            }
          }
        }
      });
    }
  }

  ~AsyncExecutor() {
    shutdown_.store(true, std::memory_order_relaxed);
    
    if (lock_free_queue_) {
      {
        std::lock_guard<std::mutex> lk(sleep_mutex_);
      }
      sleep_cv_.notify_all();
    } else if (sharded_) {
      for (size_t i = 0; i < queues_.size(); ++i) {
        queues_[i]->cv.notify_all();
      }
    } else {
      cv_.notify_all();
    }

#ifdef __linux__
    if (io_uring_enabled_) {
      WakePoller();
      if (poller_.joinable()) {
        poller_.join();
      }
      io_uring_queue_exit(&ring_);
    }
#endif

    for (std::thread& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  // Not copyable or movable
  AsyncExecutor(const AsyncExecutor&) = delete;
  AsyncExecutor& operator=(const AsyncExecutor&) = delete;

  bool is_io_uring_enabled() const { return io_uring_enabled_; }

  void Schedule(std::move_only_function<void()> task) {
    if (lock_free_queue_) {
      int id = tls_worker_id;
      TaskWrapper* task_ptr = new TaskWrapper{std::move(task)};
      total_tasks_.fetch_add(1, std::memory_order_relaxed);
      if (id >= 0 && static_cast<size_t>(id) < local_queues_.size()) {
        local_queues_[id]->push(task_ptr);
      } else {
        size_t index = next_queue_.fetch_add(1, std::memory_order_relaxed) % external_queues_.size();
        external_queues_[index]->enqueue(task_ptr);
      }
      {
        std::lock_guard<std::mutex> lk(sleep_mutex_);
      }
      sleep_cv_.notify_all();
    } else {
      if (sharded_) {
        size_t index = next_queue_.fetch_add(1, std::memory_order_relaxed) % queues_.size();
        total_tasks_.fetch_add(1, std::memory_order_relaxed);
        {
          std::lock_guard<std::mutex> lock(queues_[index]->mutex);
          queues_[index]->tasks.push(std::move(task));
        }
        queues_[index]->cv.notify_one();
        size_t next_idx = (index + 1) % queues_.size();
        queues_[next_idx]->cv.notify_one();
      } else {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          tasks_.push(std::move(task));
        }
        cv_.notify_one();
      }
    }
  }

  template <typename Func>
  auto submit(Func&& func) {
    using ReturnType = std::invoke_result_t<std::decay_t<Func>>;
    
    struct Awaiter {
      AsyncExecutor* exec;
      std::decay_t<Func> func;
      std::conditional_t<std::is_void_v<ReturnType>, bool, ReturnType> result;

      bool await_ready() const noexcept { return false; }
      
      void await_suspend(std::coroutine_handle<> h) {
        exec->Schedule([this, h]() mutable {
          if constexpr (std::is_void_v<ReturnType>) {
            func();
          } else {
            result = func();
          }
          h.resume();
        });
      }

      auto await_resume() {
        if constexpr (std::is_void_v<ReturnType>) {
          return;
        } else {
          return std::move(result);
        }
      }
    };
    
    return Awaiter{this, std::forward<Func>(func)};
  }

  auto ReadAsync(int fd, uint64_t offset, size_t n, char* scratch) {
    return URingAwaiter{this, fd, offset, n, scratch};
  }

  void SubmitRead(URingAwaiter* awaiter) {
#ifdef __linux__
    if (io_uring_enabled_) {
      std::lock_guard<std::mutex> lock(ring_mutex_);
      struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
      if (sqe) {
        io_uring_prep_read(sqe, awaiter->fd, awaiter->scratch, awaiter->n, awaiter->offset);
        io_uring_sqe_set_data(sqe, awaiter);
        io_uring_submit(&ring_);
        return;
      }
    }
#endif
    Schedule([awaiter]() {
      ssize_t r = ::pread(awaiter->fd, awaiter->scratch, awaiter->n, static_cast<off_t>(awaiter->offset));
      if (r < 0) {
        awaiter->Complete(-errno);
      } else {
        awaiter->Complete(static_cast<int>(r));
      }
    });
  }

 private:
  void PollerLoop() {
#ifdef __linux__
    struct io_uring_cqe* cqe;
    while (!shutdown_.load(std::memory_order_relaxed)) {
      int ret = io_uring_wait_cqe(&ring_, &cqe);
      if (ret < 0) {
        if (ret == -EINTR) continue;
        break;
      }
      if (cqe) {
        auto* awaiter = reinterpret_cast<URingAwaiter*>(io_uring_cqe_get_data(cqe));
        int res = cqe->res;
        io_uring_cqe_seen(&ring_, cqe);
        if (awaiter) {
          awaiter->Complete(res);
        }
      }
    }
#endif
  }

  void WakePoller() {
#ifdef __linux__
    std::lock_guard<std::mutex> lock(ring_mutex_);
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (sqe) {
      io_uring_prep_nop(sqe);
      io_uring_sqe_set_data(sqe, nullptr);
      io_uring_submit(&ring_);
    }
#endif
  }

  struct ThreadQueue {
    std::mutex mutex;
    std::condition_variable cv;
    std::queue<std::move_only_function<void()>> tasks;
  };

  bool sharded_;
  bool lock_free_queue_;
  std::atomic<size_t> next_queue_;
  std::atomic<size_t> total_tasks_;
  std::vector<std::unique_ptr<ThreadQueue>> queues_;
  std::atomic<bool> shutdown_;

  std::vector<std::thread> workers_;
  std::queue<std::move_only_function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable cv_;

  // Lock-free queues
  std::vector<std::unique_ptr<ChaseLevDeque<TaskWrapper*>>> local_queues_;
  std::vector<std::unique_ptr<MPSCQueue<TaskWrapper*>>> external_queues_;
  std::mutex sleep_mutex_;
  std::condition_variable sleep_cv_;

  // io_uring
  bool io_uring_enabled_;
#ifdef __linux__
  struct io_uring ring_;
#endif
  std::mutex ring_mutex_;
  std::thread poller_;
};

inline void URingAwaiter::await_suspend(std::coroutine_handle<> h) {
  handle = h;
  exec->SubmitRead(this);
}

inline Result<std::string_view> URingAwaiter::await_resume() {
  if (result_res < 0) {
    return std::unexpected(Status::IOError("io_uring read failed", strerror(-result_res)));
  }
  return std::string_view(scratch, static_cast<size_t>(result_res));
}

inline void URingAwaiter::Complete(int res) {
  result_res = res;
  completed.store(true, std::memory_order_release);
  exec->Schedule([h = handle]() {
    h.resume();
  });
}

} // namespace leveldb
