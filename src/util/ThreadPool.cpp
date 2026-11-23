#include "util/ThreadPool.hpp"

namespace ots {

ThreadPool::ThreadPool(unsigned threads) {
    if (threads == 0) threads = 1;
    for (unsigned i = 0; i < threads; ++i) workers_.emplace_back([this] { run(); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    workers_.clear(); // joins
}

void ThreadPool::submit(std::function<void()> job, bool urgent) {
    {
        std::lock_guard lock(mutex_);
        if (urgent) queue_.push_front(std::move(job));
        else queue_.push_back(std::move(job));
    }
    cv_.notify_one();
}

std::size_t ThreadPool::pending() const {
    std::lock_guard lock(mutex_);
    return queue_.size() + running_;
}

void ThreadPool::waitIdle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] { return queue_.empty() && running_ == 0; });
}

void ThreadPool::run() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            ++running_;
        }
        job();
        {
            std::lock_guard lock(mutex_);
            --running_;
            if (queue_.empty() && running_ == 0) idle_.notify_all();
        }
    }
}

} // namespace ots
