/*
 * @Author: DI JUNKUN
 * @Date: 2025-06-04
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _TASK_QUEUE_LOCK_FREE_H_
#define _TASK_QUEUE_LOCK_FREE_H_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "any_invocable.h"
#include "concurrentqueue.h"  // MoodyCamel's lock-free queue
#include "log.h"

namespace minirtc {

class TaskQueueLockFree {
 public:
  TaskQueueLockFree(std::string task_name, bool log_enabled = false,
                    size_t numThreads = 1)
      : task_name_(std::move(task_name)),
        log_enabled_(log_enabled),
        stop_flag_(false) {
    workers_.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i) {
      workers_.emplace_back([this]() { this->WorkerThread(); });
    }
  }

  ~TaskQueueLockFree() { Stop(); }

  void PostTask(AnyInvocable<void()> task) {
    task_queue_.enqueue(std::move(task));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cond_var_.notify_one();
    }
  }

  void Stop() {
    bool expected = false;
    if (!stop_flag_.compare_exchange_strong(expected, true)) {
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      cond_var_.notify_all();
    }

    for (std::thread& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers_.clear();
  }

 private:
  void WorkerThread() {
    AnyInvocable<void()> task;
    while (true) {
      while (task_queue_.try_dequeue(task)) {
        if (!task) continue;

        if (log_enabled_) {
          auto start = std::chrono::steady_clock::now();
          task();
          auto end = std::chrono::steady_clock::now();
          auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
              end - start);
          LOG_INFO("[TaskQueue: {}] Task executed in {} ms", task_name_,
                   duration.count());
        } else {
          task();
        }
      }

      std::unique_lock<std::mutex> lock(mutex_);
      cond_var_.wait(lock, [this] {
        return stop_flag_.load(std::memory_order_relaxed) ||
               task_queue_.size_approx() > 0;
      });

      if (stop_flag_.load(std::memory_order_relaxed)) {
        break;
      }
    }
  }

 private:
  std::string task_name_;
  bool log_enabled_;

  std::atomic<bool> stop_flag_{false};
  moodycamel::ConcurrentQueue<AnyInvocable<void()>> task_queue_;
  std::vector<std::thread> workers_;

  std::mutex mutex_;
  std::condition_variable cond_var_;
};
}  // namespace minirtc

#endif