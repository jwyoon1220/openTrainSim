#pragma once
#include "phx/core/Types.hpp"
#include "phx/gpu/Vk.hpp"

#include <filesystem>
#include <string>

namespace phx {
class Window;

struct GpuQueue {
    VkQueue queue = VK_NULL_HANDLE;
    u32 family = ~0u;
    bool dedicated = false; // false = aliases the graphics queue
};

struct GpuCaps {
    std::string deviceName;
    u32 apiVersion = 0;
    u32 driverVersion = 0;
    bool discrete = false;
    bool meshShader = false;     // VK_EXT_mesh_shader available (enabled in M5)
    bool validation = false;     // validation layer active
    VkDeviceSize deviceLocalBytes = 0;
    bool memoryBudget = false;   // VK_EXT_memory_budget (process VRAM usage / budget)
    bool lowLatency = false;     // VK_NV_low_latency2 + VK_KHR_present_id (NVIDIA Reflex)
};

// Instance, device, queues (graphics / async compute / transfer) and VMA allocator.
// Baseline is Vulkan 1.3: dynamic rendering, synchronization2, timeline semaphores,
// descriptor indexing (bindless) and buffer device address.
class VulkanContext {
public:
    VulkanContext(const Window& window, bool enableValidation);
    ~VulkanContext();
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    // Shared by every pipeline creation. Persisted with usePipelineCacheFile(): later runs and
    // pipeline rebuilds (resize, AA change) skip shader compilation.
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    // Loads `file` into the cache (if it matches this GPU / driver) and saves back to it on
    // destruction.
    void usePipelineCacheFile(const std::filesystem::path& file);
    void savePipelineCache() const;

    GpuQueue graphics; // also presents
    GpuQueue compute;
    GpuQueue transfer;
    GpuCaps caps;

private:
    void createInstance(bool enableValidation);
    void pickPhysicalDevice();
    void createDevice();
    void createAllocator();

    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    std::filesystem::path pipelineCacheFile_;
};

} // namespace phx
