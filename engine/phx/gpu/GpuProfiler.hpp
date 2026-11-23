#pragma once
// GPU timestamp profiler: named scopes per frame, resolved when the frame slot is
// reused (after its timeline wait), averaged over the stats window.
#include "phx/core/Types.hpp"
#include "phx/gpu/Vk.hpp"

#include <array>
#include <string>
#include <vector>

namespace phx {
class VulkanContext;

class GpuProfiler {
public:
    static constexpr u32 kMaxScopes = 16;

    GpuProfiler(VulkanContext& ctx, u32 framesInFlight);
    ~GpuProfiler();

    // Render thread, after the slot's previous submission completed: read back and reset.
    void beginFrame(u32 slot);
    // Recording (any thread that records the frame's command buffer).
    void begin(VkCommandBuffer cmd, u32 slot, const char* name);
    void end(VkCommandBuffer cmd, u32 slot);

    // Averages (ms) since the last call, in scope order; resets the accumulators.
    std::string report();
    // Exponentially smoothed per-scope times (ms) of recent frames, for the overlay.
    [[nodiscard]] const std::vector<std::pair<const char*, f64>>& smoothed() const { return smoothed_; }

private:
    struct Frame {
        u32 count = 0;
        std::array<const char*, kMaxScopes> names{};
    };
    VulkanContext& ctx_;
    VkQueryPool pool_ = VK_NULL_HANDLE;
    std::vector<Frame> frames_;
    f64 periodNs_ = 1.0;
    bool supported_ = false;
    std::vector<std::pair<std::string, f64>> totals_;
    u32 samples_ = 0;
    std::vector<std::pair<const char*, f64>> smoothed_;
};

} // namespace phx
