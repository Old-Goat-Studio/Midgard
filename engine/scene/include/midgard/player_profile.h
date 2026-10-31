// SPDX-License-Identifier: Apache-2.0
// The player's own settings, kept between sessions (roadmap §8, Cameras: "the player's last view
// is saved in their profile"). For now a small JSON file with the camera's view; Phase 6's
// profile system (profiles/{profile}/ in the user's data folder) takes it over.
//
//   {"camera": {"view": "first", "third_distance": 6.0, "top_distance": 18.0}}
//
// Only the view and its zoom are kept, not which way it faces: that belongs to wherever the
// player stands when the game starts. Unknown keys are kept for the profile system to come.
#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "midgard/scene.h"

namespace midgard {

class ProfileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct PlayerProfile {
    std::optional<CameraView> view;
    std::optional<float> third_distance, top_distance;
    nlohmann::json other = nlohmann::json::object();  // keys this engine doesn't know

    // A missing file is an empty profile. Throws ProfileError if it can't be read or isn't a
    // profile (a damaged file is then replaced at the next save).
    static PlayerProfile load(const std::filesystem::path& file);
    // Writes the file (and its folder) in one step, so a crash never leaves half a profile.
    // Throws ProfileError.
    void save(const std::filesystem::path& file) const;
};

}  // namespace midgard
