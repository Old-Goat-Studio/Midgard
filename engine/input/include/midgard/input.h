// SPDX-License-Identifier: Apache-2.0
// The input actions layer. Devices feed controls ("keyboard", "W"); game code reads actions
// ("move_y"). Nothing outside this layer looks at raw keys. Platform-independent: the runtime
// translates SDL events into control names (see schema/migrations/0003_input.sql).
#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace midgard {

struct Binding {
    std::string action;
    std::string device;
    std::string control;
    float scale = 1.0f;
};

class InputMap {
public:
    // Reads input_action / input_binding from a package database (schema 3+). Older
    // packages have no input tables and get the built-in defaults.
    static InputMap load(const std::filesystem::path& database);
    static InputMap defaults();

    void add(const Binding& binding);
    // The action's controls on a device become this one (the player's rebinding: a key).
    void rebind(const std::string& action, const std::string& device, const std::string& control);
    // The action's first control on a device ("" if none).
    std::string control(const std::string& action, const std::string& device) const;
    const std::vector<Binding>& bindings() const { return bindings_; }

private:
    std::vector<Binding> bindings_;
};

class Actions {
public:
    explicit Actions(InputMap map);

    // Call once per frame before feeding events: clears this frame's deltas.
    void begin_frame();

    // A control's current value: 0/1 for keys and buttons, -1..1 for sticks and triggers.
    void set(const std::string& device, const std::string& control, float value);
    // A relative amount for this frame only (mouse movement, wheel).
    void add_delta(const std::string& device, const std::string& control, float amount);

    // The action's value this frame: direction-style bindings (|scale| <= 1, such as keys and
    // stick directions) summed and clamped to -1..1, plus rate-style bindings (|scale| > 1,
    // per second, such as stick look in degrees) times dt, plus this frame's deltas (mouse
    // look in degrees, wheel steps).
    float axis(const std::string& action, float dt = 1.0f) const;
    // The player's rebinding (InputMap::rebind) while playing.
    void rebind(const std::string& action, const std::string& device, const std::string& control) {
        map_.rebind(action, device, control);
    }
    const InputMap& map() const { return map_; }
    bool down(const std::string& action) const;     // held now
    bool pressed(const std::string& action) const;  // went down this frame

private:
    float held(const std::string& action) const;

    InputMap map_;
    std::unordered_map<std::string, float> state_;   // "device:control" -> value
    std::unordered_map<std::string, float> deltas_;  // this frame only
    std::unordered_set<std::string> down_last_frame_;
};

}  // namespace midgard
