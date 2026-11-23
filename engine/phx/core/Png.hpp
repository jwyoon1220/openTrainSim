#pragma once
#include "phx/core/Types.hpp"

#include <string>

namespace phx {
// Minimal dependency-free PNG writer (stored deflate blocks). RGB8 input, top row first.
bool writePngRgb(const std::string& path, u32 width, u32 height, const u8* rgb);
} // namespace phx
