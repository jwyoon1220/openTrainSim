#pragma once
// Phanerozoic job system.
//
// Thread layout (see plan §2.1):
//   index 0       = Supervisor (main thread, owns GLFW)
//   index 1       = Render thread (owns VkQueue submit/present)
//   index 2..N+1  = Workers (take any job)
//
// Workers own one Chase-Lev deque per priority. The supervisor and render thread
// submit through global injection queues so that a job they spawn is never
// forced back onto them. The supervisor only helps with jobs flagged
// kSupervisorSafe whose cost hint fits inside its remaining tick slack.

#include "phx/core/Types.hpp"
#include "phx/jobs/WorkStealingDeque.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace phx::jobs {

enum class Priority : u8 { Critical = 0, Normal = 1, Background = 2 };
inline constexpr u32 kPriorityCount = 3;

enum JobFlags : u8 {
    kNone = 0,
    kSupervisorSafe = 1u << 0, // short, non-blocking, never waits: supervisor may run it
};

enum class ThreadRole : u8 { External, Supervisor, Render, Worker };

inline constexpr u32 kSupervisorThread = 0;
inline constexpr u32 kRenderThread = 1;
inline constexpr u32 kFirstWorkerThread = 2;
inline constexpr u32 kInvalidThread = 0xFFFFFFFFu;

class Counter;

struct alignas(kCacheLine) Job {
    using Invoke = void (*)(Job&);
    static constexpr std::size_t kPayloadSize = 96;

    Invoke invoke = nullptr;
    Counter* signal = nullptr;
    Job* nextWaiter = nullptr;      // intrusive list while parked on a Counter
    std::atomic<u32> freeNext{0};   // intrusive free-list link
    u16 costHintUs = 0;             // 0 = unknown (never eligible for supervisor)
    Priority priority = Priority::Normal;
    u8 flags = kNone;
    alignas(16) std::byte payload[kPayloadSize];
};
static_assert(sizeof(Job) == 128);

// Completion counter. Incremented by submit, decremented when a job finishes.
// Jobs submitted with submitAfter() are parked here until it reaches zero.
class Counter {
public:
    Counter() = default;
    Counter(const Counter&) = delete;
    Counter& operator=(const Counter&) = delete;

    // done() flips only after the last completing thread has finished touching the
    // counter (except for the address-only futex wake), so the owner may destroy it.
    [[nodiscard]] bool done() const { return visible_.load(std::memory_order_acquire) == 0; }
    [[nodiscard]] i32 pending() const { return visible_.load(std::memory_order_acquire); }

private:
    friend class JobSystem;
    void lock() { while (lock_.test_and_set(std::memory_order_acquire)) lock_.wait(true, std::memory_order_relaxed); }
    void unlock() { lock_.clear(std::memory_order_release); lock_.notify_one(); }

    std::atomic<i32> pending_{0}; // drives continuations (guarded transitions under lock_)
    std::atomic<i32> visible_{0}; // drives done()/wait(); decremented as the final step
    std::atomic_flag lock_;
    Job* waiters_ = nullptr;
};

struct JobDesc {
    Priority priority = Priority::Normal;
    u8 flags = kNone;
    u16 costHintUs = 0;
    Counter* signal = nullptr;
};

class JobSystem {
public:
    struct Config {
        u32 workerCount = 0;        // 0 = hardware_concurrency - 2 (min 1)
        u32 jobPoolSize = 1u << 16; // concurrent live jobs
        u32 dequeCapacity = 1u << 12;
    };

    using Clock = std::chrono::steady_clock;

    explicit JobSystem(const Config& cfg);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // Supervisor and render threads call this once from their own thread.
    void registerThread(ThreadRole role);

    [[nodiscard]] static u32 threadIndex();
    [[nodiscard]] static ThreadRole threadRole();
    [[nodiscard]] u32 workerCount() const { return u32(workers_.size()); }
    [[nodiscard]] u32 threadCount() const { return kFirstWorkerThread + workerCount(); }

    template <class F> void submit(const JobDesc& desc, F&& fn) { schedule(makeJob(desc, std::forward<F>(fn))); }

    // Runs fn once `dependency` reaches zero.
    template <class F> void submitAfter(Counter& dependency, const JobDesc& desc, F&& fn) {
        Job* job = makeJob(desc, std::forward<F>(fn));
        dependency.lock();
        if (dependency.pending_.load(std::memory_order_acquire) == 0) {
            dependency.unlock();
            schedule(job);
        } else {
            job->nextWaiter = dependency.waiters_;
            dependency.waiters_ = job;
            dependency.unlock();
        }
    }

    // Blocking parallel loop: fn(begin, end) over [0, count) in chunks of at most `grain`.
    // The range is split lazily by binary halving on the executing worker (children go
    // to its local deque), so submission cost is O(workers) on the calling thread and
    // idle workers balance by stealing the largest remaining halves.
    template <class F> void parallelFor(u32 count, u32 grain, JobDesc desc, F&& fn) {
        if (count == 0) return;
        if (grain == 0) grain = 1;
        Counter done;
        desc.signal = &done;
        using Fn = std::remove_reference_t<F>;
        const u32 roots = std::min<u32>(threadCount(), (count + grain - 1) / grain);
        const u32 step = (count + roots - 1) / roots;
        for (u32 b = 0; b < count; b += step)
            submitRange<Fn>(desc, &fn, b, std::min(count, b + step), grain);
        wait(done);
    }

    // Blocks until counter reaches zero, helping with work as the calling thread's role allows.
    void wait(Counter& counter);

    // Supervisor only: run kSupervisorSafe jobs whose cost fits in half of the remaining slack.
    // Returns at `deadline` or after `maxSlice` (input-latency cap), whichever is first.
    u32 supervisorHelp(Clock::time_point deadline,
                       std::chrono::microseconds maxSlice = std::chrono::microseconds(2000));

    struct Stats {
        u64 executed = 0;
        u64 executedBySupervisor = 0;
        u64 steals = 0;
    };
    [[nodiscard]] Stats stats() const;

private:
    template <class Fn> void submitRange(JobDesc desc, Fn* fn, u32 begin, u32 end, u32 grain) {
        submit(desc, [this, desc, fn, begin, end, grain] {
            u32 b = begin, e = end;
            while (e - b > grain) { // keep left half, publish right half for thieves
                const u32 mid = b + (e - b) / 2;
                submitRange<Fn>(desc, fn, mid, e, grain);
                e = mid;
            }
            (*fn)(b, e);
        });
    }

    template <class F> Job* makeJob(const JobDesc& desc, F&& fn) {
        using Fn = std::decay_t<F>;
        static_assert(sizeof(Fn) <= Job::kPayloadSize, "job capture too large: capture a pointer instead");
        static_assert(alignof(Fn) <= 16, "job capture over-aligned");
        Job* job = allocJob();
        ::new (static_cast<void*>(job->payload)) Fn(std::forward<F>(fn));
        job->invoke = [](Job& j) {
            Fn* f = std::launder(reinterpret_cast<Fn*>(j.payload));
            (*f)();
            f->~Fn();
        };
        job->signal = desc.signal;
        job->nextWaiter = nullptr;
        job->costHintUs = desc.costHintUs;
        job->priority = desc.priority;
        job->flags = desc.flags;
        if (desc.signal) {
            desc.signal->visible_.fetch_add(1, std::memory_order_relaxed);
            desc.signal->pending_.fetch_add(1, std::memory_order_relaxed);
        }
        return job;
    }

    struct InjectionQueue {
        std::mutex mutex;
        std::deque<Job*> jobs;
        std::atomic<u32> size{0}; // racy hint to skip locking empty queues
    };

    struct alignas(kCacheLine) WorkerState {
        WorkStealingDeque<Job> deques[kPriorityCount];
        u32 rng = 0;
        std::atomic<u64> executed{0};
        std::atomic<u64> steals{0};
        explicit WorkerState(u32 capacity)
            : deques{WorkStealingDeque<Job>(capacity), WorkStealingDeque<Job>(capacity),
                     WorkStealingDeque<Job>(capacity)} {}
    };

    Job* allocJob();
    void freeJob(Job* job);
    void schedule(Job* job);
    void execute(Job* job);
    void complete(Counter* counter);
    void notifyWork();

    void workerMain(u32 workerIndex);
    Job* findWorkerJob(u32 workerIndex);
    Job* stealFrom(u32 thiefWorker, Priority p);
    Job* popInjection(Priority p);
    Job* takeSupervisorSafe(i64 costLimitUs);
    bool helpOnce(); // role-aware single step, used by wait()

    std::unique_ptr<Job[]> pool_;
    u32 poolSize_ = 0;
    alignas(kCacheLine) std::atomic<u64> freeHead_{0}; // (tag << 32) | index

    InjectionQueue injection_[kPriorityCount];
    std::vector<std::unique_ptr<WorkerState>> workerStates_;
    std::vector<std::thread> workers_;

    alignas(kCacheLine) std::atomic<u32> epoch_{0};
    std::atomic<u32> sleepers_{0};
    std::atomic<bool> running_{true};
    std::atomic<u64> supervisorExecuted_{0};
    std::atomic<u64> externalExecuted_{0};
};

} // namespace phx::jobs
