// SPDX-License-Identifier: Apache-2.0
// The on-screen touch controls (milestone 6.0, midgard/ui/touch.h): which finger does what, what
// the game gets, and the screen that shows them; and the "touch" device's default bindings.
#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <string>

#include <RmlUi/Core.h>
#include <nlohmann/json.hpp>

#include "midgard/input.h"
#include "midgard/render/renderer.h"
#include "midgard/ui/hud.h"
#include "midgard/ui/touch.h"
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

ui::Input finger(T type, int64_t id, float x, float y) {
    ui::Input in;
    in.type = type;
    in.touch = id;
    in.x = x;
    in.y = y;
    return in;
}

// The middle of an element on the screen, in pixels.
Rml::Vector2f middle(Rml::Element* e) {
    return e->GetAbsoluteOffset(Rml::BoxArea::Border) +
           e->GetBox().GetSize(Rml::BoxArea::Border) * 0.5f;
}

}  // namespace

TEST_CASE("touch: the stick walks, the right half looks, and the buttons press, all at once") {
    auto renderer = offscreen(1280, 720);
    if (!renderer) return;
    ui::Options o;
    o.width = 1280;
    o.height = 720;
    ui::Ui u(*renderer, o);
    ui::TouchControls touch(u);
    // Hidden until shown: then no finger is theirs.
    CHECK_FALSE(touch.shown());
    CHECK_FALSE(touch.handle(finger(T::TouchDown, 1, 200, 500)));
    touch.set_shown(true);

    // The stick rests bottom left, 110 dp in.
    const ui::TouchControls::Circle rest = touch.stick();
    CHECK(rest.x == doctest::Approx(110.0f));
    CHECK(rest.y == doctest::Approx(720.0f - 110.0f));
    CHECK(rest.r == doctest::Approx(60.0f));

    // A finger on the left half puts the stick there; moving it up walks forward.
    CHECK(touch.handle(finger(T::TouchDown, 1, 300, 400)));
    CHECK(touch.stick().x == doctest::Approx(300.0f));
    CHECK(touch.peek().stick_y == 0.0f);
    touch.handle(finger(T::TouchMove, 1, 300, 340));  // the rim, straight up: full forward
    CHECK(touch.peek().stick_x == doctest::Approx(0.0f));
    CHECK(touch.peek().stick_y == doctest::Approx(1.0f));
    touch.handle(finger(T::TouchMove, 1, 304, 398));  // within the dead zone: still
    CHECK(touch.peek().stick_y == 0.0f);
    touch.handle(finger(T::TouchMove, 1, 330, 400));  // half right
    CHECK(touch.peek().stick_x > 0.3f);
    CHECK(touch.peek().stick_x < 0.6f);
    // Past the rim the stick follows the finger, the knob on its rim.
    touch.handle(finger(T::TouchMove, 1, 500, 400));
    CHECK(touch.stick().x == doctest::Approx(440.0f));
    CHECK(touch.knob().x == doctest::Approx(500.0f));
    CHECK(touch.peek().stick_x == doctest::Approx(1.0f));

    // A second finger on the right half looks: the drag, in dp, this frame only.
    CHECK(touch.handle(finger(T::TouchDown, 2, 900, 300)));
    touch.handle(finger(T::TouchMove, 2, 940, 290));
    touch.handle(finger(T::TouchMove, 2, 960, 290));
    ui::TouchControls::Output out = touch.take();
    CHECK(out.drag_x == doctest::Approx(60.0f));
    CHECK(out.drag_y == doctest::Approx(-10.0f));
    CHECK(out.stick_x == doctest::Approx(1.0f));  // (the stick still held)
    CHECK(touch.take().drag_x == 0.0f);

    // A third finger on Jump presses it while the others walk and look.
    const ui::TouchControls::Circle jump = touch.button("jump");
    REQUIRE(jump.r > 0.0f);
    CHECK(touch.handle(finger(T::TouchDown, 3, jump.x + 10, jump.y - 10)));
    CHECK(touch.peek().buttons.at("jump") == 1.0f);
    touch.handle(finger(T::TouchUp, 3, jump.x, jump.y));
    CHECK(touch.peek().buttons.at("jump") == 0.0f);
    // Run is a switch: a tap runs, another walks.
    const ui::TouchControls::Circle run = touch.button("run");
    touch.handle(finger(T::TouchDown, 4, run.x, run.y));
    touch.handle(finger(T::TouchUp, 4, run.x, run.y));
    CHECK(touch.peek().buttons.at("run") == 1.0f);
    touch.handle(finger(T::TouchDown, 4, run.x, run.y));
    touch.handle(finger(T::TouchUp, 4, run.x, run.y));
    CHECK(touch.peek().buttons.at("run") == 0.0f);

    // Letting go of the stick stops the walk; it goes back to rest.
    touch.handle(finger(T::TouchUp, 1, 500, 400));
    CHECK(touch.peek().stick_x == 0.0f);
    CHECK(touch.stick().x == doctest::Approx(rest.x));
    // A finger that went down on the right half never becomes the stick, wherever it goes.
    touch.handle(finger(T::TouchUp, 2, 960, 290));
    touch.handle(finger(T::TouchDown, 5, 1000, 200));
    touch.handle(finger(T::TouchMove, 5, 100, 200));
    CHECK(touch.peek().stick_x == 0.0f);
    CHECK(touch.take().drag_x == doctest::Approx(-900.0f));
    touch.handle(finger(T::TouchCancel, 5, 100, 200));

    // Down shows only while flying.
    CHECK(touch.button("descend").r == 0.0f);
    touch.set_flying(true);
    const ui::TouchControls::Circle down = touch.button("descend");
    REQUIRE(down.r > 0.0f);
    touch.handle(finger(T::TouchDown, 6, down.x, down.y));
    CHECK(touch.peek().buttons.at("descend") == 1.0f);
    touch.set_flying(false);  // (landing lets go of it)
    CHECK(touch.peek().buttons.at("descend") == 0.0f);
    touch.handle(finger(T::TouchUp, 6, down.x, down.y));

    // Hiding lets go of everything (and stops running).
    touch.handle(finger(T::TouchDown, 7, 200, 400));
    touch.handle(finger(T::TouchMove, 7, 200, 300));
    touch.handle(finger(T::TouchDown, 8, run.x, run.y));
    touch.set_shown(false);
    CHECK(touch.peek().stick_y == 0.0f);
    CHECK(touch.peek().buttons.at("run") == 0.0f);
    CHECK_FALSE(touch.handle(finger(T::TouchMove, 7, 200, 250)));
    // Other input isn't theirs.
    touch.set_shown(true);
    ui::Input key;
    key.type = T::KeyDown;
    key.key = "Space";
    CHECK_FALSE(touch.handle(key));
}

TEST_CASE("touch: the screen shows the controls where they work, clear of the safe area") {
    // A phone held upright, 1.5 pixels a dp, with a notch and a gesture bar.
    auto renderer = offscreen(540, 1170);
    if (!renderer) return;
    ui::Options o;
    o.width = 540;
    o.height = 1170;
    o.scale = 1.5f;
    o.safe_area = {0.0f, 90.0f, 0.0f, 60.0f};
    ui::Ui u(*renderer, o);
    ui::Hud hud(u);
    ui::TouchControls touch(u);
    touch.set_shown(true);
    touch.update();
    hud.set_touch(true);
    u.update(0.0);
    Rml::ElementDocument* doc = touch.document();
    REQUIRE(doc);
    // The stick and each button drawn where the controls take them.
    Rml::Element* stick = doc->GetElementById("stick");
    REQUIRE(stick);
    CHECK(stick->IsVisible(true));
    const ui::TouchControls::Circle st = touch.stick();
    CHECK(middle(stick).x == doctest::Approx(st.x).epsilon(0.01));
    CHECK(middle(stick).y == doctest::Approx(st.y).epsilon(0.01));
    CHECK(stick->GetBox().GetSize(Rml::BoxArea::Border).x ==
          doctest::Approx(2 * st.r).epsilon(0.02));
    for (const char* id : {"jump", "use", "run", "view"}) {
        CAPTURE(id);
        Rml::Element* b = doc->GetElementById(std::string("touch-") + id);
        REQUIRE(b);
        CHECK(b->IsVisible(true));
        const ui::TouchControls::Circle c = touch.button(id);
        CHECK(middle(b).x == doctest::Approx(c.x).epsilon(0.01));
        CHECK(middle(b).y == doctest::Approx(c.y).epsilon(0.01));
        // Big enough for a finger, inside the safe area, and clear of the stick.
        CHECK(2.0f * c.r / 1.5f >= ui::kMinTouchTarget);
        CHECK(c.y + c.r <= 1170.0f - 60.0f);
        CHECK(c.x + c.r <= 540.0f);
        CHECK(std::hypot(c.x - st.x, c.y - st.y) > c.r + st.r);
    }
    // The buttons don't overlap one another (Down too, while flying; Menu, with a pause menu).
    CHECK(touch.button("menu").r == 0.0f);  // (no pause menu: no Menu button)
    touch.set_flying(true);
    touch.set_menu(true);
    CHECK(touch.button("menu").r > 0.0f);
    const char* ids[] = {"jump", "use", "run", "view", "descend", "menu"};
    for (const char* a : ids)
        for (const char* b : ids) {
            if (std::string(a) >= b) continue;
            const ui::TouchControls::Circle p = touch.button(a), q = touch.button(b);
            CAPTURE(a);
            CAPTURE(b);
            CHECK(std::hypot(p.x - q.x, p.y - q.y) > p.r + q.r);
        }
    // Down hidden while not flying; pressing a button lights it.
    touch.set_flying(false);
    const ui::TouchControls::Circle jump = touch.button("jump");
    touch.handle(finger(T::TouchDown, 1, jump.x, jump.y));
    touch.update();
    u.update(0.1);
    CHECK_FALSE(doc->GetElementById("touch-descend")->IsVisible(true));
    CHECK(doc->GetElementById("touch-jump")->IsClassSet("down"));
    // The HUD makes room: the minimap at the top right, above the numbers, clear of the
    // buttons.
    ui::HudData d;
    d.map.shown = true;
    d.stats = {{"score", "Score", false, 10.0, 0.0, ""}};
    hud.show(d);
    u.update(0.3);
    Rml::Element* minimap = hud.document()->GetElementById("minimap");
    Rml::Element* right = hud.document()->GetElementById("right");
    REQUIRE(minimap);
    REQUIRE(right);
    const float map_bottom = minimap->GetAbsoluteOffset(Rml::BoxArea::Border).y +
                             minimap->GetBox().GetSize(Rml::BoxArea::Border).y;
    CHECK(map_bottom < right->GetAbsoluteOffset(Rml::BoxArea::Border).y);
    CHECK(map_bottom < 1170.0f / 2.0f);
    // Hidden, the screen shows nothing.
    touch.set_shown(false);
    touch.update();
    u.update(0.2);
    CHECK_FALSE(doc->GetElementById("stick")->IsVisible(true));
}

TEST_CASE("touch: the default bindings walk, look and press from the touch device") {
    midgard::Actions actions(midgard::InputMap::defaults());
    actions.set("touch", "stick_y", 0.75f);
    actions.set("touch", "stick_x", -0.5f);
    actions.add_delta("touch", "drag_x", 100.0f);  // 100 dp: 30 degrees
    actions.add_delta("touch", "drag_y", 10.0f);   // (down the screen: looks down)
    actions.set("touch", "jump", 1.0f);
    actions.set("touch", "run", 1.0f);
    CHECK(actions.axis("move_y") == doctest::Approx(0.75f));
    CHECK(actions.axis("move_x") == doctest::Approx(-0.5f));
    CHECK(actions.axis("look_x") == doctest::Approx(30.0f));
    CHECK(actions.axis("look_y") == doctest::Approx(-3.0f));
    CHECK(actions.pressed("jump"));
    CHECK(actions.down("run"));
    actions.begin_frame();
    CHECK(actions.axis("look_x") == 0.0f);  // (the drag was the last frame's)
    CHECK_FALSE(actions.pressed("jump"));   // (held, not pressed again)
}

TEST_CASE("touch: a game's own layout, its buttons shown by context, kept as JSON") {
    // A layout of two: Jump where it was, and the game's own Honk, only in vehicles.
    midgard::TouchLayout layout;
    layout.buttons = {{"jump", "Jump", 84, 96, 40, 74, 92, 38, false, "always"},
                      {"honk", "Honk", 200, 100, 30, 150, 100, 30, false, "vehicle"}};
    CHECK(midgard::touch_layout_problems(layout).empty());
    CHECK(midgard::touch_layout_from_json(midgard::touch_layout_to_json(layout)) == layout);
    CHECK(midgard::touch_layout_from_json(nlohmann::json()) == midgard::default_touch_layout());
    midgard::TouchLayout twice = layout;
    twice.buttons.push_back(layout.buttons[0]);
    twice.buttons.back().shows = "underwater";
    CHECK(midgard::touch_layout_problems(twice).size() == 2);

    auto renderer = offscreen(1280, 720);
    if (!renderer) return;
    ui::Options o;
    o.width = 1280;
    o.height = 720;
    ui::Ui u(*renderer, o);
    ui::TouchControls t(u, layout);
    t.set_shown(true);
    t.set_context("on_foot");
    CHECK(t.button("honk").r == 0.0f);  // (a vehicle's: not shown on foot)
    CHECK(t.button("use").r == 0.0f);   // (not in this layout)
    t.set_context("vehicle");
    const ui::TouchControls::Circle honk = t.button("honk");
    REQUIRE(honk.r > 0.0f);
    CHECK(honk.x == doctest::Approx(1280.0f - 200.0f * u.dp_ratio()));
    t.handle(finger(T::TouchDown, 1, honk.x, honk.y));
    CHECK(t.peek().buttons.at("honk") == 1.0f);
    t.handle(finger(T::TouchUp, 1, honk.x, honk.y));
    CHECK(t.peek().buttons.at("honk") == 0.0f);
    // Changed while playing (the editor's edits, live): the new places.
    layout.buttons[1].x = 300;
    t.set_layout(layout);
    CHECK(t.button("honk").x == doctest::Approx(1280.0f - 300.0f * u.dp_ratio()));
    t.update();
    u.update(0.0);
    REQUIRE(t.document() != nullptr);
    CHECK(t.document()->GetElementById("touch-honk") != nullptr);  // (drawn)
}
