#pragma once
#include "phx/gpu/Vk.hpp"
#include "phx/platform/Window.hpp"

#include <vector>

namespace phx {
class VulkanContext;

enum class PresentPolicy { Vsync, LowLatency, Uncapped }; // FIFO / MAILBOX / IMMEDIATE

// Owned and used exclusively by the render thread.
class Swapchain {
public:
    // extraImages: images the caller may hold acquired at the same time beyond one
    // (frame generation presents several images per rendered frame).
    Swapchain(VulkanContext& ctx, Extent2D extent, PresentPolicy policy, u32 extraImages = 0);
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Caller guarantees the GPU no longer uses the old images (render thread waits idle first).
    void recreate(Extent2D extent);
    // NVIDIA Reflex low-latency mode (VK_NV_low_latency2); no-op where unsupported.
    void setLowLatency(bool enabled);
    [[nodiscard]] bool lowLatency() const { return lowLatency_; }

    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR; // what the policy got
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    // One per image: a present may still be reading a semaphore after the
    // frame slot that signalled it has been recycled.
    std::vector<VkSemaphore> renderFinished;

private:
    void create(Extent2D extent, VkSwapchainKHR old);
    void destroyViews();

    VulkanContext& ctx_;
    PresentPolicy policy_;
    u32 extraImages_ = 0;
    bool lowLatency_ = false;
};

} // namespace phx
