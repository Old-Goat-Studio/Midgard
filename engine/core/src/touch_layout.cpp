// SPDX-License-Identifier: Apache-2.0
#include "midgard/touch_layout.h"

#include <algorithm>
#include <set>

#include <nlohmann/json.hpp>

namespace midgard {

TouchLayout default_touch_layout() {
    // (Places in dp in from the safe area's bottom right: sideways, then upright, where the
    // buttons draw in closer to the right edge to keep clear of the stick.)
    TouchLayout l;
    l.buttons = {
        {"jump", "Jump", 84, 96, 40, 74, 92, 38, false, "always"},
        {"use", "Use", 186, 62, 30, 166, 56, 28, false, "always"},
        {"run", "Run", 180, 170, 30, 162, 152, 28, true, "always"},
        {"view", "View", 78, 206, 26, 70, 196, 26, false, "always"},
        {"descend", "Down", 84, 290, 30, 152, 240, 28, false, "flying"},
        {"menu", "Menu", 186, 252, 26, 70, 290, 26, false, "pause_menu"},
    };
    return l;
}

TouchLayout touch_layout_from_json(const nlohmann::json& j) {
    if (!j.is_object() || !j.contains("buttons") || !j["buttons"].is_array())
        return default_touch_layout();
    TouchLayout l;
    const auto number = [](const nlohmann::json& b, const char* key, float fallback) {
        return b.contains(key) && b[key].is_number() ? b[key].get<float>() : fallback;
    };
    for (const nlohmann::json& b : j["buttons"]) {
        if (!b.is_object() || !b.contains("id") || !b["id"].is_string()) continue;
        TouchButton t;
        t.id = b["id"].get<std::string>();
        if (t.id.empty()) continue;
        t.label =
            b.contains("label") && b["label"].is_string() ? b["label"].get<std::string>() : t.id;
        t.x = number(b, "x", t.x);
        t.y = number(b, "y", t.y);
        t.r = std::clamp(number(b, "r", t.r), 12.0f, 120.0f);
        t.nx = t.x;
        t.ny = t.y;
        t.nr = t.r;
        if (b.contains("narrow") && b["narrow"].is_array() && b["narrow"].size() == 3 &&
            b["narrow"][0].is_number() && b["narrow"][1].is_number() &&
            b["narrow"][2].is_number()) {
            t.nx = b["narrow"][0].get<float>();
            t.ny = b["narrow"][1].get<float>();
            t.nr = std::clamp(b["narrow"][2].get<float>(), 12.0f, 120.0f);
        }
        t.toggles = b.value("toggles", false);
        if (b.contains("shows") && b["shows"].is_string()) t.shows = b["shows"].get<std::string>();
        l.buttons.push_back(t);
    }
    return l;
}

nlohmann::json touch_layout_to_json(const TouchLayout& layout) {
    nlohmann::json buttons = nlohmann::json::array();
    for (const TouchButton& b : layout.buttons)
        buttons.push_back({{"id", b.id},
                           {"label", b.label},
                           {"x", b.x},
                           {"y", b.y},
                           {"r", b.r},
                           {"narrow", {b.nx, b.ny, b.nr}},
                           {"toggles", b.toggles},
                           {"shows", b.shows}});
    return {{"buttons", buttons}};
}

std::vector<std::string> touch_layout_problems(const TouchLayout& layout) {
    std::vector<std::string> out;
    std::set<std::string> ids;
    static const std::set<std::string> shows = {"always", "on_foot", "flying",
                                                "menu",   "vehicle", "pause_menu"};
    for (const TouchButton& b : layout.buttons) {
        if (!ids.insert(b.id).second) out.push_back("touch button '" + b.id + "' is there twice");
        if (!shows.contains(b.shows))
            out.push_back("touch button '" + b.id + "' shows '" + b.shows +
                          "', which isn't always, a context or pause_menu");
    }
    return out;
}

}  // namespace midgard
