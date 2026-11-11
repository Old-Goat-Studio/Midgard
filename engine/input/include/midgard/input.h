// SPDX-License-Identifier: Apache-2.0
// The input actions layer. Devices feed controls ("keyboard", "W"); game code reads actions
// ("move_y"). Nothing outside this layer looks at raw keys. Platform-independent: the runtime
// translates SDL events into control names (see schema/migrations/0003_input.sql).
#pragma once

#include <filesystem>
#include <map>
#include <optional>
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

// Contexts (milestone 6.1): an action counts only while its context is the game's (on foot,
// flying, in a menu, in a vehicle), or always. So one key can do one thing on foot and another
// flying. The game chooses the context each frame (a script can choose its own).
inline constexpr const char* kAlwaysContext = "always";
inline constexpr const char* kContexts[] = {"on_foot", "flying", "menu", "vehicle"};

// A control the player can rebind (milestone 6.1 step 4), as a screen's button names it:
// "key:jump" (a key), "pad:jump" (a gamepad button), "key:move_y:+" / "key:move_y:-" (one way of
// an axis: forward, back). The player's choices are kept in the profile by slot_key().
struct ControlSlot {
    std::string device;  // "keyboard" or "gamepad"
    std::string action;
    int sign = 0;  // 0 a button's; +1 or -1 one way of an axis
};
std::optional<ControlSlot> control_slot(const std::string& binds);
std::string slot_key(const ControlSlot& slot);  // "jump", "move_y:+"
// A control as players read it: "Space", "A", "LB", "D-pad up".
std::string control_label(const std::string& device, const std::string& control);

class InputMap {
public:
    // Reads input_action / input_binding from a package database (schema 3+). Older
    // packages have no input tables and get the built-in defaults.
    static InputMap load(const std::filesystem::path& database);
    static InputMap defaults();

    void add(const Binding& binding);
    // The action's controls on a device become this one (the player's rebinding: a key); with
    // a sign, only those pushing that way (an axis's: W forward, the rest kept).
    void rebind(const std::string& action, const std::string& device, const std::string& control,
                int sign = 0);
    // The action's first control on a device ("" if none); with a sign, pushing that way.
    std::string control(const std::string& action, const std::string& device, int sign = 0) const;
    // An action's kind ("button" or "axis") and whether players may rebind it (schema 22 on;
    // before, buttons and axes as the bindings say, and all but looking and the menu).
    struct Info {
        std::string kind = "button";
        bool rebindable = true;
    };
    void set_info(const std::string& action, Info info) { info_[action] = std::move(info); }
    Info info(const std::string& action) const;
    // The slots players may rebind: each rebindable button's key and gamepad button, and each
    // way of a rebindable axis's keys, in the actions' order.
    std::vector<ControlSlot> rebindable_slots() const;
    // An action's context ("always" unless set: one that isn't known counts always).
    void set_context(const std::string& action, const std::string& context);
    const std::string& context(const std::string& action) const;
    const std::vector<Binding>& bindings() const { return bindings_; }

private:
    std::vector<Binding> bindings_;
    std::map<std::string, std::string> contexts_;
    std::map<std::string, Info> info_;
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
    void rebind(const std::string& action, const std::string& device, const std::string& control,
                int sign = 0) {
        map_.rebind(action, device, control, sign);
    }
    const InputMap& map() const { return map_; }
    // The game's context now (actions of others read nothing; "always" ones always count). The
    // runtime sets it each frame; until it is set, every action counts.
    void set_context(const std::string& context) { context_ = context; }
    const std::string& context() const { return context_; }
    bool active(const std::string& action) const;
    bool down(const std::string& action) const;     // held now
    bool pressed(const std::string& action) const;  // went down this frame

private:
    float held(const std::string& action) const;

    InputMap map_;
    std::string context_;                            // ("": none chosen yet, every action counts)
    std::unordered_map<std::string, float> state_;   // "device:control" -> value
    std::unordered_map<std::string, float> deltas_;  // this frame only
    std::unordered_set<std::string> down_last_frame_;
};

}  // namespace midgard
