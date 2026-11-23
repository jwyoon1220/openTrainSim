#include "phx/gpu/GpuProfiler.hpp"

#include "phx/gpu/VulkanContext.hpp"

#include <format>

namespace phx {

GpuProfiler::GpuProfiler(VulkanContext& ctx, u32 framesInFlight) : ctx_(ctx), frames_(framesInFlight) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(ctx.physicalDevice, &props);
    u32 n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &n, nullptr);
    std::vector<VkQueueFamilyProperties> fam(n);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &n, fam.data());
    supported_ = props.limits.timestampPeriod > 0 && fam[ctx.graphics.family].timestampValidBits > 0;
    periodNs_ = props.limits.timestampPeriod;
    if (!supported_) return;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = framesInFlight * kMaxScopes * 2;
    VK_CHECK(vkCreateQueryPool(ctx.device, &qi, nullptr, &pool_));
    vkResetQueryPool(ctx.device, pool_, 0, qi.queryCount);
}

GpuProfiler::~GpuProfiler() {
    if (pool_) vkDestroyQueryPool(ctx_.device, pool_, nullptr);
}

void GpuProfiler::beginFrame(u32 slot) {
    if (!supported_) return;
    Frame& f = frames_[slot];
    if (f.count) {
        u64 ts[kMaxScopes * 2];
        if (vkGetQueryPoolResults(ctx_.device, pool_, slot * kMaxScopes * 2, f.count * 2, sizeof(ts), ts, sizeof(u64),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            if (totals_.size() < f.count) totals_.resize(f.count);
            for (u32 i = 0; i < f.count; ++i) {
                totals_[i].first = f.names[i];
                const f64 ms = f64(ts[i * 2 + 1] - ts[i * 2]) * periodNs_ * 1e-6;
                totals_[i].second += ms;
                if (smoothed_.size() < f.count) smoothed_.resize(f.count, {f.names[i], ms});
                smoothed_[i].first = f.names[i];
                smoothed_[i].second += (ms - smoothed_[i].second) * 0.05;
            }
            ++samples_;
        }
        vkResetQueryPool(ctx_.device, pool_, slot * kMaxScopes * 2, kMaxScopes * 2);
    }
    f.count = 0;
}

void GpuProfiler::begin(VkCommandBuffer cmd, u32 slot, const char* name) {
    if (!supported_) return;
    Frame& f = frames_[slot];
    if (f.count >= kMaxScopes) return;
    f.names[f.count] = name;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_, slot * kMaxScopes * 2 + f.count * 2);
}

void GpuProfiler::end(VkCommandBuffer cmd, u32 slot) {
    if (!supported_) return;
    Frame& f = frames_[slot];
    if (f.count >= kMaxScopes) return;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_, slot * kMaxScopes * 2 + f.count * 2 + 1);
    ++f.count;
}

std::string GpuProfiler::report() {
    std::string s = "gpu:";
    f64 sum = 0;
    for (auto& [name, total] : totals_) {
        const f64 avg = samples_ ? total / samples_ : 0.0;
        sum += avg;
        s += std::format(" {} {:.3f}", name, avg);
        total = 0;
    }
    s += std::format(" | total {:.3f} ms", sum);
    samples_ = 0;
    return s;
}

} // namespace phx
