// SPDX-License-Identifier: Apache-2.0
#include "midgard/input.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <stdexcept>

#include <sqlite3.h>

namespace midgard {
namespace {

std::string key(const std::string& device, const std::string& control) {
    return device + ":" + control;
}

}  // namespace

InputMap InputMap::defaults() {
    // The same defaults as schema/migrations/0003_input.sql, 0007_camera_view.sql,
    // 0008_run.sql, 0013_use.sql and 0020_touch.sql, for older packages.
    // (A plain array: MSVC 19.40 crashes on a range-for over a braced initializer list here.)
    static const Binding kDefaults[] = {
        {"move_x", "keyboard", "D", 1.0f},
        {"move_x", "keyboard", "A", -1.0f},
        {"move_x", "gamepad", "leftx", 1.0f},
        {"move_y", "keyboard", "W", 1.0f},
        {"move_y", "keyboard", "S", -1.0f},
        {"move_y", "gamepad", "lefty", -1.0f},
        {"look_x", "mouse", "delta_x", 0.15f},
        {"look_x", "gamepad", "rightx", 120.0f},
        {"look_y", "mouse", "delta_y", -0.15f},
        {"look_y", "gamepad", "righty", -90.0f},
        {"zoom", "mouse", "wheel", -1.0f},
        {"jump", "keyboard", "Space", 1.0f},
        {"jump", "gamepad", "a", 1.0f},
        {"view", "keyboard", "V", 1.0f},
        {"view", "gamepad", "y", 1.0f},
        {"run", "keyboard", "Left Shift", 1.0f},
        {"run", "gamepad", "leftstick", 1.0f},
        {"menu", "keyboard", "Escape", 1.0f},
        {"menu", "gamepad", "start", 1.0f},
        {"use", "keyboard", "E", 1.0f},
        {"use", "gamepad", "x", 1.0f},
        {"descend", "keyboard", "Left Ctrl", 1.0f},
        {"descend", "gamepad", "b", 1.0f},
        // The on-screen touch controls (midgard/ui/touch.h): the stick, dragging to look (dp),
        // and the buttons.
        {"move_x", "touch", "stick_x", 1.0f},
        {"move_y", "touch", "stick_y", 1.0f},
        {"look_x", "touch", "drag_x", 0.3f},
        {"look_y", "touch", "drag_y", -0.3f},
        {"jump", "touch", "jump", 1.0f},
        {"use", "touch", "use", 1.0f},
        {"run", "touch", "run", 1.0f},
        {"view", "touch", "view", 1.0f},
        {"descend", "touch", "descend", 1.0f},
    };
    InputMap m;
    for (const Binding& b : kDefaults) m.add(b);
    m.set_context("descend", "flying");  // (the rest count always)
    for (const char* axis : {"move_x", "move_y", "look_x", "look_y", "zoom"})
        m.set_info(axis, {"axis", true});
    for (const char* fixed : {"look_x", "look_y", "zoom", "menu"})
        m.set_info(fixed, {m.info(fixed).kind, false});
    return m;
}
InputMap InputMap::load(const std::filesystem::path& database) {
    sqlite3* raw = nullptr;
    const std::u8string u8 = database.u8string();
    if (sqlite3_open_v2(std::string(u8.begin(), u8.end()).c_str(), &raw, SQLITE_OPEN_READONLY,
                        nullptr) != SQLITE_OK) {
        sqlite3_close(raw);
        throw std::runtime_error("cannot open " + database.string());
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), "SELECT action, device, control, scale FROM input_binding", -1,
                           &stmt, nullptr) != SQLITE_OK)
        return defaults();  // schema 1-2 packages have no input tables
    InputMap m;
    auto text = [&](int col) {
        const auto* t = sqlite3_column_text(stmt, col);
        return std::string(t ? reinterpret_cast<const char*>(t) : "");
    };
    std::set<std::string> bound;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        m.add({text(0), text(1), text(2), static_cast<float>(sqlite3_column_double(stmt, 3))});
        bound.insert(text(0));
    }
    sqlite3_finalize(stmt);
    // Actions added to Midgard after the project was made get their default controls.
    const InputMap fallback = defaults();  // (kept: its bindings are a reference into it)
    for (const Binding& b : fallback.bindings())
        if (!bound.contains(b.action)) m.add(b);
    // Their contexts (schema 23 on: before, every action said "on_foot" and none was kept to
    // it, so they all count always, as the defaults' do).
    int version = 0;
    if (sqlite3_prepare_v2(db.get(), "PRAGMA user_version", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) version = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    m.set_context("descend", "flying");
    for (const char* axis : {"move_x", "move_y", "look_x", "look_y", "zoom"})
        m.set_info(axis, {"axis", true});
    for (const char* fixed : {"look_x", "look_y", "zoom", "menu"})
        m.set_info(fixed, {m.info(fixed).kind, false});
    if (version >= 22 &&
        sqlite3_prepare_v2(db.get(), "SELECT id, kind, rebindable FROM input_action", -1, &stmt,
                           nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW)
            m.set_info(text(0), {text(1), sqlite3_column_int(stmt, 2) != 0});
        sqlite3_finalize(stmt);
    }
    if (version >= 23 && sqlite3_prepare_v2(db.get(), "SELECT id, context FROM input_action", -1,
                                            &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) m.set_context(text(0), text(1));
        sqlite3_finalize(stmt);
    }
    return m;
}

void InputMap::add(const Binding& binding) {
    bindings_.push_back(binding);
}

namespace {
// A binding pushing one way (a key or button on an axis: |scale| <= 1), or any with sign 0.
bool pushes(const Binding& b, int sign) {
    return sign == 0 || (std::abs(b.scale) <= 1.0f && (b.scale > 0.0f) == (sign > 0));
}
}  // namespace

void InputMap::rebind(const std::string& action, const std::string& device,
                      const std::string& control, int sign) {
    std::erase_if(bindings_, [&](const Binding& b) {
        return b.action == action && b.device == device && pushes(b, sign);
    });
    bindings_.push_back({action, device, control, sign < 0 ? -1.0f : 1.0f});
}

std::string InputMap::control(const std::string& action, const std::string& device,
                              int sign) const {
    for (const Binding& b : bindings_)
        if (b.action == action && b.device == device && pushes(b, sign)) return b.control;
    return {};
}

InputMap::Info InputMap::info(const std::string& action) const {
    const auto it = info_.find(action);
    return it == info_.end() ? Info{} : it->second;
}

std::vector<ControlSlot> InputMap::rebindable_slots() const {
    std::vector<std::string> actions;  // (in the bindings' order: Midgard's, then the game's)
    for (const Binding& b : bindings_)
        if (std::find(actions.begin(), actions.end(), b.action) == actions.end())
            actions.push_back(b.action);
    std::vector<ControlSlot> out;
    for (const std::string& a : actions) {
        const Info i = info(a);
        if (!i.rebindable) continue;
        if (i.kind == "axis") {
            out.push_back({"keyboard", a, 1});
            out.push_back({"keyboard", a, -1});
        } else {
            out.push_back({"keyboard", a, 0});
            out.push_back({"gamepad", a, 0});
        }
    }
    return out;
}

std::optional<ControlSlot> control_slot(const std::string& binds) {
    ControlSlot s;
    std::string rest;
    if (binds.starts_with("key:")) {
        s.device = "keyboard";
        rest = binds.substr(4);
    } else if (binds.starts_with("pad:")) {
        s.device = "gamepad";
        rest = binds.substr(4);
    } else {
        return std::nullopt;
    }
    if (rest.ends_with(":+") || rest.ends_with(":-")) {
        s.sign = rest.back() == '+' ? 1 : -1;
        rest.resize(rest.size() - 2);
    }
    if (rest.empty() || rest.find(':') != std::string::npos) return std::nullopt;
    s.action = rest;
    return s;
}

std::string slot_key(const ControlSlot& slot) {
    return slot.action + (slot.sign > 0 ? ":+" : slot.sign < 0 ? ":-" : "");
}

std::string control_label(const std::string& device, const std::string& control) {
    if (device == "gamepad") {
        static const std::map<std::string, std::string> pad = {{"a", "A"},
                                                               {"b", "B"},
                                                               {"x", "X"},
                                                               {"y", "Y"},
                                                               {"leftshoulder", "LB"},
                                                               {"rightshoulder", "RB"},
                                                               {"lefttrigger", "LT"},
                                                               {"righttrigger", "RT"},
                                                               {"leftstick", "L3"},
                                                               {"rightstick", "R3"},
                                                               {"dpup", "D-pad up"},
                                                               {"dpdown", "D-pad down"},
                                                               {"dpleft", "D-pad left"},
                                                               {"dpright", "D-pad right"},
                                                               {"back", "Back"},
                                                               {"start", "Start"},
                                                               {"guide", "Guide"}};
        const auto it = pad.find(control);
        return it == pad.end() ? control : it->second;
    }
    if (device == "mouse") {
        static const std::map<std::string, std::string> mouse = {
            {"left", "Left click"}, {"right", "Right click"}, {"middle", "Middle click"},
            {"x1", "Mouse back"},   {"x2", "Mouse forward"},  {"wheel", "Mouse wheel"}};
        const auto it = mouse.find(control);
        return it == mouse.end() ? control : it->second;
    }
    return control;
}

void InputMap::set_context(const std::string& action, const std::string& context) {
    if (context.empty() || context == kAlwaysContext)
        contexts_.erase(action);
    else
        contexts_[action] = context;
}

const std::string& InputMap::context(const std::string& action) const {
    static const std::string always = kAlwaysContext;
    const auto it = contexts_.find(action);
    return it == contexts_.end() ? always : it->second;
}

Actions::Actions(InputMap map) : map_(std::move(map)) {}

bool Actions::active(const std::string& action) const {
    const std::string& c = map_.context(action);
    return context_.empty() || c == kAlwaysContext || c == context_;
}

void Actions::begin_frame() {
    deltas_.clear();
    down_last_frame_.clear();
    for (const Binding& b : map_.bindings())
        if (down(b.action)) down_last_frame_.insert(b.action);
}

void Actions::set(const std::string& device, const std::string& control, float value) {
    state_[key(device, control)] = value;
}

void Actions::add_delta(const std::string& device, const std::string& control, float amount) {
    deltas_[key(device, control)] += amount;
}

float Actions::held(const std::string& action) const {
    if (!active(action)) return 0.0f;  // (another context's)
    // Direction-style bindings (|scale| <= 1: keys, stick directions), clamped to -1..1.
    float sum = 0.0f;
    for (const Binding& b : map_.bindings()) {
        if (b.action != action || std::abs(b.scale) > 1.0f) continue;
        if (auto it = state_.find(key(b.device, b.control)); it != state_.end())
            sum += it->second * b.scale;
    }
    return std::clamp(sum, -1.0f, 1.0f);
}

float Actions::axis(const std::string& action, float dt) const {
    if (!active(action)) return 0.0f;
    float value = held(action);
    for (const Binding& b : map_.bindings()) {
        if (b.action != action) continue;
        const std::string k = key(b.device, b.control);
        // Rate-style bindings (|scale| > 1: stick look in degrees per second) are not clamped.
        if (std::abs(b.scale) > 1.0f)
            if (auto s = state_.find(k); s != state_.end()) value += s->second * b.scale * dt;
        // Per-frame movement (mouse look, wheel) is added on top, unclamped.
        if (auto d = deltas_.find(k); d != deltas_.end()) value += d->second * b.scale;
    }
    return value;
}
bool Actions::down(const std::string& action) const {
    return std::abs(held(action)) > 0.5f;
}

bool Actions::pressed(const std::string& action) const {
    return down(action) && !down_last_frame_.contains(action);
}

}  // namespace midgard
