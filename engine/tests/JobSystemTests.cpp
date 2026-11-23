// Minimal self-contained tests + micro benchmark for the job system (plan M1).
#include "phx/jobs/JobSystem.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <thread>
#include <vector>

using namespace phx;
using namespace phx::jobs;
using Clock = std::chrono::steady_clock;

static int gFailures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++gFailures;                                                                           \
        }                                                                                          \
    } while (0)

static void testParallelForSum(JobSystem& js) {
    constexpr u32 n = 1'000'000;
    std::vector<u32> data(n);
    std::iota(data.begin(), data.end(), 0u);
    std::atomic<u64> sum{0};
    js.parallelFor(n, 1000, {}, [&](u32 b, u32 e) {
        u64 local = 0;
        for (u32 i = b; i < e; ++i) local += data[i];
        sum.fetch_add(local, std::memory_order_relaxed);
    });
    CHECK(sum.load() == u64(n) * (n - 1) / 2);
}

static void testDependencies(JobSystem& js) {
    // A (100 jobs) -> B (1 job) -> C (1 job); verify ordering.
    Counter a, b, c;
    std::atomic<int> stageA{0};
    std::atomic<int> observedByB{-1}, observedByC{-1};
    std::atomic<int> bDone{0};
    for (int i = 0; i < 100; ++i) js.submit({.signal = &a}, [&] { stageA.fetch_add(1); });
    js.submitAfter(a, {.signal = &b}, [&] {
        observedByB = stageA.load();
        bDone = 1;
    });
    js.submitAfter(b, {.signal = &c}, [&] { observedByC = bDone.load(); });
    js.wait(c);
    CHECK(observedByB.load() == 100);
    CHECK(observedByC.load() == 1);
}

static void testNestedSpawn(JobSystem& js) {
    // Jobs spawning jobs (goes to worker-local deques) and waiting inside a job.
    std::atomic<u32> leaves{0};
    Counter root;
    for (int i = 0; i < 64; ++i) {
        js.submit({.signal = &root}, [&] {
            Counter inner;
            for (int k = 0; k < 64; ++k) js.submit({.signal = &inner}, [&] { leaves.fetch_add(1); });
            js.wait(inner);
        });
    }
    js.wait(root);
    CHECK(leaves.load() == 64u * 64u);
}

static void testSupervisorPolicy(JobSystem& js) {
    js.registerThread(ThreadRole::Supervisor);
    // Unsafe or unknown-cost jobs must never run on the supervisor.
    std::atomic<int> wrongThread{0};
    std::atomic<int> ranOnSupervisor{0};
    Counter c;
    for (int i = 0; i < 2000; ++i) {
        const bool safe = (i % 2) == 0;
        js.submit({.priority = Priority::Normal, .flags = u8(safe ? kSupervisorSafe : kNone),
                   .costHintUs = u16(safe ? 5 : 0), .signal = &c},
                  [&, safe] {
                      if (JobSystem::threadRole() == ThreadRole::Supervisor) {
                          ranOnSupervisor.fetch_add(1);
                          if (!safe) wrongThread.fetch_add(1);
                      }
                      volatile int spin = 0;
                      for (int k = 0; k < 2000; ++k) spin = spin + k;
                  });
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(5);
    while (!c.done() && Clock::now() < deadline) js.supervisorHelp(deadline);
    js.wait(c);
    CHECK(wrongThread.load() == 0);

    // A job whose cost exceeds half the slack must be skipped.
    std::atomic<int> bigRanOnSupervisor{0};
    Counter big;
    js.submit({.flags = kSupervisorSafe, .costHintUs = 60000, .signal = &big}, [&] {
        if (JobSystem::threadRole() == ThreadRole::Supervisor) bigRanOnSupervisor = 1;
    });
    js.supervisorHelp(Clock::now() + std::chrono::milliseconds(10));
    js.wait(big);
    CHECK(bigRanOnSupervisor.load() == 0);
    std::printf("  supervisor helped with %d safe jobs\n", ranOnSupervisor.load());
}

// Regression: parallelFor issued from the render thread (Critical priority, helping
// restricted to Critical work) must run every index exactly once and only return
// after all of them finished.
static void testRenderThreadParallelFor(JobSystem& js) {
    std::atomic<int> failures{0};
    std::thread render([&] {
        js.registerThread(ThreadRole::Render);
        for (int iter = 0; iter < 2000; ++iter) {
            constexpr u32 n = 126;
            std::vector<std::atomic<int>> hits(n);
            std::vector<int> payload(n, 0);
            js.parallelFor(n, 1, {.priority = Priority::Critical}, [&](u32 b, u32 e) {
                for (u32 i = b; i < e; ++i) {
                    volatile int spin = 0;
                    for (int k = 0; k < 200; ++k) spin = spin + k;
                    payload[i] = int(i) + 1;
                    hits[i].fetch_add(1);
                }
            });
            for (u32 i = 0; i < n; ++i)
                if (hits[i].load() != 1 || payload[i] != int(i) + 1) {
                    failures.fetch_add(1);
                    break;
                }
        }
    });
    render.join();
    CHECK(failures.load() == 0);
    if (failures.load()) std::printf("  render-thread parallelFor failures: %d / 2000\n", failures.load());
}

static void benchMicroJobs(JobSystem& js) {
    constexpr u32 kJobs = 100'000;
    std::atomic<u64> sink{0};
    for (int round = 0; round < 3; ++round) {
        const auto t0 = Clock::now();
        js.parallelFor(kJobs, 1, {}, [&](u32 b, u32) { sink.fetch_add(b, std::memory_order_relaxed); });
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        std::printf("  100k micro-jobs: %.2f ms (%.0f ns/job)\n", ms, ms * 1e6 / kJobs);
    }
}

int main() {
    JobSystem js({});
    std::printf("workers: %u\n", js.workerCount());

    testParallelForSum(js);
    testDependencies(js);
    testNestedSpawn(js);
    testSupervisorPolicy(js);
    testRenderThreadParallelFor(js);
    benchMicroJobs(js);

    const auto s = js.stats();
    std::printf("executed=%llu steals=%llu bySupervisor=%llu\n", (unsigned long long)s.executed,
                (unsigned long long)s.steals, (unsigned long long)s.executedBySupervisor);
    if (gFailures) {
        std::fprintf(stderr, "%d check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("all job system tests passed\n");
    return 0;
}
