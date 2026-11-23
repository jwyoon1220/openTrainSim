#include "phx/gpu/Textures.hpp"

#include "phx/gpu/VulkanContext.hpp"

#define STB_IMAGE_IMPLEMENTATION
// All stb_image formats (BMP/GIF/TGA are common in legacy content); the implementation lives
// here once and games include <stb_image.h> for the declarations.
#include <stb_image.h>

#include <algorithm>
#include <bit>
#include <cstring>

namespace phx {

void loadTextures(VulkanContext& ctx, jobs::JobSystem& jobs, std::vector<TextureRequest>& reqs) {
    struct Decoded {
        int w = 0, h = 0;
        unsigned char* pixels = nullptr;
    };
    std::vector<Decoded> dec(reqs.size());
    jobs.parallelFor(u32(reqs.size()), 1, {.priority = jobs::Priority::Critical}, [&](u32 b, u32 e) {
        for (u32 i = b; i < e; ++i) {
            int c = 0;
            dec[i].pixels = stbi_load(reqs[i].path.c_str(), &dec[i].w, &dec[i].h, &c, 4);
        }
    });

    ImmediateSubmitter sub(ctx);
    for (std::size_t i = 0; i < reqs.size(); ++i) {
        Decoded& d = dec[i];
        static unsigned char fallback[16 * 4];
        if (!d.pixels) {
            log::warn("Texture missing or unreadable: {}", reqs[i].path);
            const bool normal = reqs[i].path.find("nor") != std::string::npos;
            for (int k = 0; k < 16; ++k) {
                fallback[k * 4 + 0] = normal ? 128 : 128;
                fallback[k * 4 + 1] = normal ? 128 : 128;
                fallback[k * 4 + 2] = normal ? 255 : 128;
                fallback[k * 4 + 3] = 255;
            }
            d.w = d.h = 4;
        }
        const u32 w = u32(d.w), h = u32(d.h);
        const u32 mips = u32(std::bit_width(std::max(w, h)));
        const VkFormat fmt = reqs[i].srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

        Image img;
        img.format = fmt;
        img.extent = {w, h};
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = fmt;
        ci.extent = {w, h, 1};
        ci.mipLevels = mips;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator, &ci, &ai, &img.image, &img.allocation, nullptr));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
        VK_CHECK(vkCreateImageView(ctx.device, &vi, nullptr, &img.view));

        const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
        Buffer staging = createBuffer(ctx, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::HostUpload);
        std::memcpy(staging.mapped, d.pixels ? d.pixels : fallback, bytes);
        vmaFlushAllocation(ctx.allocator, staging.allocation, 0, bytes);
        sub.run([&](VkCommandBuffer cmd) {
            auto barrier = [&](u32 mip, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                               VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
                VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                b.srcStageMask = ss, b.srcAccessMask = sa, b.dstStageMask = ds, b.dstAccessMask = da;
                b.oldLayout = from, b.newLayout = to;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = img.image;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers = &b;
                vkCmdPipelineBarrier2(cmd, &dep);
            };
            for (u32 m = 0; m < mips; ++m)
                barrier(m, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {w, h, 1};
            vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            i32 mw = i32(w), mh = i32(h);
            for (u32 m = 1; m < mips; ++m) {
                barrier(m - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
                blit.srcOffsets[1] = {mw, mh, 1};
                mw = std::max(1, mw / 2), mh = std::max(1, mh / 2);
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
                blit.dstOffsets[1] = {mw, mh, 1};
                vkCmdBlitImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &blit, VK_FILTER_LINEAR);
                barrier(m - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            }
            barrier(mips - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        destroyBuffer(ctx, staging);
        if (d.pixels) stbi_image_free(d.pixels);
        *reqs[i].out = img;
    }
}

} // namespace phx
