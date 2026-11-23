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
    CHECK(vertices.size() % 4 == 0);
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
    for (const Vertex &v : clippedVertices)
        CHECK(v.x >= 0 && v.x <= 300 && v.y >= 0 && v.y <= 200);

    if (failures)
        return 1;
    std::puts("all UI tests passed");
    return 0;
}
