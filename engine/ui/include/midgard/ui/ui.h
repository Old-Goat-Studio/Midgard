// SPDX-License-Identifier: Apache-2.0
// The in-game UI engine (milestone 6.0): menus, screens and the HUD as RmlUi documents (HTML and
// CSS-like .rml and .rcss files), drawn by the renderer's UI layer (render/ui_draw.h).
//
// It takes input from the mouse, keyboard, gamepad and touch (the runtime turns SDL's events
// into ui::Input; nothing here knows SDL), sizes screens by the display's density times the
// player's UI scale (lengths in "dp" follow it), and keeps screens clear of a phone's notch and
// rounded corners (the safe area pads every screen's <body>).
//
// Midgard's fonts are built in: Inconsolata (the default) and OpenDyslexic (regular and bold,
// for players who read more easily with it), both SIL Open Font License 1.1. Documents can
// link Midgard's base style sheet as "midgard://base.rcss".
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace Rml {
class Context;
class ElementDocument;
class Element;
}  // namespace Rml

namespace midgard::render {
class Renderer;
}

namespace midgard::ui {

class UiError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// What the screen keeps clear of at each edge, in pixels (a notch, rounded corners, the
// system's bars).
struct Insets {
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
};

// One piece of input, as the runtime has it from the window system. Positions are in pixels
// from the screen's top-left.
struct Input {
    enum class Type {
        PointerMove,
        PointerDown,  // button: 0 left, 1 right, 2 middle
        PointerUp,
        PointerLeave,  // the pointer left the window
        Wheel,         // x, y: steps (y positive away from the player, as SDL has it)
        KeyDown,       // key: SDL's key name ("Return", "Up", "A", "Backspace")
        KeyUp,
        Text,       // text: UTF-8 typed (or from a phone's keyboard)
        TouchDown,  // touch: the finger; x, y its place
        TouchMove,
        TouchUp,
        TouchCancel,
        PadDown,  // key: SDL's gamepad button name ("a", "b", "dpup", "start")
        PadUp,
        PadAxis,  // key: SDL's axis name ("leftx", "lefty"); x: -1..1
    };
    Type type = Type::PointerMove;
    float x = 0.0f, y = 0.0f;
    int button = 0;
    int64_t touch = 0;
    std::string key;
    std::string text;
    bool shift = false, ctrl = false, alt = false, meta = false;
};

// What the window system gives the UI (the runtime fills these with SDL's; offscreen, in tests,
// they are empty and do nothing).
struct Platform {
    std::function<void(const std::string& cursor)> set_cursor;  // "pointer", "text", ""
    std::function<void(const std::string& text)> set_clipboard;
    std::function<std::string()> get_clipboard;
    // A text field gained (true) or lost the keyboard: phones show or hide theirs. x, y: the
    // caret, in pixels.
    std::function<void(bool on, float x, float y)> keyboard;
    // RmlUi's messages: 0 information, 1 a warning, 2 an error.
    std::function<void(int level, const std::string& message)> log;
};

struct Options {
    uint32_t width = 1280, height = 720;  // the screen, in pixels
    float density = 1.0f;  // the display's: pixels per dp (SDL's display scale; 2.6 on a phone)
    float scale = 1.0f;    // the player's UI scale setting, on top of the density
    Insets safe_area;
    Platform platform;
};

// A problem a screen check found (Phase 6's screen tests, Phase6_Roadmap.md 2.1).
struct Problem {
    // "unreachable", "off_screen", "text_overflow", and with touch "small_target" and
    // "outside_safe_area"
    std::string kind;
    std::string element;  // which (its id, else its tag and classes)
    std::string message;  // in words
};

// The smallest a control may be for a finger, in dp (Android's guideline is 48; 44 is Apple's).
inline constexpr float kMinTouchTarget = 44.0f;

class Ui {
public:
    // Throws UiError if RmlUi can't start or its fonts can't be read. One Ui at a time.
    Ui(render::Renderer& renderer, Options options);
    ~Ui();
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    // ---- screens ---------------------------------------------------------------------------
    // A screen from a .rml file, or from RML text (`name` for messages and relative paths),
    // shown. A modal screen (a menu) takes all input while it is shown; others (the HUD) only
    // what is aimed at them. nullptr, with the reason logged, if it can't be read.
    Rml::ElementDocument* open(const std::filesystem::path& file, bool modal);
    Rml::ElementDocument* open_text(const std::string& rml, const std::string& name, bool modal);
    void close(Rml::ElementDocument* document);
    // A modal screen is shown: the game's own controls wait.
    bool modal() const;

    // ---- size, scale and safe area -------------------------------------------------------
    void resize(uint32_t width, uint32_t height);
    void set_density(float density);
    void set_scale(float scale);
    void set_safe_area(const Insets& insets);
    float dp_ratio() const;  // pixels per dp: density times scale
    const Insets& safe_area() const;
    uint32_t width() const;
    uint32_t height() const;

    // ---- input and frames ----------------------------------------------------------------
    // Gives the UI one piece of input. True if the UI used it (the game shouldn't): the pointer
    // is over a screen's element, a text field has the keyboard, or a modal screen is shown.
    bool handle(const Input& input);
    // Animations, held gamepad directions repeating, layout. `seconds`: the time since the
    // game started (the UI's clock; tests pass their own).
    void update(double seconds);
    // The screens as triangles, handed to the renderer for its next frame.
    void render();

    // ---- checks (screen tests) -----------------------------------------------------------
    // Every control a gamepad can't reach from the first, or off the screen where nothing
    // scrolls to it; text that doesn't fit its box; with `touch`, controls smaller than
    // kMinTouchTarget or under the notch.
    std::vector<Problem> check(Rml::ElementDocument* document, bool touch);

    Rml::Context* context();
    // A picture made by the game (the minimap's ground), for screens to show as
    // <img src="midgard://image/NAME"/>: RGBA8, top row first. Setting it again shows the new
    // one. False if the pixels don't match the size.
    bool set_image(const std::string& name, uint32_t width, uint32_t height,
                   const std::vector<uint8_t>& rgba);
    // A direction key ("Up", "Down", "Left", "Right" as RmlUi's key) to the focused screen, as
    // the d-pad and arrow keys move: into and out of scrolling lists too.
    void navigate(int rml_key);
    // A short description of an element for messages: its id, else its tag and classes.
    static std::string describe(Rml::Element* element);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The built-in test screen (midgard_runtime --ui-test), shown modal: buttons, a check box, a
// slider, a text field and a scrolling list, each saying what it did, to try the mouse, keys, a
// gamepad and touch on. Its Close button, Esc or a gamepad's B closes it.
Rml::ElementDocument* open_test_screen(Ui& ui);
const std::string& test_screen_rml();

}  // namespace midgard::ui
