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

namespace leveldb {

class AsyncExecutor {
 public:
  explicit AsyncExecutor(size_t threads = 4, bool sharded = false)
      : sharded_(sharded), next_queue_(0), total_tasks_(0), shutdown_(false) {
    if (sharded_) {
      for (size_t i = 0; i < threads; ++i) {
        queues_.push_back(std::make_unique<ThreadQueue>());
      }
    }
    for (size_t i = 0; i < threads; ++i) {
      workers_.emplace_back([this, i]() {
        while (true) {
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
      });
    }
  }

  ~AsyncExecutor() {
    shutdown_.store(true, std::memory_order_relaxed);
    if (sharded_) {
      for (size_t i = 0; i < queues_.size(); ++i) {
        queues_[i]->cv.notify_all();
      }
    } else {
      cv_.notify_all();
    }
    for (std::thread& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  // Not copyable or movable
  AsyncExecutor(const AsyncExecutor&) = delete;
  AsyncExecutor& operator=(const AsyncExecutor&) = delete;

  void Schedule(std::move_only_function<void()> task) {
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

 private:
  struct ThreadQueue {
    std::mutex mutex;
    std::condition_variable cv;
    std::queue<std::move_only_function<void()>> tasks;
  };

  bool sharded_;
  std::atomic<size_t> next_queue_;
  std::atomic<size_t> total_tasks_;
  std::vector<std::unique_ptr<ThreadQueue>> queues_;
  std::atomic<bool> shutdown_;

  std::vector<std::thread> workers_;
  std::queue<std::move_only_function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable cv_;
};

} // namespace leveldb
