#include "phx/jobs/JobSystem.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define PHX_CPU_PAUSE() _mm_pause()
#else
#define PHX_CPU_PAUSE() std::this_thread::yield()
#endif

namespace phx::jobs {
namespace {
constexpr u32 kNil = 0xFFFFFFFFu;
constexpr u32 kIdleSpins = 64;
constexpr std::size_t kSupervisorScanDepth = 64;

thread_local u32 tThreadIndex = kInvalidThread;
thread_local ThreadRole tRole = ThreadRole::External;
thread_local JobSystem* tOwner = nullptr;

u32 xorshift(u32& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
} // namespace

JobSystem::JobSystem(const Config& cfg) {
    poolSize_ = std::max<u32>(cfg.jobPoolSize, 64);
    pool_ = std::make_unique<Job[]>(poolSize_);
    for (u32 i = 0; i < poolSize_; ++i)
        pool_[i].freeNext.store(i + 1 < poolSize_ ? i + 1 : kNil, std::memory_order_relaxed);
    freeHead_.store(0, std::memory_order_relaxed);

    u32 workers = cfg.workerCount;
    if (workers == 0) {
        const u32 hw = std::max(1u, std::thread::hardware_concurrency());
        workers = hw > 2 ? hw - 2 : 1;
    }
    workerStates_.reserve(workers);
    for (u32 i = 0; i < workers; ++i) {
        workerStates_.push_back(std::make_unique<WorkerState>(cfg.dequeCapacity));
        workerStates_.back()->rng = 0x9E3779B9u * (i + 1);
    }
    workers_.reserve(workers);
    for (u32 i = 0; i < workers; ++i) workers_.emplace_back([this, i] { workerMain(i); });
}

JobSystem::~JobSystem() {
    running_.store(false, std::memory_order_seq_cst);
    epoch_.fetch_add(1, std::memory_order_seq_cst);
    epoch_.notify_all();
    for (auto& t : workers_) t.join();
}

void JobSystem::registerThread(ThreadRole role) {
    assert(role == ThreadRole::Supervisor || role == ThreadRole::Render);
    tOwner = this;
    tRole = role;
    tThreadIndex = role == ThreadRole::Supervisor ? kSupervisorThread : kRenderThread;
}

u32 JobSystem::threadIndex() { return tThreadIndex; }
ThreadRole JobSystem::threadRole() { return tRole; }

// ------------------------------------------------------------------ job pool (tagged Treiber stack)

Job* JobSystem::allocJob() {
    for (;;) {
        u64 head = freeHead_.load(std::memory_order_acquire);
        while (static_cast<u32>(head) != kNil) {
            Job* job = &pool_[static_cast<u32>(head)];
            const u32 next = job->freeNext.load(std::memory_order_relaxed);
            const u64 newHead = (((head >> 32) + 1) << 32) | next;
            if (freeHead_.compare_exchange_weak(head, newHead, std::memory_order_acq_rel, std::memory_order_acquire))
                return job;
        }
        // Pool exhausted: make progress instead of failing.
        if (!helpOnce()) std::this_thread::yield();
    }
}

void JobSystem::freeJob(Job* job) {
    const u32 index = u32(job - pool_.get());
    u64 head = freeHead_.load(std::memory_order_relaxed);
    u64 newHead;
    do {
        job->freeNext.store(u32(head), std::memory_order_relaxed);
        newHead = (((head >> 32) + 1) << 32) | index;
    } while (!freeHead_.compare_exchange_weak(head, newHead, std::memory_order_release, std::memory_order_relaxed));
}

// ------------------------------------------------------------------ scheduling

void JobSystem::schedule(Job* job) {
    const u32 p = u32(job->priority);
    bool pushed = false;
    if (tOwner == this && tRole == ThreadRole::Worker)
        pushed = workerStates_[tThreadIndex - kFirstWorkerThread]->deques[p].push(job);
    if (!pushed) {
        InjectionQueue& q = injection_[p];
        std::lock_guard lock(q.mutex);
        q.jobs.push_back(job);
        q.size.fetch_add(1, std::memory_order_relaxed);
    }
    notifyWork();
}

void JobSystem::notifyWork() {
    epoch_.fetch_add(1, std::memory_order_seq_cst);
    if (sleepers_.load(std::memory_order_seq_cst) > 0) epoch_.notify_one();
}

void JobSystem::execute(Job* job) {
    job->invoke(*job);
    Counter* signal = job->signal;
    freeJob(job);
    complete(signal);

    if (tOwner == this) {
        switch (tRole) {
        case ThreadRole::Worker:
            workerStates_[tThreadIndex - kFirstWorkerThread]->executed.fetch_add(1, std::memory_order_relaxed);
            break;
        case ThreadRole::Supervisor: supervisorExecuted_.fetch_add(1, std::memory_order_relaxed); break;
        default: externalExecuted_.fetch_add(1, std::memory_order_relaxed); break;
        }
    } else {
        externalExecuted_.fetch_add(1, std::memory_order_relaxed);
    }
}

void JobSystem::complete(Counter* counter) {
    if (!counter) return;
    if (counter->pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        counter->lock();
        Job* waiter = counter->waiters_;
        counter->waiters_ = nullptr;
        counter->unlock();
        while (waiter) {
            Job* next = waiter->nextWaiter;
            waiter->nextWaiter = nullptr;
            schedule(waiter);
            waiter = next;
        }
    }
    // Last access to *counter: after this a waiter may return and destroy it.
    // notify_all only hashes the address into the wait table (same contract as std::latch).
    if (counter->visible_.fetch_sub(1, std::memory_order_acq_rel) == 1) counter->visible_.notify_all();
}

Job* JobSystem::popInjection(Priority p) {
    InjectionQueue& q = injection_[u32(p)];
    if (q.size.load(std::memory_order_relaxed) == 0) return nullptr;
    std::lock_guard lock(q.mutex);
    if (q.jobs.empty()) return nullptr;
    Job* job = q.jobs.front();
    q.jobs.pop_front();
    q.size.fetch_sub(1, std::memory_order_relaxed);
    return job;
}

Job* JobSystem::stealFrom(u32 thief, Priority p) {
    const u32 n = u32(workerStates_.size());
    if (n <= 1 && thief != kNil) return nullptr;
    u32 seed = thief != kNil ? xorshift(workerStates_[thief]->rng) : u32(epoch_.load(std::memory_order_relaxed)) | 1u;
    const u32 start = seed % n;
    for (u32 i = 0; i < n; ++i) {
        const u32 victim = (start + i) % n;
        if (victim == thief) continue;
        auto& dq = workerStates_[victim]->deques[u32(p)];
        if (dq.maybeEmpty()) continue;
        if (Job* job = dq.steal()) {
            if (thief != kNil) workerStates_[thief]->steals.fetch_add(1, std::memory_order_relaxed);
            return job;
        }
    }
    return nullptr;
}

Job* JobSystem::findWorkerJob(u32 w) {
    WorkerState& self = *workerStates_[w];
    for (Priority p : {Priority::Critical, Priority::Normal}) {
        if (Job* j = self.deques[u32(p)].pop()) return j;
        if (Job* j = popInjection(p)) return j;
    }
    for (Priority p : {Priority::Critical, Priority::Normal})
        if (Job* j = stealFrom(w, p)) return j;
    if (Job* j = self.deques[u32(Priority::Background)].pop()) return j;
    if (Job* j = popInjection(Priority::Background)) return j;
    return stealFrom(w, Priority::Background);
}

void JobSystem::workerMain(u32 w) {
    tOwner = this;
    tRole = ThreadRole::Worker;
    tThreadIndex = kFirstWorkerThread + w;

    while (running_.load(std::memory_order_relaxed)) {
        if (Job* job = findWorkerJob(w)) {
            execute(job);
            continue;
        }
        // Spin briefly, then park on the epoch. Reading the epoch before the final
        // check closes the lost-wakeup window (see notifyWork()).
        const u32 seen = epoch_.load(std::memory_order_seq_cst);
        Job* job = nullptr;
        for (u32 i = 0; i < kIdleSpins && !job; ++i) {
            PHX_CPU_PAUSE();
            job = findWorkerJob(w);
        }
        if (job) {
            execute(job);
            continue;
        }
        sleepers_.fetch_add(1, std::memory_order_seq_cst);
        job = findWorkerJob(w);
        if (!job && running_.load(std::memory_order_seq_cst)) epoch_.wait(seen, std::memory_order_seq_cst);
        sleepers_.fetch_sub(1, std::memory_order_seq_cst);
        if (job) execute(job);
    }
}

// ------------------------------------------------------------------ supervisor helping

Job* JobSystem::takeSupervisorSafe(i64 costLimitUs) {
    for (Priority p : {Priority::Critical, Priority::Normal}) { // never Background (long tasks)
        InjectionQueue& q = injection_[u32(p)];
        if (q.size.load(std::memory_order_relaxed) == 0) continue;
        std::lock_guard lock(q.mutex);
        const std::size_t depth = std::min(q.jobs.size(), kSupervisorScanDepth);
        for (std::size_t i = 0; i < depth; ++i) {
            Job* job = q.jobs[i];
            if (!(job->flags & kSupervisorSafe) || job->costHintUs == 0 || job->costHintUs > costLimitUs) continue;
            q.jobs.erase(q.jobs.begin() + std::ptrdiff_t(i));
            q.size.fetch_sub(1, std::memory_order_relaxed);
            return job;
        }
    }
    return nullptr;
}

u32 JobSystem::supervisorHelp(Clock::time_point deadline, std::chrono::microseconds maxSlice) {
    assert(tOwner == this && tRole == ThreadRole::Supervisor);
    const auto sliceEnd = Clock::now() + maxSlice;
    if (sliceEnd < deadline) deadline = sliceEnd;
    u32 ran = 0;
    for (;;) {
        const auto now = Clock::now();
        if (now >= deadline) break;
        const i64 slackUs = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
        Job* job = takeSupervisorSafe(slackUs / 2);
        if (!job) break;
        execute(job);
        ++ran;
    }
    return ran;
}

// ------------------------------------------------------------------ waiting

bool JobSystem::helpOnce() {
    if (tOwner != this) return false;
    Job* job = nullptr;
    switch (tRole) {
    case ThreadRole::Worker: job = findWorkerJob(tThreadIndex - kFirstWorkerThread); break;
    case ThreadRole::Render: // same-frame work only
        job = popInjection(Priority::Critical);
        if (!job) job = stealFrom(kNil, Priority::Critical);
        break;
    case ThreadRole::Supervisor: job = takeSupervisorSafe(INT64_MAX); break;
    case ThreadRole::External: break;
    }
    if (!job) return false;
    execute(job);
    return true;
}

void JobSystem::wait(Counter& counter) {
    u32 idle = 0;
    while (!counter.done()) {
        if (helpOnce()) {
            idle = 0;
            continue;
        }
        if (++idle < kIdleSpins) {
            PHX_CPU_PAUSE();
            continue;
        }
        // Nothing we may run: block until the counter changes (complete() notifies at zero).
        const i32 v = counter.visible_.load(std::memory_order_acquire);
        if (v == 0) break;
        if (tRole == ThreadRole::Worker) {
            std::this_thread::yield(); // a worker must keep scanning: its own deque may refill
            idle = 0;
        } else {
            counter.visible_.wait(v, std::memory_order_acquire);
        }
    }
}

JobSystem::Stats JobSystem::stats() const {
    Stats s;
    for (const auto& w : workerStates_) {
        s.executed += w->executed.load(std::memory_order_relaxed);
        s.steals += w->steals.load(std::memory_order_relaxed);
    }
    s.executedBySupervisor = supervisorExecuted_.load(std::memory_order_relaxed);
    s.executed += s.executedBySupervisor + externalExecuted_.load(std::memory_order_relaxed);
    return s;
}

} // namespace phx::jobs
