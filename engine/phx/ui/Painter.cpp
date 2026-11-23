#include "phx/ui/Ui.hpp"

#include <stb_easy_font.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace phx::ui {
namespace {
Rect intersect(Rect a, Rect b) {
    const f32 x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    const f32 r = std::min(a.x + a.w, b.x + b.w), d = std::min(a.y + a.h, b.y + b.h);
    return {x, y, std::max(0.0f, r - x), std::max(0.0f, d - y)};
}
} // namespace

Painter::Painter(std::vector<Vertex> &vertices, const Theme &theme, Rect clip)
    : vertices_(vertices), theme_(theme), clips_{clip} {
}

void Painter::pushClip(Rect clip) {
    clips_.push_back(intersect(clips_.back(), clip));
}
void Painter::popClip() {
    if (clips_.size() > 1)
        clips_.pop_back();
}

void Painter::fillRect(Rect rect, u32 colour) {
    rect = intersect(rect, clips_.back());
    if (rect.w <= 0 || rect.h <= 0)
        return;
    vertices_.insert(vertices_.end(), {{rect.x, rect.y, 0, colour},
                                       {rect.x + rect.w, rect.y, 0, colour},
                                       {rect.x + rect.w, rect.y + rect.h, 0, colour},
                                       {rect.x, rect.y + rect.h, 0, colour}});
}

void Painter::drawBorder(Rect r, u32 colour, f32 t) {
    fillRect({r.x, r.y, r.w, t}, colour);
    fillRect({r.x, r.y + r.h - t, r.w, t}, colour);
    fillRect({r.x, r.y + t, t, r.h - 2 * t}, colour);
    fillRect({r.x + r.w - t, r.y + t, t, r.h - 2 * t}, colour);
}

void Painter::drawText(f32 x, f32 y, std::string_view text, u32 colour) {
    if (text.empty())
        return;
    textBuffer_.assign(text.data(), text.size());
    drawTerminatedText(x, y, textBuffer_.data(), text.size(), colour);
}

void Painter::drawText(f32 x, f32 y, const std::string &text, u32 colour) {
    drawTerminatedText(x, y, text.c_str(), text.size(), colour);
}

void Painter::drawText(f32 x, f32 y, const char *text, u32 colour) {
    drawTerminatedText(x, y, text, std::strlen(text), colour);
}

void Painter::drawTerminatedText(f32 x, f32 y, const char *text, std::size_t length, u32 colour) {
    if (length == 0)
        return;
    glyphBuffer_.resize(length * 16 + 1);
    const int count = stb_easy_font_print(0, 0, const_cast<char *>(text), nullptr, glyphBuffer_.data(),
                                          static_cast<int>(glyphBuffer_.size() * sizeof(glyphBuffer_[0])));
    const f32 scale = theme_.textScale;
    for (int i = 0; i < count; ++i) {
        const f32 *q = glyphBuffer_[static_cast<std::size_t>(i)].data();
        fillRect({x + q[0] * scale, y + q[1] * scale, std::max(scale, (q[4] - q[0]) * scale),
                  std::max(scale, (q[9] - q[1]) * scale)},
                 colour);
    }
}

f32 Painter::textWidth(std::string_view text) const {
    return measureText(text, theme_);
}
f32 Painter::lineHeight() const {
    return 12.0f * theme_.textScale;
}

f32 measureText(std::string_view text, const Theme &theme) {
    if (text.empty()) {
        return 0;
    }
    thread_local std::string terminated;
    terminated.assign(text.data(), text.size());
    return static_cast<f32>(stb_easy_font_width(terminated.data())) * theme.textScale;
}
} // namespace phx::ui
