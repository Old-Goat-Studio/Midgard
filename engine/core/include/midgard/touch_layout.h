// SPDX-License-Identifier: Apache-2.0
// The on-screen touch controls' buttons (milestone 6.1 step 3), as the Input editor's Touch
// Layout lays them out; kept in the game's settings (midgard/game_settings.h) as "touch":
//
//   {"buttons": [{"id": "jump", "label": "Jump",      its touch control (input bindings name it)
//                 "x": 84, "y": 96, "r": 40,          dp in from the safe area's bottom right
//                 "narrow": [74, 92, 38],              the same on a phone held upright
//                 "toggles": false,                   tap on, tap off (Run)
//                 "shows": "always"}, ...]}           "always", a context ("flying": while the
//                                                     player flies), or "pause_menu" (when the
//                                                     game has one: it opens it)
//
// The stick (bottom left) and dragging to look (the right half) are always there. Without a
// layout, Midgard's own: Jump, Use, Run, View, Down (flying) and Menu.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace midgard {

struct TouchButton {
    std::string id, label;
    float x = 100.0f, y = 100.0f, r = 30.0f;     // a phone held sideways (or a tablet)
    float nx = 100.0f, ny = 100.0f, nr = 30.0f;  // held upright (under 560 dp across)
    bool toggles = false;
    std::string shows = "always";
    bool operator==(const TouchButton&) const = default;
};

struct TouchLayout {
    std::vector<TouchButton> buttons;
    bool operator==(const TouchLayout&) const = default;
};

TouchLayout default_touch_layout();
// From its JSON (anything unusable left out); the default layout for null or empty.
TouchLayout touch_layout_from_json(const nlohmann::json& j);
nlohmann::json touch_layout_to_json(const TouchLayout& layout);
// What's wrong (the editor says it; the game uses what it can).
std::vector<std::string> touch_layout_problems(const TouchLayout& layout);

}  // namespace midgard
