// SPDX-License-Identifier: Apache-2.0
// A world's folder (roadmap §7, "Worlds"; started in milestone 5.7): one folder per world, named
// by its id, in the player's data folder (or a dedicated server's saves/).
//
//   world.json   who the world is: id, name, description, seed, the game it belongs to. Read by
//                the world list without opening a database.
//   world.db     its state (SQLite). For now the clock: game_time and the time scale, so loading
//                restores the date to the second. Discovered bodies, quests, banks and the rest
//                join it in later phases.
//
// Phase 6's world list and New World screen create worlds; until then the runtime keeps one.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

namespace midgard {

class WorldError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct WorldInfo {
    std::string id;  // 32 hex digits
    std::string name = "World";
    std::string description;
    std::uint64_t seed = 0;
    std::string game;     // the game's name, from its manifest
    std::string created;  // when, UTC (ISO 8601)
};

struct WorldClock {
    std::int64_t game_time = 0;
    double scale = 1.0;
};

class WorldSave {
public:
    // Opens the world in `folder`, making it (world.json with a new id and seed, and world.db)
    // if it isn't there yet. Throws WorldError if it can't be read or written.
    static WorldSave open(const std::filesystem::path& folder, const std::string& game);

    const WorldInfo& info() const { return info_; }
    const std::filesystem::path& folder() const { return folder_; }
    // The clock as last saved; none in a new world.
    std::optional<WorldClock> clock() const;
    // Saves the clock (in one transaction). Throws WorldError.
    void save_clock(const WorldClock& clock) const;

private:
    std::filesystem::path folder_;
    WorldInfo info_;
};

}  // namespace midgard
