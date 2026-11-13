// SPDX-License-Identifier: Apache-2.0
// The portal menu (midgard/scene.h's Portal): shown when the player presses Use at a portal.
// At the top, the portal's name in a text field with a Rename button (Enter renames too);
// below, every other portal as a button ("Hub", and the world it's in), and Close. The mouse,
// keys, a gamepad (B closes) and touch all work it, as every Midgard screen.
//
// It only says what the player chose (take()); the runtime asks the game to do it, after the
// UI's events, so nothing closes the menu while it is handling one.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Rml {
class ElementDocument;
}

namespace midgard::ui {

class Ui;

class PortalMenu {
public:
    struct Destination {
        std::string id;     // the portal's entity id
        std::string name;   // its name
        std::string place;  // where it is (its scene's name)
    };
    // Opens the menu (modal: it takes the input) for the portal named `name`, listing
    // `destinations`. Throws UiError if its screen can't be read.
    PortalMenu(Ui& ui, const std::string& name, const std::vector<Destination>& destinations);
    ~PortalMenu();
    PortalMenu(const PortalMenu&) = delete;
    PortalMenu& operator=(const PortalMenu&) = delete;

    // What the player chose since the last take(), if anything.
    struct Choice {
        enum class Kind { Rename, Travel, Close } kind = Kind::Close;
        std::string value;  // the new name, or the destination's id
    };
    std::optional<Choice> take();
    // A line under the name ("Renamed."; "A name can't be empty.").
    void say(const std::string& text);
    // The name shown, as the game now has it.
    void set_name(const std::string& name);
    Rml::ElementDocument* document();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace midgard::ui
