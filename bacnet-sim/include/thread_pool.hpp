// thread_pool.hpp - fixed-size worker pool (mutex + condition_variable work queue).
#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool {
 public:
  explicit ThreadPool(std::size_t workers) {
    if (workers == 0) workers = 1;
    for (std::size_t i = 0; i < workers; ++i) threads_.emplace_back([this] { run(); });
  }
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  ~ThreadPool() {
    {
      std::lock_guard<std::mutex> lk(m_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();  // drains already-queued jobs first
  }

  void submit(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> lk(m_);
      if (stop_) return;
      jobs_.push(std::move(job));
    }
    cv_.notify_one();
  }

 private:
  void run() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return stop_ || !jobs_.empty(); });
        if (jobs_.empty()) return;  // stop_ is set and queue drained
        job = std::move(jobs_.front());
        jobs_.pop();
      }
      job();
    }
  }

  std::mutex m_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> jobs_;
  std::vector<std::thread> threads_;
  bool stop_ = false;
};
