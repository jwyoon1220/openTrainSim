#include "phx/ui/Ui.hpp"

#include <algorithm>

namespace phx::ui {
namespace {
Window *containingWindow(Component *c) {
    while (c && !dynamic_cast<Window *>(c))
        c = c->parent();
    return dynamic_cast<Window *>(c);
}
bool isInteractive(Component *component) {
    for (Component *current = component; current; current = current->parent()) {
        if (!current->visible() || !current->enabled()) {
            return false;
        }
    }
    return component != nullptr;
}
} // namespace

Ui::Ui(Theme theme) : theme_(theme) {
}
Window &Ui::addWindow(std::string title) {
    auto window = std::make_unique<Window>(std::move(title));
    Window &result = *window;
    std::function<void(Component &)> attach = [&](Component &c) {
        c.owner_ = this;
        if (auto *container = dynamic_cast<Container *>(&c))
            for (const auto &child : container->children())
                attach(*child);
    };
    attach(result);
    result.setInsets({theme_.titleHeight, 0, 0, 0});
    result.content().setInsets({theme_.padding, theme_.padding, theme_.padding, theme_.padding});
    result.content().setLayout(std::make_unique<BoxLayout>(Axis::Y, theme_.padding / 2));
    windows_.push_back(std::move(window));
    dirty();
    return result;
}
void Ui::setScreenSize(f32 w, f32 h) {
    if (screen_.w != w || screen_.h != h) {
        screen_ = {w, h};
        dirty(true);
    }
}
void Ui::dirty(bool layout) {
    dirty_ = true;
    if (layout)
        for (auto &w : windows_)
            w->layoutDirty_ = true;
}
Window *Ui::frontWindow() const {
    for (auto i = windows_.rbegin(); i != windows_.rend(); ++i)
        if ((*i)->visible())
            return i->get();
    return nullptr;
}
Component *Ui::hit(f32 x, f32 y) const {
    for (auto i = windows_.rbegin(); i != windows_.rend(); ++i)
        if (Component *h = (*i)->hitTest(x, y))
            return h;
    return nullptr;
}
void Ui::bringToFront(Window &w) {
    auto i = std::find_if(windows_.begin(), windows_.end(), [&w](const auto &p) { return p.get() == &w; });
    if (i != windows_.end() && i + 1 != windows_.end()) {
        auto p = std::move(*i);
        windows_.erase(i);
        windows_.push_back(std::move(p));
        dirty();
    }
}
void Ui::setFocus(Component *c) {
    if (c && (!c->focusable() || !c->visible() || !c->enabled()))
        c = nullptr;
    if (focus_ == c)
        return;
    if (focus_) {
        focus_->focused_ = false;
        focus_->onFocusLost();
    }
    focus_ = c;
    if (focus_) {
        focus_->focused_ = true;
        focus_->onFocusGained();
    }
}

bool Ui::mouseMove(f32 x, f32 y) {
    mouseX_ = x;
    mouseY_ = y;
    Component *next = popup_ && popupRect_.contains(x, y) ? popup_ : hit(x, y);
    if (next != hover_) {
        if (hover_)
            hover_->onMouseExit();
        hover_ = next;
        if (hover_)
            hover_->onMouseEnter();
    }
    if (popup_) {
        int nextHover = -1;
        if (popupRect_.contains(x, y)) {
            nextHover = popupFirst_ + static_cast<int>((y - popupRect_.y) / theme_.rowHeight);
            if (nextHover >= static_cast<int>(popup_->items_.size()))
                nextHover = -1;
        }
        if (nextHover != popupHover_) {
            popupHover_ = nextHover;
            dirty();
        }
    }
    if (capture_)
        capture_->onMouseDrag(x, y);
    else if (hover_)
        hover_->onMouseMove(x, y);
    return wantsMouse();
}
bool Ui::popupHit(f32 x, f32 y, bool select) {
    if (!popup_ || !popupRect_.contains(x, y))
        return false;
    const int index = popupFirst_ + static_cast<int>((y - popupRect_.y) / theme_.rowHeight);
    if (select && index >= 0 && index < static_cast<int>(popup_->items_.size())) {
        if (popup_->itemEnabled(index))
            popup_->setSelectedIndex(index);
        closePopup();
    }
    return true;
}
bool Ui::mouseButton(MouseButton b, bool down) {
    if (down && popup_) {
        if (popupHit(mouseX_, mouseY_, true))
            return true;
        closePopup();
    }
    if (down) {
        Component *target = hit(mouseX_, mouseY_);
        if (!target)
            return false;
        if (Window *w = containingWindow(target))
            bringToFront(*w);
        setFocus(target);
        capture_ = target;
        target->onMousePress(b, mouseX_, mouseY_);
        return true;
    }
    if (!capture_)
        return hit(mouseX_, mouseY_) != nullptr;
    Component *target = capture_;
    capture_ = nullptr;
    target->onMouseRelease(b, mouseX_, mouseY_);
    return true;
}
bool Ui::mouseWheel(f32 a) {
    if (popup_) {
        const int visibleRows = static_cast<int>(popupRect_.h / theme_.rowHeight);
        const int maximum = std::max(0, static_cast<int>(popup_->items_.size()) - visibleRows);
        popupFirst_ = std::clamp(popupFirst_ + (a < 0 ? 1 : -1), 0, maximum);
        popupHover_ = -1;
        dirty();
        return true;
    }
    Component *c = hit(mouseX_, mouseY_);
    if (!c)
        return false;
    c->onMouseWheel(a);
    return true;
}
void Ui::collectFocusable(Component &c, std::vector<Component *> &out) {
    if (!c.visible() || !c.enabled())
        return;
    if (c.focusable())
        out.push_back(&c);
    if (auto *p = dynamic_cast<Container *>(&c))
        for (const auto &child : p->children())
            collectFocusable(*child, out);
}
void Ui::cycleFocus(bool reverse) {
    Window *w = frontWindow();
    if (!w)
        return;
    std::vector<Component *> list;
    collectFocusable(*w, list);
    if (list.empty())
        return;
    auto it = std::find(list.begin(), list.end(), focus_);
    std::size_t n = it == list.end()
                        ? (reverse ? list.size() - 1 : 0)
                        : (static_cast<std::size_t>(it - list.begin()) + (reverse ? list.size() - 1 : 1)) % list.size();
    setFocus(list[n]);
}
bool Ui::key(Key k, bool down, bool shift, bool ctrl) {
    if (k == Key::Tab && down) {
        cycleFocus(shift);
        return frontWindow() != nullptr;
    }
    if (k == Key::Escape && down) {
        if (popup_)
            closePopup();
        else if (Window *w = frontWindow(); w && w->closable())
            w->close();
        else
            return false;
        return true;
    }
    if (!wantsKeyboard())
        return false;
    focus_->onKey(k, down, shift, ctrl);
    return true;
}
bool Ui::character(u32 c) {
    if (!wantsKeyboard())
        return false;
    focus_->onChar(c);
    return true;
}
bool Ui::wantsMouse() const {
    return capture_ || popup_ || hit(mouseX_, mouseY_);
}
bool Ui::wantsKeyboard() const {
    return isInteractive(focus_) && containingWindow(focus_) != nullptr;
}
bool Ui::anyWindowVisible() const {
    return std::any_of(windows_.begin(), windows_.end(), [](const auto &w) { return w->visible(); });
}
void Ui::openCombo(ComboBox &c) {
    popup_ = &c;
    popupFirst_ = 0;
    popupHover_ = -1;
    const f32 desiredHeight = theme_.rowHeight * static_cast<f32>(c.items_.size());
    const f32 below = std::max(0.0f, screen_.h - (c.bounds().y + c.bounds().h));
    const f32 above = std::max(0.0f, c.bounds().y);
    const bool openUp = desiredHeight > below && above > below;
    const f32 available = openUp ? above : below;
    const f32 height = std::min(desiredHeight, available);
    const f32 y = openUp ? c.bounds().y - height : c.bounds().y + c.bounds().h;
    const f32 x = std::clamp(c.bounds().x, 0.0f, std::max(0.0f, screen_.w - c.bounds().w));
    popupRect_ = {x, y, std::min(c.bounds().w, screen_.w), height};
    dirty();
}
void Ui::closePopup() {
    if (popup_) {
        popup_ = nullptr;
        popupHover_ = -1;
        dirty();
    }
}

void Ui::windowHidden(Window &window) {
    auto belongs = [&window](Component *component) { return component && containingWindow(component) == &window; };
    if (belongs(hover_)) {
        hover_->onMouseExit();
        hover_ = nullptr;
    }
    if (belongs(capture_))
        capture_ = nullptr;
    if (belongs(focus_))
        setFocus(nullptr);
    if (belongs(popup_))
        closePopup();
}

bool Ui::build(std::vector<Vertex> &out) {
    if (!dirty_)
        return false;
    out.clear();
    Painter painter(out, theme_, {0, 0, screen_.w, screen_.h});
    for (auto &w : windows_)
        w->paintTree(painter);
    if (popup_) {
        painter.pushClip({0, 0, screen_.w, screen_.h});
        painter.fillRect(popupRect_, theme_.window);
        painter.drawBorder(popupRect_, theme_.border);
        const int rows = static_cast<int>(popupRect_.h / theme_.rowHeight);
        for (int row = 0; row < rows; ++row) {
            const int index = popupFirst_ + row;
            if (index >= static_cast<int>(popup_->items_.size()))
                break;
            Rect r{popupRect_.x + 1, popupRect_.y + row * theme_.rowHeight, popupRect_.w - 2, theme_.rowHeight};
            if (index == popup_->selected_ || index == popupHover_)
                painter.fillRect(r, theme_.selection);
            painter.drawText(r.x + theme_.padding, r.y + (r.h - painter.lineHeight()) / 2,
                             popup_->items_[static_cast<std::size_t>(index)],
                             popup_->itemEnabled(index) ? theme_.text : theme_.disabledText);
        }
        painter.popClip();
    }
    dirty_ = false;
    ++revision_;
    return true;
}
} // namespace phx::ui
