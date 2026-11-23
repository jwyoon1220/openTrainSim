#include "phx/gpu/BlockCompress.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

namespace phx::tex {
namespace {

// 2×2 box downsample with alpha weighting (colour of transparent texels does not bleed).
void downsample(const u8* src, u32 sw, u32 sh, std::vector<u8>& dst, u32& dw, u32& dh) {
    dw = std::max(1u, sw / 2);
    dh = std::max(1u, sh / 2);
    dst.resize(std::size_t(dw) * dh * 4);
    for (u32 y = 0; y < dh; ++y) {
        const u32 y0 = std::min(sh - 1, y * 2), y1 = std::min(sh - 1, y * 2 + 1);
        for (u32 x = 0; x < dw; ++x) {
            const u32 x0 = std::min(sw - 1, x * 2), x1 = std::min(sw - 1, x * 2 + 1);
            const u8* p[4] = {src + (std::size_t(y0) * sw + x0) * 4, src + (std::size_t(y0) * sw + x1) * 4,
                              src + (std::size_t(y1) * sw + x0) * 4, src + (std::size_t(y1) * sw + x1) * 4};
            u32 a = 0, r = 0, g = 0, b = 0, ur = 0, ug = 0, ub = 0;
            for (const u8* q : p) {
                a += q[3];
                r += u32(q[0]) * q[3];
                g += u32(q[1]) * q[3];
                b += u32(q[2]) * q[3];
                ur += q[0], ug += q[1], ub += q[2];
            }
            u8* o = dst.data() + (std::size_t(y) * dw + x) * 4;
            if (a > 0) {
                o[0] = u8((r + a / 2) / a);
                o[1] = u8((g + a / 2) / a);
                o[2] = u8((b + a / 2) / a);
            } else {
                o[0] = u8(ur / 4), o[1] = u8(ug / 4), o[2] = u8(ub / 4);
            }
            o[3] = u8((a + 2) / 4);
        }
    }
}

f32 coverage(const u8* rgba, u32 w, u32 h, f32 scale) {
    std::size_t n = 0;
    const std::size_t count = std::size_t(w) * h;
    for (std::size_t i = 0; i < count; ++i)
        if (f32(rgba[i * 4 + 3]) * scale >= 127.5f) ++n;
    return f32(n) / f32(count);
}

// Scales a mip's alpha so its alpha-test coverage matches the top level (Castaño 2010).
void preserveCoverage(u8* rgba, u32 w, u32 h, f32 target) {
    f32 lo = 0.0f, hi = 8.0f, best = 1.0f;
    for (int it = 0; it < 12; ++it) {
        const f32 mid = 0.5f * (lo + hi);
        if (coverage(rgba, w, h, mid) < target) lo = mid;
        else hi = mid, best = mid;
    }
    const std::size_t count = std::size_t(w) * h;
    for (std::size_t i = 0; i < count; ++i) rgba[i * 4 + 3] = u8(std::min(255.0f, f32(rgba[i * 4 + 3]) * best + 0.5f));
}

void compressLevel(const u8* rgba, u32 w, u32 h, bool alpha, std::vector<u8>& out) {
    const u32 bw = (w + 3) / 4, bh = (h + 3) / 4;
    const u32 blockBytes = alpha ? 16 : 8;
    const std::size_t base = out.size();
    out.resize(base + std::size_t(bw) * bh * blockBytes);
    u8 block[64];
    for (u32 by = 0; by < bh; ++by)
        for (u32 bx = 0; bx < bw; ++bx) {
            for (u32 y = 0; y < 4; ++y)
                for (u32 x = 0; x < 4; ++x) {
                    const u32 sx = std::min(w - 1, bx * 4 + x), sy = std::min(h - 1, by * 4 + y);
                    std::memcpy(block + (y * 4 + x) * 4, rgba + (std::size_t(sy) * w + sx) * 4, 4);
                }
            stb_compress_dxt_block(out.data() + base + (std::size_t(by) * bw + bx) * blockBytes, block, alpha ? 1 : 0,
                                   STB_DXT_HIGHQUAL);
        }
}

} // namespace

void analyseAlpha(const u8* rgba, u32 w, u32 h, bool& hasAlpha, bool& translucent) {
    hasAlpha = translucent = false;
    const std::size_t count = std::size_t(w) * h;
    for (std::size_t i = 0; i < count; ++i) {
        const u8 a = rgba[i * 4 + 3];
        if (a != 255) {
            hasAlpha = true;
            if (a != 0) {
                translucent = true;
                return;
            }
        }
    }
}

void bleedTransparentColours(u8* rgba, u32 w, u32 h, u32 passes) {
    std::vector<u8> known(std::size_t(w) * h);
    std::size_t unknown = 0;
    for (std::size_t i = 0; i < known.size(); ++i) {
        known[i] = rgba[i * 4 + 3] != 0;
        unknown += !known[i];
    }
    if (unknown == 0 || unknown == known.size()) {
        if (unknown == known.size()) // all transparent: neutral colour
            for (std::size_t i = 0; i < known.size(); ++i) rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 0;
        return;
    }
    std::vector<u8> next = known;
    for (u32 pass = 0; pass < passes; ++pass) {
        bool changed = false;
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) {
                const std::size_t i = std::size_t(y) * w + x;
                if (known[i]) continue;
                u32 r = 0, g = 0, b = 0, n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = int(x) + dx, ny = int(y) + dy;
                        if (nx < 0 || ny < 0 || nx >= int(w) || ny >= int(h)) continue;
                        const std::size_t j = std::size_t(ny) * w + std::size_t(nx);
                        if (!known[j]) continue;
                        r += rgba[j * 4], g += rgba[j * 4 + 1], b += rgba[j * 4 + 2], ++n;
                    }
                if (n) {
                    rgba[i * 4] = u8(r / n), rgba[i * 4 + 1] = u8(g / n), rgba[i * 4 + 2] = u8(b / n);
                    next[i] = 1;
                    changed = true;
                }
            }
        known = next;
        if (!changed) break;
    }
    // Remaining far-away transparent texels: the average opaque colour.
    u64 r = 0, g = 0, b = 0, n = 0;
    for (std::size_t i = 0; i < known.size(); ++i)
        if (known[i]) r += rgba[i * 4], g += rgba[i * 4 + 1], b += rgba[i * 4 + 2], ++n;
    if (!n) return;
    for (std::size_t i = 0; i < known.size(); ++i)
        if (!known[i]) rgba[i * 4] = u8(r / n), rgba[i * 4 + 1] = u8(g / n), rgba[i * 4 + 2] = u8(b / n);
}

PreparedTexture prepare(std::vector<u8>& rgba, u32 w, u32 h, const PrepareOptions& opt) {
    PreparedTexture out;
    analyseAlpha(rgba.data(), w, h, out.hasAlpha, out.translucent);
    if (out.hasAlpha) bleedTransparentColours(rgba.data(), w, h);

    std::vector<u8> tmp;
    while (std::max(w, h) > opt.maxSize && std::max(w, h) > 1) {
        u32 nw, nh;
        downsample(rgba.data(), w, h, tmp, nw, nh);
        rgba.swap(tmp);
        w = nw, h = nh;
    }
    out.width = w, out.height = h;

    // BC needs at least one full block for good results; tiny textures stay uncompressed.
    const bool bc = opt.compression != Compression::None && w >= 4 && h >= 4;
    const bool alpha = out.hasAlpha;
    out.format = !bc ? VK_FORMAT_R8G8B8A8_UNORM : (alpha ? VK_FORMAT_BC3_UNORM_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK);

    const f32 topCoverage = (opt.alphaTest && alpha) ? coverage(rgba.data(), w, h, 1.0f) : 0.0f;
    std::vector<u8> level = std::move(rgba), smaller;
    u32 lw = w, lh = h;
    for (;;) {
        MipLevel m;
        m.offset = u32(out.data.size());
        m.width = lw, m.height = lh;
        if (bc) {
            compressLevel(level.data(), lw, lh, alpha, out.data);
        } else {
            out.data.insert(out.data.end(), level.begin(), level.begin() + std::ptrdiff_t(std::size_t(lw) * lh * 4));
        }
        m.size = u32(out.data.size()) - m.offset;
        out.levels.push_back(m);
        if (!opt.mipmaps || (lw == 1 && lh == 1)) break;
        // BC: stop at the last level that still holds a whole block row (mips < 4 texels are
        // valid but compress poorly and are rarely sampled).
        u32 nw, nh;
        downsample(level.data(), lw, lh, smaller, nw, nh);
        if (opt.alphaTest && alpha && topCoverage > 0.0f) preserveCoverage(smaller.data(), nw, nh, topCoverage);
        level.swap(smaller);
        lw = nw, lh = nh;
    }
    return out;
}

} // namespace phx::tex
