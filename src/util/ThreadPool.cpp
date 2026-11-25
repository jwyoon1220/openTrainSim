#include "util/ThreadPool.hpp"

#include <algorithm>

namespace ots {

struct JobTicket::Task {
    std::function<void()> fn;
    std::size_t level = 0;
    bool queued = true; // false once taken by a worker or cancelled
};

namespace {
constexpr std::size_t kFirstPrefetchLevel = std::size_t(JobPriority::Ahead);
constexpr std::size_t levelOf(JobPriority p) { return std::size_t(std::clamp<int>(std::int8_t(p), 0, int(kJobLevels) - 1)); }
} // namespace

ThreadPool::ThreadPool(unsigned threads) {
    threads = std::max(1u, threads);
    prefetchLimit_ = threads > 1 ? threads - 1 : 1; // keep one worker for visible work
    for (unsigned i = 0; i < threads; ++i) workers_.emplace_back([this] { run(); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cancelQueued();
    cv_.notify_all();
    workers_.clear(); // joins
}

JobTicket ThreadPool::submit(std::function<void()> job, JobPriority priority) {
    if (priority == JobPriority::Immediate) {
        job();
        return {};
    }
    auto task = std::make_shared<Task>(Task{.fn = std::move(job), .level = levelOf(priority)});
    {
        std::lock_guard lock(mutex_);
        queues_[task->level].push_back(task);
        ++queued_[task->level];
    }
    cv_.notify_one();
    return JobTicket(std::move(task));
}

bool ThreadPool::raise(const JobTicket& ticket, JobPriority priority) {
    if (!ticket) return false;
    const auto& task = ticket.task_;
    {
        std::lock_guard lock(mutex_);
        const std::size_t level = levelOf(priority);
        if (!task->queued || level >= task->level) return false;
        --queued_[task->level];
        task->level = level;
        ++queued_[level];
        queues_[level].push_back(task);
    }
    cv_.notify_one(); // may now be runnable on the reserved worker
    return true;
}

bool ThreadPool::cancel(const JobTicket& ticket) {
    if (!ticket) return false;
    std::function<void()> dropped; // destroyed outside the lock
    {
        std::lock_guard lock(mutex_);
        const auto& task = ticket.task_;
        if (!task->queued) return false;
        task->queued = false;
        --queued_[task->level];
        dropped = std::move(task->fn);
        if (running_ == 0 && std::ranges::all_of(queued_, [](std::size_t n) { return n == 0; })) idle_.notify_all();
    }
    return true;
}

void ThreadPool::cancelQueued() {
    std::array<std::deque<std::shared_ptr<Task>>, kJobLevels> dropped;
    {
        std::lock_guard lock(mutex_);
        for (auto& q : queues_)
            for (auto& t : q) t->queued = false;
        dropped = std::move(queues_);
        queues_ = {};
        queued_ = {};
        if (running_ == 0) idle_.notify_all();
    }
}

std::size_t ThreadPool::pending() const {
    std::lock_guard lock(mutex_);
    std::size_t n = running_;
    for (std::size_t q : queued_) n += q;
    return n;
}

ThreadPool::Load ThreadPool::load() const {
    std::lock_guard lock(mutex_);
    return {queued_, running_, unsigned(workers_.size())};
}

void ThreadPool::waitIdle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] { return running_ == 0 && std::ranges::all_of(queued_, [](std::size_t n) { return n == 0; }); });
}

std::shared_ptr<ThreadPool::Task> ThreadPool::takeLocked() {
    for (std::size_t level = 0; level < kJobLevels; ++level) {
        auto& q = queues_[level];
        while (!q.empty() && !(q.front()->queued && q.front()->level == level)) q.pop_front(); // cancelled / raised away
        if (q.empty()) continue;
        if (level >= kFirstPrefetchLevel && prefetchRunning_ >= prefetchLimit_) return nullptr; // reserved worker
        auto task = std::move(q.front());
        q.pop_front();
        task->queued = false;
        --queued_[level];
        return task;
    }
    return nullptr;
}

void ThreadPool::run() {
    for (;;) {
        std::shared_ptr<Task> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || (task = takeLocked()); });
            if (stop_ && !task) return;
            ++running_;
            if (task->level >= kFirstPrefetchLevel) ++prefetchRunning_;
        }
        std::function<void()> fn = std::move(task->fn);
        fn();
        fn = nullptr;
        {
            std::lock_guard lock(mutex_);
            --running_;
            if (task->level >= kFirstPrefetchLevel) --prefetchRunning_;
            if (running_ == 0 && std::ranges::all_of(queued_, [](std::size_t n) { return n == 0; })) idle_.notify_all();
        }
        if (task->level >= kFirstPrefetchLevel) cv_.notify_one(); // a prefetch slot freed up
    }
}

} // namespace ots
