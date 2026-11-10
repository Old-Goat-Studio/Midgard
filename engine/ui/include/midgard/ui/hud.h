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
//   compass, compass_left       the compass shows; its strip's place (dp) for the heading
//   compass_marks               the strip's marks: {text, left (dp), major}
//   minimap, has_ground, arrow  the minimap shows; it has ground; the player's arrow (degrees)
//   ground_left, ground_top     where its ground picture (midgard://image/minimap) sits (%)
//   ground_width                its width (and height), % of the minimap's
//   map_marks                   {kind ("exit", "mark"), label, left, top (%)}
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
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
        std::string text;  // a number's own text in place of its value (a position, a clock)
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
    // The compass: the way the camera faces, degrees from north (none: no compass).
    std::optional<double> heading;
    // The minimap (north up, the player in the middle).
    struct Map {
        bool shown = false;
        double radius = 120.0;  // metres from the middle to each edge
        // Its ground (none: flat, no terrain): size x size RGBA8 pixels, `ground_radius`
        // metres from its middle to each edge (0: `radius`), made around a place `east` and
        // `north` metres from the player; `version` changes when they do.
        const std::vector<uint8_t>* ground = nullptr;
        int size = 0;
        double ground_radius = 0.0;
        std::uint64_t version = 0;
        double east = 0.0, north = 0.0;
        struct Mark {
            std::string kind, label;  // kind: "exit" (a scene exit) or "mark" (a script's)
            double east = 0.0, north = 0.0;
        };
        std::vector<Mark> marks;
    };
    Map map;
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
