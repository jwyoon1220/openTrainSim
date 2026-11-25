#pragma once
// TrueType / OpenType text for phx/ui (stb_truetype). Glyphs are rasterised on demand at the
// exact pixel size into one R8 coverage atlas; phx::UiRenderer uploads the rows that changed.
// Faces form a fallback chain (e.g. a Latin face, then a CJK face for Hangul): each character
// is drawn with the first face that has it. Main thread only.
#include "phx/core/Types.hpp"

#include <filesystem>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace phx::ui {

// One glyph to draw: screen rectangle (pixels) and atlas UVs.
struct GlyphQuad {
    f32 x0, y0, x1, y1;
    f32 u0, v0, u1, v1;
};

struct FontMetrics {
    f32 ascent = 0, descent = 0, lineGap = 0; // pixels; descent is negative
};

class FontAtlas {
public:
    static constexpr u32 kSize = 2048; // atlas width = height (texels)

    FontAtlas();
    ~FontAtlas();
    FontAtlas(const FontAtlas&) = delete;
    FontAtlas& operator=(const FontAtlas&) = delete;

    // Appends a face to the fallback chain (.ttf / .otf / .ttc with `index`). False if the file
    // cannot be read or parsed.
    bool addFace(const std::filesystem::path& file, int index = 0);
    // Appends the first readable system face of each kind (UI Latin, then Korean / CJK), once.
    // Returns the number of faces added.
    int addSystemFaces();
    // True once a face is available; the first call without any face tries addSystemFaces().
    [[nodiscard]] bool available();
    [[nodiscard]] std::size_t faceCount() const { return faces_.size(); }

    // Metrics of the first face at `px` pixels per em.
    [[nodiscard]] FontMetrics metrics(f32 px);
    // Advance width of UTF-8 text (pixels, kerning included).
    [[nodiscard]] f32 measure(std::string_view utf8, f32 px);
    // Lays out UTF-8 text with the pen at (x, baseline), pixel-snapped; appends one quad per
    // visible glyph. Returns the pen position after the text.
    f32 layout(f32 x, f32 baseline, std::string_view utf8, f32 px, std::vector<GlyphQuad>& out);
    // Whether every character of the text has a glyph in some face.
    [[nodiscard]] bool covers(std::string_view utf8);

    // Atlas texels (kSize × kSize, row-major). Texel (1, 1) is opaque white: solid fills sample it.
    [[nodiscard]] const u8* pixels() const { return pixels_.data(); }
    [[nodiscard]] static constexpr f32 whiteU() { return 1.5f / f32(kSize); }
    // Bumped when the atlas is cleared (full): cached vertices with glyph UVs are stale.
    [[nodiscard]] u32 generation() const { return generation_; }
    // Rows written since the last call: [y0, y1). Empty when y1 <= y0.
    struct Band {
        u32 y0 = 0, y1 = 0;
        [[nodiscard]] bool empty() const { return y1 <= y0; }
    };
    Band takeDirty();

private:
    struct Face;
    struct Glyph {
        f32 advance = 0;      // pixels
        i32 x0 = 0, y0 = 0;   // bitmap offset from the pen at the baseline
        u32 w = 0, h = 0;     // bitmap size
        u32 ax = 0, ay = 0;   // atlas position
        u32 face = 0, index = 0;
    };
    struct Shelf {
        u32 y, h, x;
    };
    const Glyph* glyph(u32 codepoint, f32 px);
    bool place(u32 w, u32 h, u32& x, u32& y);
    void reset();
    void markDirty(u32 y0, u32 y1);

    std::vector<std::unique_ptr<Face>> faces_;
    bool triedSystem_ = false;
    std::vector<u8> pixels_;
    std::vector<Shelf> shelves_;
    u32 nextShelfY_ = 0;
    std::unordered_map<u64, Glyph> glyphs_; // key: codepoint | pixel size (quarter px) << 32
    u32 generation_ = 1;
    Band dirty_{};
};

// The process-wide atlas used by Painter / measureText and uploaded by UiRenderer.
FontAtlas& fonts();

// Next code point of UTF-8 text (invalid bytes → U+FFFD); advances `i`.
u32 decodeUtf8(std::string_view s, std::size_t& i);

} // namespace phx::ui
