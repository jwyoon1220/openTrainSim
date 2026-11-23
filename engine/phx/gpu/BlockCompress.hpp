#pragma once
// CPU texture preparation for low VRAM use: mip chain generation (alpha-weighted, with colour
// bleeding into transparent texels and alpha-test coverage preservation) and BC1/BC3 block
// compression (stb_dxt). BC1 is 4 bits/texel and BC3 8 bits/texel, against 32 for RGBA8.
// Thread-safe: meant to run on worker threads, the result is uploaded with UploadQueue.
#include "phx/core/Types.hpp"
#include "phx/gpu/Vk.hpp"

#include <vector>

namespace phx::tex {

enum class Compression : u8 { None, BC, Auto };

struct PrepareOptions {
    u32 maxSize = 4096;          // larger textures are box-downscaled (powers of two) to fit
    Compression compression = Compression::BC;
    bool mipmaps = true;
    bool alphaTest = false;      // keep the 0.5-threshold coverage of the top level in every mip
};

struct MipLevel {
    u32 offset = 0, size = 0;
    u32 width = 0, height = 0;
};

struct PreparedTexture {
    VkFormat format = VK_FORMAT_UNDEFINED;
    u32 width = 0, height = 0;
    std::vector<MipLevel> levels;
    std::vector<u8> data;
    bool hasAlpha = false;        // any texel with alpha < 255
    bool translucent = false;     // any texel with 0 < alpha < 255 (needs blending, not just a test)
    [[nodiscard]] std::size_t bytes() const { return data.size(); }
};

// Scans RGBA8 pixels for alpha usage.
void analyseAlpha(const u8* rgba, u32 w, u32 h, bool& hasAlpha, bool& translucent);
// Replaces the colour of fully transparent texels by that of nearby opaque ones so filtering
// and mip-mapping never pull in a colour key (in place).
void bleedTransparentColours(u8* rgba, u32 w, u32 h, u32 passes = 4);
// Full pipeline: downscale to maxSize, mips, compression. `rgba` is consumed (modified).
PreparedTexture prepare(std::vector<u8>& rgba, u32 w, u32 h, const PrepareOptions& opt);

} // namespace phx::tex
