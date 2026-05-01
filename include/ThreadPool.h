#ifndef THREAD_POOL_HPP
#define THREAD_POOL_HPP

#include <concepts>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

class ThreadPool {
public:
  // Automatically sized based on the number of hardware logical processors
  explicit ThreadPool(size_t threads = std::thread::hardware_concurrency());
  ~ThreadPool();

  // Prevent accidental copying or moving of the pool
  ThreadPool(const ThreadPool &) = delete;
  ThreadPool &operator=(const ThreadPool &) = delete;

  // C++20 Concepts: F and Args parameters must be invocable
  template <typename F, typename... Args>
    requires std::invocable<std::decay_t<F>, std::decay_t<Args>...>
  auto enqueue(F &&f, Args &&...args) -> std::future<
      std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>;

private:
  std::queue<std::move_only_function<void()>> tasks;
  std::mutex queue_mutex;
  std::condition_variable condition;

  // workers must be declared last so it is destroyed first, joining threads
  // safely.
  std::vector<std::jthread> workers;
};

// --- IMPLEMENTATION ---

inline ThreadPool::ThreadPool(size_t threads) {
  if (threads == 0)
    threads = 1; // Ensure at least 1 thread

  for (size_t i = 0; i < threads; ++i) {
    workers.emplace_back([this](std::stop_token stoken) {
      while (true) {
        // Move-only task wrapper to be pulled from the queue
        std::move_only_function<void()> task;
        {
          std::unique_lock<std::mutex> lock(this->queue_mutex);

          this->condition.wait(lock, [this, &stoken] {
            return stoken.stop_requested() || !this->tasks.empty();
          });

          if (stoken.stop_requested() && this->tasks.empty()) {
            return;
          }

          task = std::move(this->tasks.front());
          this->tasks.pop();
        }

        try {
          task(); // Execute the task
        } catch (const std::exception &e) {
          std::cerr << "[ThreadPool Worker Exception] " << e.what() << '\n';
        } catch (...) {
          std::cerr << "[ThreadPool Worker Exception] Unknown Error!\n";
        }
      }
    });
  }
}

template <typename F, typename... Args>
  requires std::invocable<std::decay_t<F>, std::decay_t<Args>...>
auto ThreadPool::enqueue(F &&f, Args &&...args) -> std::future<
    std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
  using return_type =
      std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

  // C++20 Lambda Pack Init-Capture
  auto task = std::packaged_task<return_type()>(
      [func = std::forward<F>(f),
       ... args = std::forward<Args>(args)]() mutable {
        return std::invoke(std::move(func), std::move(args)...);
      });

  std::future<return_type> res = task.get_future();

  {
    std::unique_lock<std::mutex> lock(queue_mutex);

    // std::move_only_function<void()> ignores the return value of the assigned
    // function. Therefore, even if task() returns type R, it can be pushed
    // directly.
    tasks.push(std::move(task));
  }
  condition.notify_one();

  return res;
}

inline ThreadPool::~ThreadPool() {
  // Send stop request to all workers and wake up those in sleep state.
  // jthread objects automatically call std::thread::join() when the vector is
  // destroyed.
  for (auto &worker : workers) {
    worker.request_stop();
  }
  condition.notify_all();
}

#endif // THREAD_POOL_HPP