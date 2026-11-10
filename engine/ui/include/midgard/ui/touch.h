// SPDX-License-Identifier: Apache-2.0
// On-screen touch controls (milestone 6.0): a phone plays without a game controller.
//
//   the stick     a finger anywhere on the left half (not on a button) puts the stick there and
//                 walks: the further from its middle, the faster (a dead zone in the middle).
//                 Past its rim the stick follows the finger. Let go and it goes back to its
//                 resting place, bottom left, drawn faint.
//   looking       a finger dragged on the right half (not on a button) turns the camera.
//   buttons       bottom right: Jump, Use, Run (a switch: tap to run, tap again to walk), View
//                 (first person, third person, from above) and, while flying, Down.
//
// Each finger does one thing, from where it went down, until it lifts: the stick, the camera
// and the buttons work together (walk, look and jump at once). The controls give the game a
// "touch" device's controls (schema/migrations/0020_touch.sql binds them to the actions):
//   stick_x, stick_y   -1..1 (y up the screen: forward)
//   drag_x, drag_y     this frame's dragging, in dp (y down the screen)
//   jump, use, run, view, descend    0 or 1
// Everything is placed in dp from the safe area's corners, so it sits clear of a phone's notch
// and rounded corners and is the same size to a finger on every screen.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Rml {
class ElementDocument;
}

namespace midgard::ui {

class Ui;
struct Input;

class TouchControls {
public:
    // The controls' screen (resources/touch.rml), over the game and under any menu, hidden
    // until shown. Throws UiError if it can't be read.
    explicit TouchControls(Ui& ui);
    ~TouchControls();
    TouchControls(const TouchControls&) = delete;
    TouchControls& operator=(const TouchControls&) = delete;

    // Shown (a phone, or a finger used) or hidden (a controller or keyboard in use, or a menu
    // up). Hiding lets go of every finger.
    void set_shown(bool shown);
    bool shown() const;
    // The Down button shows while the player flies.
    void set_flying(bool flying);

    // A finger (TouchDown, TouchMove, TouchUp, TouchCancel; x and y in pixels). True if the
    // controls took it (always, while shown: no finger on the game reaches anything else).
    bool handle(const Input& input);

    // What the controls give the game now. The drag is this frame's: take() clears it.
    struct Output {
        float stick_x = 0.0f, stick_y = 0.0f;
        float drag_x = 0.0f, drag_y = 0.0f;    // dp
        std::map<std::string, float> buttons;  // "jump", "use", "run", "view", "descend"
    };
    Output take();
    const Output& peek() const;

    // The screen follows the controls (call each frame, before Ui::update).
    void update();
    Rml::ElementDocument* document();

    // Where things are, in pixels (for tests and the screen): the stick's middle and the
    // knob's, and each button's middle and radius.
    struct Circle {
        float x = 0.0f, y = 0.0f, r = 0.0f;
    };
    Circle stick() const;
    Circle knob() const;
    // A button by id ("jump"...): r 0 if it isn't shown.
    Circle button(const std::string& id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The dead zone in the stick's middle, as a share of its radius: no walking.
inline constexpr float kStickDeadZone = 0.12f;

}  // namespace midgard::ui
