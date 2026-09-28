#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace navigamer::detail {

// Reuse workers between streamed blocks. Exceptions never strand a barrier.
class WorkerPool {
 public:
  explicit WorkerPool(std::size_t count) : count_(count), errors_(count) {
    if (count == 0) throw std::invalid_argument("worker count must be positive");
    try {
      for (std::size_t id = 1; id < count; ++id) {
        workers_.emplace_back([this, id] {
          std::uint64_t seen = 0;
          for (;;) {
            std::unique_lock lock(mutex_);
            start_.wait(lock, [&] { return stop_ || generation_ != seen; });
            if (stop_) return;
            seen = generation_;
            lock.unlock();
            try { function_(id, count_); } catch (...) { errors_[id] = std::current_exception(); }
            lock.lock();
            if (++finished_ == count_ - 1) done_.notify_one();
          }
        });
      }
    } catch (...) { shutdown(); throw; }
  }
  ~WorkerPool() { shutdown(); }
  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;
  void run(std::function<void(std::size_t, std::size_t)> function) {
    {
      std::lock_guard lock(mutex_);
      function_ = std::move(function);
      std::fill(errors_.begin(), errors_.end(), nullptr);
      finished_ = 0;
      ++generation_;
    }
    start_.notify_all();
    try { function_(0, count_); } catch (...) { errors_[0] = std::current_exception(); }
    std::unique_lock lock(mutex_);
    done_.wait(lock, [&] { return finished_ == count_ - 1; });
    for (const auto& error : errors_) if (error) std::rethrow_exception(error);
  }
 private:
  void shutdown() {
    { std::lock_guard lock(mutex_); stop_ = true; }
    start_.notify_all();
    for (auto& thread : workers_) if (thread.joinable()) thread.join();
  }
  std::size_t count_, finished_{0};
  std::uint64_t generation_{0};
  bool stop_{false};
  std::mutex mutex_;
  std::condition_variable start_, done_;
  std::function<void(std::size_t, std::size_t)> function_;
  std::vector<std::exception_ptr> errors_;
  std::vector<std::thread> workers_;
};

}  // namespace navigamer::detail
