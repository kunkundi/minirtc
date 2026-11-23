#include "thread_base.h"

#include "log.h"

namespace minirtc {

ThreadBase::ThreadBase()
    : running_(false),
      pause_(false),
      period_(std::chrono::milliseconds(100)),
      thread_name_("UnnamedThread") {}

ThreadBase::~ThreadBase() { Stop(); }

void ThreadBase::Start() {
  {
    std::lock_guard<std::mutex> lock(cv_mtx_);
    if (running_) {
      return;
    }
    running_ = true;
  }

  std::thread t(&ThreadBase::Run, this);
  thread_ = std::move(t);
}

void ThreadBase::Stop() {
  {
    std::lock_guard<std::mutex> lock(cv_mtx_);
    if (!running_) {
      return;
    }
    running_ = false;
  }

  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void ThreadBase::Pause() { pause_ = true; }

void ThreadBase::Resume() { pause_ = false; }

void ThreadBase::SetPeriod(std::chrono::milliseconds period) {
  std::lock_guard<std::mutex> lock(cv_mtx_);
  period_ = period;
}

void ThreadBase::SetThreadName(const std::string& name) {
  std::lock_guard<std::mutex> lock(cv_mtx_);
  thread_name_ = name;
}

std::string ThreadBase::GetThreadName() {
  std::lock_guard<std::mutex> lock(cv_mtx_);
  return thread_name_;
}

void ThreadBase::RequestProcess() {
  {
    std::lock_guard<std::mutex> lock(cv_mtx_);
    process_requested_ = true;
  }
  cv_.notify_one();
}

void ThreadBase::Run() {
  std::unique_lock<std::mutex> lock(cv_mtx_);
  while (running_) {
    cv_.wait_for(lock, period_,
                 [this] { return !running_ || process_requested_; });
    if (!running_) break;
    process_requested_ = false;
    lock.unlock();
    Process();
    lock.lock();
  }
}
}  // namespace minirtc
