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
    // 0008_run.sql and 0013_use.sql, for older packages.
    // (A plain array: MSVC 19.40 crashes on a range-for over a braced initializer list here.)
    static const Binding kDefaults[] = {
        {"move_x", "keyboard", "D", 1.0f},      {"move_x", "keyboard", "A", -1.0f},
        {"move_x", "gamepad", "leftx", 1.0f},   {"move_y", "keyboard", "W", 1.0f},
        {"move_y", "keyboard", "S", -1.0f},     {"move_y", "gamepad", "lefty", -1.0f},
        {"look_x", "mouse", "delta_x", 0.15f},  {"look_x", "gamepad", "rightx", 120.0f},
        {"look_y", "mouse", "delta_y", -0.15f}, {"look_y", "gamepad", "righty", -90.0f},
        {"zoom", "mouse", "wheel", -1.0f},      {"jump", "keyboard", "Space", 1.0f},
        {"jump", "gamepad", "a", 1.0f},         {"view", "keyboard", "V", 1.0f},
        {"view", "gamepad", "y", 1.0f},         {"run", "keyboard", "Left Shift", 1.0f},
        {"run", "gamepad", "leftstick", 1.0f},  {"menu", "keyboard", "Escape", 1.0f},
        {"menu", "gamepad", "start", 1.0f},     {"use", "keyboard", "E", 1.0f},
        {"use", "gamepad", "x", 1.0f},          {"descend", "keyboard", "Left Ctrl", 1.0f},
        {"descend", "gamepad", "b", 1.0f},
    };
    InputMap m;
    for (const Binding& b : kDefaults) m.add(b);
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
    return m;
}

void InputMap::add(const Binding& binding) {
    bindings_.push_back(binding);
}

Actions::Actions(InputMap map) : map_(std::move(map)) {}

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
