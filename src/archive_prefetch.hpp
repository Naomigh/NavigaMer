#pragma once

#include "navigamer/types.hpp"

#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace navigamer::archive_detail {

// One queued ID batch plus the consumer's current batch. Only the producer
// calls read; destroying the stream discards read-ahead and joins the producer.
class MemberPrefetch {
 public:
  explicit MemberPrefetch(std::function<std::vector<SequenceId>()> read, bool enabled = true)
      : read_(std::move(read)) {
    if (enabled) worker_ = std::thread([this] { produce(); });
  }
  ~MemberPrefetch() {
    if (!worker_.joinable()) return;
    { std::lock_guard lock(mutex_); stop_ = true; }
    changed_.notify_all();
    worker_.join();
  }
  MemberPrefetch(const MemberPrefetch&) = delete;
  MemberPrefetch& operator=(const MemberPrefetch&) = delete;

  std::vector<SequenceId> next() {
    if (!worker_.joinable()) return read_();
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return ready_ || done_; });
    if (error_) std::rethrow_exception(error_);
    if (!ready_) return {};
    auto result = std::move(batch_);
    ready_ = false;
    lock.unlock();
    changed_.notify_all();
    return result;
  }

 private:
  void produce() noexcept {
    try {
      for (;;) {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return stop_ || !ready_; });
        if (stop_) return;
        lock.unlock();
        auto batch = read_();
        lock.lock();
        if (stop_) return;
        done_ = batch.empty();
        if (!done_) { batch_ = std::move(batch); ready_ = true; }
        lock.unlock();
        changed_.notify_all();
        if (done_) return;
      }
    } catch (...) {
      { std::lock_guard lock(mutex_); error_ = std::current_exception(); done_ = true; }
      changed_.notify_all();
    }
  }
  std::function<std::vector<SequenceId>()> read_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<SequenceId> batch_;
  std::exception_ptr error_;
  bool ready_{false}, done_{false}, stop_{false};
  std::thread worker_;
};

}  // namespace navigamer::archive_detail
