#include "phx/ui/Font.hpp"
#include "phx/ui/Ui.hpp"

#include <stb_easy_font.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <string>

namespace phx::ui {
namespace {

Rect intersect(Rect a, Rect b) {
    const f32 x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    const f32 r = std::min(a.x + a.w, b.x + b.w), d = std::min(a.y + a.h, b.y + b.h);
    return {x, y, std::max(0.0f, r - x), std::max(0.0f, d - y)};
}

u32 packPixel(f32 x, f32 y) {
    const auto c = [](f32 v) { return u32(std::clamp(v, 0.0f, 65535.0f)); };
    return c(x) | (c(y) << 16u);
}

constexpr u32 transparent(u32 c) { return c & 0x00FFFFFFu; }

u32 withAlpha(u32 c, f32 scale) {
    const u32 a = u32(std::lround(f32(c >> 24u) * std::clamp(scale, 0.0f, 1.0f)));
    return transparent(c) | (a << 24u);
}

u32 lerpColour(u32 a, u32 b, f32 t) {
    u32 out = 0;
    for (u32 shift = 0; shift < 32; shift += 8) {
        const f32 ca = f32((a >> shift) & 0xFFu), cb = f32((b >> shift) & 0xFFu);
        out |= u32(std::lround(ca + (cb - ca) * t)) << shift;
    }
    return out;
}

Point add(Point a, Point b) { return {a.x + b.x, a.y + b.y}; }
Point sub(Point a, Point b) { return {a.x - b.x, a.y - b.y}; }
Point mul(Point a, f32 s) { return {a.x * s, a.y * s}; }

// Unit normal of a → b, pointing to its right (x right, y down: clockwise side).
Point normal(Point a, Point b) {
    const f32 dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
    return len > 1e-6f ? Point{dy / len, -dx / len} : Point{0, 0};
}

// Miter direction at a join of two unit normals, scaled so offsetting by it keeps the
// distance to both edges (length 1 / cos(half angle), capped for sharp spikes).
Point miter(Point n0, Point n1) {
    Point m{(n0.x + n1.x) * 0.5f, (n0.y + n1.y) * 0.5f};
    const f32 d2 = m.x * m.x + m.y * m.y;
    if (d2 < 1e-6f) return n1; // reversal: fall back to the next edge's normal
    m = mul(m, 1.0f / d2);
    const f32 len = std::sqrt(m.x * m.x + m.y * m.y);
    return len > 4.0f ? mul(m, 4.0f / len) : m;
}

void removeDuplicates(std::vector<Point>& pts, bool closed) {
    auto same = [](Point a, Point b) { return std::abs(a.x - b.x) < 1e-3f && std::abs(a.y - b.y) < 1e-3f; };
    pts.erase(std::unique(pts.begin(), pts.end(), same), pts.end());
    if (closed && pts.size() > 1 && same(pts.front(), pts.back())) pts.pop_back();
}

} // namespace

f32 fontPixels(const Theme& theme) { return theme.fontSize > 0 ? theme.fontSize : 9.5f * theme.textScale; }

void arcPath(Point c, f32 r, f32 a0, f32 a1, std::vector<Point>& out) {
    const f32 sweep = a1 - a0;
    const int n = std::clamp(int(std::ceil(std::abs(sweep) * std::max(r, 0.0f) / 2.5f)), 2, 256); // ~2.5 px chords
    for (int i = 0; i <= n; ++i) {
        const f32 a = a0 + sweep * f32(i) / f32(n);
        out.push_back({c.x + std::cos(a) * r, c.y + std::sin(a) * r});
    }
}

void roundedRectPath(Rect rc, f32 radius, std::vector<Point>& out) {
    const f32 r = std::clamp(radius, 0.0f, 0.5f * std::min(rc.w, rc.h));
    const f32 x0 = rc.x, y0 = rc.y, x1 = rc.x + rc.w, y1 = rc.y + rc.h;
    if (r < 0.5f) {
        out.insert(out.end(), {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}});
        return;
    }
    constexpr f32 pi = std::numbers::pi_v<f32>;
    arcPath({x0 + r, y0 + r}, r, pi, 1.5f * pi, out);        // top-left
    arcPath({x1 - r, y0 + r}, r, 1.5f * pi, 2.0f * pi, out); // top-right
    arcPath({x1 - r, y1 - r}, r, 0.0f, 0.5f * pi, out);      // bottom-right
    arcPath({x0 + r, y1 - r}, r, 0.5f * pi, pi, out);        // bottom-left
}

Painter::Painter(std::vector<Vertex>& vertices, const Theme& theme, Rect clip) : vertices_(vertices), theme_(theme), clips_{clip} {
    clipMin_ = packPixel(std::floor(clip.x), std::floor(clip.y));
    clipMax_ = packPixel(std::ceil(clip.x + clip.w), std::ceil(clip.y + clip.h));
}

void Painter::pushClip(Rect clip) {
    clips_.push_back(intersect(clips_.back(), clip));
    const Rect& c = clips_.back();
    clipMin_ = packPixel(std::floor(c.x), std::floor(c.y));
    clipMax_ = packPixel(std::ceil(c.x + c.w), std::ceil(c.y + c.h));
}

void Painter::popClip() {
    if (clips_.size() > 1) clips_.pop_back();
    const Rect& c = clips_.back();
    clipMin_ = packPixel(std::floor(c.x), std::floor(c.y));
    clipMax_ = packPixel(std::ceil(c.x + c.w), std::ceil(c.y + c.h));
}

// ------------------------------------------------------------------ primitives

void Painter::vertex(f32 x, f32 y, f32 u, f32 v, u32 colour, u32 texture) {
    vertices_.push_back({x, y, u, v, colour, clipMin_, clipMax_, texture});
}

void Painter::quad(Rect r, Rect uv, u32 colour, u32 texture) {
    const Rect c = intersect(r, clips_.back());
    if (c.w <= 0 || c.h <= 0 || (colour >> 24u) == 0) return;
    // UVs of the clipped part
    const f32 su = r.w > 0 ? uv.w / r.w : 0, sv = r.h > 0 ? uv.h / r.h : 0;
    const f32 u0 = uv.x + (c.x - r.x) * su, v0 = uv.y + (c.y - r.y) * sv;
    const f32 u1 = u0 + c.w * su, v1 = v0 + c.h * sv;
    vertex(c.x, c.y, u0, v0, colour, texture);
    vertex(c.x + c.w, c.y, u1, v0, colour, texture);
    vertex(c.x + c.w, c.y + c.h, u1, v1, colour, texture);
    vertex(c.x, c.y, u0, v0, colour, texture);
    vertex(c.x + c.w, c.y + c.h, u1, v1, colour, texture);
    vertex(c.x, c.y + c.h, u0, v1, colour, texture);
}

void Painter::triangle(Point a, Point b, Point c, u32 ca, u32 cb, u32 cc) {
    const f32 w = FontAtlas::whiteU();
    vertex(a.x, a.y, w, w, ca);
    vertex(b.x, b.y, w, w, cb);
    vertex(c.x, c.y, w, w, cc);
}

void Painter::fillRect(Rect rect, u32 colour) {
    const f32 w = FontAtlas::whiteU();
    quad(rect, {w, w, 0, 0}, colour);
}

void Painter::fillRectGradient(Rect r, u32 top, u32 bottom) {
    const Rect c = intersect(r, clips_.back());
    if (c.w <= 0 || c.h <= 0 || r.h <= 0) return;
    const u32 ct = lerpColour(top, bottom, (c.y - r.y) / r.h), cb = lerpColour(top, bottom, (c.y + c.h - r.y) / r.h);
    const Point p0{c.x, c.y}, p1{c.x + c.w, c.y}, p2{c.x + c.w, c.y + c.h}, p3{c.x, c.y + c.h};
    triangle(p0, p1, p2, ct, ct, cb);
    triangle(p0, p2, p3, ct, cb, cb);
}

void Painter::drawBorder(Rect r, u32 colour, f32 t) {
    fillRect({r.x, r.y, r.w, t}, colour);
    fillRect({r.x, r.y + r.h - t, r.w, t}, colour);
    fillRect({r.x, r.y + t, t, r.h - 2 * t}, colour);
    fillRect({r.x + r.w - t, r.y + t, t, r.h - 2 * t}, colour);
}

void Painter::drawImage(Rect rect, u32 texture, u32 tint, Rect uv) { quad(rect, uv, tint, texture); }

// ------------------------------------------------------------------ vector shapes

void Painter::fillConvex(std::span<const Point> points, u32 colour) {
    if ((colour >> 24u) == 0) return;
    path_.assign(points.begin(), points.end());
    removeDuplicates(path_, true);
    const std::size_t n = path_.size();
    if (n < 3) return;
    f32 area = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const Point a = path_[i], b = path_[(i + 1) % n];
        area += a.x * b.y - b.x * a.y;
    }
    const f32 side = area > 0 ? 1.0f : -1.0f; // makes the edge normals point outwards
    inner_.resize(n), outer_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Point prev = path_[(i + n - 1) % n], p = path_[i], next = path_[(i + 1) % n];
        const Point m = mul(miter(normal(prev, p), normal(p, next)), 0.5f * side);
        inner_[i] = sub(p, m);
        outer_[i] = add(p, m);
    }
    const u32 edge = transparent(colour);
    for (std::size_t i = 1; i + 1 < n; ++i) triangle(inner_[0], inner_[i], inner_[i + 1], colour, colour, colour);
    for (std::size_t i = 0; i < n; ++i) { // 1-pixel fringe: opaque inside → transparent outside
        const std::size_t j = (i + 1) % n;
        triangle(inner_[i], inner_[j], outer_[j], colour, colour, edge);
        triangle(inner_[i], outer_[j], outer_[i], colour, edge, edge);
    }
}

void Painter::fillTriangle(Point a, Point b, Point c, u32 colour) {
    const std::array<Point, 3> pts{a, b, c};
    fillConvex(pts, colour);
}

void Painter::strokePolyline(std::span<const Point> points, f32 width, u32 colour, bool closed) {
    if ((colour >> 24u) == 0 || width <= 0) return;
    path_.assign(points.begin(), points.end());
    removeDuplicates(path_, closed);
    const std::size_t n = path_.size();
    if (n < 2) return;
    if (n < 3) closed = false;
    if (width < 1.0f) colour = withAlpha(colour, width), width = 1.0f; // hairlines: fainter, not thinner
    const f32 outerOff = 0.5f * width + 0.5f, innerOff = std::max(0.0f, 0.5f * width - 0.5f);
    // per point: miter normal (ends of an open line: the segment's own normal)
    inner_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const bool first = i == 0, last = i + 1 == n;
        const Point p = path_[i];
        if (!closed && first) inner_[i] = normal(p, path_[1]);
        else if (!closed && last) inner_[i] = normal(path_[i - 1], p);
        else inner_[i] = miter(normal(path_[(i + n - 1) % n], p), normal(p, path_[(i + 1) % n]));
    }
    const u32 edge = transparent(colour);
    auto band = [&](Point a0, Point b0, Point a1, Point b1, u32 ca, u32 cb) { // a: one side, b: other; 0 → 1 along the line
        triangle(a0, b0, b1, ca, cb, cb);
        triangle(a0, b1, a1, ca, cb, ca);
    };
    const std::size_t segments = closed ? n : n - 1;
    for (std::size_t s = 0; s < segments; ++s) {
        const std::size_t i = s, j = (s + 1) % n;
        const Point p = path_[i], q = path_[j], mi = inner_[i], mj = inner_[j];
        const Point pA = add(p, mul(mi, outerOff)), pB = add(p, mul(mi, innerOff)), pC = sub(p, mul(mi, innerOff)), pD = sub(p, mul(mi, outerOff));
        const Point qA = add(q, mul(mj, outerOff)), qB = add(q, mul(mj, innerOff)), qC = sub(q, mul(mj, innerOff)), qD = sub(q, mul(mj, outerOff));
        band(pA, pB, qA, qB, edge, colour);
        if (innerOff > 0) band(pB, pC, qB, qC, colour, colour);
        band(pC, pD, qC, qD, colour, edge);
    }
    if (!closed) { // butt caps with a half-pixel fringe
        for (const bool start : {true, false}) {
            const Point p = start ? path_.front() : path_.back(), other = start ? path_[1] : path_[n - 2];
            const Point m = start ? inner_.front() : inner_.back();
            Point d = sub(p, other);
            const f32 len = std::sqrt(d.x * d.x + d.y * d.y);
            d = mul(d, 0.5f / std::max(len, 1e-6f));
            const Point a = add(p, mul(m, innerOff + 0.5f)), b = sub(p, mul(m, innerOff + 0.5f));
            // half-pixel fringe beyond the end
            triangle(a, b, add(b, d), colour, colour, edge);
            triangle(a, add(b, d), add(a, d), colour, edge, edge);
        }
    }
}

void Painter::drawLine(Point a, Point b, f32 width, u32 colour) {
    const std::array<Point, 2> pts{a, b};
    strokePolyline(pts, width, colour);
}

void Painter::fillRoundedRect(Rect rect, f32 radius, u32 colour) {
    if (radius < 0.5f) return fillRect(rect, colour);
    std::vector<Point> pts;
    roundedRectPath(rect, radius, pts);
    fillConvex(pts, colour);
}

void Painter::strokeRoundedRect(Rect rect, f32 radius, f32 width, u32 colour) {
    // the stroke lies inside the rectangle (like a CSS border)
    const f32 h = 0.5f * width;
    std::vector<Point> pts;
    roundedRectPath({rect.x + h, rect.y + h, rect.w - width, rect.h - width}, std::max(0.0f, radius - h), pts);
    strokePolyline(pts, width, colour, true);
}

void Painter::fillCircle(Point c, f32 r, u32 colour) {
    std::vector<Point> pts;
    arcPath(c, r, 0.0f, 2.0f * std::numbers::pi_v<f32>, pts);
    fillConvex(pts, colour);
}

void Painter::strokeCircle(Point c, f32 r, f32 width, u32 colour) {
    std::vector<Point> pts;
    arcPath(c, r, 0.0f, 2.0f * std::numbers::pi_v<f32>, pts);
    strokePolyline(pts, width, colour, true);
}

void Painter::strokeArc(Point c, f32 r, f32 a0, f32 a1, f32 width, u32 colour) {
    std::vector<Point> pts;
    arcPath(c, r, a0, a1, pts);
    strokePolyline(pts, width, colour, false);
}

void Painter::fillSector(Point c, f32 inner, f32 outer, f32 a0, f32 a1, u32 colour) {
    if ((colour >> 24u) == 0 || outer <= inner) return;
    std::vector<Point> dir;
    arcPath({0, 0}, outer, a0, a1, dir); // chords sized for the outer edge
    for (Point &p : dir)
        p = mul(p, 1.0f / outer); // → unit directions
    const u32 edge = transparent(colour);
    const bool ring = inner > 0.5f;
    auto at = [&](std::size_t k, f32 r) { return add(c, mul(dir[k], std::max(r, 0.0f))); };
    for (std::size_t k = 0; k + 1 < dir.size(); ++k) {
        const std::size_t l = k + 1;
        // outer fringe, body, inner fringe (or the centre)
        triangle(at(k, outer - 0.5f), at(k, outer + 0.5f), at(l, outer + 0.5f), colour, edge, edge);
        triangle(at(k, outer - 0.5f), at(l, outer + 0.5f), at(l, outer - 0.5f), colour, edge, colour);
        if (ring) {
            triangle(at(k, inner + 0.5f), at(k, outer - 0.5f), at(l, outer - 0.5f), colour, colour, colour);
            triangle(at(k, inner + 0.5f), at(l, outer - 0.5f), at(l, inner + 0.5f), colour, colour, colour);
            triangle(at(k, inner - 0.5f), at(k, inner + 0.5f), at(l, inner + 0.5f), edge, colour, colour);
            triangle(at(k, inner - 0.5f), at(l, inner + 0.5f), at(l, inner - 0.5f), edge, colour, edge);
        } else {
            triangle(c, at(k, outer - 0.5f), at(l, outer - 0.5f), colour, colour, colour);
        }
    }
}

// ------------------------------------------------------------------ text

void Painter::drawText(f32 x, f32 y, std::string_view text, u32 colour) {
    if (text.empty() || (colour >> 24u) == 0) return;
    FontAtlas& f = fonts();
    if (!f.available()) return bitmapText(x, y, text, colour);
    const f32 px = fontPixels(theme_);
    const FontMetrics m = f.metrics(px);
    const f32 baseline = y + 0.5f * (lineHeight() - (m.ascent - m.descent)) + m.ascent; // font box centred in the line
    thread_local std::vector<GlyphQuad> glyphs;
    glyphs.clear();
    f.layout(x, baseline, text, px, glyphs);
    for (const GlyphQuad& g : glyphs) quad({g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0}, {g.u0, g.v0, g.u1 - g.u0, g.v1 - g.v0}, colour);
}

void Painter::drawText(f32 x, f32 y, const std::string& text, u32 colour) { drawText(x, y, std::string_view(text), colour); }
void Painter::drawText(f32 x, f32 y, const char* text, u32 colour) { drawText(x, y, std::string_view(text), colour); }

// stb_easy_font fallback (ASCII bitmap font made of rectangles)
void Painter::bitmapText(f32 x, f32 y, std::string_view text, u32 colour) {
    thread_local std::string terminated;
    thread_local std::vector<std::array<f32, 16>> buffer;
    terminated.assign(text);
    buffer.resize(text.size() * 16 + 1);
    const int count = stb_easy_font_print(0, 0, terminated.data(), nullptr, buffer.data(), int(buffer.size() * sizeof(buffer[0])));
    const f32 scale = theme_.textScale;
    for (int i = 0; i < count; ++i) {
        const f32* q = buffer[std::size_t(i)].data();
        fillRect({x + q[0] * scale, y + q[1] * scale, std::max(scale, (q[4] - q[0]) * scale), std::max(scale, (q[9] - q[1]) * scale)}, colour);
    }
}

f32 Painter::textWidth(std::string_view text) const { return measureText(text, theme_); }

f32 Painter::lineHeight() const { return theme_.fontSize > 0 ? std::ceil(theme_.fontSize * 1.3f) : 12.0f * theme_.textScale; }

f32 measureText(std::string_view text, const Theme& theme) {
    if (text.empty()) return 0;
    FontAtlas& f = fonts();
    if (f.available()) return std::ceil(f.measure(text, fontPixels(theme)));
    thread_local std::string terminated;
    terminated.assign(text);
    return f32(stb_easy_font_width(terminated.data())) * theme.textScale;
}

} // namespace phx::ui
