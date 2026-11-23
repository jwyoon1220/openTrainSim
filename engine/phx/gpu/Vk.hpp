#pragma once
// Single include point for Vulkan: volk (function loader) + VMA.
#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include "phx/core/Log.hpp"

const char* phxVkResultString(VkResult r);

#define VK_CHECK(expr)                                                                             \
    do {                                                                                           \
        const VkResult phx_vk_result_ = (expr);                                                    \
        if (phx_vk_result_ != VK_SUCCESS)                                                          \
            PHX_FATAL("{} failed: {} ({}:{})", #expr, phxVkResultString(phx_vk_result_), __FILE__, \
                      __LINE__);                                                                   \
    } while (0)
