#include "phx/gpu/VulkanContext.hpp"

#include "phx/core/Log.hpp"
#include "phx/platform/Window.hpp"
#include "phx/render/Upscaler.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

const char* phxVkResultString(VkResult r) {
    switch (r) {
#define PHX_CASE(x) \
    case x: return #x;
        PHX_CASE(VK_SUCCESS)
        PHX_CASE(VK_NOT_READY)
        PHX_CASE(VK_TIMEOUT)
        PHX_CASE(VK_INCOMPLETE)
        PHX_CASE(VK_SUBOPTIMAL_KHR)
        PHX_CASE(VK_ERROR_OUT_OF_HOST_MEMORY)
        PHX_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        PHX_CASE(VK_ERROR_INITIALIZATION_FAILED)
        PHX_CASE(VK_ERROR_DEVICE_LOST)
        PHX_CASE(VK_ERROR_LAYER_NOT_PRESENT)
        PHX_CASE(VK_ERROR_EXTENSION_NOT_PRESENT)
        PHX_CASE(VK_ERROR_FEATURE_NOT_PRESENT)
        PHX_CASE(VK_ERROR_INCOMPATIBLE_DRIVER)
        PHX_CASE(VK_ERROR_SURFACE_LOST_KHR)
        PHX_CASE(VK_ERROR_OUT_OF_DATE_KHR)
#undef PHX_CASE
    default: return "VkResult(?)";
    }
}

namespace phx {
namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

bool hasLayer(const char* name) {
    u32 n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> layers(n);
    vkEnumerateInstanceLayerProperties(&n, layers.data());
    return std::any_of(layers.begin(), layers.end(), [&](auto& l) { return std::strcmp(l.layerName, name) == 0; });
}

bool hasInstanceExtension(const char* name) {
    u32 n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data());
    return std::any_of(exts.begin(), exts.end(), [&](auto& e) { return std::strcmp(e.extensionName, name) == 0; });
}

std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice pd) {
    u32 n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, exts.data());
    return exts;
}

bool hasExt(const std::vector<VkExtensionProperties>& exts, const char* name) {
    return std::any_of(exts.begin(), exts.end(), [&](auto& e) { return std::strcmp(e.extensionName, name) == 0; });
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        log::error("[vk] {}", data->pMessage);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        log::warn("[vk] {}", data->pMessage);
    return VK_FALSE;
}

struct Features {
    VkPhysicalDeviceFeatures2 core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    Features() {
        core.pNext = &v12;
        v12.pNext = &v13;
    }
    Features(const Features&) = delete;
};

// Features the engine relies on (plan §2.4).
bool supportsRequired(const Features& f) {
    return f.v13.dynamicRendering && f.v13.synchronization2 && f.v13.maintenance4 && f.v12.timelineSemaphore &&
           f.v12.bufferDeviceAddress && f.v12.descriptorIndexing && f.v12.runtimeDescriptorArray &&
           f.v12.descriptorBindingPartiallyBound && f.v12.descriptorBindingVariableDescriptorCount &&
           f.v12.shaderSampledImageArrayNonUniformIndexing && f.v12.descriptorBindingSampledImageUpdateAfterBind && f.v12.descriptorBindingStorageImageUpdateAfterBind &&
           f.v12.descriptorBindingUpdateUnusedWhilePending &&
           f.v12.drawIndirectCount && f.v12.scalarBlockLayout && f.v12.hostQueryReset &&
           f.core.features.multiDrawIndirect && f.core.features.samplerAnisotropy &&
           f.core.features.textureCompressionBC && f.core.features.shaderInt64;
}

} // namespace

VulkanContext::VulkanContext(const Window& window, bool enableValidation) {
    VK_CHECK(volkInitialize());
    createInstance(enableValidation);
    VK_CHECK(glfwCreateWindowSurface(instance, window.handle(), nullptr, &surface));
    pickPhysicalDevice();
    createDevice();
    createAllocator();
    log::info("GPU: {} (Vulkan {}.{}.{}), {} MiB device-local, mesh shader: {}", caps.deviceName,
              VK_API_VERSION_MAJOR(caps.apiVersion), VK_API_VERSION_MINOR(caps.apiVersion),
              VK_API_VERSION_PATCH(caps.apiVersion), caps.deviceLocalBytes >> 20, caps.meshShader ? "yes" : "no");
    log::info("Queues: graphics={} compute={}{} transfer={}{}", graphics.family, compute.family,
              compute.dedicated ? " (async)" : " (shared)", transfer.family,
              transfer.dedicated ? " (dedicated)" : " (shared)");
}

VulkanContext::~VulkanContext() {
    if (device) vkDeviceWaitIdle(device);
    if (allocator) vmaDestroyAllocator(allocator);
    if (device) vkDestroyDevice(device, nullptr);
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    if (messenger_) vkDestroyDebugUtilsMessengerEXT(instance, messenger_, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
}

void VulkanContext::createInstance(bool enableValidation) {
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) PHX_FATAL("Vulkan 1.3 loader required");

    u32 glfwCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwCount);
    std::vector<const char*> extensions(glfwExts, glfwExts + glfwCount);
    std::vector<const char*> layers;

    {   // Upscaler (NGX/DLSS) requirements, where the loader offers them.
        std::vector<const char*> ngx;
        Upscaler::instanceExtensions(ngx);
        for (const char* e : ngx)
            if (hasInstanceExtension(e) && std::none_of(extensions.begin(), extensions.end(), [&](const char* x) { return !std::strcmp(x, e); }))
                extensions.push_back(e);
    }
    const bool debugUtils = hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debugUtils) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (enableValidation) {
        if (hasLayer(kValidationLayer)) {
            layers.push_back(kValidationLayer);
            caps.validation = true;
        } else {
            log::warn("Validation requested but {} is not installed (package: vulkan-validation-layers)",
                      kValidationLayer);
        }
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "phx";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "phx";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dbg.pfnUserCallback = debugCallback;

    // Synchronization validation is part of the plan's debug preset (§6).
    VkValidationFeatureEnableEXT syncValidation = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures = &syncValidation;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = u32(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount = u32(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    if (caps.validation) {
        validationFeatures.pNext = debugUtils ? &dbg : nullptr;
        ci.pNext = &validationFeatures;
    }
    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance));
    volkLoadInstanceOnly(instance);

    if (debugUtils && caps.validation) VK_CHECK(vkCreateDebugUtilsMessengerEXT(instance, &dbg, nullptr, &messenger_));
}

void VulkanContext::pickPhysicalDevice() {
    u32 n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance, &n, devices.data());

    int bestScore = -1;
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;

        const auto exts = deviceExtensions(pd);
        if (!hasExt(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

        Features f;
        vkGetPhysicalDeviceFeatures2(pd, &f.core);
        if (!supportsRequired(f)) {
            log::info("Skipping {}: missing required features", props.deviceName);
            continue;
        }

        u32 qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        bool canPresent = false;
        for (u32 i = 0; i < qn && !canPresent; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present);
            canPresent = present;
        }
        if (!canPresent) continue;

        int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 1000
                    : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 100
                                                                                 : 1;
        if (score > bestScore) {
            bestScore = score;
            physicalDevice = pd;
        }
    }
    if (!physicalDevice) PHX_FATAL("No Vulkan 1.3 GPU with the required features was found");

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    caps.deviceName = props.deviceName;
    caps.apiVersion = props.apiVersion;
    caps.driverVersion = props.driverVersion;
    caps.discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    caps.meshShader = hasExt(deviceExtensions(physicalDevice), VK_EXT_MESH_SHADER_EXTENSION_NAME);

    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &mem);
    for (u32 i = 0; i < mem.memoryHeapCount; ++i)
        if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            caps.deviceLocalBytes = std::max(caps.deviceLocalBytes, mem.memoryHeaps[i].size);
}

void VulkanContext::createDevice() {
    u32 qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> families(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qn, families.data());

    std::optional<u32> gfx, comp, xfer;
    for (u32 i = 0; i < qn; ++i) {
        const auto flags = families[i].queueFlags;
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface, &present);
        if (!gfx && (flags & VK_QUEUE_GRAPHICS_BIT) && (flags & VK_QUEUE_COMPUTE_BIT) && present) gfx = i;
        if (!comp && (flags & VK_QUEUE_COMPUTE_BIT) && !(flags & VK_QUEUE_GRAPHICS_BIT)) comp = i;
        if (!xfer && (flags & VK_QUEUE_TRANSFER_BIT) && !(flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
            xfer = i;
    }
    if (!gfx) PHX_FATAL("No graphics+compute queue family that can present");

    graphics = {VK_NULL_HANDLE, *gfx, true};
    compute = {VK_NULL_HANDLE, comp.value_or(*gfx), comp.has_value()};
    transfer = {VK_NULL_HANDLE, xfer.value_or(compute.family), xfer.has_value()};

    const float priorities[] = {1.0f};
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    for (u32 family : {graphics.family, compute.family, transfer.family}) {
        if (std::any_of(queueInfos.begin(), queueInfos.end(), [&](auto& q) { return q.queueFamilyIndex == family; }))
            continue;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = priorities;
        queueInfos.push_back(qi);
    }

    Features f; // request exactly what supportsRequired() checked
    f.core.features.multiDrawIndirect = VK_TRUE;
    f.core.features.samplerAnisotropy = VK_TRUE;
    f.core.features.textureCompressionBC = VK_TRUE;
    f.core.features.shaderInt64 = VK_TRUE;
    f.v12.timelineSemaphore = VK_TRUE;
    f.v12.bufferDeviceAddress = VK_TRUE;
    f.v12.descriptorIndexing = VK_TRUE;
    f.v12.runtimeDescriptorArray = VK_TRUE;
    f.v12.descriptorBindingPartiallyBound = VK_TRUE;
    f.v12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    f.v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f.v12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f.v12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    f.v12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    f.v12.drawIndirectCount = VK_TRUE;
    f.v12.scalarBlockLayout = VK_TRUE;
    f.v12.hostQueryReset = VK_TRUE;
    f.v13.dynamicRendering = VK_TRUE;
    f.v13.synchronization2 = VK_TRUE;
    f.v13.maintenance4 = VK_TRUE;

    std::vector<const char*> extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    caps.memoryBudget = hasExt(deviceExtensions(physicalDevice), VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (caps.memoryBudget) extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    // NVIDIA Reflex (latency sleep + markers) needs present IDs on the swapchain.
    VkPhysicalDevicePresentIdFeaturesKHR presentId{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    {
        const auto available = deviceExtensions(physicalDevice);
        VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        q.pNext = &presentId;
        vkGetPhysicalDeviceFeatures2(physicalDevice, &q);
        caps.lowLatency = hasExt(available, VK_NV_LOW_LATENCY_2_EXTENSION_NAME) && hasExt(available, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
                          presentId.presentId;
        presentId.pNext = nullptr;
        if (caps.lowLatency) {
            extensions.push_back(VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
            extensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
            f.v13.pNext = &presentId; // presentId = VK_TRUE from the query
        }
    }
    {   // Upscaler (NGX/DLSS) requirements. VK_EXT_buffer_device_address is skipped: the
        // core 1.2 feature is enabled and the two must not be combined.
        const auto available = deviceExtensions(physicalDevice);
        std::vector<const char*> ngx;
        Upscaler::deviceExtensions(ngx);
        std::string list;
        for (const char* e : ngx) {
            if (!std::strcmp(e, "VK_EXT_buffer_device_address") || !hasExt(available, e)) continue;
            if (std::none_of(extensions.begin(), extensions.end(), [&](const char* x) { return !std::strcmp(x, e); })) {
                extensions.push_back(e);
                list += std::string(" ") + e;
            }
        }
        if (!list.empty()) log::info("Device extensions for NGX:{}", list);
    }

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f.core;
    ci.queueCreateInfoCount = u32(queueInfos.size());
    ci.pQueueCreateInfos = queueInfos.data();
    ci.enabledExtensionCount = u32(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    VK_CHECK(vkCreateDevice(physicalDevice, &ci, nullptr, &device));
    volkLoadDevice(device);

    vkGetDeviceQueue(device, graphics.family, 0, &graphics.queue);
    vkGetDeviceQueue(device, compute.family, 0, &compute.queue);
    vkGetDeviceQueue(device, transfer.family, 0, &transfer.queue);
}

void VulkanContext::createAllocator() {
    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo ci{};
    ci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT | (caps.memoryBudget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0);
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    ci.physicalDevice = physicalDevice;
    ci.device = device;
    ci.instance = instance;
    ci.pVulkanFunctions = &fns;
    VK_CHECK(vmaCreateAllocator(&ci, &allocator));
}

} // namespace phx
