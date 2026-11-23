#include "phx/ui/Ui.hpp"

#include <algorithm>
#include <cmath>

namespace phx::ui {
namespace {
Rect inner(const Container &p) {
    const Rect b = p.bounds();
    const Insets i = p.insets();
    return {b.x + i.left, b.y + i.top, std::max(0.0f, b.w - i.left - i.right), std::max(0.0f, b.h - i.top - i.bottom)};
}
std::vector<Component *> visible(const Container &p) {
    std::vector<Component *> result;
    for (const auto &c : p.children())
        if (c->visible())
            result.push_back(c.get());
    return result;
}
} // namespace

void BorderLayout::layout(Container &p) {
    Rect r = inner(p);
    Component *centre = nullptr;
    for (Component *c : visible(p)) {
        const Size s = c->preferredSize();
        switch (p.constraint(*c).region) {
        case BorderRegion::North:
            c->setBounds({r.x, r.y, r.w, s.h});
            r.y += s.h + vgap_;
            r.h -= s.h + vgap_;
            break;
        case BorderRegion::South:
            c->setBounds({r.x, r.y + r.h - s.h, r.w, s.h});
            r.h -= s.h + vgap_;
            break;
        case BorderRegion::West:
            c->setBounds({r.x, r.y, s.w, r.h});
            r.x += s.w + hgap_;
            r.w -= s.w + hgap_;
            break;
        case BorderRegion::East:
            c->setBounds({r.x + r.w - s.w, r.y, s.w, r.h});
            r.w -= s.w + hgap_;
            break;
        case BorderRegion::Center:
            centre = c;
            break;
        }
    }
    if (centre)
        centre->setBounds(r);
}
Size BorderLayout::preferredSize(const Container &p) const {
    f32 middleW = 0, middleH = 0, outerW = 0, outerH = 0;
    for (Component *c : visible(p)) {
        const Size s = c->preferredSize();
        const auto region = p.constraint(*c).region;
        if (region == BorderRegion::North || region == BorderRegion::South) {
            outerW = std::max(outerW, s.w);
            outerH += s.h + vgap_;
        } else {
            middleW += s.w + hgap_;
            middleH = std::max(middleH, s.h);
        }
    }
    const Insets i = p.insets();
    return {std::max(outerW, middleW) + i.left + i.right, outerH + middleH + i.top + i.bottom};
}

void FlowLayout::layout(Container &p) {
    const Rect r = inner(p);
    auto cs = visible(p);
    f32 total = 0, maxH = 0;
    for (Component *c : cs) {
        Size s = c->preferredSize();
        total += s.w;
        maxH = std::max(maxH, s.h);
    }
    if (!cs.empty())
        total += hgap_ * static_cast<f32>(cs.size() - 1);
    f32 x = r.x + (align_ == Alignment::Center ? (r.w - total) / 2 : align_ == Alignment::Right ? r.w - total : 0);
    for (Component *c : cs) {
        Size s = c->preferredSize();
        c->setBounds({x, r.y + vgap_, s.w, s.h});
        x += s.w + hgap_;
    }
    (void)maxH;
}
Size FlowLayout::preferredSize(const Container &p) const {
    f32 w = 0, h = 0;
    auto cs = visible(p);
    for (Component *c : cs) {
        Size s = c->preferredSize();
        w += s.w;
        h = std::max(h, s.h);
    }
    if (!cs.empty())
        w += hgap_ * static_cast<f32>(cs.size() - 1);
    const Insets i = p.insets();
    return {w + i.left + i.right, h + 2 * vgap_ + i.top + i.bottom};
}

void GridLayout::layout(Container &p) {
    auto cs = visible(p);
    if (cs.empty())
        return;
    const u32 rows = rows_ ? rows_ : static_cast<u32>((cs.size() + columns_ - 1) / columns_);
    const u32 cols = columns_ ? columns_ : static_cast<u32>((cs.size() + rows - 1) / rows);
    const Rect r = inner(p);
    const f32 w = (r.w - hgap_ * (cols - 1)) / cols, h = (r.h - vgap_ * (rows - 1)) / rows;
    for (std::size_t n = 0; n < cs.size(); ++n)
        cs[n]->setBounds({r.x + (n % cols) * (w + hgap_), r.y + (n / cols) * (h + vgap_), w, h});
}
Size GridLayout::preferredSize(const Container &p) const {
    auto cs = visible(p);
    f32 w = 0, h = 0;
    for (Component *c : cs) {
        Size s = c->preferredSize();
        w = std::max(w, s.w);
        h = std::max(h, s.h);
    }
    const u32 rows = rows_ ? rows_ : 1,
              cols = columns_ ? columns_ : static_cast<u32>(std::max<std::size_t>(1, cs.size()));
    const Insets i = p.insets();
    return {w * cols + hgap_ * (cols - 1) + i.left + i.right, h * rows + vgap_ * (rows - 1) + i.top + i.bottom};
}

void BoxLayout::layout(Container &p) {
    Rect r = inner(p);
    f32 cursor = axis_ == Axis::X ? r.x : r.y;
    for (Component *c : visible(p)) {
        Size s = c->preferredSize();
        if (axis_ == Axis::X) {
            c->setBounds({cursor, r.y, s.w, r.h});
            cursor += s.w + gap_;
        } else {
            c->setBounds({r.x, cursor, r.w, s.h});
            cursor += s.h + gap_;
        }
    }
}
Size BoxLayout::preferredSize(const Container &p) const {
    f32 main = 0, cross = 0;
    auto cs = visible(p);
    for (Component *c : cs) {
        Size s = c->preferredSize();
        main += axis_ == Axis::X ? s.w : s.h;
        cross = std::max(cross, axis_ == Axis::X ? s.h : s.w);
    }
    if (!cs.empty())
        main += gap_ * static_cast<f32>(cs.size() - 1);
    const Insets i = p.insets();
    return axis_ == Axis::X ? Size{main + i.left + i.right, cross + i.top + i.bottom}
                            : Size{cross + i.left + i.right, main + i.top + i.bottom};
}

void FormLayout::layout(Container &p) {
    auto cs = visible(p);
    Rect r = inner(p);
    f32 left = 0;
    for (std::size_t n = 0; n < cs.size(); n += 2)
        left = std::max(left, cs[n]->preferredSize().w);
    f32 y = r.y;
    for (std::size_t n = 0; n < cs.size(); n += 2) {
        const Size a = cs[n]->preferredSize();
        const Size b = n + 1 < cs.size() ? cs[n + 1]->preferredSize() : Size{};
        const f32 h = std::max(a.h, b.h);
        cs[n]->setBounds({r.x, y, left, h});
        if (n + 1 < cs.size())
            cs[n + 1]->setBounds({r.x + left + hgap_, y, r.w - left - hgap_, h});
        y += h + vgap_;
    }
}
Size FormLayout::preferredSize(const Container &p) const {
    auto cs = visible(p);
    f32 left = 0, right = 0, h = 0;
    for (std::size_t n = 0; n < cs.size(); n += 2) {
        Size a = cs[n]->preferredSize();
        Size b = n + 1 < cs.size() ? cs[n + 1]->preferredSize() : Size{};
        left = std::max(left, a.w);
        right = std::max(right, b.w);
        h += std::max(a.h, b.h) + vgap_;
    }
    if (!cs.empty())
        h -= vgap_;
    const Insets i = p.insets();
    return {left + right + hgap_ + i.left + i.right, h + i.top + i.bottom};
}
} // namespace phx::ui
