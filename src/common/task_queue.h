#ifndef _TASK_QUEUE_H_
#define _TASK_QUEUE_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
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

  void PostTask(AnyInvocable<void()> task) {
    PostDelayedTask(std::move(task), 0);
  }

  void PostDelayedTask(AnyInvocable<void()> task, int delay_ms) {
    auto execute_time =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);

    bool notify = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (taskQueue_.empty() || execute_time < taskQueue_.top().execute_time) {
        notify = true;
      }
      taskQueue_.emplace(execute_time, std::move(task));
    }
    if (notify) {
      cond_var_.notify_one();
    }
  }

  void ClearTasks() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!taskQueue_.empty()) {
      taskQueue_.pop();
    }
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
  struct TaskItem {
    std::chrono::steady_clock::time_point enqueue_time;
    std::chrono::steady_clock::time_point execute_time;
    AnyInvocable<void()> task = nullptr;

    TaskItem(std::chrono::steady_clock::time_point execute_time_,
             AnyInvocable<void()> func)
        : enqueue_time(std::chrono::steady_clock::now()),
          execute_time(execute_time_),
          task(std::move(func)) {}

    bool operator>(const TaskItem& other) const {
      return execute_time > other.execute_time;
    }
  };

  void WorkerThread() {
    while (true) {
      TaskItem task_item(std::chrono::steady_clock::now(), nullptr);

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

          if (top.execute_time > now) {
            cond_var_.wait_until(lock, top.execute_time, [this, now]() {
              return stop_ || (!taskQueue_.empty() &&
                               taskQueue_.top().execute_time <=
                                   std::chrono::steady_clock::now());
            });
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
};

#endif  // _TASK_QUEUE_H_
