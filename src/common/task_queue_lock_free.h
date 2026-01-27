/*
 * @Author: DI JUNKUN
 * @Date: 2025-06-04
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _TASK_QUEUE_LOCK_FREE_H_
#define _TASK_QUEUE_LOCK_FREE_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
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
    if (!task) {
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_flag_.load(std::memory_order_relaxed)) {
        return;
      }

      task_queue_.enqueue(std::move(task));
      pending_tasks_.fetch_add(1, std::memory_order_release);
    }

    cond_var_.notify_one();
  }

  void Stop() {
    bool expected = false;
    if (!stop_flag_.compare_exchange_strong(expected, true)) {
      return;
    }

    cond_var_.notify_all();

    std::vector<std::thread> workers_to_join;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      workers_to_join.swap(workers_);
    }

    for (std::thread& worker : workers_to_join) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  int PendingTasks() const {
    return pending_tasks_.load(std::memory_order_acquire);
  }

 private:
  void WorkerThread() {
    AnyInvocable<void()> task;
    while (true) {
      while (task_queue_.try_dequeue(task)) {
        // A successful dequeue must always reduce the pending count.
        pending_tasks_.fetch_sub(1, std::memory_order_acq_rel);

        if (!task) {
          LOG_ERROR("[TaskQueue: {}] Dequeued empty task", task_name_);
          continue;
        }

        if (log_enabled_) {
          auto start = std::chrono::steady_clock::now();
          try {
            task();
          } catch (const std::exception& e) {
            LOG_ERROR("[TaskQueue: {}] Task threw exception: {}", task_name_,
                      e.what());
          } catch (...) {
            LOG_ERROR("[TaskQueue: {}] Task threw unknown exception",
                      task_name_);
          }
          auto end = std::chrono::steady_clock::now();
          auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
              end - start);
          LOG_INFO("[TaskQueue: {}] Task executed in {} ms", task_name_,
                   duration.count());
        } else {
          try {
            task();
          } catch (const std::exception& e) {
            LOG_ERROR("[TaskQueue: {}] Task threw exception: {}", task_name_,
                      e.what());
          } catch (...) {
            LOG_ERROR("[TaskQueue: {}] Task threw unknown exception",
                      task_name_);
          }
        }
      }

      std::unique_lock<std::mutex> lock(mutex_);
      cond_var_.wait(lock, [this] {
        return stop_flag_.load(std::memory_order_relaxed) ||
               pending_tasks_.load(std::memory_order_acquire) > 0;
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
  std::atomic<int> pending_tasks_{0};
  moodycamel::ConcurrentQueue<AnyInvocable<void()>> task_queue_;
  std::vector<std::thread> workers_;

  std::mutex mutex_;
  std::condition_variable cond_var_;
};
}  // namespace minirtc

#endif