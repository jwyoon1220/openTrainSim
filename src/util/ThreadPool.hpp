#pragma once
// Background workers for streaming (object parsing, image decoding, BC compression).
// Two priorities: urgent work (things near the camera) jumps ahead of prefetching.
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ots {

class ThreadPool {
public:
    explicit ThreadPool(unsigned threads);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(std::function<void()> job, bool urgent = false);
    [[nodiscard]] std::size_t pending() const;
    // Blocks until the queue is empty and no job is running.
    void waitIdle();
    [[nodiscard]] unsigned size() const { return unsigned(workers_.size()); }

private:
    void run();
    mutable std::mutex mutex_;
    std::condition_variable cv_, idle_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::jthread> workers_;
    unsigned running_ = 0;
    bool stop_ = false;
};

} // namespace ots
