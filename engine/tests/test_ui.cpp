// SPDX-License-Identifier: Apache-2.0
// The in-game UI engine (milestone 6.0): RmlUi screens drawn by the renderer's UI layer, their
// input (mouse, keys, gamepad, touch), scale and safe area, and the screen checks.
#include <doctest/doctest.h>

#include <memory>
#include <string>

#include <RmlUi/Core.h>

#include "midgard/render/renderer.h"
#include "midgard/ui/fonts.h"
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
