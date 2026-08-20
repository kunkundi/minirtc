#ifndef _TASK_QUEUE_H_
#define _TASK_QUEUE_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "any_invocable.h"
#include "log.h"

namespace minirtc {

class TaskQueue {
 public:
  TaskQueue(std::string task_name, bool log_enabled = false,
            size_t numThreads = 1)
      : task_name_(std::move(task_name)),
        log_enabled_(log_enabled),
        stop_(false) {
    for (size_t i = 0; i < numThreads; ++i) {
      workers_.emplace_back([this]() { this->WorkerThread(); });
    }
  }

  ~TaskQueue() { Stop(); }

  bool PostTask(AnyInvocable<void()> task) {
    return PostDelayedTask(std::move(task), 0);
  }

  bool IsCurrent() const { return current_queue_ == this; }

  bool PostDelayedTask(AnyInvocable<void()> task, int delay_ms) {
    auto delay = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::milliseconds(delay_ms));
    return PostDelayedTaskAt(std::move(task),
                             std::chrono::steady_clock::now() + delay);
  }

  bool PostDelayedHighPrecisionTask(AnyInvocable<void()> task,
                                    std::chrono::microseconds delay) {
    auto precise_delay =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay);
    return PostDelayedTaskAt(std::move(task),
                             std::chrono::steady_clock::now() + precise_delay);
  }

  void ClearTasks() {
    decltype(taskQueue_) cleared_tasks;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      taskQueue_.swap(cleared_tasks);
    }
    cond_var_.notify_all();
  }

  void Stop() {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cond_var_.notify_all();
    for (std::thread& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers_.clear();
  }

 private:
  bool PostDelayedTaskAt(
      AnyInvocable<void()> task,
      std::chrono::steady_clock::time_point execute_time) {
    bool notify = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (stop_) {
        return false;
      }
      if (taskQueue_.empty() || execute_time < taskQueue_.top().execute_time) {
        notify = true;
      }
      taskQueue_.emplace(execute_time, next_sequence_id_++, std::move(task));
    }
    if (notify) {
      cond_var_.notify_one();
    }
    return true;
  }

  struct TaskItem {
    std::chrono::steady_clock::time_point enqueue_time;
    std::chrono::steady_clock::time_point execute_time;
    uint64_t sequence_id;
    AnyInvocable<void()> task = nullptr;

    TaskItem(std::chrono::steady_clock::time_point execute_time_,
             uint64_t sequence_id_,
             AnyInvocable<void()> func)
        : enqueue_time(std::chrono::steady_clock::now()),
          execute_time(execute_time_),
          sequence_id(sequence_id_),
          task(std::move(func)) {}

    bool operator>(const TaskItem& other) const {
      if (execute_time != other.execute_time) {
        return execute_time > other.execute_time;
      }
      return sequence_id > other.sequence_id;
    }
  };

  void WorkerThread() {
    const TaskQueue* previous_queue = current_queue_;
    current_queue_ = this;
    auto reset_current_queue =
        std::unique_ptr<const TaskQueue,
                        std::function<void(const TaskQueue*)>>(
            this, [previous_queue](const TaskQueue*) {
              current_queue_ = previous_queue;
            });

    while (true) {
      TaskItem task_item(std::chrono::steady_clock::now(), 0, nullptr);

      {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_var_.wait(lock, [this]() { return stop_ || !taskQueue_.empty(); });

        if (stop_ && taskQueue_.empty()) return;

        while (!taskQueue_.empty()) {
          if (log_enabled_) {
            LOG_INFO("[TaskQueue: {}] size {}", task_name_.c_str(),
                     taskQueue_.size());
          }
          auto now = std::chrono::steady_clock::now();
          const auto& top = taskQueue_.top();
          auto execute_time = top.execute_time;

          if (execute_time > now) {
            cond_var_.wait_until(lock, execute_time);
            if (stop_) return;
            continue;
          }

          task_item = std::move(const_cast<TaskItem&>(taskQueue_.top()));
          taskQueue_.pop();
          break;
        }
      }

      if (!task_item.task) {
        continue;
      }

      if (log_enabled_) {
        auto start_exec = std::chrono::steady_clock::now();
        auto queue_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                start_exec - task_item.enqueue_time);

        LOG_INFO("[TaskQueue: {}] Task queued for {} ms", task_name_.c_str(),
                 queue_duration.count());

        auto exec_begin = std::chrono::steady_clock::now();
        task_item.task();
        auto exec_end = std::chrono::steady_clock::now();
        auto exec_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(exec_end -
                                                                  exec_begin);

        LOG_INFO("[TaskQueue: {}] Task executed in {} ms", task_name_,
                 exec_duration.count());
      } else {
        task_item.task();
      }
    }
  }

  std::string task_name_;
  bool log_enabled_ = false;
  std::vector<std::thread> workers_;
  std::priority_queue<TaskItem, std::vector<TaskItem>, std::greater<>>
      taskQueue_;
  std::mutex mutex_;
  std::condition_variable cond_var_;
  bool stop_;
  uint64_t next_sequence_id_ = 0;

  inline static thread_local const TaskQueue* current_queue_ = nullptr;
};
}  // namespace minirtc

#endif  // _TASK_QUEUE_H_
