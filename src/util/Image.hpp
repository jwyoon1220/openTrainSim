#pragma once
// Image decoding for GPU-free code (DEM tiles). The renderer has its own texture loader.
#include "phx/core/Types.hpp"

#include <filesystem>
#include <optional>
#include <vector>

namespace ots {

struct RgbImage {
    phx::u32 width = 0, height = 0;
    std::vector<phx::u8> pixels; // RGB8, top row first
};

// PNG / JPG / BMP ... as RGB8; nullopt if the file cannot be read or decoded.
std::optional<RgbImage> loadRgbImage(const std::filesystem::path& file);

} // namespace ots
