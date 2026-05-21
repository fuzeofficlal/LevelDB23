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

namespace leveldb {

class AsyncExecutor {
 public:
  explicit AsyncExecutor(size_t threads = 4) : shutdown_(false) {
    for (size_t i = 0; i < threads; ++i) {
      workers_.emplace_back([this]() {
        while (true) {
          std::move_only_function<void()> task;
          {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return shutdown_ || !tasks_.empty(); });
            if (shutdown_ && tasks_.empty()) {
              return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
          }
          task();
        }
      });
    }
  }

  ~AsyncExecutor() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      shutdown_ = true;
    }
    cv_.notify_all();
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
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push(std::move(task));
    }
    cv_.notify_one();
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
  std::vector<std::thread> workers_;
  std::queue<std::move_only_function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool shutdown_;
};

} // namespace leveldb
