// SPDX-License-Identifier: Apache-2.0
// The HUD (milestone 6.0): the player's stats, timers, the note feed, a prompt and the
// crosshair over the game, as an RmlUi screen bound to a data model.
//
// Midgard's own HUD screen (midgard://hud.rml) shows them all; a game's own HUD screens (the
// Menu editor, step 6) bind to the same model, "hud":
//   visible, crosshair          true or false
//   bars                        the stats shown as bars: {id, name, text ("87/100"), percent}
//   numbers                     scores and counters: {id, name, text ("40", "3/5")}
//   timers                      {id, label, text ("1:05")}
//   notes                       {text, opacity}
//   prompt, has_prompt          the prompt's text, and whether there is one
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace Rml {
class ElementDocument;
}

namespace midgard::ui {

class Ui;

// What the HUD shows this frame (the runtime makes it from the game).
struct HudData {
    bool visible = true;
    bool crosshair = false;
    struct Stat {
        std::string id, name;
        bool bar = true;  // a stat: a bar; a score or counter: a number
        double value = 0.0;
        double max = 0.0;  // a bar's most; a counter's target (0: none)
    };
    std::vector<Stat> stats;
    struct Timer {
        std::string id, label;
        double seconds = 0.0;
    };
    std::vector<Timer> timers;
    struct Note {
        std::string text;
        float opacity = 1.0f;
    };
    std::vector<Note> notes;
    std::string prompt;
};

// A timer's seconds as the HUD writes them: "0:07", "1:05", "1:02:03".
std::string clock_text(double seconds);

class Hud {
public:
    // Makes the "hud" data model and shows Midgard's HUD screen (not modal: it never takes the
    // game's input). Throws UiError if the screen can't be made.
    explicit Hud(Ui& ui);
    ~Hud();
    Hud(const Hud&) = delete;
    Hud& operator=(const Hud&) = delete;

    // This frame's data (only what changed is laid out again).
    void show(const HudData& data);
    Rml::ElementDocument* document();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace midgard::ui
