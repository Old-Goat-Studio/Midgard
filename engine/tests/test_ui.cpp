// SPDX-License-Identifier: Apache-2.0
// The in-game UI engine (milestone 6.0): RmlUi screens drawn by the renderer's UI layer, their
// input (mouse, keys, gamepad, touch), scale and safe area, and the screen checks.
#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <string>

#include <RmlUi/Core.h>

#include "midgard/render/renderer.h"
#include "midgard/ui/fonts.h"
#include "midgard/ui/hud.h"
#include "midgard/ui/ui.h"

namespace render = midgard::render;
namespace ui = midgard::ui;
using T = ui::Input::Type;

namespace {

std::unique_ptr<render::Renderer> offscreen(uint32_t w, uint32_t h) {
    try {
        render::RendererDesc desc;
        desc.offscreen = true;
        desc.width = w;
        desc.height = h;
        return std::make_unique<render::Renderer>(desc);
    } catch (const std::exception& e) {
        MESSAGE("no Vulkan here: " << e.what());
        return nullptr;
    }
}

ui::Options sized(uint32_t w, uint32_t h, float scale = 1.0f) {
    ui::Options o;
    o.width = w;
    o.height = h;
    o.scale = scale;
    return o;
}

ui::Input at(T type, float x, float y) {
    ui::Input in;
    in.type = type;
    in.x = x;
    in.y = y;
    return in;
}

ui::Input named(T type, const std::string& key) {
    ui::Input in;
    in.type = type;
    in.key = key;
    return in;
}

// An element's middle, in pixels.
std::pair<float, float> middle(Rml::Element* e) {
    const Rml::Vector2f p = e->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f s = e->GetBox().GetSize(Rml::BoxArea::Border);
    return {p.x + s.x / 2, p.y + s.y / 2};
}

void click(ui::Ui& u, Rml::Element* e) {
    const auto [x, y] = middle(e);
    CHECK(u.handle(at(T::PointerMove, x, y)));
    CHECK(u.handle(at(T::PointerDown, x, y)));
    u.handle(at(T::PointerUp, x, y));  // (a button that closes its screen leaves nothing here)
}

void pad(ui::Ui& u, const std::string& button) {
    u.handle(named(T::PadDown, button));
    u.handle(named(T::PadUp, button));
}

std::string text_of(Rml::ElementDocument* doc, const std::string& id) {
    Rml::Element* e = doc->GetElementById(id);
    return e ? e->GetInnerRML() : "(none)";
}

std::string focused(ui::Ui& u) {
    Rml::Element* e = u.context()->GetFocusElement();
    return e ? e->GetId() : "";
}

}  // namespace

TEST_CASE("UI colours become linear for the sRGB framebuffer, premultiplied") {
    const uint8_t white[4] = {255, 255, 255, 255};
    const glm::vec4 w = render::ui_linear(white);
    CHECK(w.r == doctest::Approx(1.0f));
    CHECK(w.a == doctest::Approx(1.0f));
    const uint8_t half_grey[4] = {94, 94, 94, 128};  // sRGB 0.735 at half alpha, premultiplied
    const glm::vec4 g = render::ui_linear(half_grey);
    CHECK(g.a == doctest::Approx(128 / 255.0f));
    CHECK(g.r / g.a == doctest::Approx(render::srgb_to_linear(94.0f / 128.0f)).epsilon(0.01));
    const uint8_t clear[4] = {0, 0, 0, 0};
    CHECK(render::ui_linear(clear) == glm::vec4(0.0f));
    // Pixels map to clip space: the top-left corner to (-1, -1), the bottom-right to (1, 1).
    const glm::mat4 m = render::ui_projection(200, 100);
    CHECK((m * glm::vec4(0, 0, 0, 1)).x == doctest::Approx(-1.0f));
    CHECK((m * glm::vec4(200, 100, 0, 1)).y == doctest::Approx(1.0f));
}

TEST_CASE("Midgard's fonts are built in") {
    CHECK(ui::fonts::inconsolata().size() == 58464);
    CHECK(ui::fonts::opendyslexic_regular().size() > 100000);
    CHECK(ui::fonts::opendyslexic_bold().size() > 100000);
    // (OpenType and TrueType files start with their version tags)
    CHECK(ui::fonts::inconsolata()[0] == 'O');
    CHECK(ui::fonts::opendyslexic_regular()[0] == 0);
}

TEST_CASE("the UI test screen passes its checks at desktop and phone sizes") {
    auto renderer = offscreen(1280, 720);
    if (!renderer) return;
    {
        ui::Ui u(*renderer, sized(1280, 720));
        Rml::ElementDocument* screen = ui::open_test_screen(u);
        REQUIRE(screen);
        u.update(0.0);
        const auto problems = u.check(screen, false);
        for (const auto& p : problems) MESSAGE(p.kind << " " << p.element << ": " << p.message);
        CHECK(problems.empty());
    }
    {
        ui::Options phone = sized(540, 1170, 1.5f);
        phone.safe_area = {0, 90, 0, 60};
        ui::Ui u(*renderer, phone);
        Rml::ElementDocument* screen = ui::open_test_screen(u);
        u.update(0.0);
        const auto problems = u.check(screen, true);
        for (const auto& p : problems) MESSAGE(p.kind << " " << p.element << ": " << p.message);
        CHECK(problems.empty());
        // The safe area pads the screen: its panel starts below the notch.
        CHECK(screen->GetElementById("panel")->GetAbsoluteOffset().y >= 90.0f);
    }
}

TEST_CASE("the checks find what a gamepad can't reach, small targets and text that doesn't fit") {
    auto renderer = offscreen(800, 600);
    if (!renderer) return;
    ui::Options o = sized(800, 600);
    o.safe_area = {0, 30, 0, 0};  // (a notch along the top)
    ui::Ui u(*renderer, o);
    Rml::ElementDocument* doc = u.open_text(R"(<rml><head>
        <link type="text/rcss" href="midgard://base.rcss"/>
        <style>
            #tiny { min-height: 0; height: 20dp; width: 20dp; padding: 0; }
            #stuck { nav: none; position: absolute; left: 600dp; top: 500dp; }
            #narrow { width: 40dp; white-space: nowrap; }
            #off { position: absolute; left: 780dp; top: 10dp; }
            #notch { position: absolute; left: 300dp; top: 2dp; }
        </style></head>
        <body>
            <button id="first">First</button>
            <button id="tiny">x</button>
            <p id="narrow">far too long to fit in forty</p>
            <button id="off">Off</button>
            <button id="notch">Notch</button>
        </body></rml>)",
                                            "bad.rml", true);
    REQUIRE(doc);
    u.update(0.0);
    const auto problems = u.check(doc, true);
    const auto has = [&](const std::string& kind, const std::string& element) {
        for (const auto& p : problems)
            if (p.kind == kind && p.element == element) return true;
        return false;
    };
    CHECK(has("small_target", "#tiny"));
    CHECK(has("text_overflow", "#narrow"));
    CHECK(has("off_screen", "#off"));
    CHECK(has("outside_safe_area", "#notch"));
    CHECK_FALSE(has("small_target", "#first"));
    // Without touch, only what matters to a mouse and a gamepad.
    for (const auto& p : u.check(doc, false)) {
        CHECK(p.kind != "small_target");
        CHECK(p.kind != "outside_safe_area");
    }
    // A screen too tall for the window passes when it scrolls.
    u.close(doc);
    Rml::ElementDocument* tall = u.open_text(R"(<rml><head>
        <link type="text/rcss" href="midgard://base.rcss"/>
        <style>body { overflow-y: auto; } div { height: 900dp; }</style></head>
        <body><div/><button id="below">Below</button></body></rml>)",
                                             "tall.rml", true);
    u.update(0.1);
    CHECK(u.check(tall, false).empty());
    tall->SetProperty("overflow-y", "visible");
    const auto unscrolled = u.check(tall, false);
    REQUIRE(unscrolled.size() == 1);
    CHECK(unscrolled[0].kind == "off_screen");
}

TEST_CASE("the mouse presses buttons, and a menu takes the input while the HUD lets it through") {
    auto renderer = offscreen(1280, 720);
    if (!renderer) return;
    ui::Ui u(*renderer, sized(1280, 720));
    Rml::ElementDocument* hud = u.open_text(R"(<rml><head>
        <link type="text/rcss" href="midgard://base.rcss"/></head>
        <body><p id="score" style="position: absolute; left: 10dp; top: 10dp;">Score 0</p>
        </body></rml>)",
                                            "hud.rml", false);
    REQUIRE(hud);
    u.update(0.0);
    CHECK_FALSE(u.modal());
    // Over the HUD's empty background the game keeps the mouse; and the keys.
    CHECK_FALSE(u.handle(at(T::PointerMove, 640, 400)));
    CHECK_FALSE(u.handle(named(T::KeyDown, "W")));

    Rml::ElementDocument* screen = ui::open_test_screen(u);
    u.update(0.1);
    CHECK(u.modal());
    click(u, screen->GetElementById("one"));
    u.update(0.2);
    CHECK(text_of(screen, "status") == "Pressed One (1 time)");
    click(u, screen->GetElementById("one"));
    u.update(0.3);
    CHECK(text_of(screen, "status") == "Pressed One (2 times)");
    // A menu takes everything, even where nothing of it is.
    CHECK(u.handle(at(T::PointerMove, 5, 5)));
    CHECK(u.handle(named(T::KeyDown, "W")));
    click(u, screen->GetElementById("close"));
    u.update(0.4);
    CHECK_FALSE(u.modal());
}

TEST_CASE("a gamepad moves between controls, into and out of a list, presses and goes back") {
    auto renderer = offscreen(1280, 720);
    if (!renderer) return;
    ui::Ui u(*renderer, sized(1280, 720));
    Rml::ElementDocument* screen = ui::open_test_screen(u);
    u.update(0.0);
    pad(u, "dpdown");  // (the first press finds a control)
    CHECK(focused(u) == "one");
    pad(u, "dpright");
    CHECK(focused(u) == "two");
    pad(u, "a");
    u.update(0.1);
    CHECK(text_of(screen, "status") == "Pressed Two (1 time)");
    // Down past the check box, slider and text field into the scrolling list, and out of it.
    for (int i = 0; i < 4; ++i) pad(u, "dpdown");
    CHECK(focused(u) == "item1");
    for (int i = 0; i < 9; ++i) pad(u, "dpdown");
    CHECK(focused(u) == "item10");
    pad(u, "dpdown");
    CHECK(focused(u) == "close");
    pad(u, "dpup");
    CHECK(focused(u).rfind("item", 0) == 0);
    // A held direction repeats.
    u.handle(named(T::PadDown, "dpup"));
    const std::string before = focused(u);
    u.update(1.0);  // (past the delay)
    u.update(1.2);
    u.handle(named(T::PadUp, "dpup"));
    CHECK(focused(u) != before);
    // The stick moves too.
    ui::Input stick = named(T::PadAxis, "lefty");
    stick.x = 0.9f;
    const std::string was = focused(u);
    u.handle(stick);
    CHECK(focused(u) != was);
    stick.x = 0.0f;
    u.handle(stick);
    // B goes back: the test screen closes.
    pad(u, "b");
    u.update(2.0);
    CHECK_FALSE(u.modal());
}

TEST_CASE("typing goes to a text field, and touch presses like a click") {
    auto renderer = offscreen(1280, 720);
    if (!renderer) return;
    bool keyboard = false;
    ui::Options o = sized(1280, 720);
    o.platform.keyboard = [&](bool on, float, float) { keyboard = on; };
    ui::Ui u(*renderer, o);
    Rml::ElementDocument* screen = ui::open_test_screen(u);
    u.update(0.0);
    Rml::Element* name = screen->GetElementById("name");
    click(u, name);
    u.update(0.1);
    CHECK(keyboard);  // (a phone shows its keyboard)
    CHECK(u.handle(named(T::KeyDown, "End")));
    ui::Input text;
    text.type = T::Text;
    text.text = "ine";
    CHECK(u.handle(text));
    CHECK(name->GetAttribute<Rml::String>("value", "") == "Heroine");
    // A finger's tap presses a button.
    const auto [x, y] = middle(screen->GetElementById("two"));
    ui::Input finger = at(T::TouchDown, x, y);
    finger.touch = 7;
    CHECK(u.handle(finger));
    finger.type = T::TouchUp;
    CHECK(u.handle(finger));
    u.update(0.2);
    CHECK(text_of(screen, "status") == "Pressed Two (1 time)");
    CHECK_FALSE(keyboard);
}

TEST_CASE("the UI is drawn over the scene, scaled, and gives its textures back") {
    auto renderer = offscreen(320, 240);
    if (!renderer) return;
    const render::Camera camera;
    const render::Light light;
    const auto pixel = [&](int x, int y) {
        const auto rgba = renderer->read_pixels();
        const size_t i = (size_t(y) * 320 + x) * 4;
        return glm::ivec3(rgba[i], rgba[i + 1], rgba[i + 2]);
    };
    renderer->render_frame(camera, light, {}, {0, 0, 0, 1});
    CHECK(pixel(160, 120) == glm::ivec3(0));
    {
        ui::Ui u(*renderer, sized(320, 240, 2.0f));
        CHECK(u.dp_ratio() == doctest::Approx(2.0f));
        Rml::ElementDocument* doc = u.open_text(R"(<rml><head>
            <link type="text/rcss" href="midgard://base.rcss"/></head>
            <body><div id="box" style="position: absolute; left: 50dp; top: 40dp; width: 20dp;
                height: 10dp; background-color: #ff0000;"></div>Hello</body></rml>)",
                                                "box.rml", false);
        REQUIRE(doc);
        u.update(0.0);
        // (dp follow the scale: the box is at 100, 80 and 40 x 20 pixels)
        CHECK(doc->GetElementById("box")->GetAbsoluteOffset().x == doctest::Approx(100.0f));
        u.render();
        renderer->render_frame(camera, light, {}, {0, 0, 0, 1});
        CHECK(pixel(120, 90) == glm::ivec3(255, 0, 0));
        CHECK(pixel(150, 90) == glm::ivec3(0));
        CHECK(renderer->ui_texture_count() >= 1);  // (the font's glyphs)
        // Only one UI at a time.
        CHECK_THROWS_AS(ui::Ui(*renderer, sized(320, 240)), ui::UiError);
    }
    CHECK(renderer->ui_texture_count() == 0);
    renderer->set_ui({});
    renderer->render_frame(camera, light, {}, {0, 0, 0, 1});
    CHECK(pixel(120, 90) == glm::ivec3(0));
    // And another can start afterwards.
    ui::Ui again(*renderer, sized(320, 240));
    CHECK(again.open_text("<rml><body/></rml>", "empty.rml", false));
}

TEST_CASE("a timer's seconds read as a clock") {
    CHECK(ui::clock_text(0.0) == "0:00");
    CHECK(ui::clock_text(6.2) == "0:07");  // (whole seconds left, rounded up)
    CHECK(ui::clock_text(65.0) == "1:05");
    CHECK(ui::clock_text(3723.0) == "1:02:03");
    CHECK(ui::clock_text(-5.0) == "0:00");
}

TEST_CASE("the HUD shows the game's stats, timers, notes and prompt, and follows them") {
    auto renderer = offscreen(1280, 720);
    if (!renderer) return;
    ui::Ui u(*renderer, sized(1280, 720));
    ui::Hud hud(u);
    ui::HudData d;
    d.stats = {{"health", "Health", true, 72.0, 100.0},
               {"score", "Score", false, 40.0, 0.0},
               {"keys", "Keys", false, 3.0, 5.0}};
    d.timers = {{"bomb", "Bomb", 65.0}};
    d.notes = {{"Picked up a key", 1.0f}};
    hud.show(d);
    u.update(0.0);
    Rml::ElementDocument* doc = hud.document();
    REQUIRE(doc);
    CHECK_FALSE(u.modal());  // (it never takes the game's input)
    Rml::Element* bar = doc->GetElementById("bar-health");
    REQUIRE(bar);
    const std::string text = bar->GetInnerRML();
    CHECK(text.find("72/100") != std::string::npos);
    Rml::ElementList fills;
    doc->GetElementsByClassName(fills, "fill");
    REQUIRE(fills.size() == 1);
    const float track = fills[0]->GetParentNode()->GetBox().GetSize().x;
    CHECK(fills[0]->GetBox().GetSize(Rml::BoxArea::Border).x ==
          doctest::Approx(track * 0.72f).epsilon(0.02));
    const std::string right = doc->GetElementById("right")->GetInnerRML();
    CHECK(right.find("40") != std::string::npos);
    CHECK(right.find("3/5") != std::string::npos);
    CHECK(right.find("Bomb 1:05") != std::string::npos);
    CHECK(right.find("Picked up a key") != std::string::npos);
    const auto shown = [&](const char* id) {
        Rml::Element* e = doc->GetElementById(id);
        return e && e->IsVisible(true);
    };
    CHECK_FALSE(shown("prompt"));  // (none: hidden)
    CHECK_FALSE(shown("crosshair"));

    // Health drops, a prompt and the crosshair come; the HUD follows.
    d.stats[0].value = 30.0;
    d.prompt = "Press E to open";
    d.crosshair = true;
    hud.show(d);
    u.update(0.1);
    CHECK(doc->GetElementById("bar-health")->GetInnerRML().find("30/100") != std::string::npos);
    REQUIRE(shown("prompt"));
    CHECK(doc->GetElementById("prompt")->GetInnerRML().find("Press E to open") !=
          std::string::npos);
    CHECK(shown("crosshair"));
    // Clicks and moves over it reach the game.
    const auto [x, y] = middle(doc->GetElementById("bar-health"));
    CHECK_FALSE(u.handle(at(T::PointerMove, x, y)));
    CHECK_FALSE(u.handle(at(T::PointerDown, x, y)));
    // Hidden, nothing of it is there.
    d.visible = false;
    hud.show(d);
    u.update(0.2);
    CHECK_FALSE(shown("bar-health"));
}

TEST_CASE("the HUD's compass slides to the heading, and the minimap places its marks") {
    auto renderer = offscreen(1280, 720);
    if (!renderer) return;
    ui::Ui u(*renderer, sized(1280, 720));
    ui::Hud hud(u);
    const std::vector<uint8_t> ground(16 * 16 * 4, 200);
    ui::HudData d;
    d.heading = 90.0;  // facing east
    d.map.shown = true;
    d.map.radius = 100.0;
    d.map.ground = &ground;
    d.map.size = 16;
    d.map.version = 1;
    d.map.marks = {{"exit", "", 50.0, 0.0}, {"mark", "Shrine", 0.0, 100.0}};
    hud.show(d);
    u.update(0.0);
    Rml::ElementDocument* doc = hud.document();
    // East is under the needle: the strip's E (at 270 degrees from its start) is in the middle.
    Rml::Element* compass = doc->GetElementById("compass");
    REQUIRE(compass);
    CHECK(compass->IsVisible(true));
    Rml::ElementList ticks;
    doc->GetElementsByClassName(ticks, "major");
    Rml::Element* east = nullptr;
    const float middle_x = compass->GetAbsoluteOffset(Rml::BoxArea::Border).x +
                           compass->GetBox().GetSize(Rml::BoxArea::Border).x / 2.0f;
    for (Rml::Element* t : ticks) {
        if (t->GetInnerRML() != "E") continue;
        const float x = t->GetAbsoluteOffset(Rml::BoxArea::Border).x +
                        t->GetBox().GetSize(Rml::BoxArea::Border).x / 2.0f;
        if (std::abs(x - middle_x) < 2.0f) east = t;
    }
    CHECK(east);
    // The marks: the exit half way to the right edge, the shrine at the top edge.
    Rml::Element* map = doc->GetElementById("minimap");
    Rml::ElementList all_marks, marks;
    doc->GetElementsByClassName(all_marks, "mark");
    for (Rml::Element* e : all_marks)  // (data-for keeps its template, hidden)
        if (e->IsVisible(true)) marks.push_back(e);
    REQUIRE(marks.size() == 2);
    const Rml::Vector2f at = map->GetAbsoluteOffset(Rml::BoxArea::Padding);
    const Rml::Vector2f size = map->GetBox().GetSize(Rml::BoxArea::Padding);
    const auto centre = [&](Rml::Element* e) {
        return e->GetAbsoluteOffset(Rml::BoxArea::Border) +
               e->GetBox().GetSize(Rml::BoxArea::Border) * 0.5f;
    };
    CHECK(centre(marks[0]).x == doctest::Approx(at.x + size.x * 0.75f).epsilon(0.02));
    CHECK(centre(marks[0]).y == doctest::Approx(at.y + size.y * 0.5f).epsilon(0.02));
    CHECK(centre(marks[1]).y == doctest::Approx(at.y).epsilon(0.05));
    CHECK(marks[1]->GetInnerRML().find("Shrine") != std::string::npos);
    // The ground is drawn: the minimap's picture made a texture.
    u.render();
    const size_t textures = renderer->ui_texture_count();
    CHECK(textures >= 2);  // (the font's glyphs, and the ground)
    // A ground twice as wide as what shows, the player 60 m east and 30 m north of its middle:
    // the picture is twice the minimap's size, shifted so it still covers all of it.
    d.map.ground_radius = 200.0;
    d.map.east = 60.0;
    d.map.north = 30.0;
    hud.show(d);
    u.update(0.05);
    Rml::Element* picture = doc->GetElementById("ground");
    REQUIRE(picture);
    const Rml::Vector2f p_at = picture->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f p_size = picture->GetBox().GetSize(Rml::BoxArea::Border);
    CHECK(p_size.x == doctest::Approx(size.x * 2.0f).epsilon(0.02));
    CHECK(p_size.y == doctest::Approx(size.y * 2.0f).epsilon(0.02));
    // Its middle is 60 m west and 30 m south of the minimap's (0.3 and 0.15 of its width).
    CHECK(p_at.x + p_size.x / 2.0f == doctest::Approx(at.x + size.x * 0.2f).epsilon(0.02));
    CHECK(p_at.y + p_size.y / 2.0f == doctest::Approx(at.y + size.y * 0.65f).epsilon(0.02));
    CHECK(p_at.x <= at.x + 0.5f);
    CHECK(p_at.y <= at.y + 0.5f);
    CHECK(p_at.x + p_size.x >= at.x + size.x - 0.5f);
    CHECK(p_at.y + p_size.y >= at.y + size.y - 0.5f);
    // Without a compass or minimap, neither shows.
    d.heading.reset();
    d.map.shown = false;
    hud.show(d);
    u.update(0.1);
    CHECK_FALSE(doc->GetElementById("compass")->IsVisible(true));
    CHECK_FALSE(doc->GetElementById("minimap")->IsVisible(true));
}

TEST_CASE("the HUD's crosshair: Midgard's dot, or the game's own picture at its size") {
    auto renderer = offscreen(800, 600);
    if (!renderer) return;
    ui::Ui u(*renderer, sized(800, 600));
    ui::Hud hud(u);
    ui::HudData d;
    d.crosshair = true;
    hud.show(d);
    u.update(0.0);
    Rml::ElementDocument* doc = hud.document();
    CHECK(doc->GetElementById("crosshair")->IsVisible(true));
    CHECK_FALSE(doc->GetElementById("crosshair_picture")->IsVisible(true));
    // The game's picture, 48 dp across, in the middle.
    d.crosshair_picture = true;
    d.crosshair_size = 48.0f;
    hud.show(d);
    u.update(0.1);
    Rml::Element* picture = doc->GetElementById("crosshair_picture");
    CHECK_FALSE(doc->GetElementById("crosshair")->IsVisible(true));
    REQUIRE(picture->IsVisible(true));
    CHECK(picture->GetBox().GetSize(Rml::BoxArea::Border).x == doctest::Approx(48.0f));
    const auto [x, y] = middle(picture);
    CHECK(x == doctest::Approx(400.0f));
    CHECK(y == doctest::Approx(300.0f));
    // No crosshair: neither.
    d.crosshair = false;
    hud.show(d);
    u.update(0.2);
    CHECK_FALSE(picture->IsVisible(true));
}
