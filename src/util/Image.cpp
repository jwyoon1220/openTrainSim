#include "util/Image.hpp"

#include "util/Paths.hpp"

// Private copy: the engine compiles its own stb_image into phx_engine, which GPU-free tools
// (ots_check) do not link.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include <stb_image.h>

#include <memory>

namespace ots {

std::optional<RgbImage> loadRgbImage(const std::filesystem::path& file) {
    int w = 0, h = 0, n = 0;
    const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> px(stbi_load(pathString(file).c_str(), &w, &h, &n, 3), &stbi_image_free);
    if (!px) return std::nullopt;
    RgbImage img;
    img.width = phx::u32(w), img.height = phx::u32(h);
    img.pixels.assign(px.get(), px.get() + std::size_t(w) * std::size_t(h) * 3);
    return img;
}

} // namespace ots
