#include "phx/gpu/Swapchain.hpp"

#include "phx/gpu/VulkanContext.hpp"

#include <algorithm>

namespace phx {

Swapchain::Swapchain(VulkanContext& ctx, Extent2D extent, PresentPolicy policy, u32 extraImages)
    : ctx_(ctx), policy_(policy), extraImages_(extraImages) {
    create(extent, VK_NULL_HANDLE);
}

Swapchain::~Swapchain() {
    destroyViews();
    if (handle) vkDestroySwapchainKHR(ctx_.device, handle, nullptr);
}

void Swapchain::recreate(Extent2D e) {
    VkSwapchainKHR old = handle;
    destroyViews();
    create(e, old);
    vkDestroySwapchainKHR(ctx_.device, old, nullptr);
}

void Swapchain::destroyViews() {
    for (VkImageView v : views) vkDestroyImageView(ctx_.device, v, nullptr);
    for (VkSemaphore s : renderFinished) vkDestroySemaphore(ctx_.device, s, nullptr);
    views.clear();
    renderFinished.clear();
}

void Swapchain::create(Extent2D want, VkSwapchainKHR old) {
    VkSurfaceCapabilitiesKHR sc;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_.physicalDevice, ctx_.surface, &sc));

    u32 n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physicalDevice, ctx_.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physicalDevice, ctx_.surface, &n, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (auto f : formats) // UNORM target: tonemapping / sRGB encode happens in the post pass
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }

    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_.physicalDevice, ctx_.surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_.physicalDevice, ctx_.surface, &n, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (policy_ == PresentPolicy::LowLatency && has(VK_PRESENT_MODE_MAILBOX_KHR)) mode = VK_PRESENT_MODE_MAILBOX_KHR;
    if (policy_ == PresentPolicy::Uncapped && has(VK_PRESENT_MODE_IMMEDIATE_KHR)) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;

    if (sc.currentExtent.width != 0xFFFFFFFFu) {
        extent = sc.currentExtent;
    } else {
        extent.width = std::clamp(want.width, sc.minImageExtent.width, sc.maxImageExtent.width);
        extent.height = std::clamp(want.height, sc.minImageExtent.height, sc.maxImageExtent.height);
    }

    u32 imageCount = std::max(3u, sc.minImageCount + extraImages_);
    if (sc.maxImageCount) imageCount = std::min(imageCount, sc.maxImageCount);

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = ctx_.surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    (sc.supportedUsageFlags & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = sc.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    VkSwapchainLatencyCreateInfoNV latency{VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV};
    latency.latencyModeEnable = VK_TRUE;
    if (ctx_.caps.lowLatency) ci.pNext = &latency;
    VK_CHECK(vkCreateSwapchainKHR(ctx_.device, &ci, nullptr, &handle));
    format = chosen.format;
    presentMode = mode;

    vkGetSwapchainImagesKHR(ctx_.device, handle, &n, nullptr);
    images.resize(n);
    vkGetSwapchainImagesKHR(ctx_.device, handle, &n, images.data());

    views.resize(n);
    renderFinished.resize(n);
    for (u32 i = 0; i < n; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(ctx_.device, &vi, nullptr, &views[i]));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(ctx_.device, &si, nullptr, &renderFinished[i]));
    }
    static const char* kModeName[] = {"IMMEDIATE", "MAILBOX", "FIFO", "FIFO_RELAXED"};
    log::info("Swapchain {}x{}, {} images, {}", extent.width, extent.height, n,
              u32(mode) < 4 ? kModeName[mode] : "other");
    if (ctx_.caps.lowLatency) setLowLatency(lowLatency_); // the mode belongs to the swapchain: re-apply
}

void Swapchain::setLowLatency(bool enabled) {
    lowLatency_ = enabled && ctx_.caps.lowLatency;
    if (!ctx_.caps.lowLatency) return;
    VkLatencySleepModeInfoNV mode{VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
    mode.lowLatencyMode = lowLatency_;
    mode.lowLatencyBoost = lowLatency_;
    mode.minimumIntervalUs = 0;
    VK_CHECK(vkSetLatencySleepModeNV(ctx_.device, handle, &mode));
}

} // namespace phx
