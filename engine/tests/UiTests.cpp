#include "phx/ui/Font.hpp"
#include "phx/ui/Ui.hpp"

#include <cmath>
#include <cstdio>

using namespace phx;
using namespace phx::ui;

static int failures = 0;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                                          \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (false)
static bool near(f32 a, f32 b) {
    return std::abs(a - b) < 0.01f;
}
struct TestContainer : Container {
    using Container::hitTest;
};

static u32 alphaAt(const std::vector<Vertex> &vs, f32 x, f32 y) { // alpha of the vertex nearest (x, y)
    const Vertex *best = nullptr;
    f32 bd = 1e30f;
    for (const Vertex &v : vs) {
        const f32 d = (v.x - x) * (v.x - x) + (v.y - y) * (v.y - y);
        if (d < bd)
            bd = d, best = &v;
    }
    return best ? best->rgba >> 24u : 0;
}

static void vectorTests() {
    Theme theme;
    std::vector<Vertex> vs;
    Painter p(vs, theme, {0, 0, 400, 400});
    const u32 red = rgba(255, 0, 0);
    // convex fill: triangles, opaque core, transparent 1-px fringe half a pixel outside
    p.fillRoundedRect({100, 100, 80, 40}, 10, red);
    CHECK(!vs.empty() && vs.size() % 3 == 0);
    {
        std::vector<Vertex> sq;
        Painter q(sq, theme, {0, 0, 400, 400});
        const Point square[4] = {{100, 100}, {180, 100}, {180, 140}, {100, 140}};
        q.fillConvex(square, red);
        CHECK(alphaAt(sq, 100.5f, 100.5f) == 255); // corner, half a pixel inside: opaque
        CHECK(alphaAt(sq, 99.5f, 99.5f) == 0);     // half a pixel outside: transparent
    }
    f32 minX = 1e9f, maxX = -1e9f;
    for (const Vertex &v : vs)
        minX = std::min(minX, v.x), maxX = std::max(maxX, v.x);
    CHECK(near(minX, 99.5f) && near(maxX, 180.5f));
    // both windings fill the same way
    vs.clear();
    const Point cw[3] = {{0, 0}, {10, 0}, {0, 10}}, ccw[3] = {{0, 0}, {0, 10}, {10, 0}};
    p.fillConvex(cw, red);
    const std::size_t n = vs.size();
    std::vector<Vertex> a = vs;
    vs.clear();
    p.fillConvex(ccw, red);
    CHECK(vs.size() == n);
    f32 ax = 1e9f, bx = 1e9f;
    for (const Vertex &v : a)
        ax = std::min(ax, v.x);
    for (const Vertex &v : vs)
        bx = std::min(bx, v.x);
    CHECK(near(ax, bx) && ax < 0); // fringe outside in both cases
    // strokes: an open line has a core, fringes and caps; hairlines fade instead of thinning
    vs.clear();
    p.drawLine({10, 10}, {110, 10}, 4, red);
    CHECK(!vs.empty() && vs.size() % 3 == 0);
    CHECK(alphaAt(vs, 60, 11.5f) == 255 && alphaAt(vs, 60, 12.5f) == 0);
    vs.clear();
    p.drawLine({10, 10}, {110, 10}, 0.5f, red);
    CHECK(alphaAt(vs, 60, 10) < 255 && alphaAt(vs, 60, 10) > 100);
    // degenerate input draws nothing
    vs.clear();
    const Point one[1] = {{5, 5}};
    p.strokePolyline(one, 2, red);
    p.fillConvex(one, red);
    p.fillRoundedRect({0, 0, 50, 50}, 5, rgba(0, 0, 0, 0));
    CHECK(vs.empty());
    // the current clip travels with every vertex
    p.pushClip({20, 30, 40, 50});
    p.fillCircle({40, 50}, 30, red);
    for (const Vertex &v : vs)
        CHECK(v.clipMin == (20u | (30u << 16)) && v.clipMax == (60u | (80u << 16)));
    p.popClip();
    // arcs and sectors
    vs.clear();
    p.strokeArc({200, 200}, 50, 0, 3.14159f, 3, red);
    p.fillSector({200, 200}, 20, 50, 0, 1.5f, red);
    CHECK(!vs.empty() && vs.size() % 3 == 0);
}

static void fontTests() {
    std::size_t i = 0;
    const std::string_view s = "A\xea\xb0\x80\xe2\x82\xac\xff"; // A, 가, €, invalid
    CHECK(decodeUtf8(s, i) == 'A');
    CHECK(decodeUtf8(s, i) == 0xAC00);
    CHECK(decodeUtf8(s, i) == 0x20AC);
    CHECK(decodeUtf8(s, i) == 0xFFFD && i == s.size());
    i = 0;
    CHECK(decodeUtf8(std::string_view("\xea\xb0", 2), i) == 0xFFFD); // truncated
    FontAtlas &f = fonts();
    if (!f.available()) {
        std::puts("(no system font: TrueType tests skipped)");
        return;
    }
    CHECK(f.measure("iiii", 20) < f.measure("WWWW", 20)); // proportional
    CHECK(near(f.measure("AB", 20) * 2, f.measure("AB", 40)) || std::abs(f.measure("AB", 20) * 2 - f.measure("AB", 40)) < 2);
    std::vector<GlyphQuad> q;
    const f32 end = f.layout(10, 50, "Hi", 24, q);
    CHECK(q.size() == 2 && end > 10 && q[0].y1 <= 51 && q[0].y0 < 50); // glyphs above the baseline
    CHECK(q[0].u1 > q[0].u0 && q[0].v1 > q[0].v0);
    (void)f.takeDirty();
    q.clear();
    f.layout(0, 50, "Hi", 24, q); // cached: nothing new rasterised
    CHECK(f.takeDirty().empty());
    f.layout(0, 50, "Hi", 25, q); // another size: new glyphs
    CHECK(!f.takeDirty().empty());
    CHECK(f.pixels()[1 * FontAtlas::kSize + 1] == 255); // white texel for solid fills
}

static void layoutTests() {
    TestContainer border;
    border.setBounds({0, 0, 300, 200});
    border.setLayout(std::make_unique<BorderLayout>());
    auto &north = border.add<Label>(std::make_unique<Label>("n"), Constraint{BorderRegion::North});
    north.setPreferredSize({20, 30});
    auto &west = border.add<Label>(std::make_unique<Label>("w"), Constraint{BorderRegion::West});
    west.setPreferredSize({50, 20});
    auto &centre = border.add<Label>(std::make_unique<Label>("c"));
    (void)border.preferredSize();
    std::vector<Vertex> scratch;
    Painter painter(scratch, Theme{}, {0, 0, 300, 200});
    // A rootless container lays out while hit-testing too.
    border.hitTest(1, 1);
    CHECK(near(north.bounds().h, 30));
    CHECK(near(west.bounds().w, 50));
    CHECK(near(centre.bounds().x, 50));

    TestContainer grid;
    grid.setBounds({0, 0, 200, 100});
    grid.setLayout(std::make_unique<GridLayout>(2, 2, 10, 10));
    auto &g0 = grid.add<Label>("0");
    auto &g1 = grid.add<Label>("1");
    auto &g2 = grid.add<Label>("2");
    grid.hitTest(1, 1);
    CHECK(near(g0.bounds().w, 95));
    CHECK(near(g1.bounds().x, 105));
    CHECK(near(g2.bounds().y, 55));

    TestContainer form;
    form.setBounds({0, 0, 300, 80});
    form.setLayout(std::make_unique<FormLayout>(10, 4));
    auto &label = form.add<Label>("label");
    label.setPreferredSize({60, 20});
    auto &field = form.add<TextField>();
    field.setPreferredSize({100, 24});
    form.hitTest(1, 1);
    CHECK(near(field.bounds().x, 70));
    CHECK(near(label.bounds().h, 24));
}

static void hiddenWindowInputTest() {
    Ui ui;
    ui.setScreenSize(320, 200);
    Window &window = ui.addWindow("Input");
    window.setBounds({10, 10, 200, 120});
    auto &field = window.content().add<TextField>();
    field.setPreferredSize({120, 28});
    std::vector<Vertex> vertices;
    ui.build(vertices);

    ui.mouseMove(field.bounds().x + 2, field.bounds().y + 2);
    ui.mouseButton(MouseButton::Left, true);
    ui.mouseButton(MouseButton::Left, false);
    CHECK(ui.wantsKeyboard());

    window.setVisible(false);
    CHECK(!ui.wantsKeyboard());
    CHECK(!ui.key(Key::Space, true));
    CHECK(!ui.character('x'));
    CHECK(!ui.key(Key::Escape, true));
}

static void tabbedPaneTest() {
    Ui ui;
    ui.setScreenSize(480, 320);
    Window &window = ui.addWindow("Tabs");
    window.setBounds({20, 20, 360, 240});
    auto &tabs = window.content().add<TabbedPane>();
    tabs.setPreferredSize({300, 160});
    tabs.addTab("Short").add<Label>("First");
    Container &second = tabs.addTab("A much wider tab");
    auto &button = second.add<Button>("Inside second tab");
    int clicks = 0;
    button.addActionListener([&] { ++clicks; });

    std::vector<Vertex> vertices;
    ui.build(vertices);
    tabs.setSelectedIndex(1);
    ui.build(vertices);
    CHECK(button.bounds().w > 0);
    CHECK(button.bounds().h > 0);

    ui.mouseMove(button.bounds().x + 2, button.bounds().y + 2);
    ui.mouseButton(MouseButton::Left, true);
    ui.mouseButton(MouseButton::Left, false);
    CHECK(clicks == 1);
}

int main() {
    layoutTests();
    hiddenWindowInputTest();
    tabbedPaneTest();
    Ui ui;
    ui.setScreenSize(640, 480);
    Window &w = ui.addWindow("Settings");
    w.setBounds({50, 50, 300, 320});
    auto &panel = w.content();
    panel.setLayout(std::make_unique<BoxLayout>(Axis::Y, 4));
    auto &button = panel.add<Button>("Apply");
    auto &slider = panel.add<Slider>(0, 10, 0, 2);
    auto &combo = panel.add<ComboBox>(std::vector<std::string>{"One", "Two", "Three"});
    auto &check = panel.add<CheckBox>("Enabled");
    auto &other = panel.add<Button>("Other");
    button.setPreferredSize({200, 30});
    slider.setPreferredSize({200, 30});
    combo.setPreferredSize({200, 30});
    check.setPreferredSize({200, 30});
    other.setPreferredSize({200, 30});
    int clicks = 0, changes = 0, selections = 0;
    button.addActionListener([&] { ++clicks; });
    slider.addChangeListener([&](f32) { ++changes; });
    combo.addActionListener([&](int) { ++selections; });
    std::vector<Vertex> vertices;
    CHECK(ui.build(vertices));
    CHECK(!vertices.empty());
    CHECK(vertices.size() % 3 == 0); // triangle list
    const std::size_t oldSize = vertices.size();
    CHECK(!ui.build(vertices));
    CHECK(vertices.size() == oldSize);

    ui.mouseMove(button.bounds().x + 5, button.bounds().y + 5);
    CHECK(ui.build(vertices));
    ui.mouseMove(panel.bounds().x + panel.bounds().w - 1, panel.bounds().y + panel.bounds().h - 1);
    CHECK(ui.build(vertices));
    ui.mouseMove(10, 10);
    CHECK(!ui.build(vertices));

    auto click = [&](f32 x, f32 y) {
        ui.mouseMove(x, y);
        ui.mouseButton(MouseButton::Left, true);
        ui.mouseButton(MouseButton::Left, false);
    };
    click(button.bounds().x + 5, button.bounds().y + 5);
    CHECK(clicks == 1);
    ui.mouseMove(button.bounds().x + 5, button.bounds().y + 5);
    ui.mouseButton(MouseButton::Left, true);
    ui.mouseMove(500, 400);
    ui.mouseButton(MouseButton::Left, false);
    CHECK(clicks == 1);
    CHECK(ui.build(vertices));

    ui.mouseMove(slider.bounds().x + 2, slider.bounds().y + 10);
    ui.mouseButton(MouseButton::Left, true);
    ui.mouseMove(slider.bounds().x + slider.bounds().w + 100, slider.bounds().y + 10);
    ui.mouseButton(MouseButton::Left, false);
    CHECK(near(slider.value(), 10));
    CHECK(changes > 0);
    slider.setValue(3);
    CHECK(near(slider.value(), 4));

    click(combo.bounds().x + 5, combo.bounds().y + 5);
    const f32 popupY = combo.bounds().y + combo.bounds().h;
    click(combo.bounds().x + 5, popupY + 2 * ui.theme().rowHeight + 4);
    CHECK(combo.selectedIndex() == 2);
    CHECK(selections == 1);
    combo.setItemEnabled(1, false);
    click(combo.bounds().x + 5, combo.bounds().y + 5);
    click(combo.bounds().x + 5, popupY + ui.theme().rowHeight + 4);
    CHECK(combo.selectedIndex() == 2);

    click(check.bounds().x + 5, check.bounds().y + 5);
    CHECK(check.selected());
    ui.key(Key::Space, true);
    CHECK(!check.selected());
    ui.key(Key::Tab, true);
    ui.key(Key::Space, true);
    CHECK(clicks == 1);

    ui.mouseMove(10, 10);
    CHECK(!ui.wantsMouse());
    ui.mouseMove(60, 60);
    CHECK(ui.wantsMouse());
    const f32 oldX = w.bounds().x;
    ui.mouseButton(MouseButton::Left, true);
    ui.mouseMove(100, 100);
    ui.mouseButton(MouseButton::Left, false);
    CHECK(w.bounds().x != oldX);
    click(w.bounds().x + w.bounds().w - 10, w.bounds().y + 10);
    CHECK(!w.visible());

    Ui clipped;
    clipped.setScreenSize(300, 200);
    Window &cw = clipped.addWindow("Clip");
    cw.setBounds({10, 10, 200, 150});
    auto &scroll = cw.content().add<ScrollPane>();
    scroll.setBounds({20, 50, 150, 80});
    auto tall = std::make_unique<Container>();
    tall->setPreferredSize({140, 500});
    tall->add<Label>("far").setBounds({20, 450, 50, 20});
    scroll.setView(std::move(tall));
    std::vector<Vertex> clippedVertices;
    clipped.build(clippedVertices);
    // vector shapes are clipped per fragment: every vertex carries a clip rect inside the screen
    for (const Vertex &v : clippedVertices)
        CHECK((v.clipMax & 0xFFFFu) <= 300 && (v.clipMax >> 16) <= 200);
    // rectangles and text glyphs are clipped on the CPU: their vertices stay on screen
    for (const Vertex &v : clippedVertices)
        if (v.u != FontAtlas::whiteU())
            CHECK(v.x >= 0 && v.x <= 300 && v.y >= 0 && v.y <= 200);

    vectorTests();
    fontTests();

    if (failures)
        return 1;
    std::puts("all UI tests passed");
    return 0;
}
