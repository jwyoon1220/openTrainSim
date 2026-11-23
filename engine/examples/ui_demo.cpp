// Minimal engine client: window + Vulkan + swapchain, the UI library drawn through
// UiRenderer, and the post-processing modules created (DLSS probe, AA, bloom) — the
// skeleton a new game starts from. One frame in flight for brevity.
//   ./phx_ui_demo [frames]   (frames > 0: quit after that many frames; smoke test)
#include "phx/core/Log.hpp"
#include "phx/gpu/Resources.hpp"
#include "phx/gpu/Swapchain.hpp"
#include "phx/gpu/VulkanContext.hpp"
#include "phx/platform/UiInput.hpp"
#include "phx/platform/Window.hpp"
#include "phx/render/AntiAliasing.hpp"
#include "phx/render/Bloom.hpp"
#include "phx/render/UiRenderer.hpp"
#include "phx/render/Upscaler.hpp"
#include "phx/ui/Ui.hpp"

#include <cstdlib>
#include <format>
#include <memory>

using namespace phx;

int main(int argc, char** argv) {
    const long maxFrames = argc > 1 ? std::strtol(argv[1], nullptr, 10) : 0;
    Window window(1280, 720, "phx engine — UI demo");
    VulkanContext ctx(window, false);
    auto swapchain = std::make_unique<Swapchain>(ctx, window.framebufferSize(), PresentPolicy::Vsync);
    Bindless bindless(ctx);

    // Post-processing modules a game wires into its frame (see engine/README.md).
    Upscaler upscaler(ctx);
    AntiAliasing aa(ctx, bindless);
    Bloom bloom(ctx, bindless);
    log::info("DLSS: {}", upscaler.status());

    // UI: one window with a label and two buttons.
    ui::Ui gui;
    ui::Window& win = gui.addWindow("Hello from phx");
    ui::Label& label = win.content().add<ui::Label>("Clicks: 0");
    int clicks = 0;
    win.content().add<ui::Button>("Click me").addActionListener([&] { label.setText(std::format("Clicks: {}", ++clicks)); });
    win.content().add<ui::Button>("Quit").addActionListener([&] { window.requestClose(); });
    bool placed = false;
    std::vector<ui::Vertex> verts;
    auto uiRenderer = std::make_unique<UiRenderer>(ctx, bindless, swapchain->format, 1);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx.graphics.family;
    VkCommandPool pool;
    VK_CHECK(vkCreateCommandPool(ctx.device, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device, &ai, &cmd));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence;
    VK_CHECK(vkCreateFence(ctx.device, &fci, nullptr, &fence));
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquired;
    VK_CHECK(vkCreateSemaphore(ctx.device, &sci, nullptr, &acquired));

    auto recreate = [&] {
        vkDeviceWaitIdle(ctx.device);
        swapchain->recreate(window.framebufferSize());
        placed = false;
    };

    for (long frame = 0; !window.shouldClose() && (maxFrames <= 0 || frame < maxFrames); ++frame) {
        window.pollEvents();
        for (const InputEvent& e : window.drainInput().events) feedUi(gui, e);
        const Extent2D fb = window.framebufferSize();
        if (fb.empty()) {
            window.waitEvents(0.1);
            continue;
        }
        gui.setScreenSize(f32(fb.width), f32(fb.height));
        if (!placed) {
            win.pack();
            win.centre(f32(fb.width), f32(fb.height));
            placed = true;
        }

        VK_CHECK(vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_MAX));
        u32 image = 0;
        VkResult r = vkAcquireNextImageKHR(ctx.device, swapchain->handle, UINT64_MAX, acquired, VK_NULL_HANDLE, &image);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            recreate();
            continue;
        }
        VK_CHECK(vkResetFences(ctx.device, 1, &fence));
        if (gui.build(verts)) uiRenderer->upload(0, verts); // only when the UI changed

        VK_CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
        imageBarrier(cmd, swapchain->images[image], VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = swapchain->views[image];
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color = {{0.08f, 0.10f, 0.14f, 1.0f}};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, swapchain->extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport vp{0, 0, f32(swapchain->extent.width), f32(swapchain->extent.height), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &ri.renderArea);
        uiRenderer->record(cmd, 0, swapchain->extent);
        vkCmdEndRendering(cmd);
        imageBarrier(cmd, swapchain->images[image], VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        VK_CHECK(vkEndCommandBuffer(cmd));

        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait.semaphore = acquired;
        wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = swapchain->renderFinished[image];
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.waitSemaphoreInfoCount = 1;
        si.pWaitSemaphoreInfos = &wait;
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = &signal;
        VK_CHECK(vkQueueSubmit2(ctx.graphics.queue, 1, &si, fence));

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &swapchain->renderFinished[image];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain->handle;
        pi.pImageIndices = &image;
        r = vkQueuePresentKHR(ctx.graphics.queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || window.framebufferSize() != Extent2D{swapchain->extent.width, swapchain->extent.height})
            recreate();
    }

    vkDeviceWaitIdle(ctx.device);
    vkDestroySemaphore(ctx.device, acquired, nullptr);
    vkDestroyFence(ctx.device, fence, nullptr);
    vkDestroyCommandPool(ctx.device, pool, nullptr);
    return 0;
}
