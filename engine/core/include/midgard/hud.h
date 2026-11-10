// SPDX-License-Identifier: Apache-2.0
// What the HUD shows besides the stats (milestone 6.0): short notes that fade (the pick-up
// feed), a prompt ("Press E to open"), timers counting down or up, and whether the HUD and its
// crosshair show. Scripts change it (midgard.ui); the runtime draws it (midgard/ui/hud.h). It
// knows nothing of drawing, so games without a window (tests, servers) keep it too.
#pragma once

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace midgard::hud {

struct Note {
    std::string text;
    double age = 0.0;   // seconds shown
    double life = 4.0;  // seconds it shows for (it fades over the last second)
    // How visible it is now: 1, fading to 0 over its last second.
    float opacity() const;
};

struct Timer {
    std::string id, label;
    double value = 0.0;  // seconds: left (counting down) or passed (counting up)
    bool down = true;
    double limit = 0.0;  // counting up: when it ends (0: never)
};

inline constexpr size_t kMaxNotes = 5;  // the oldest goes when a sixth comes

// The player did something on a screen (midgard/screen.h): clicked a button ("click"), or moved
// a slider or ticked a check ("change", its value: a check's 0 or 1).
struct UiEvent {
    std::string screen, element, kind;
    double value = 0.0;
};

class HudState {
public:
    // The whole HUD (false: hidden, as in a cut scene).
    bool visible = true;
    // The crosshair: shown, hidden, or (none) as the camera has it: in first person only.
    std::optional<bool> crosshair;
    // The compass (top middle) and the minimap (bottom right).
    bool compass = true;
    bool minimap = true;
    // Entities marked on the minimap by scripts: entity id -> label ("" for none).
    std::map<std::string, std::string> marks;

    void notify(const std::string& text, double seconds = 4.0);
    const std::deque<Note>& notes() const { return notes_; }
    void clear_notes() { notes_.clear(); }

    // The prompt at the bottom ("" for none).
    void set_prompt(std::string text) { prompt_ = std::move(text); }
    const std::string& prompt() const { return prompt_; }

    // A timer by id (a new one with the same id replaces it): counting down from `seconds` to
    // 0, or up from 0 (to `seconds` if above 0, else for ever).
    void start_timer(const std::string& id, double seconds, bool down = true,
                     const std::string& label = "");
    bool stop_timer(const std::string& id);
    std::optional<double> timer(const std::string& id) const;
    const std::vector<Timer>& timers() const { return timers_; }

    // Notes age and go; timers run. Timers that ended are taken out, and told by
    // take_finished().
    void update(double dt);
    std::vector<std::string> take_finished();

    // Screens (the Menu editor's): those shown, bottom to top. Showing one already shown brings
    // it to the top.
    void show_screen(const std::string& id);
    bool hide_screen(const std::string& id);
    bool screen_shown(const std::string& id) const;
    const std::vector<std::string>& screens() const { return screens_; }
    // Their elements' text and values as scripts set them, or the player changed them, by
    // "screen/element" (none: as the screen was made).
    std::map<std::string, std::string> texts;
    std::map<std::string, double> values;
    // Elements scripts enabled or disabled (midgard.ui.set_enabled), by "screen/element".
    std::map<std::string, bool> enabled;
    // The player's clicks and changes (the runtime posts them; the game's next update hands
    // them to scripts, through take_events).
    void post(UiEvent event);
    std::vector<UiEvent> take_events();

    nlohmann::json dump() const;

private:
    std::vector<std::string> screens_;
    std::vector<UiEvent> events_;
    std::deque<Note> notes_;
    std::string prompt_;
    std::vector<Timer> timers_;
    std::vector<std::string> finished_;
};

}  // namespace midgard::hud
