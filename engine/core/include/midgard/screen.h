// SPDX-License-Identifier: Apache-2.0
// Screens made in the Menu editor (milestone 6.0): menus, the title screen, pause and game over.
//
// A screen (the screen table, migration 21) is JSON:
//
//   {"role": "pause",      what the game uses it for itself: "splash" (shown while the game
//                          loads, before anything else), "title" (shown when the game
//                          starts), "pause" (the menu action opens it, and goes back from
//                          it), "settings", "game_over" (shown when "stat" runs out: the
//                          player died), or "" (only as scripts and buttons show it)
//    "stat": "health",     a game_over screen's stat
//    "modal": true,        takes the player's input and frees the mouse while shown
//    "pauses": false,      the game stands still while it is shown
//    "seconds": 3,         a splash screen shows at least this long, however soon the game
//                          has loaded (0: only while it loads)
//    "music": true,        a modal screen plays the game's menu music (midgard/game_settings.h)
//                          while it shows; false keeps it off
//    "background": [0, 0, 0, 0.6],     over the whole screen (r, g, b, a: 0 to 1)
//    "image": "ui:...",    a picture over the whole screen (from the GUI section)
//    "elements": [...]}    drawn in order, each after its parent
//
// An element:
//
//   {"id": "play", "kind": "button", "parent": "",   a panel's id, or "" for the screen
//    "anchor": "center",   where on its parent it hangs: top_left, top, top_right, left,
//                          center, right, bottom_left, bottom, bottom_right, or fill
//    "x": 0, "y": 0,       dp from the anchor (right and down are positive); fill: the margin
//    "w": 240, "h": 56,    dp (fill: the parent's size less the margins)
//    "text": "New Game",   text, button and check
//    "image": "ui:library/gui/kenney/button_blue.png",   any element: a picture (from the
//                          Asset Library's GUI section) stretched over it; a button with one
//                          has no fill or border of its own unless its style gives them
//    "images": {...},      pictures for its states (each falls back as said):
//                            button  hover, pressed (else hover), disabled (else "image": its
//                                    normal picture)
//                            check, option   unchecked, checked (else unchecked): its box
//                            slider  track (the bar along it), pointer (the handle)
//    "enabled": true,      false: it can't be used (greyed, or its disabled picture) until a
//                          script enables it (midgard.ui.set_enabled)
//    "group": "difficulty",        an option: choosing one unchooses the others in its group
//    "style": {"font_size": 20, "color": [r, g, b, a], "background": [..], "border": [..],
//              "border_width": 2, "radius": 6, "align": "center", "bold": false,
//              "italic": false, "font": "OpenDyslexic" or "ui:library/fonts/roboto"},
//    "action": {"do": "close"}     a button: none, close (this screen), open (another:
//                                  "screen": id), quit (the game), respawn (the player back
//                                  where it arrived, its stats refilled), or event (scripts)
//    "binds": "volume:music",      a slider: a bus's volume (master, music, effects, voice,
//                                  ambience, interface), the player's setting
//    "binds": "key:jump",          a button: the key for an action; clicked, the next key
//                                  pressed becomes it (the player's setting)
//    "min": 0, "max": 1, "step": 0.1, "value": 0.5}    a slider; a check's "value" is true/false
//
// Kinds: panel (a box that holds others), text, button, slider, check (a box to tick with its
// text), option (one choice of a group, with its text), image (a picture alone: a logo). Lengths
// are in dp, so a screen is the same size to the eye on every display, inside the safe area (clear
// of a phone's notch). Scripts show and hide screens and hear their buttons (midgard.ui.show,
// on_click); the runtime draws them with RmlUi (screen_rml).
#pragma once

#include <array>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace midgard {

class ScreenError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using Rgba = std::array<double, 4>;

struct ScreenStyle {
    std::optional<double> font_size;  // dp
    std::optional<Rgba> color, background, border;
    std::optional<double> border_width, radius;  // dp
    std::string align;                           // "", left, center, right
    bool bold = false, italic = false;
    // The font: "" (Inconsolata, the default), "Inconsolata", "OpenDyslexic" (Midgard's own), or
    // a font from the Asset Library's Fonts section: "ui:" and its folder (all its .ttf and
    // .otf files: regular, bold, italic, as it has them).
    std::string font;
};

struct ScreenElement {
    std::string id, kind, parent;
    std::string anchor = "top_left";
    double x = 0.0, y = 0.0, w = 100.0, h = 40.0;
    std::string text;
    std::string image;                          // a "ui:" content id ("": none)
    std::map<std::string, std::string> images;  // by state (screen_image_states)
    bool enabled = true;
    std::string group;  // an option's
    ScreenStyle style;
    std::string action = "none";  // buttons: none, close, open, quit, respawn, event
    std::string binds;            // "volume:<bus>" (a slider), "key:<action>" (a button)
    std::string target;           // open: the screen
    double min = 0.0, max = 1.0, step = 0.0, value = 0.0;  // sliders; checks: value 0 or 1
};

struct ScreenDef {
    std::string id, name;
    std::string role;             // "", splash, title, pause, settings, game_over
    std::string stat = "health";  // game_over: the stat that, run out, shows it
    bool modal = true, pauses = false, music = true;
    std::optional<Rgba> background;
    std::string image;     // over the whole screen
    double seconds = 0.0;  // a splash screen: it shows at least this long (0 to 60)
    std::vector<ScreenElement> elements;

    const ScreenElement* find(const std::string& element) const;
};

// The names the format knows.
const std::vector<std::string>& screen_kinds();  // panel, text, button, slider, check, option,
                                                 // image
// The pictures a kind has for its states (besides its "image"): a button's hover, pressed and
// disabled; a check's and an option's unchecked and checked; a slider's track and pointer.
const std::vector<std::string>& screen_image_states(const std::string& kind);
const std::vector<std::string>& screen_anchors();  // top_left ... bottom_right, fill
const std::vector<std::string>& screen_actions();  // none, close, open, quit, respawn, event
const std::vector<std::string>& screen_roles();    // "", splash, title, pause, settings,
                                                   // game_over
const std::vector<std::string>& screen_buses();    // master, music, effects, voice, ambience,
                                                   // interface

// A screen from its table row. Throws ScreenError naming the screen and what's wrong: an
// unknown kind or anchor, an element without an id or with one used twice, a parent that isn't
// a panel before it, a number that isn't one.
ScreenDef screen_from_json(const std::string& id, const std::string& name, const std::string& json);

// The screen as an RmlUi document: each element a <div> (a panel or text), <button> or <input>
// with id "el-<id>", placed by its anchor. Its look comes from midgard://base.rcss and its own
// style.
std::string screen_rml(const ScreenDef& screen);

// The pictures a screen shows ("ui:" content ids, each once), and the name each goes by in
// the UI (Ui::set_image; midgard://image/NAME).
std::vector<std::string> screen_images(const ScreenDef& screen);
std::string screen_image_name(const std::string& content);
// The Asset Library fonts a screen's text uses (their "ui:" folders, each once), and the family
// name each is loaded under in the UI (Ui::add_font).
std::vector<std::string> screen_fonts(const ScreenDef& screen);
std::string screen_font_family(const std::string& font);

// Text as RML shows it (<, >, & and quotes escaped).
std::string rml_escape(const std::string& text);

}  // namespace midgard
