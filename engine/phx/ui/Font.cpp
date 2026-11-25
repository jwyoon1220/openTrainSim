#include "phx/ui/Font.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <span>
#include <string>

namespace phx::ui {

struct FontAtlas::Face {
    std::vector<unsigned char> data; // stbtt_fontinfo points into it: never moved (Face is heap-held)
    stbtt_fontinfo info{};
};

namespace {

// Coverage → alpha. Blending happens on display-encoded colours, which makes linear coverage
// look thin and dark-fringed; a mild lift evens the stroke weight.
const std::array<u8, 256>& coverageCurve() {
    static const std::array<u8, 256> lut = [] {
        std::array<u8, 256> t{};
        for (std::size_t i = 0; i < t.size(); ++i) t[i] = u8(std::lround(255.0 * std::pow(double(i) / 255.0, 1.0 / 1.3)));
        return t;
    }();
    return lut;
}

std::vector<unsigned char> readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

u32 quarterPixels(f32 px) { return u32(std::lround(std::clamp(px, 1.0f, 512.0f) * 4.0f)); }

struct Candidate {
    const char* path;
    const char* name; // face name inside a collection (nullptr: index 0)
};

// UI faces first (Latin), then Korean-capable ones: the first readable file of each list.
constexpr std::array kLatin{
    Candidate{"/usr/share/fonts/noto/NotoSans-Regular.ttf", nullptr},
    Candidate{"/usr/share/fonts/noto-cjk/NotoSans-Regular.ttf", nullptr},
    Candidate{"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", nullptr},
    Candidate{"/usr/share/fonts/TTF/DejaVuSans.ttf", nullptr},
    Candidate{"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", nullptr},
    Candidate{"/usr/share/fonts/liberation/LiberationSans-Regular.ttf", nullptr},
    Candidate{"C:/Windows/Fonts/segoeui.ttf", nullptr},
    Candidate{"/System/Library/Fonts/SFNS.ttf", nullptr},
    Candidate{"/Library/Fonts/Arial.ttf", nullptr},
};
constexpr std::array kKorean{
    Candidate{"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", "Noto Sans CJK KR"},
    Candidate{"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "Noto Sans CJK KR"},
    Candidate{"/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", "Noto Sans CJK KR"},
    Candidate{"/usr/share/fonts/adobe-source-han-sans/SourceHanSansKR-Regular.otf", nullptr},
    Candidate{"/usr/share/fonts/truetype/nanum/NanumGothic.ttf", nullptr},
    Candidate{"/usr/share/fonts/nanum/NanumGothic.ttf", nullptr},
    Candidate{"C:/Windows/Fonts/malgun.ttf", nullptr},
    Candidate{"/System/Library/Fonts/AppleSDGothicNeo.ttc", nullptr},
};

} // namespace

u32 decodeUtf8(std::string_view s, std::size_t& i) {
    const u32 c = u8(s[i]);
    std::size_t n = 0;
    if (c < 0x80) return ++i, c;
    if ((c >> 5) == 0x6) n = 1;
    else if ((c >> 4) == 0xE) n = 2;
    else if ((c >> 3) == 0x1E) n = 3;
    else return ++i, 0xFFFD; // stray continuation / invalid lead byte
    if (i + n >= s.size()) return i = s.size(), 0xFFFD; // truncated
    u32 cp = c & (0x3Fu >> n);
    for (std::size_t k = 1; k <= n; ++k) {
        const u32 b = u8(s[i + k]);
        if ((b >> 6) != 0x2) return i += k, 0xFFFD;
        cp = (cp << 6) | (b & 0x3F);
    }
    i += n + 1;
    return cp;
}

FontAtlas::FontAtlas() : pixels_(std::size_t(kSize) * kSize, 0) { reset(); }
FontAtlas::~FontAtlas() = default;

bool FontAtlas::addFace(const std::filesystem::path& file, int index) {
    auto face = std::make_unique<Face>();
    face->data = readFile(file);
    if (face->data.empty()) return false;
    const int offset = stbtt_GetFontOffsetForIndex(face->data.data(), index);
    if (offset < 0 || !stbtt_InitFont(&face->info, face->data.data(), offset)) return false;
    faces_.push_back(std::move(face));
    return true;
}

int FontAtlas::addSystemFaces() {
    if (triedSystem_) return 0; // once (also tried lazily by available())
    triedSystem_ = true;
    int added = 0;
    for (const std::span<const Candidate> list : {std::span<const Candidate>(kLatin), std::span<const Candidate>(kKorean)}) {
        for (const Candidate& c : list) {
            std::error_code ec;
            if (!std::filesystem::exists(c.path, ec)) continue;
            int index = 0;
            if (c.name) { // collection: find the named face (Korean glyph forms for shared Han)
                const std::vector<unsigned char> data = readFile(c.path);
                for (int k = 0; !data.empty() && stbtt_GetFontOffsetForIndex(data.data(), k) >= 0; ++k) {
                    stbtt_fontinfo info{};
                    if (!stbtt_InitFont(&info, data.data(), stbtt_GetFontOffsetForIndex(data.data(), k))) continue;
                    int len = 0;
                    const char* n = stbtt_GetFontNameString(&info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
                                                            STBTT_MS_LANG_ENGLISH, 1); // family, UTF-16BE
                    std::string family;
                    for (int b = 1; n && b < len; b += 2) family += n[b];
                    if (family == c.name) {
                        index = k;
                        break;
                    }
                }
            }
            if (addFace(c.path, index)) {
                ++added;
                break;
            }
        }
    }
    return added;
}

bool FontAtlas::available() {
    if (faces_.empty() && !triedSystem_) addSystemFaces();
    return !faces_.empty();
}

FontMetrics FontAtlas::metrics(f32 px) {
    if (!available()) return {};
    const stbtt_fontinfo& f = faces_.front()->info;
    int a = 0, d = 0, g = 0;
    stbtt_GetFontVMetrics(&f, &a, &d, &g);
    const f32 s = stbtt_ScaleForMappingEmToPixels(&f, px);
    return {f32(a) * s, f32(d) * s, f32(g) * s};
}

const FontAtlas::Glyph* FontAtlas::glyph(u32 codepoint, f32 px) {
    const u64 key = u64(codepoint) | (u64(quarterPixels(px)) << 32);
    if (auto it = glyphs_.find(key); it != glyphs_.end()) return &it->second;
    Glyph g;
    int index = 0;
    for (u32 f = 0; f < faces_.size() && !index; ++f)
        if ((index = stbtt_FindGlyphIndex(&faces_[f]->info, int(codepoint)))) g.face = f;
    if (!index) { // nowhere: the first face's .notdef box (still advances)
        g.face = 0;
        index = 0;
    }
    g.index = u32(index);
    const stbtt_fontinfo& info = faces_[g.face]->info;
    const f32 size = f32(quarterPixels(px)) / 4.0f;
    const f32 scale = stbtt_ScaleForMappingEmToPixels(&info, size);
    int adv = 0, lsb = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphHMetrics(&info, index, &adv, &lsb);
    stbtt_GetGlyphBitmapBox(&info, index, scale, scale, &x0, &y0, &x1, &y1);
    g.advance = f32(adv) * scale;
    g.x0 = x0, g.y0 = y0;
    g.w = u32(std::max(0, x1 - x0)), g.h = u32(std::max(0, y1 - y0));
    if (g.w && g.h) {
        if (!place(g.w + 1, g.h + 1, g.ax, g.ay)) { // full: start over (callers re-layout every build)
            reset();
            return glyph(codepoint, px);
        }
        u8* dst = pixels_.data() + std::size_t(g.ay) * kSize + g.ax;
        stbtt_MakeGlyphBitmap(&info, dst, int(g.w), int(g.h), int(kSize), scale, scale, index);
        const auto& curve = coverageCurve();
        for (u32 y = 0; y < g.h; ++y)
            for (u32 x = 0; x < g.w; ++x) dst[std::size_t(y) * kSize + x] = curve[dst[std::size_t(y) * kSize + x]];
        markDirty(g.ay, g.ay + g.h);
    }
    return &glyphs_.emplace(key, g).first->second;
}

bool FontAtlas::place(u32 w, u32 h, u32& x, u32& y) {
    if (w > kSize || h > kSize) return false;
    // the best-fitting shelf (tallest waste ≤ 30%) with room left
    Shelf* best = nullptr;
    for (Shelf& s : shelves_)
        if (h <= s.h && h * 10 >= s.h * 7 && s.x + w <= kSize && (!best || s.h < best->h)) best = &s;
    if (!best) {
        if (nextShelfY_ + h > kSize) return false;
        shelves_.push_back({nextShelfY_, h, 0});
        nextShelfY_ += h;
        best = &shelves_.back();
    }
    x = best->x, y = best->y;
    best->x += w;
    return true;
}

void FontAtlas::reset() {
    std::fill(pixels_.begin(), pixels_.end(), u8(0));
    for (u32 y = 0; y < 3; ++y)
        for (u32 x = 0; x < 3; ++x) pixels_[std::size_t(y) * kSize + x] = 255; // white texel (1, 1) with a border
    glyphs_.clear();
    shelves_.clear();
    shelves_.push_back({0, 4, 4}); // the white block's row, for small glyphs
    nextShelfY_ = 4;
    ++generation_;
    markDirty(0, kSize);
}

void FontAtlas::markDirty(u32 y0, u32 y1) {
    if (dirty_.empty()) dirty_ = {y0, y1};
    else dirty_ = {std::min(dirty_.y0, y0), std::max(dirty_.y1, y1)};
}

FontAtlas::Band FontAtlas::takeDirty() {
    const Band b = dirty_;
    dirty_ = {};
    return b;
}

f32 FontAtlas::layout(f32 x, f32 baseline, std::string_view s, f32 px, std::vector<GlyphQuad>& out) {
    if (!available()) return x;
    const f32 inv = 1.0f / f32(kSize);
    const f32 by = std::round(baseline);
    f32 pen = x;
    const Glyph* prev = nullptr;
    for (std::size_t i = 0; i < s.size();) {
        const u32 cp = decodeUtf8(s, i);
        if (cp == '\n' || cp == '\r') continue;
        const Glyph* g = glyph(cp, px);
        if (prev && prev->face == g->face) {
            const stbtt_fontinfo& info = faces_[g->face]->info;
            pen += f32(stbtt_GetGlyphKernAdvance(&info, int(prev->index), int(g->index))) * stbtt_ScaleForMappingEmToPixels(&info, px);
        }
        if (g->w && g->h) {
            const f32 gx = std::round(pen) + f32(g->x0), gy = by + f32(g->y0);
            out.push_back({gx, gy, gx + f32(g->w), gy + f32(g->h), f32(g->ax) * inv, f32(g->ay) * inv, f32(g->ax + g->w) * inv,
                           f32(g->ay + g->h) * inv});
        }
        pen += g->advance;
        prev = g;
    }
    return pen;
}

f32 FontAtlas::measure(std::string_view s, f32 px) {
    if (!available()) return 0.0f;
    f32 pen = 0.0f;
    const Glyph* prev = nullptr;
    for (std::size_t i = 0; i < s.size();) {
        const u32 cp = decodeUtf8(s, i);
        if (cp == '\n' || cp == '\r') continue;
        const Glyph* g = glyph(cp, px);
        if (prev && prev->face == g->face) {
            const stbtt_fontinfo& info = faces_[g->face]->info;
            pen += f32(stbtt_GetGlyphKernAdvance(&info, int(prev->index), int(g->index))) * stbtt_ScaleForMappingEmToPixels(&info, px);
        }
        pen += g->advance;
        prev = g;
    }
    return pen;
}

bool FontAtlas::covers(std::string_view s) {
    if (!available()) return false;
    for (std::size_t i = 0; i < s.size();) {
        const u32 cp = decodeUtf8(s, i);
        bool found = false;
        for (const auto& f : faces_) found = found || stbtt_FindGlyphIndex(&f->info, int(cp)) != 0;
        if (!found) return false;
    }
    return true;
}

FontAtlas& fonts() {
    static FontAtlas atlas;
    return atlas;
}

} // namespace phx::ui
