#pragma once

#include "phx/core/Types.hpp"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace phx::ui {

struct Rect {
    f32 x = 0;
    f32 y = 0;
    f32 w = 0;
    f32 h = 0;
    bool contains(f32 px, f32 py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};
struct Size {
    f32 w = 0;
    f32 h = 0;
};
struct Insets {
    f32 top = 0;
    f32 left = 0;
    f32 bottom = 0;
    f32 right = 0;
};
struct Point {
    f32 x = 0;
    f32 y = 0;
};
// Triangle-list vertex (phx::UiRenderer draws vertices in threes).
struct Vertex {
    f32 x, y;             // pixels, top-left origin
    f32 u, v;             // texture coordinates
    u32 rgba;             // display-encoded colour (premultiplied-free; see rgba())
    u32 clipMin, clipMax; // clip rectangle in pixels, packed x | y << 16 (fragments outside are discarded)
    u32 texture;          // 0: font atlas (u, v sample coverage; solid fills use its white texel)
                          // else: bindless sampled slot whose colour multiplies `rgba`
};
static_assert(sizeof(Vertex) == 32);

constexpr u32 rgba(u8 r, u8 g, u8 b, u8 a = 255) {
    return u32(r) | (u32(g) << 8u) | (u32(b) << 16u) | (u32(a) << 24u);
}

struct Theme {
    u32 window = rgba(27, 30, 38, 235);
    u32 panel = rgba(38, 42, 52);
    u32 control = rgba(55, 61, 74);
    u32 controlHover = rgba(70, 79, 96);
    u32 controlPressed = rgba(42, 47, 58);
    u32 border = rgba(112, 122, 142);
    u32 text = rgba(232, 235, 242);
    u32 disabledText = rgba(130, 135, 146);
    u32 accent = rgba(75, 145, 230);
    u32 selection = rgba(45, 92, 150);
    u32 shadow = rgba(0, 0, 0, 110);
    f32 textScale = 2.0f;
    f32 fontSize = 0.0f; // pixels per em for TrueType text; 0 = derived from textScale
    f32 cornerRadius = 4.0f; // windows, buttons, fields
    f32 padding = 8.0f;
    f32 rowHeight = 28.0f;
    f32 titleHeight = 30.0f;
};

f32 measureText(std::string_view text, const Theme &theme = Theme{});

enum class MouseButton : u8 { Left, Right, Middle };
enum class Key : u8 {
    Tab,
    Enter,
    Escape,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    Backspace,
    Delete,
    Space,
    PageUp,
    PageDown,
    Other
};
enum class Alignment : u8 { Left, Center, Right };
enum class BorderRegion : u8 { North, South, East, West, Center };
enum class Axis : u8 { X, Y };
struct Constraint {
    BorderRegion region = BorderRegion::Center;
};

class Component;
class Container;
class Ui;

// Immediate-mode drawing into a triangle list: rectangles, TrueType text (stb_truetype; the
// stb_easy_font bitmap font when no font file is found) and anti-aliased vector shapes.
// Vector shapes get a 1-pixel coverage fringe, so edges are smooth without MSAA.
class Painter {
  public:
    Painter(std::vector<Vertex> &vertices, const Theme &theme, Rect clip);
    void pushClip(Rect clip);
    void popClip();
    [[nodiscard]] Rect clip() const {
        return clips_.back();
    }

    // --- rectangles (pixel-aligned, no fringe)
    void fillRect(Rect rect, u32 colour);
    void fillRectGradient(Rect rect, u32 top, u32 bottom); // vertical
    void drawBorder(Rect rect, u32 colour, f32 thickness = 1.0f);
    // `texture`: a bindless sampled slot; `uv` the part of it to show
    void drawImage(Rect rect, u32 texture, u32 tint = 0xFFFFFFFFu, Rect uv = {0, 0, 1, 1});

    // --- vector shapes (anti-aliased)
    void fillRoundedRect(Rect rect, f32 radius, u32 colour);
    void strokeRoundedRect(Rect rect, f32 radius, f32 width, u32 colour);
    void fillCircle(Point centre, f32 radius, u32 colour);
    void strokeCircle(Point centre, f32 radius, f32 width, u32 colour);
    // Arc from angle a0 to a1 (radians, 0 = +x, clockwise on screen since y points down).
    void strokeArc(Point centre, f32 radius, f32 a0, f32 a1, f32 width, u32 colour);
    // Pie slice / annulus sector filled between two radii (inner 0 = pie).
    void fillSector(Point centre, f32 inner, f32 outer, f32 a0, f32 a1, u32 colour);
    void drawLine(Point a, Point b, f32 width, u32 colour);
    void strokePolyline(std::span<const Point> points, f32 width, u32 colour, bool closed = false);
    // Convex polygon, either winding.
    void fillConvex(std::span<const Point> points, u32 colour);
    void fillTriangle(Point a, Point b, Point c, u32 colour);

    // --- text (y = top of the line box, lineHeight() tall)
    void drawText(f32 x, f32 y, std::string_view text, u32 colour);
    void drawText(f32 x, f32 y, const std::string &text, u32 colour);
    void drawText(f32 x, f32 y, const char *text, u32 colour);
    f32 textWidth(std::string_view text) const;
    f32 lineHeight() const;
    const Theme &theme() const {
        return theme_;
    }

  private:
    void vertex(f32 x, f32 y, f32 u, f32 v, u32 colour, u32 texture = 0);
    void quad(Rect r, Rect uv, u32 colour, u32 texture = 0); // clipped on the CPU
    void triangle(Point a, Point b, Point c, u32 ca, u32 cb, u32 cc);
    void bitmapText(f32 x, f32 y, std::string_view text, u32 colour);
    std::vector<Vertex> &vertices_;
    const Theme &theme_;
    std::vector<Rect> clips_;
    u32 clipMin_ = 0, clipMax_ = 0;
    std::vector<Point> path_, inner_, outer_;
};

// Pixels per em of TrueType text for a theme.
f32 fontPixels(const Theme &theme);
// Outline of a rounded rectangle / circle as a closed polygon (appended to `out`).
void roundedRectPath(Rect rect, f32 radius, std::vector<Point> &out);
void arcPath(Point centre, f32 radius, f32 a0, f32 a1, std::vector<Point> &out);

class LayoutManager {
  public:
    virtual ~LayoutManager() = default;
    virtual void layout(Container &parent) = 0;
    virtual Size preferredSize(const Container &parent) const = 0;
};

class Component {
  public:
    virtual ~Component();
    void setVisible(bool value);
    bool visible() const {
        return visible_;
    }
    void setEnabled(bool value);
    bool enabled() const {
        return enabled_;
    }
    void setPreferredSize(Size value);
    virtual Size preferredSize();
    const Rect &bounds() const {
        return bounds_;
    }
    void setBounds(Rect value);
    void setToolTip(std::string value) {
        toolTip_ = std::move(value);
    }
    const std::string &toolTip() const {
        return toolTip_;
    }
    Container *parent() const {
        return parent_;
    }
    void repaint();
    void revalidate();
    virtual bool focusable() const {
        return false;
    }
    bool focused() const {
        return focused_;
    }
    virtual void onMouseEnter();
    virtual void onMouseExit();
    virtual void onMouseMove(f32 x, f32 y);
    virtual void onMousePress(MouseButton button, f32 x, f32 y);
    virtual void onMouseRelease(MouseButton button, f32 x, f32 y);
    virtual void onMouseDrag(f32 x, f32 y);
    virtual void onMouseWheel(f32 amount);
    virtual void onKey(Key key, bool down, bool shift, bool ctrl);
    virtual void onChar(u32 codepoint);
    virtual void onFocusGained();
    virtual void onFocusLost();

  protected:
    virtual void paint(Painter &painter);
    virtual Component *hitTest(f32 x, f32 y);
    Ui *ui() const;
    const Theme &theme() const;

  private:
    friend class Container;
    friend class Ui;
    virtual void paintTree(Painter &painter);
    Rect bounds_{};
    std::optional<Size> preferred_;
    Container *parent_ = nullptr;
    Ui *owner_ = nullptr;
    bool visible_ = true;
    bool enabled_ = true;
    bool focused_ = false;
    std::string toolTip_;
};

class Container : public Component {
  public:
    template <class T, class... A> T &add(A &&...args) {
        return add(std::make_unique<T>(std::forward<A>(args)...));
    }
    template <class T> T &add(std::unique_ptr<T> child, Constraint constraint = {}) {
        T &result = *child;
        addImpl(std::move(child), constraint);
        return result;
    }
    void remove(Component &child);
    void setLayout(std::unique_ptr<LayoutManager> layout);
    void setInsets(Insets insets);
    const Insets &insets() const {
        return insets_;
    }
    const std::vector<std::unique_ptr<Component>> &children() const {
        return children_;
    }
    Constraint constraint(const Component &child) const;
    Size preferredSize() override;

  protected:
    void paint(Painter &painter) override;
    Component *hitTest(f32 x, f32 y) override;

  private:
    friend class Component;
    friend class Ui;
    void addImpl(std::unique_ptr<Component> child, Constraint constraint);
    void doLayout();
    void paintTree(Painter &painter) override;
    std::vector<std::unique_ptr<Component>> children_;
    std::vector<Constraint> constraints_;
    std::unique_ptr<LayoutManager> layout_;
    Insets insets_{};
    bool layoutDirty_ = true;
};

class BorderLayout final : public LayoutManager {
  public:
    explicit BorderLayout(f32 hgap = 0, f32 vgap = 0) : hgap_(hgap), vgap_(vgap) {
    }
    void layout(Container &parent) override;
    Size preferredSize(const Container &parent) const override;

  private:
    f32 hgap_;
    f32 vgap_;
};
class FlowLayout final : public LayoutManager {
  public:
    explicit FlowLayout(Alignment align = Alignment::Center, f32 hgap = 5, f32 vgap = 5)
        : align_(align), hgap_(hgap), vgap_(vgap) {
    }
    void layout(Container &parent) override;
    Size preferredSize(const Container &parent) const override;

  private:
    Alignment align_;
    f32 hgap_;
    f32 vgap_;
};
class GridLayout final : public LayoutManager {
  public:
    GridLayout(u32 rows, u32 columns, f32 hgap = 0, f32 vgap = 0)
        : rows_(rows), columns_(columns), hgap_(hgap), vgap_(vgap) {
    }
    void layout(Container &parent) override;
    Size preferredSize(const Container &parent) const override;

  private:
    u32 rows_;
    u32 columns_;
    f32 hgap_;
    f32 vgap_;
};
class BoxLayout final : public LayoutManager {
  public:
    explicit BoxLayout(Axis axis, f32 gap = 0) : axis_(axis), gap_(gap) {
    }
    void layout(Container &parent) override;
    Size preferredSize(const Container &parent) const override;

  private:
    Axis axis_;
    f32 gap_;
};
class FormLayout final : public LayoutManager {
  public:
    explicit FormLayout(f32 hgap = 8, f32 vgap = 6) : hgap_(hgap), vgap_(vgap) {
    }
    void layout(Container &parent) override;
    Size preferredSize(const Container &parent) const override;

  private:
    f32 hgap_;
    f32 vgap_;
};

class Label : public Component {
  public:
    explicit Label(std::string text = {});
    void setText(std::string text);
    const std::string &text() const;
    void setAlignment(Alignment alignment);
    Size preferredSize() override;

  protected:
    void paint(Painter &painter) override;

  private:
    std::string text_;
    Alignment alignment_ = Alignment::Left;
};
class Button : public Component {
  public:
    explicit Button(std::string text = {});
    void setText(std::string text);
    void addActionListener(std::function<void()> listener);
    bool focusable() const override {
        return true;
    }
    Size preferredSize() override;
    void onMouseEnter() override;
    void onMouseExit() override;
    void onMousePress(MouseButton, f32, f32) override;
    void onMouseRelease(MouseButton, f32, f32) override;
    void onKey(Key, bool, bool, bool) override;

  protected:
    void paint(Painter &) override;

  private:
    void fire();
    std::string text_;
    std::vector<std::function<void()>> listeners_;
    bool hover_ = false;
    bool pressed_ = false;
};
class CheckBox : public Component {
  public:
    explicit CheckBox(std::string text = {}, bool selected = false);
    bool selected() const;
    void setSelected(bool);
    void addItemListener(std::function<void(bool)> listener);
    bool focusable() const override {
        return true;
    }
    Size preferredSize() override;
    void onMouseRelease(MouseButton, f32, f32) override;
    void onKey(Key, bool, bool, bool) override;

  protected:
    void paint(Painter &) override;

  private:
    void toggle();
    std::string text_;
    bool selected_;
    std::vector<std::function<void(bool)>> listeners_;
};
class ButtonGroup;
class RadioButton : public CheckBox {
  public:
    using CheckBox::CheckBox;
};
class ButtonGroup {
  public:
    void add(RadioButton &button);

  private:
    std::vector<RadioButton *> buttons_;
};
class Slider : public Component {
  public:
    Slider(f32 minimum = 0, f32 maximum = 100, f32 value = 0, f32 step = 0);
    f32 value() const;
    void setValue(f32 value);
    void setRange(f32 minimum, f32 maximum);
    void setStep(f32 step);
    void addChangeListener(std::function<void(f32)> listener);
    void setValueFormatter(std::function<std::string(f32)> formatter);
    bool focusable() const override {
        return true;
    }
    Size preferredSize() override;
    void onMousePress(MouseButton, f32, f32) override;
    void onMouseDrag(f32, f32) override;
    void onMouseWheel(f32) override;
    void onKey(Key, bool, bool, bool) override;

  protected:
    void paint(Painter &) override;

  private:
    f32 valueWidth() const;
    void setFromX(f32);
    f32 minimum_;
    f32 maximum_;
    f32 value_;
    f32 step_;
    std::vector<std::function<void(f32)>> listeners_;
    std::function<std::string(f32)> formatter_;
};
class ComboBox : public Component {
  public:
    explicit ComboBox(std::vector<std::string> items = {});
    int selectedIndex() const;
    void setSelectedIndex(int);
    void setItemEnabled(int index, bool enabled);
    bool itemEnabled(int index) const;
    void addActionListener(std::function<void(int)> listener);
    bool focusable() const override {
        return true;
    }
    Size preferredSize() override;
    void onMousePress(MouseButton, f32, f32) override;
    void onMouseWheel(f32) override;
    void onKey(Key, bool, bool, bool) override;

  protected:
    void paint(Painter &) override;

  private:
    friend class Ui;
    std::vector<std::string> items_;
    std::vector<bool> enabledItems_;
    int selected_ = -1;
    std::vector<std::function<void(int)>> listeners_;
};
class TextField : public Component {
  public:
    explicit TextField(std::string text = {}, u32 columns = 12);
    const std::string &text() const;
    void setText(std::string);
    void addActionListener(std::function<void()>);
    bool focusable() const override {
        return true;
    }
    Size preferredSize() override;
    void onMousePress(MouseButton, f32, f32) override;
    void onKey(Key, bool, bool, bool) override;
    void onChar(u32) override;

  protected:
    void paint(Painter &) override;

  private:
    std::string text_;
    u32 columns_;
    u32 caret_ = 0;
    std::vector<std::function<void()>> listeners_;
};
class Separator : public Component {
  public:
    Size preferredSize() override;

  protected:
    void paint(Painter &) override;
};
class ScrollPane : public Container {
  public:
    ScrollPane();
    Container &view();
    void setView(std::unique_ptr<Component> child);
    f32 scrollOffset() const;
    void onMouseWheel(f32) override;

  protected:
    void paint(Painter &) override;

  private:
    void updateView();
    f32 offset_ = 0;
};
class TabbedPane : public Container {
  public:
    TabbedPane();
    Container &addTab(std::string title);
    int selectedIndex() const;
    void setSelectedIndex(int);
    Size preferredSize() override;

  protected:
    void paint(Painter &) override;
    void onMousePress(MouseButton, f32, f32) override;

  private:
    void layoutPages();
    f32 tabWidth(std::size_t index) const;
    std::vector<std::string> titles_;
    int selected_ = 0;
};
class Window : public Container {
  public:
    explicit Window(std::string title = {});
    Container &content();
    void setTitle(std::string);
    const std::string &title() const;
    void pack();
    void centre(Size screen);
    void centre(f32 w, f32 h) {
        centre({w, h});
    }
    void setClosable(bool value);
    bool closable() const;
    void setModal(bool value);
    bool modal() const;
    void addCloseListener(std::function<void()> listener);
    void close();
    Size preferredSize() override;
    void onMousePress(MouseButton, f32, f32) override;
    void onMouseDrag(f32, f32) override;
    void onMouseRelease(MouseButton, f32, f32) override;

  protected:
    void paint(Painter &) override;

  private:
    friend class Ui;
    std::string title_;
    Container *content_ = nullptr;
    bool closable_ = true;
    bool modal_ = false;
    bool dragging_ = false;
    f32 dragX_ = 0;
    f32 dragY_ = 0;
    std::vector<std::function<void()>> closeListeners_;
};

class Ui {
  public:
    explicit Ui(Theme theme = {});
    Window &addWindow(std::string title);
    void setScreenSize(f32 w, f32 h);
    bool mouseMove(f32 x, f32 y);
    bool mouseButton(MouseButton button, bool down);
    bool mouseWheel(f32 amount);
    bool key(Key key, bool down, bool shift = false, bool ctrl = false);
    bool character(u32 codepoint);
    bool wantsMouse() const;
    bool wantsKeyboard() const;
    bool anyWindowVisible() const;
    bool build(std::vector<Vertex> &out);
    u64 revision() const {
        return revision_;
    }
    const Theme &theme() const {
        return theme_;
    }

  private:
    friend class Component;
    friend class ComboBox;
    friend class Window;
    void dirty(bool layout = false);
    Component *hit(f32 x, f32 y) const;
    Window *frontWindow() const;
    void setFocus(Component *component);
    void cycleFocus(bool reverse);
    void collectFocusable(Component &, std::vector<Component *> &);
    void openCombo(ComboBox &combo);
    void closePopup();
    bool popupHit(f32 x, f32 y, bool select);
    void bringToFront(Window &window);
    void windowHidden(Window &window);
    Theme theme_;
    u32 fontGeneration_ = 0;
    Size screen_{};
    std::vector<std::unique_ptr<Window>> windows_;
    Component *hover_ = nullptr;
    Component *capture_ = nullptr;
    Component *focus_ = nullptr;
    ComboBox *popup_ = nullptr;
    Rect popupRect_{};
    f32 mouseX_ = 0;
    f32 mouseY_ = 0;
    bool dirty_ = true;
    u64 revision_ = 0;
    int popupFirst_ = 0;
    int popupHover_ = -1;
};

} // namespace phx::ui
