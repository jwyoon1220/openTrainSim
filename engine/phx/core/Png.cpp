#include "phx/core/Png.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace phx {
namespace {
u32 crcTable[256];
bool crcInit = [] {
    for (u32 n = 0; n < 256; ++n) {
        u32 c = n;
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTable[n] = c;
    }
    return true;
}();
u32 crc(const u8* p, std::size_t n, u32 c = 0xFFFFFFFFu) {
    for (std::size_t i = 0; i < n; ++i) c = crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}
void be32(std::vector<u8>& v, u32 x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(u8(x >> s));
}
void chunk(std::FILE* f, const char* type, const std::vector<u8>& data) {
    std::vector<u8> buf;
    be32(buf, u32(data.size()));
    buf.insert(buf.end(), type, type + 4);
    buf.insert(buf.end(), data.begin(), data.end());
    const u32 c = crc(buf.data() + 4, buf.size() - 4) ^ 0xFFFFFFFFu;
    be32(buf, c);
    std::fwrite(buf.data(), 1, buf.size(), f);
}
} // namespace

bool writePngRgb(const std::string& path, u32 w, u32 h, const u8* rgb) {
    (void)crcInit;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    static const u8 sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    std::fwrite(sig, 1, 8, f);
    std::vector<u8> ihdr;
    be32(ihdr, w);
    be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8-bit RGB
    chunk(f, "IHDR", ihdr);

    std::vector<u8> raw;
    raw.reserve(std::size_t(h) * (w * 3 + 1));
    for (u32 y = 0; y < h; ++y) {
        raw.push_back(0); // filter: none
        raw.insert(raw.end(), rgb + std::size_t(y) * w * 3, rgb + std::size_t(y + 1) * w * 3);
    }
    std::vector<u8> z = {0x78, 0x01};
    u32 a = 1, b = 0; // adler32
    for (u8 c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
    for (std::size_t off = 0; off < raw.size(); off += 65535) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - off);
        z.push_back(off + n == raw.size() ? 1 : 0);
        z.push_back(u8(n)), z.push_back(u8(n >> 8)), z.push_back(u8(~n)), z.push_back(u8(~n >> 8));
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(off), raw.begin() + std::ptrdiff_t(off + n));
    }
    be32(z, (b << 16) | a);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    return std::fclose(f) == 0;
}
} // namespace phx
