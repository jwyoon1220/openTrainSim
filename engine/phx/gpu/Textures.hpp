#pragma once
// Image textures from disk (JPG/PNG via stb_image), mip-mapped on the GPU.
#include "phx/gpu/Resources.hpp"
#include "phx/jobs/JobSystem.hpp"

#include <string>
#include <vector>

namespace phx {

struct TextureRequest {
    std::string path;
    bool srgb = true;
    Image* out = nullptr;
};

// Decodes all files in parallel on the job system, then uploads with full mip chains.
// Missing files become a 4×4 mid-grey (or flat normal) texture and are reported.
void loadTextures(VulkanContext& ctx, jobs::JobSystem& jobs, std::vector<TextureRequest>& requests);

} // namespace phx
