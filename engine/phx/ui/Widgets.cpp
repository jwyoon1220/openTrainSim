#include "phx/ui/Ui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace phx::ui {
Component::~Component() = default;
void Component::setVisible(bool v) {
    if (visible_ != v) {
        visible_ = v;
        if (!v) {
            if (auto *window = dynamic_cast<Window *>(this); window && ui()) {
                ui()->windowHidden(*window);
            }
        }
        revalidate();
    }
}
void Component::setEnabled(bool v) {
    if (enabled_ != v) {
        enabled_ = v;
        repaint();
    }
}
void Component::setPreferredSize(Size v) {
    preferred_ = v;
    revalidate();
}
Size Component::preferredSize() {
    return preferred_.value_or(Size{});
}
void Component::setBounds(Rect v) {
    if (bounds_.x == v.x && bounds_.y == v.y && bounds_.w == v.w && bounds_.h == v.h)
        return;
    bounds_ = v;
    // Child bounds are absolute, so moving a container also invalidates its layout.
    if (auto *container = dynamic_cast<Container *>(this))
        container->layoutDirty_ = true;
    repaint();
}
Ui *Component::ui() const {
    return owner_ ? owner_ : parent_ ? parent_->ui() : nullptr;
}
const Theme &Component::theme() const {
    static const Theme fallback;
    return ui() ? ui()->theme() : fallback;
}
void Component::repaint() {
    if (parent_)
        parent_->repaint();
    else if (owner_)
        owner_->dirty();
}
void Component::revalidate() {
    if (parent_) {
        parent_->layoutDirty_ = true;
        parent_->revalidate();
    } else if (owner_)
        owner_->dirty(true);
}
void Component::paint(Painter &) {
}
void Component::paintTree(Painter &p) {
    if (visible_)
        paint(p);
}
Component *Component::hitTest(f32 x, f32 y) {
    return visible_ && enabled_ && bounds_.contains(x, y) ? this : nullptr;
}
void Component::onMouseEnter() {
}
void Component::onMouseExit() {
}
void Component::onMouseMove(f32, f32) {
}
void Component::onMousePress(MouseButton, f32, f32) {
}
void Component::onMouseRelease(MouseButton, f32, f32) {
}
void Component::onMouseDrag(f32, f32) {
}
void Component::onMouseWheel(f32) {
}
void Component::onKey(Key, bool, bool, bool) {
}
void Component::onChar(u32) {
}
void Component::onFocusGained() {
    repaint();
}
void Component::onFocusLost() {
    repaint();
}

void Container::addImpl(std::unique_ptr<Component> c, Constraint constraint) {
    c->parent_ = this;
    c->owner_ = owner_;
    children_.push_back(std::move(c));
    constraints_.push_back(constraint);
    revalidate();
}
void Container::remove(Component &c) {
    for (std::size_t i = 0; i < children_.size(); ++i)
        if (children_[i].get() == &c) {
            children_.erase(children_.begin() + static_cast<i64>(i));
            constraints_.erase(constraints_.begin() + static_cast<i64>(i));
            revalidate();
            return;
        }
}
void Container::setLayout(std::unique_ptr<LayoutManager> l) {
    layout_ = std::move(l);
    revalidate();
}
void Container::setInsets(Insets i) {
    insets_ = i;
    revalidate();
}
Constraint Container::constraint(const Component &c) const {
    for (std::size_t i = 0; i < children_.size(); ++i)
        if (children_[i].get() == &c)
            return constraints_[i];
    return {};
}
Size Container::preferredSize() {
    Size s = Component::preferredSize();
    return (s.w || s.h) ? s : layout_ ? layout_->preferredSize(*this) : s;
}
void Container::doLayout() {
    if (!layoutDirty_)
        return;
    if (layout_)
        layout_->layout(*this);
    layoutDirty_ = false;
    for (auto &c : children_)
        if (auto *p = dynamic_cast<Container *>(c.get()))
            p->doLayout();
}
void Container::paint(Painter &) {
}
void Container::paintTree(Painter &p) {
    if (!visible())
        return;
    doLayout();
    paint(p);
    p.pushClip(bounds());
    for (auto &c : children_)
        c->paintTree(p);
    p.popClip();
}
Component *Container::hitTest(f32 x, f32 y) {
    if (!visible() || !enabled() || !bounds().contains(x, y))
        return nullptr;
    doLayout();
    for (auto i = children_.rbegin(); i != children_.rend(); ++i)
        if (Component *h = (*i)->hitTest(x, y))
            return h;
    return this;
}

Label::Label(std::string t) : text_(std::move(t)) {
}
void Label::setText(std::string t) {
    text_ = std::move(t);
    revalidate();
}
const std::string &Label::text() const {
    return text_;
}
void Label::setAlignment(Alignment a) {
    alignment_ = a;
    repaint();
}
Size Label::preferredSize() {
    Size s = Component::preferredSize();
    return (s.w || s.h) ? s : Size{measureText(text_, theme()), theme().rowHeight};
}
void Label::paint(Painter &p) {
    f32 w = p.textWidth(text_), x = bounds().x;
    if (alignment_ == Alignment::Center)
        x += (bounds().w - w) / 2;
    else if (alignment_ == Alignment::Right)
        x += bounds().w - w;
    p.drawText(x, bounds().y + (bounds().h - p.lineHeight()) / 2, text_,
               enabled() ? p.theme().text : p.theme().disabledText);
}

Button::Button(std::string t) : text_(std::move(t)) {
}
void Button::setText(std::string t) {
    text_ = std::move(t);
    revalidate();
}
void Button::addActionListener(std::function<void()> f) {
    listeners_.push_back(std::move(f));
}
Size Button::preferredSize() {
    Size s = Component::preferredSize();
    return (s.w || s.h) ? s : Size{measureText(text_, theme()) + theme().padding * 2, theme().rowHeight};
}
void Button::onMouseEnter() {
    hover_ = true;
    repaint();
}
void Button::onMouseExit() {
    hover_ = false;
    repaint();
}
void Button::onMousePress(MouseButton b, f32, f32) {
    if (b == MouseButton::Left) {
        pressed_ = true;
        repaint();
    }
}
void Button::onMouseRelease(MouseButton b, f32 x, f32 y) {
    if (b == MouseButton::Left && pressed_) {
        pressed_ = false;
        repaint();
        if (bounds().contains(x, y))
            fire();
    }
}
void Button::onKey(Key k, bool down, bool, bool) {
    if ((k == Key::Space || k == Key::Enter) && down) {
        pressed_ = true;
        repaint();
    } else if ((k == Key::Space || k == Key::Enter) && pressed_) {
        pressed_ = false;
        fire();
        repaint();
    }
}
void Button::fire() {
    for (auto &f : listeners_)
        f();
}
void Button::paint(Painter &p) {
    const Theme &t = p.theme();
    p.fillRoundedRect(bounds(), t.cornerRadius,
                      !enabled() ? t.controlPressed
                      : pressed_ ? t.controlPressed
                      : hover_   ? t.controlHover
                                 : t.control);
    p.strokeRoundedRect(bounds(), t.cornerRadius, focused() ? 2.0f : 1.0f, focused() ? t.accent : t.border);
    const f32 w = p.textWidth(text_);
    p.drawText(bounds().x + (bounds().w - w) / 2, bounds().y + (bounds().h - p.lineHeight()) / 2, text_,
               enabled() ? t.text : t.disabledText);
}

CheckBox::CheckBox(std::string t, bool s) : text_(std::move(t)), selected_(s) {
}
bool CheckBox::selected() const {
    return selected_;
}
void CheckBox::setSelected(bool s) {
    if (selected_ != s) {
        selected_ = s;
        repaint();
        for (auto &f : listeners_)
            f(s);
    }
}
void CheckBox::addItemListener(std::function<void(bool)> f) {
    listeners_.push_back(std::move(f));
}
Size CheckBox::preferredSize() {
    Size s = Component::preferredSize();
    return (s.w || s.h) ? s : Size{measureText(text_, theme()) + theme().rowHeight, theme().rowHeight};
}
void CheckBox::toggle() {
    if (enabled())
        setSelected(!selected_);
}
void CheckBox::onMouseRelease(MouseButton b, f32 x, f32 y) {
    if (b == MouseButton::Left && bounds().contains(x, y))
        toggle();
}
void CheckBox::onKey(Key k, bool down, bool, bool) {
    if (k == Key::Space && down)
        toggle();
}
void CheckBox::paint(Painter &p) {
    const f32 boxSize = std::max(1.0f, bounds().h - p.theme().padding * 1.5f);
    Rect box{bounds().x + p.theme().padding / 2, bounds().y + (bounds().h - boxSize) / 2, boxSize, boxSize};
    const f32 r = std::min(p.theme().cornerRadius, box.w * 0.25f);
    if (selected_) {
        p.fillRoundedRect(box, r, p.theme().accent);
        const std::array<Point, 3> tick{Point{box.x + box.w * 0.24f, box.y + box.h * 0.52f}, Point{box.x + box.w * 0.43f, box.y + box.h * 0.72f},
                                        Point{box.x + box.w * 0.78f, box.y + box.h * 0.30f}};
        p.strokePolyline(tick, std::max(1.5f, box.w * 0.13f), p.theme().text);
    } else {
        p.fillRoundedRect(box, r, p.theme().control);
    }
    p.strokeRoundedRect(box, r, 1.0f, focused() ? p.theme().accent : p.theme().border);
    p.drawText(box.x + box.w + p.theme().padding / 2, bounds().y + (bounds().h - p.lineHeight()) / 2, text_,
               enabled() ? p.theme().text : p.theme().disabledText);
}
void ButtonGroup::add(RadioButton &b) {
    buttons_.push_back(&b);
    b.addItemListener([this, &b](bool selected) {
        if (selected)
            for (RadioButton *other : buttons_)
                if (other != &b && other->selected())
                    other->setSelected(false);
    });
}

Slider::Slider(f32 mn, f32 mx, f32 v, f32 step) : minimum_(mn), maximum_(std::max(mn, mx)), value_(v), step_(step) {
    setValue(v);
}
f32 Slider::value() const {
    return value_;
}
void Slider::setRange(f32 a, f32 b) {
    minimum_ = a;
    maximum_ = std::max(a, b);
    setValue(value_);
    revalidate();
}
void Slider::setStep(f32 s) {
    step_ = std::max(0.0f, s);
    setValue(value_);
}
void Slider::setValue(f32 v) {
    v = std::clamp(v, minimum_, maximum_);
    if (step_ > 0)
        v = minimum_ + std::round((v - minimum_) / step_) * step_;
    v = std::clamp(v, minimum_, maximum_);
    if (v != value_) {
        value_ = v;
        repaint();
        for (auto &f : listeners_)
            f(v);
    }
}
void Slider::addChangeListener(std::function<void(f32)> f) {
    listeners_.push_back(std::move(f));
}
void Slider::setValueFormatter(std::function<std::string(f32)> f) {
    formatter_ = std::move(f);
    revalidate();
}
Size Slider::preferredSize() {
    Size s = Component::preferredSize();
    return (s.w || s.h) ? s : Size{180 + valueWidth(), theme().rowHeight};
}
// Room for the formatted value right of the track: the wider of the two range ends.
f32 Slider::valueWidth() const {
    if (!formatter_)
        return 0;
    const f32 w = std::max(measureText(formatter_(minimum_), theme()), measureText(formatter_(maximum_), theme()));
    return w + theme().padding;
}
void Slider::setFromX(f32 x) {
    const f32 extra = valueWidth(), span = std::max(1.0f, bounds().w - 20 - extra);
    setValue(minimum_ + std::clamp((x - bounds().x - 10) / span, 0.0f, 1.0f) * (maximum_ - minimum_));
}
void Slider::onMousePress(MouseButton b, f32 x, f32) {
    if (b == MouseButton::Left)
        setFromX(x);
}
void Slider::onMouseDrag(f32 x, f32) {
    setFromX(x);
}
void Slider::onMouseWheel(f32 a) {
    setValue(value_ + (step_ > 0 ? step_ : (maximum_ - minimum_) / 20) * a);
}
void Slider::onKey(Key k, bool down, bool, bool) {
    if (!down)
        return;
    const f32 d = step_ > 0 ? step_ : (maximum_ - minimum_) / 100;
    if (k == Key::Left || k == Key::Down)
        setValue(value_ - d);
    if (k == Key::Right || k == Key::Up)
        setValue(value_ + d);
    if (k == Key::Home)
        setValue(minimum_);
    if (k == Key::End)
        setValue(maximum_);
}
void Slider::paint(Painter &p) {
    const f32 extra = valueWidth(), span = std::max(1.0f, bounds().w - 20 - extra);
    const f32 t = maximum_ == minimum_ ? 0 : (value_ - minimum_) / (maximum_ - minimum_);
    Rect track{bounds().x + 10, bounds().y + bounds().h / 2 - 2, span, 4};
    p.fillRoundedRect(track, 2.0f, p.theme().controlPressed);
    p.fillRoundedRect({track.x, track.y, span * t, 4}, 2.0f, p.theme().accent);
    const Point centre{track.x + span * t, bounds().y + bounds().h / 2};
    const f32 radius = std::max(4.0f, bounds().h / 2 - 5);
    p.fillCircle(centre, radius, p.theme().controlHover);
    p.strokeCircle(centre, radius - 0.5f, focused() ? 2.0f : 1.0f, focused() ? p.theme().accent : p.theme().border);
    if (formatter_)
        p.drawText(bounds().x + bounds().w - extra + 4, bounds().y + (bounds().h - p.lineHeight()) / 2,
                   formatter_(value_), p.theme().text);
}

ComboBox::ComboBox(std::vector<std::string> i)
    : items_(std::move(i)), enabledItems_(items_.size(), true), selected_(items_.empty() ? -1 : 0) {
}
int ComboBox::selectedIndex() const {
    return selected_;
}
bool ComboBox::itemEnabled(int i) const {
    return i >= 0 && i < static_cast<int>(enabledItems_.size()) && enabledItems_[i];
}
void ComboBox::setItemEnabled(int i, bool e) {
    if (i >= 0 && i < static_cast<int>(enabledItems_.size())) {
        enabledItems_[i] = e;
        repaint();
    }
}
void ComboBox::setSelectedIndex(int i) {
    if (i >= 0 && i < static_cast<int>(items_.size()) && itemEnabled(i) && i != selected_) {
        selected_ = i;
        repaint();
        for (auto &f : listeners_)
            f(i);
    }
}
void ComboBox::addActionListener(std::function<void(int)> f) {
    listeners_.push_back(std::move(f));
}
Size ComboBox::preferredSize() {
    Size s = Component::preferredSize();
    if (s.w || s.h)
        return s;
    f32 width = measureText("MMMMMM", theme());
    for (const auto &item : items_)
        width = std::max(width, measureText(item, theme()));
    return {width + theme().padding * 2 + theme().rowHeight / 2, theme().rowHeight};
}
void ComboBox::onMousePress(MouseButton b, f32, f32) {
    if (b == MouseButton::Left && ui())
        ui()->openCombo(*this);
}
void ComboBox::onMouseWheel(f32 a) {
    int n = selected_ + (a < 0 ? 1 : -1);
    while (n >= 0 && n < static_cast<int>(items_.size()) && !itemEnabled(n))
        n += a < 0 ? 1 : -1;
    setSelectedIndex(n);
}
void ComboBox::onKey(Key k, bool down, bool, bool) {
    if (!down)
        return;
    if (k == Key::Enter || k == Key::Space) {
        if (ui())
            ui()->openCombo(*this);
    } else if (k == Key::Down)
        onMouseWheel(-1);
    else if (k == Key::Up)
        onMouseWheel(1);
}
void ComboBox::paint(Painter &p) {
    p.fillRoundedRect(bounds(), p.theme().cornerRadius, p.theme().control);
    p.strokeRoundedRect(bounds(), p.theme().cornerRadius, 1.0f, focused() ? p.theme().accent : p.theme().border);
    if (selected_ >= 0)
        p.drawText(bounds().x + p.theme().padding, bounds().y + (bounds().h - p.lineHeight()) / 2, items_[selected_],
                   enabled() ? p.theme().text : p.theme().disabledText);
    const f32 ax = bounds().x + bounds().w - 12, ay = bounds().y + bounds().h / 2; // drop-down arrow
    p.fillTriangle({ax - 5, ay - 2.5f}, {ax + 5, ay - 2.5f}, {ax, ay + 3.5f}, p.theme().text);
}

TextField::TextField(std::string t, u32 c) : text_(std::move(t)), columns_(c), caret_(static_cast<u32>(text_.size())) {
}
const std::string &TextField::text() const {
    return text_;
}
void TextField::setText(std::string t) {
    text_ = std::move(t);
    caret_ = static_cast<u32>(text_.size());
    repaint();
}
void TextField::addActionListener(std::function<void()> f) {
    listeners_.push_back(std::move(f));
}
Size TextField::preferredSize() {
    Size s = Component::preferredSize();
    const std::string sample(columns_, 'M');
    return (s.w || s.h) ? s : Size{measureText(sample, theme()) + theme().padding * 2, theme().rowHeight};
}
void TextField::onMousePress(MouseButton, f32 x, f32) {
    const f32 local = x - bounds().x - theme().padding;
    caret_ = 0;
    for (u32 index = 1; index <= text_.size(); ++index) {
        const f32 previous = measureText(std::string_view(text_).substr(0, index - 1), theme());
        const f32 current = measureText(std::string_view(text_).substr(0, index), theme());
        if (local < (previous + current) / 2)
            break;
        caret_ = index;
    }
    repaint();
}
void TextField::onKey(Key k, bool down, bool, bool) {
    if (!down)
        return;
    if (k == Key::Left && caret_)
        --caret_;
    else if (k == Key::Right && caret_ < text_.size())
        ++caret_;
    else if (k == Key::Home)
        caret_ = 0;
    else if (k == Key::End)
        caret_ = static_cast<u32>(text_.size());
    else if (k == Key::Backspace && caret_) {
        text_.erase(--caret_, 1);
        revalidate();
    } else if (k == Key::Delete && caret_ < text_.size()) {
        text_.erase(caret_, 1);
        revalidate();
    } else if (k == Key::Enter)
        for (auto &f : listeners_)
            f();
    repaint();
}
void TextField::onChar(u32 c) {
    if (c >= 32 && c < 127) {
        text_.insert(text_.begin() + caret_, static_cast<char>(c));
        ++caret_;
        revalidate();
    }
}
void TextField::paint(Painter &p) {
    p.fillRoundedRect(bounds(), p.theme().cornerRadius, p.theme().controlPressed);
    p.strokeRoundedRect(bounds(), p.theme().cornerRadius, focused() ? 2.0f : 1.0f, focused() ? p.theme().accent : p.theme().border);
    const f32 textX = bounds().x + p.theme().padding;
    p.drawText(textX, bounds().y + (bounds().h - p.lineHeight()) / 2, text_, p.theme().text);
    if (focused()) {
        const f32 caretX = textX + p.textWidth(std::string_view(text_).substr(0, caret_));
        p.fillRect({caretX, bounds().y + p.theme().padding / 2, 1, bounds().h - p.theme().padding}, p.theme().text);
    }
}

Size Separator::preferredSize() {
    Size s = Component::preferredSize();
    return (s.w || s.h) ? s : Size{1, theme().padding};
}
void Separator::paint(Painter &p) {
    p.fillRect({bounds().x, bounds().y + bounds().h / 2, bounds().w, 1}, p.theme().border);
}

ScrollPane::ScrollPane() {
    setLayout(std::make_unique<BorderLayout>());
    add<Container>();
}
Container &ScrollPane::view() {
    return *dynamic_cast<Container *>(children().front().get());
}
void ScrollPane::setView(std::unique_ptr<Component> c) {
    if (!children().empty())
        remove(*children().front());
    add(std::move(c));
    revalidate();
}
f32 ScrollPane::scrollOffset() const {
    return offset_;
}
void ScrollPane::updateView() {
    if (children().empty())
        return;
    Component *c = children().front().get();
    Size s = c->preferredSize();
    c->setBounds({bounds().x, bounds().y - offset_, bounds().w - 10, std::max(bounds().h, s.h)});
}
void ScrollPane::onMouseWheel(f32 a) {
    if (children().empty())
        return;
    const f32 max = std::max(0.0f, children().front()->preferredSize().h - bounds().h);
    offset_ = std::clamp(offset_ - a * 24, 0.0f, max);
    updateView();
    repaint();
}
void ScrollPane::paint(Painter &p) {
    updateView();
    p.drawBorder(bounds(), p.theme().border);
    if (!children().empty()) {
        const f32 full = std::max(bounds().h, children().front()->preferredSize().h);
        const f32 thumb = bounds().h * bounds().h / full;
        p.fillRoundedRect({bounds().x + bounds().w - 7, bounds().y + offset_ / full * bounds().h, 5, thumb}, 2.5f, p.theme().border);
    }
}

TabbedPane::TabbedPane() {
}
Container &TabbedPane::addTab(std::string title) {
    titles_.push_back(std::move(title));
    auto &page = add<Container>();
    page.setLayout(std::make_unique<BoxLayout>(Axis::Y, theme().padding / 2));
    page.setVisible(titles_.size() == 1);
    revalidate();
    return page;
}
int TabbedPane::selectedIndex() const {
    return selected_;
}
void TabbedPane::setSelectedIndex(int i) {
    if (i < 0 || i >= static_cast<int>(titles_.size()) || i == selected_)
        return;
    selected_ = i;
    for (std::size_t n = 0; n < children().size(); ++n)
        children()[n]->setVisible(static_cast<int>(n) == i);
    repaint();
}
f32 TabbedPane::tabWidth(std::size_t index) const {
    return measureText(titles_[index], theme()) + theme().padding * 2;
}
void TabbedPane::layoutPages() {
    for (const auto &child : children()) {
        child->setBounds(
            {bounds().x, bounds().y + theme().rowHeight, bounds().w, std::max(0.0f, bounds().h - theme().rowHeight)});
    }
}
Size TabbedPane::preferredSize() {
    Size explicitSize = Component::preferredSize();
    if (explicitSize.w || explicitSize.h)
        return explicitSize;
    Size result{};
    f32 tabsWidth = 0;
    for (std::size_t index = 0; index < children().size(); ++index) {
        const Size page = children()[index]->preferredSize();
        result.w = std::max(result.w, page.w);
        result.h = std::max(result.h, page.h);
        tabsWidth += tabWidth(index);
    }
    result.w = std::max(result.w, tabsWidth);
    result.h += theme().rowHeight;
    return result;
}
void TabbedPane::onMousePress(MouseButton b, f32 x, f32 y) {
    if (b != MouseButton::Left || y >= bounds().y + theme().rowHeight)
        return;
    f32 tabX = bounds().x;
    for (std::size_t index = 0; index < titles_.size(); ++index) {
        const f32 width = tabWidth(index);
        if (x >= tabX && x < tabX + width) {
            setSelectedIndex(static_cast<int>(index));
            return;
        }
        tabX += width;
    }
}
void TabbedPane::paint(Painter &p) {
    layoutPages();
    f32 tabX = bounds().x;
    for (std::size_t index = 0; index < titles_.size(); ++index) {
        const f32 width = tabWidth(index);
        Rect tab{tabX, bounds().y, width, p.theme().rowHeight};
        const bool active = static_cast<int>(index) == selected_;
        const Rect pill{tab.x + 2, tab.y + 2, tab.w - 4, tab.h - 4};
        p.fillRoundedRect(pill, p.theme().cornerRadius, active ? p.theme().controlHover : p.theme().control);
        if (active) p.fillRoundedRect({pill.x + 6, pill.y + pill.h - 3, pill.w - 12, 3}, 1.5f, p.theme().accent);
        p.drawText(tab.x + p.theme().padding, tab.y + (tab.h - p.lineHeight()) / 2, titles_[index], p.theme().text);
        tabX += width;
    }
}

Window::Window(std::string t) : title_(std::move(t)) {
    setLayout(std::make_unique<BorderLayout>());
    setInsets({theme().titleHeight, 0, 0, 0});
    auto page = std::make_unique<Container>();
    content_ = page.get();
    content_->setInsets({theme().padding, theme().padding, theme().padding, theme().padding});
    content_->setLayout(std::make_unique<BoxLayout>(Axis::Y, theme().padding / 2));
    add(std::move(page), {BorderRegion::Center});
    setBounds({40, 40, 320, 240});
}
Container &Window::content() {
    return *content_;
}
void Window::setTitle(std::string t) {
    title_ = std::move(t);
    repaint();
}
const std::string &Window::title() const {
    return title_;
}
Size Window::preferredSize() {
    Size s = Component::preferredSize();
    if (s.w || s.h)
        return s;
    s = content_->preferredSize();
    const f32 titleWidth = measureText(title_, theme()) + theme().padding * 4;
    return {std::max(titleWidth, s.w), s.h + theme().titleHeight};
}
void Window::pack() {
    const Size s = preferredSize();
    setBounds({bounds().x, bounds().y, s.w, s.h});
}
void Window::centre(Size s) {
    setBounds({(s.w - bounds().w) / 2, (s.h - bounds().h) / 2, bounds().w, bounds().h});
}
void Window::setClosable(bool v) {
    closable_ = v;
    repaint();
}
bool Window::closable() const {
    return closable_;
}
void Window::setModal(bool v) {
    modal_ = v;
}
bool Window::modal() const {
    return modal_;
}
void Window::addCloseListener(std::function<void()> f) {
    closeListeners_.push_back(std::move(f));
}
void Window::close() {
    if (!visible())
        return;
    setVisible(false);
    for (auto &f : closeListeners_)
        f();
}
void Window::onMousePress(MouseButton b, f32 x, f32 y) {
    if (b != MouseButton::Left || y >= bounds().y + theme().titleHeight)
        return;
    if (closable_ && x >= bounds().x + bounds().w - theme().titleHeight) {
        close();
        return;
    }
    dragging_ = true;
    dragX_ = x - bounds().x;
    dragY_ = y - bounds().y;
}
void Window::onMouseDrag(f32 x, f32 y) {
    if (!dragging_)
        return;
    setBounds({x - dragX_, y - dragY_, bounds().w, bounds().h});
}
void Window::onMouseRelease(MouseButton, f32, f32) {
    dragging_ = false;
}
void Window::paint(Painter &p) {
    const Theme &t = p.theme();
    const f32 r = t.cornerRadius * 1.5f;
    const Rect b = bounds();
    p.pushClip({b.x - 16, b.y - 16, b.w + 32, b.h + 32}); // the shadow reaches outside the window
    for (int k = 3; k >= 1; --k) // soft drop shadow: a few widening, fading layers
        p.fillRoundedRect({b.x - f32(k) * 2 + 3, b.y - f32(k) * 2 + 5, b.w + f32(k) * 4, b.h + f32(k) * 4}, r + f32(k) * 2,
                          (t.shadow & 0x00FFFFFFu) | (((t.shadow >> 24u) / u32(k + 1)) << 24u));
    p.popClip();
    p.fillRoundedRect(b, r, t.window);
    p.pushClip({b.x, b.y, b.w, t.titleHeight}); // title bar: the window's top, rounded
    p.fillRoundedRect(b, r, t.control);
    p.popClip();
    p.strokeRoundedRect(b, r, 1.0f, t.border);
    p.drawText(bounds().x + p.theme().padding, bounds().y + (p.theme().titleHeight - p.lineHeight()) / 2, title_,
               p.theme().text);
    if (closable_) {
        const f32 size = p.theme().titleHeight - p.theme().padding;
        Rect close{bounds().x + bounds().w - size - p.theme().padding / 2, bounds().y + p.theme().padding / 2, size,
                   size};
        const Point c{close.x + close.w / 2, close.y + close.h / 2};
        p.fillCircle(c, close.w / 2, p.theme().controlHover);
        const f32 k = close.w * 0.2f;
        p.drawLine({c.x - k, c.y - k}, {c.x + k, c.y + k}, 1.6f, p.theme().text);
        p.drawLine({c.x - k, c.y + k}, {c.x + k, c.y - k}, 1.6f, p.theme().text);
    }
}
} // namespace phx::ui
