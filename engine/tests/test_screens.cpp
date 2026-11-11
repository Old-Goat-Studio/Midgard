// SPDX-License-Identifier: Apache-2.0
// Screens made in the Menu editor (milestone 6.0, midgard/screen.h and midgard/ui/screens.h): the
// format and its checks, each element placed by its anchor, and what the player does on them.
#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <string>

#include <RmlUi/Core.h>

#include "midgard/render/renderer.h"
#include "midgard/screen.h"
#include "midgard/ui/fonts.h"
#include "midgard/ui/screens.h"
#include "midgard/ui/ui.h"

namespace render = midgard::render;
namespace ui = midgard::ui;
using midgard::screen_from_json;
using midgard::ScreenError;
using T = ui::Input::Type;

namespace {

const char* kPause = R"({"modal": true, "pauses": true, "background": [0, 0, 0, 0.5],
  "elements": [
    {"id": "shade", "kind": "panel", "anchor": "fill", "x": 8, "y": 8,
     "style": {"background": [0, 0, 1, 0.1]}},
    {"id": "box", "kind": "panel", "anchor": "center", "w": 400, "h": 300},
    {"id": "title", "kind": "text", "parent": "box", "anchor": "top", "y": 20, "w": 360, "h": 40,
     "text": "Paused <now>", "style": {"font_size": 28, "align": "center", "bold": true}},
    {"id": "resume", "kind": "button", "parent": "box", "anchor": "center", "w": 240, "h": 56,
     "text": "Resume", "action": {"do": "close"}},
    {"id": "volume", "kind": "slider", "parent": "box", "anchor": "bottom", "y": -70, "w": 300,
     "h": 44, "min": 0, "max": 1, "value": 0.5},
    {"id": "subs", "kind": "check", "parent": "box", "anchor": "bottom_left", "x": 20, "y": -10,
     "w": 240, "h": 44, "text": "Subtitles", "value": true},
    {"id": "quit", "kind": "button", "anchor": "bottom_right", "x": -16, "y": -16, "w": 120,
     "h": 48, "text": "Quit", "action": {"do": "quit"}}]})";

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

struct Box {
    float x, y, w, h;
};
Box box(Rml::Element* e) {
    const Rml::Vector2f at = e->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
    return {at.x, at.y, size.x, size.y};
}

ui::Input pointer(T type, float x, float y) {
    ui::Input in;
    in.type = type;
    in.x = x;
    in.y = y;
    return in;
}

}  // namespace

TEST_CASE("screens: the format, and what it refuses") {
    const midgard::ScreenDef s = screen_from_json("scr_pause", "Pause", kPause);
    CHECK(s.modal);
    CHECK(s.pauses);
    REQUIRE(s.elements.size() == 7);
    CHECK(s.elements.front().id == "shade");
    CHECK(s.find("resume")->action == "close");
    CHECK(s.find("subs")->value == 1.0);
    CHECK(s.find("volume")->value == 0.5);
    CHECK(s.find("title")->style.font_size == 28.0);
    CHECK(s.find("nothing") == nullptr);
    // Defaults: modal, not pausing, nothing in it.
    const midgard::ScreenDef empty = screen_from_json("e", "", "{}");
    CHECK(empty.modal);
    CHECK_FALSE(empty.pauses);
    CHECK(empty.name == "e");
    // What it refuses, saying which.
    const auto refuses = [](const char* json, const char* words) {
        CAPTURE(json);
        try {
            screen_from_json("s", "Menu", json);
            FAIL("accepted");
        } catch (const ScreenError& e) {
            CHECK(std::string(e.what()).find(words) != std::string::npos);
        }
    };
    refuses("[]", "isn't a JSON object");
    refuses(R"({"elements": [{"kind": "text"}]})", "without an id");
    refuses(R"({"elements": [{"id": "a"}, {"id": "a"}]})", "used twice");
    refuses(R"({"elements": [{"id": "a", "kind": "video"}]})", "no kind 'video'");
    refuses(R"({"elements": [{"id": "a", "anchor": "middle"}]})", "no anchor 'middle'");
    refuses(R"({"elements": [{"id": "a", "parent": "b"}]})", "isn't a panel before it");
    refuses(R"({"elements": [{"id": "b", "kind": "text"}, {"id": "a", "parent": "b"}]})",
            "isn't a panel before it");
    refuses(R"({"elements": [{"id": "a", "x": "left"}]})", "'x' isn't a number");
    refuses(R"({"elements": [{"id": "a", "kind": "button", "action": {"do": "open"}}]})",
            "doesn't say which");
    refuses(R"({"elements": [{"id": "a", "kind": "button", "action": {"do": "fly"}}]})",
            "no action 'fly'");
    refuses(R"({"elements": [{"id": "a", "kind": "slider", "min": 1, "max": 1}]})",
            "most isn't above its least");
    refuses(R"({"elements": [{"id": "a", "style": {"color": "red"}}]})", "isn't a colour");
    refuses(R"({"modal": 1})", "isn't true or false");
    // Its RML: escaped text, every element by id.
    const std::string rml = midgard::screen_rml(s);
    CHECK(rml.find("Paused &lt;now&gt;") != std::string::npos);
    for (const char* id : {"el-box", "el-title", "el-resume", "el-volume", "el-subs", "el-subs-box",
                           "el-quit", "el-shade"})
        CHECK(rml.find(std::string("id=\"") + id + "\"") != std::string::npos);
}

TEST_CASE("screens: each element where its anchor puts it; clicks and changes told back") {
    auto renderer = offscreen(1280, 720);
    if (!renderer) return;
    ui::Options o;
    o.width = 1280;
    o.height = 720;
    ui::Ui u(*renderer, o);
    ui::Screens screens(u);
    const std::vector<midgard::ScreenDef> defs = {screen_from_json("pause", "Pause", kPause)};
    std::map<std::string, std::string> texts;
    std::map<std::string, double> values;
    screens.sync(defs, {"pause"}, texts, values);
    u.update(0.0);
    Rml::ElementDocument* doc = screens.document("pause");
    REQUIRE(doc);
    CHECK(u.modal());
    CHECK(screens.pauses());
    const auto at = [&](const char* id) {
        Rml::Element* e = doc->GetElementById(id);
        REQUIRE(e);
        return box(e);
    };
    const Box panel = at("el-box");  // centred: 400 x 300
    CHECK(panel.x == doctest::Approx(440.0f));
    CHECK(panel.y == doctest::Approx(210.0f));
    CHECK(panel.w == doctest::Approx(400.0f));
    const Box resume = at("el-resume");  // the panel's middle
    CHECK(resume.x + resume.w / 2 == doctest::Approx(640.0f));
    CHECK(resume.y + resume.h / 2 == doctest::Approx(360.0f));
    const Box volume = at("el-volume");  // the panel's bottom, 70 up
    // (inside the panel's 1 dp border: within a pixel)
    CHECK(std::abs(volume.y + volume.h - (210.0f + 300.0f - 70.0f)) <= 1.0f);
    CHECK(std::abs(volume.x - 490.0f) <= 1.0f);
    const Box subs = at("el-subs");  // the panel's bottom left, 20 in and 10 up
    CHECK(std::abs(subs.x - 460.0f) <= 1.0f);
    CHECK(std::abs(subs.y + subs.h - 500.0f) <= 1.0f);
    const Box quit = at("el-quit");  // the screen's bottom right, 16 in
    CHECK(quit.x + quit.w == doctest::Approx(1280.0f - 16.0f));
    CHECK(quit.y + quit.h == doctest::Approx(720.0f - 16.0f));
    const Box shade = at("el-shade");  // the whole screen less 8 each side
    CHECK(shade.x == doctest::Approx(8.0f));
    CHECK(shade.w == doctest::Approx(1280.0f - 16.0f));
    CHECK(shade.h == doctest::Approx(720.0f - 16.0f));
    // (the player's changes are heard from the next frame: the game syncs every frame)
    screens.sync(defs, {"pause"}, texts, values);
    CHECK(screens.take_events().empty());  // (the slider settling as it opened)
    // A gamepad reaches every control; text fits; big enough for a finger.
    for (const ui::Problem& p : u.check(doc, true)) FAIL(p.kind << ": " << p.message);
    screens.take_events();  // (the checks worked its controls as a gamepad would)

    // Clicking Resume: told back as a click.
    u.handle(pointer(T::PointerMove, 640, 360));
    u.handle(pointer(T::PointerDown, 640, 360));
    u.handle(pointer(T::PointerUp, 640, 360));
    std::vector<ui::Screens::Event> events = screens.take_events();
    REQUIRE(events.size() == 1);
    CHECK(events[0].screen == "pause");
    CHECK(events[0].element == "resume");
    CHECK(events[0].kind == "click");
    screens.sync(defs, {"pause"}, texts, values);
    // Ticking the check off: a change to 0.
    Rml::Element* tick = doc->GetElementById("el-subs-box");
    REQUIRE(tick);
    const Box t = box(tick);
    u.handle(pointer(T::PointerMove, t.x + t.w / 2, t.y + t.h / 2));
    u.handle(pointer(T::PointerDown, t.x + t.w / 2, t.y + t.h / 2));
    u.handle(pointer(T::PointerUp, t.x + t.w / 2, t.y + t.h / 2));
    events = screens.take_events();
    REQUIRE_FALSE(events.empty());
    CHECK(events.back().element == "subs");
    CHECK(events.back().kind == "change");
    CHECK(events.back().value == 0.0);

    // Scripts' text and values show; the game setting a value isn't a change.
    texts["pause/title"] = "Still paused";
    values["pause/volume"] = 0.8;
    screens.sync(defs, {"pause"}, texts, values);
    u.update(0.1);
    CHECK(doc->GetElementById("el-title")->GetInnerRML() == "Still paused");
    CHECK(doc->GetElementById("el-volume")->GetAttribute<float>("value", 0.0f) ==
          doctest::Approx(0.8f));
    CHECK(screens.take_events().empty());

    // A changed design opens it again; not shown, it goes.
    std::vector<midgard::ScreenDef> changed = defs;
    changed[0].elements[3].text = "Carry on";
    screens.sync(changed, {"pause"}, texts, values);
    u.update(0.2);
    REQUIRE(screens.document("pause"));
    CHECK(screens.document("pause")->GetElementById("el-resume")->GetInnerRML() ==
          "<span>Carry on</span>");
    screens.sync(changed, {}, texts, values);
    u.update(0.3);
    CHECK(screens.document("pause") == nullptr);
    CHECK_FALSE(u.modal());
    CHECK_FALSE(screens.pauses());
}

TEST_CASE("screens: pictures from the Asset Library's GUI section, on any element") {
    const midgard::ScreenDef s = screen_from_json("p", "Pictures", R"({"elements": [
        {"id": "logo", "kind": "image", "image": "ui:library/gui/logo.png", "w": 96, "h": 96},
        {"id": "play", "kind": "button", "text": "Play", "image": "ui:library/gui/button.png"},
        {"id": "box", "kind": "panel", "image": "ui:library/gui/button.png"}]})");
    CHECK(s.find("logo")->image == "ui:library/gui/logo.png");
    // Each picture once, in order; a name a URL can hold.
    CHECK(midgard::screen_images(s) ==
          std::vector<std::string>{"ui:library/gui/logo.png", "ui:library/gui/button.png"});
    const std::string name = midgard::screen_image_name("ui:library/gui/logo.png");
    CHECK(name == "screen_ui_library_gui_logo.png");
    const std::string rml = midgard::screen_rml(s);
    CHECK(rml.find("decorator: image(midgard://image/" + name + " fill)") != std::string::npos);
    // (a button with a picture has no fill or border of its own)
    CHECK(rml.find("background-color: #00000000; border-width: 0dp;") != std::string::npos);
    CHECK_THROWS_AS(screen_from_json("p", "P", R"({"elements": [{"id": "a", "image": "tex:a"}]})"),
                    ScreenError);

    auto renderer = offscreen(640, 480);
    if (!renderer) return;
    ui::Options o;
    o.width = 640;
    o.height = 480;
    ui::Ui u(*renderer, o);
    // A 2 x 2 PNG (red, green, blue, white), as a picture file holds it.
    const std::vector<uint8_t> png = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00,
        0x00, 0xFD, 0xD4, 0x9A, 0x73, 0x00, 0x00, 0x00, 0x16, 0x49, 0x44, 0x41, 0x54, 0x78,
        0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xC0, 0xF0, 0x9F, 0x81, 0xE1, 0x3F, 0x03, 0xC3, 0x7F,
        0x20, 0x00, 0x00, 0x4E, 0xF5, 0x07, 0xF9, 0x1B, 0x3E, 0x12, 0x30, 0x00, 0x00, 0x00,
        0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    std::vector<std::string> asked;
    ui::Screens screens(u, [&](const std::string& content) {
        asked.push_back(content);
        return content.ends_with("logo.png") ? png : std::vector<uint8_t>{};
    });
    screens.sync({s}, {"p"}, {}, {});
    u.update(0.0);
    REQUIRE(screens.document("p"));
    CHECK(asked.size() == 2);
    CHECK(u.has_image(name));  // (the logo: read)
    CHECK_FALSE(u.has_image(midgard::screen_image_name("ui:library/gui/button.png")));  // (missing)
    // Asked once each, however often the screen opens.
    screens.sync({s}, {}, {}, {});
    screens.sync({s}, {"p"}, {}, {});
    CHECK(asked.size() == 2);
    CHECK_FALSE(u.set_image_file("bad", {1, 2, 3}));
}

TEST_CASE("screens: pictures for each state; options in groups; disabled controls") {
    const midgard::ScreenDef s = screen_from_json("m", "Menu", R"({"image": "ui:bg.png",
      "elements": [
        {"id": "play", "kind": "button", "text": "Play", "image": "ui:n.png", "y": 10, "w": 200,
         "h": 50, "images": {"hover": "ui:h.png", "pressed": "ui:p.png", "disabled": "ui:d.png"}},
        {"id": "load", "kind": "button", "text": "Load", "enabled": false, "y": 70, "w": 200,
         "h": 50},
        {"id": "easy", "kind": "option", "group": "level", "text": "Easy", "y": 130, "w": 200,
         "h": 50, "images": {"unchecked": "ui:o.png", "checked": "ui:x.png"}},
        {"id": "hard", "kind": "option", "group": "level", "text": "Hard", "value": true,
         "y": 190, "w": 200, "h": 50},
        {"id": "volume", "kind": "slider", "y": 250, "w": 200, "h": 50,
         "images": {"track": "ui:t.png", "pointer": "ui:k.png"}}]})");
    CHECK(s.image == "ui:bg.png");
    CHECK(s.find("load")->enabled == false);
    CHECK(s.find("hard")->value == 1.0);
    CHECK(s.find("easy")->group == "level");
    CHECK(midgard::screen_images(s).size() == 9);
    const std::string rml = midgard::screen_rml(s);
    const auto has = [&](const std::string& rule, const std::string& picture) {
        const size_t at = rml.find(rule + " {");
        CAPTURE(rule);
        REQUIRE(at != std::string::npos);
        CHECK(rml.find(midgard::screen_image_name(picture), at) <
              rml.find("}", at));  // (in that rule)
    };
    CHECK(rml.find("body { decorator: image(midgard://image/" +
                   midgard::screen_image_name("ui:bg.png")) != std::string::npos);
    has("#el-play", "ui:n.png");
    has("#el-play:hover", "ui:h.png");
    has("#el-play:active", "ui:p.png");
    has("#el-play.disabled", "ui:d.png");
    has("#el-easy-box", "ui:o.png");
    has("#el-easy-box:checked", "ui:x.png");
    has("#el-volume slidertrack", "ui:t.png");
    has("#el-volume sliderbar", "ui:k.png");
    CHECK(rml.find("type=\"radio\" name=\"level\" value=\"easy\"") != std::string::npos);
    // Pictures a kind hasn't, or that aren't the GUI section's, are refused.
    CHECK_THROWS_AS(screen_from_json("m", "M", R"({"elements": [{"id": "a", "kind": "text",
                        "images": {"hover": "ui:h.png"}}]})"),
                    ScreenError);
    CHECK_THROWS_AS(screen_from_json("m", "M", R"({"elements": [{"id": "a", "kind": "button",
                        "images": {"hover": "tex:h.png"}}]})"),
                    ScreenError);

    auto renderer = offscreen(640, 480);
    if (!renderer) return;
    ui::Options o;
    o.width = 640;
    o.height = 480;
    ui::Ui u(*renderer, o);
    ui::Screens screens(u);
    std::map<std::string, std::string> texts;
    std::map<std::string, double> values;
    std::map<std::string, bool> enabled;
    screens.sync({s}, {"m"}, texts, values, enabled);
    u.update(0.0);
    screens.sync({s}, {"m"}, texts, values, enabled);
    Rml::ElementDocument* doc = screens.document("m");
    REQUIRE(doc);
    const auto press = [&](const char* id) {
        Rml::Element* e = doc->GetElementById(id);
        REQUIRE(e);
        const Box b = box(e);
        u.handle(pointer(T::PointerMove, b.x + b.w / 2, b.y + b.h / 2));
        u.handle(pointer(T::PointerDown, b.x + b.w / 2, b.y + b.h / 2));
        u.handle(pointer(T::PointerUp, b.x + b.w / 2, b.y + b.h / 2));
    };
    // A disabled button says nothing; enabled by a script, it clicks.
    press("el-load");
    CHECK(screens.take_events().empty());
    CHECK(doc->GetElementById("el-load")->IsClassSet("disabled"));
    enabled["m/load"] = true;
    screens.sync({s}, {"m"}, texts, values, enabled);
    u.update(0.1);
    CHECK_FALSE(doc->GetElementById("el-load")->IsClassSet("disabled"));
    press("el-load");
    std::vector<ui::Screens::Event> events = screens.take_events();
    REQUIRE(events.size() == 1);
    CHECK(events[0].element == "load");
    // Choosing Easy: chosen, and Hard (its group's) not.
    press("el-easy-box");
    events = screens.take_events();
    const auto chose = std::find_if(events.begin(), events.end(), [](const auto& ev) {
        return ev.element == "easy" && ev.value == 1.0;
    });
    CHECK(chose != events.end());
    CHECK(doc->GetElementById("el-easy-box")->HasAttribute("checked"));
    CHECK_FALSE(doc->GetElementById("el-hard-box")->HasAttribute("checked"));
}

TEST_CASE("screens: text in a font from the Asset Library, loaded as the screen opens") {
    auto renderer = offscreen(640, 480);
    if (!renderer) return;
    ui::Options o;
    o.width = 640;
    o.height = 480;
    ui::Ui u(*renderer, o);
    const midgard::ScreenDef s = screen_from_json("f", "Fonts", R"({"elements": [
        {"id": "plain", "kind": "text", "text": "Wide words here and there", "y": 10, "w": 120,
         "h": 10},
        {"id": "fancy", "kind": "text", "text": "Wide words here and there", "y": 200, "w": 120,
         "h": 10,
         "style": {"font": "ui:library/fonts/dyslexic"}}]})");
    std::vector<std::string> asked;
    ui::Screens screens(u, {}, [&](const std::string& folder) {
        asked.push_back(folder);
        const auto font = midgard::ui::fonts::opendyslexic_regular();
        return std::vector<std::pair<std::string, std::vector<uint8_t>>>{
            {"Dyslexic-Regular.otf", std::vector<uint8_t>(font.begin(), font.end())}};
    });
    screens.sync({s}, {"f"}, {}, {});
    u.update(0.0);
    REQUIRE(screens.document("f"));
    CHECK(asked == std::vector<std::string>{"ui:library/fonts/dyslexic"});
    Rml::Element* plain = screens.document("f")->GetElementById("el-plain");
    Rml::Element* fancy = screens.document("f")->GetElementById("el-fancy");
    CHECK(fancy->GetComputedValues().font_family() ==
          midgard::screen_font_family("ui:library/fonts/dyslexic"));
    (void)plain;
    // The family has a face now (an unknown one has none).
    Rml::FontEngineInterface* engine = Rml::GetFontEngineInterface();
    CHECK(engine->GetFontFaceHandle(midgard::screen_font_family("ui:library/fonts/dyslexic"),
                                    Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal,
                                    16) != 0);
    CHECK(engine->GetFontFaceHandle("font_not_loaded", Rml::Style::FontStyle::Normal,
                                    Rml::Style::FontWeight::Normal, 16) == 0);
}
