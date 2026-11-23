#pragma once
// Fixed-capacity Chase-Lev work-stealing deque (Lê et al., "Correct and Efficient
// Work-Stealing for Weak Memory Models", 2013), written with seq_cst operations in
// place of standalone fences so ThreadSanitizer can reason about it.
//
// Owner: push()/pop() at the bottom. Thieves: steal() from the top.

#include "phx/core/Types.hpp"

#include <atomic>
#include <bit>
#include <memory>

namespace phx::jobs {

template <class T> class WorkStealingDeque {
public:
    explicit WorkStealingDeque(u32 capacity)
        : mask_(i64(std::bit_ceil(capacity)) - 1), buffer_(new std::atomic<T*>[std::size_t(mask_ + 1)]) {}

    // Owner only. Returns false when full (caller falls back to another queue).
    bool push(T* item) {
        const i64 b = bottom_.load(std::memory_order_relaxed);
        const i64 t = top_.load(std::memory_order_acquire);
        if (b - t > mask_) return false;
        buffer_[b & mask_].store(item, std::memory_order_relaxed);
        bottom_.store(b + 1, std::memory_order_release);
        return true;
    }

    // Owner only.
    T* pop() {
        const i64 b = bottom_.load(std::memory_order_relaxed) - 1;
        bottom_.store(b, std::memory_order_seq_cst);
        i64 t = top_.load(std::memory_order_seq_cst);
        if (t > b) { // empty
            bottom_.store(b + 1, std::memory_order_relaxed);
            return nullptr;
        }
        T* item = buffer_[b & mask_].load(std::memory_order_relaxed);
        if (t == b) { // last element: race against thieves
            if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
                item = nullptr;
            bottom_.store(b + 1, std::memory_order_relaxed);
        }
        return item;
    }

    // Any thread.
    T* steal() {
        i64 t = top_.load(std::memory_order_seq_cst);
        const i64 b = bottom_.load(std::memory_order_seq_cst);
        if (t >= b) return nullptr;
        T* item = buffer_[t & mask_].load(std::memory_order_relaxed);
        if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            return nullptr; // lost the race
        return item;
    }

    [[nodiscard]] bool maybeEmpty() const {
        return bottom_.load(std::memory_order_relaxed) <= top_.load(std::memory_order_relaxed);
    }

private:
    alignas(kCacheLine) std::atomic<i64> top_{0};
    alignas(kCacheLine) std::atomic<i64> bottom_{0};
    i64 mask_;
    std::unique_ptr<std::atomic<T*>[]> buffer_;
};

} // namespace phx::jobs
