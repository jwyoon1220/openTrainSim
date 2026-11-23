#pragma once
// Lock-free single-producer / single-consumer triple buffer (plan §2.3).
// The supervisor writes a whole snapshot and publishes it; the render thread
// always picks up the most recent complete snapshot without blocking either side.
#include "phx/core/Types.hpp"

#include <atomic>

namespace phx {

template <class T> class TripleBuffer {
public:
    // Producer
    T& writeSlot() { return slots_[write_]; }
    void publish() { write_ = middle_.exchange(u8(write_ | kFresh), std::memory_order_acq_rel) & kIndex; }

    // Consumer: returns true if a newer snapshot became current.
    bool acquireLatest() {
        if (!(middle_.load(std::memory_order_relaxed) & kFresh)) return false;
        read_ = middle_.exchange(read_, std::memory_order_acq_rel) & kIndex;
        return true;
    }
    const T& read() const { return slots_[read_]; }

private:
    static constexpr u8 kIndex = 0x3;
    static constexpr u8 kFresh = 0x4;
    T slots_[3]{};
    alignas(kCacheLine) u8 write_ = 0;
    alignas(kCacheLine) u8 read_ = 1;
    alignas(kCacheLine) std::atomic<u8> middle_{2};
};

} // namespace phx
