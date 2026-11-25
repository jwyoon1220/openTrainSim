#pragma once
// Background workers for streaming (object parsing, image decoding, BC compression), with
// priorities so what the player is about to see is never stuck behind prefetching.
//
//   Immediate (-1)  runs on the calling thread inside submit(); never queued
//   Critical   (0)  the player's train and cab
//   Near       (1)  close to the camera (or the loading position)
//   View       (2)  inside the view distance
//   Ahead      (3)  just beyond the view distance (will be visible in a moment)
//   Far        (4)  long-range prefetch
//   Idle       (5)  might be needed later (other states of animated objects, ...)
//
// Workers always take the most urgent queued job. Prefetch levels (Ahead and below) never
// occupy every worker: one is kept free for Critical..View, so a newly visible object starts
// loading at once even while thousands of prefetch jobs are queued. A queued job can be raised
// to a more urgent level (the object came into view) or cancelled (no longer wanted).
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace ots {

enum class JobPriority : std::int8_t { Immediate = -1, Critical = 0, Near = 1, View = 2, Ahead = 3, Far = 4, Idle = 5 };

inline constexpr std::size_t kJobLevels = 6; // queued levels: Critical..Idle

// Lower value = more urgent (scoped enums compare by value: Near < View).
constexpr JobPriority lessUrgent(JobPriority p, int steps = 1) {
    return JobPriority(std::min<int>(std::int8_t(p) + steps, std::int8_t(JobPriority::Idle)));
}

class ThreadPool;

// Refers to a submitted job; empty for Immediate jobs (already done) or default-constructed.
class JobTicket {
public:
    JobTicket() = default;
    [[nodiscard]] explicit operator bool() const { return task_ != nullptr; }

private:
    friend class ThreadPool;
    struct Task;
    explicit JobTicket(std::shared_ptr<Task> t) : task_(std::move(t)) {}
    std::shared_ptr<Task> task_;
};

class ThreadPool {
public:
    explicit ThreadPool(unsigned threads);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    JobTicket submit(std::function<void()> job, JobPriority priority = JobPriority::View);
    // Moves a still-queued job to a more urgent level; no effect once it started or if
    // `priority` is not more urgent. Returns true if it was raised.
    bool raise(const JobTicket& ticket, JobPriority priority);
    // Removes a still-queued job; false if it already started (or finished).
    bool cancel(const JobTicket& ticket);
    // Drops every queued job (running ones finish). For shutdown.
    void cancelQueued();

    [[nodiscard]] std::size_t pending() const; // queued + running
    struct Load {
        std::array<std::size_t, kJobLevels> queued{}; // per level, Critical..Idle
        unsigned running = 0, workers = 0;
    };
    [[nodiscard]] Load load() const;
    // Blocks until the queue is empty and no job is running.
    void waitIdle();
    [[nodiscard]] unsigned size() const { return unsigned(workers_.size()); }

private:
    using Task = JobTicket::Task;
    std::shared_ptr<Task> takeLocked();
    void run();

    mutable std::mutex mutex_;
    std::condition_variable cv_, idle_;
    // A raised job is pushed again into its new level; the old entry stays behind and is
    // skipped when reached (its task is no longer queued at that level).
    std::array<std::deque<std::shared_ptr<Task>>, kJobLevels> queues_;
    std::array<std::size_t, kJobLevels> queued_{}; // live jobs per level
    std::vector<std::jthread> workers_;
    unsigned running_ = 0, prefetchRunning_ = 0, prefetchLimit_ = 1;
    bool stop_ = false;
};

} // namespace ots
