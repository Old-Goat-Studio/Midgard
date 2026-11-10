// SPDX-License-Identifier: Apache-2.0
// The Menu editor's screens on the game's UI (milestone 6.0): the screens the game shows
// (midgard/hud.h's HudState::screens) drawn as RmlUi documents (midgard/screen.h's screen_rml),
// bottom to top, with the text and values scripts set; and what the player does on them,
// clicks and changes, told back to the game.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "midgard/screen.h"

namespace Rml {
class ElementDocument;
}

namespace midgard::ui {

class Ui;

class Screens {
public:
    // `images` gives a picture's file bytes by its "ui:" content id (empty: it isn't there,
    // and the element shows without it).
    using ImageLoader = std::function<std::vector<uint8_t>(const std::string& content)>;
    explicit Screens(Ui& ui, ImageLoader images = {});
    ~Screens();
    Screens(const Screens&) = delete;
    Screens& operator=(const Screens&) = delete;

    // Shows exactly the screens in `shown` (bottom to top) as `defs` make them: opening and
    // closing them, and opening one again when its design changed. `texts` and `values` (by
    // "screen/element") are scripts' and the player's changes. A screen that isn't in `defs` is
    // left out.
    // `enabled`: elements scripts enabled or disabled (as midgard/hud.h's HudState has them).
    void sync(const std::vector<ScreenDef>& defs, const std::vector<std::string>& shown,
              const std::map<std::string, std::string>& texts,
              const std::map<std::string, double>& values,
              const std::map<std::string, bool>& enabled = {});

    // What the player did since the last call: "click" (a button) or "change" (a slider's value,
    // a check's 0 or 1), in order.
    struct Event {
        std::string screen, element, kind;
        double value = 0.0;
    };
    std::vector<Event> take_events();

    // A shown screen stops the game (its "pauses").
    bool pauses() const;
    // A shown screen's document (nullptr if it isn't shown).
    Rml::ElementDocument* document(const std::string& screen) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace midgard::ui
