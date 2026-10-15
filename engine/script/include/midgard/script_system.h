// SPDX-License-Identifier: Apache-2.0
// Entity scripts (roadmap §4: the midgard Python API). An entity with a script component runs a
// module from the package's scripts/ folder:
//
//   import midgard
//
//   def on_start(entity): ...                      once, when the script starts
//   def on_update(entity, dt): ...                 every frame (dt in seconds)
//   def on_trigger_enter(entity, other): ...       the player entered this entity's trigger
//   def on_trigger_exit(entity, other): ...        ... or left it
//   def on_reload(entity): ...                     the module was edited and reloaded
//
// The player's own script gets on_trigger_enter(player, trigger) too. Every function is
// optional. A script that raises an error is paused (with the error reported) until it is
// reloaded.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "midgard/game.h"
#include "midgard/input.h"

namespace midgard::script {

// Something a script said or did wrong. file and line are set for errors from a script.
struct Report {
    std::string level;  // "info", "warn" or "error"
    std::string text;
    std::string module;
    std::string file;  // relative to the scripts folder, such as "door.py"
    int line = 0;
};

// Runs entity scripts. Needs a running Interpreter whose sys.path starts with the package's
// scripts/ folder. One per Game.
class ScriptSystem {
public:
    ScriptSystem(Game& game, std::function<void(const Report&)> report);
    ~ScriptSystem();
    ScriptSystem(const ScriptSystem&) = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    // One frame, after Game::update: starts new scripts, delivers this frame's trigger events,
    // then calls on_update.
    void update(const Actions& actions, float dt);

    // A module's file changed: re-imports it, resumes it if it was paused, and calls on_reload.
    void reload(const std::string& module);

    // True if any entity in the scene has a script component.
    static bool needed(const Game& game);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The names the midgard module defines and the attributes of midgard.Entity, sorted, as JSON:
// {"module": [...], "Entity": [...]} (for checking the editor's midgard.pyi stub).
std::string api_names();

}  // namespace midgard::script
