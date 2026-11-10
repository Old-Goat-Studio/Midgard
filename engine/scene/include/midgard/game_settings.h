// SPDX-License-Identifier: Apache-2.0
// The game's own settings for its screens (milestone 6.0), set on the editor's Project Page and
// kept in the database's meta table as "game.settings":
//
//   {"splash": "ui:library/gui/splash.png",   the picture shown while the game loads
//    "crosshair": "ui:...",                     the crosshair's picture ("": Midgard's dot)
//    "crosshair_views": "all",                  when it shows: "first" (first person, as it
//                                               starts), "all" (every view) or "none"
//    "crosshair_size": 32,                      dp across
//    "pointer": "ui:...",                       the mouse pointer over menus ("": the system's)
//    "menu_music": "mus_menu"}                  a playlist that plays while a menu shows ("":
//                                               none; a screen can say not to)
//
// Pictures are "ui:" content (PNG or JPEG from the Asset Library's GUI section), packed in
// game.pak. The runtime reads the splash before the game itself has loaded
// (read_package_content), so it shows while the rest loads.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace midgard {

struct GameSettings {
    std::string splash;
    std::string crosshair;
    std::string crosshair_views = "first";
    double crosshair_size = 32.0;
    std::string pointer;
    std::string menu_music;
};

inline constexpr const char* kGameSettingsKey = "game.settings";

// From its JSON; anything it can't use is left as the default (the editor checks what's set).
GameSettings game_settings_from_json(const std::string& json);
// From a package's (or project's) database: the defaults without any.
GameSettings load_game_settings(const std::filesystem::path& database);
// A packaged file's bytes by content id (the manifest's content table, then game.pak), for what
// is needed before the game loads (empty if it isn't there).
std::vector<uint8_t> read_package_content(const std::filesystem::path& package_dir,
                                          const std::string& content);

}  // namespace midgard
