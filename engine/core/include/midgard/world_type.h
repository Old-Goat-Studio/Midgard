// SPDX-License-Identifier: Apache-2.0
// World types (roadmap §7, "World types"; milestone 5.11): recipes for worlds. A recipe says which
// shapes and sizes it suits, its gravity, atmosphere, calendar and wall style, and how its ground
// is made. A scene's terrain is a world of one, made from a world type's version and a seed
// (migration 12: the world_type and world_type_version tables):
//
//   recipe   {"shapes": ["sphere_outside", ...],   the shapes it suits (none listed: any)
//             "size": [min, max],                  metres: a bounded area's width, a flat
//                                                  cylinder's circumference, or the radius
//                                                  (none: any)
//             "gravity": 9.81,                     m/s^2 at the surface (none: the body's)
//             "atmosphere": "env_earth",           the environment it is lit and hazed with
//             "calendar": "cal_standard",          its days and seasons
//             "terrain": {...},                    the ground's settings (midgard/terrain.h,
//                                                  without a seed: the world brings that)
//             "graph": {...}}                      how the ground's height is made
//                                                  (midgard/worldgen.h); none: the terrain
//                                                  settings' hills, as in milestone 5.9
//   world    {"world_type": "wt_...", "version": 2, "seed": 7}    (no seed: the body's)
//
// A version a shipped game could use is never changed or dropped, so a world still generates
// the same after its world type changes: editing it saves a new version instead.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "midgard/body.h"
#include "midgard/terrain.h"

namespace midgard {

struct WorldRecipe {
    std::vector<Shape::Kind> shapes;  // none: any
    std::optional<double> size_min, size_max;
    std::optional<double> gravity;
    std::string atmosphere;  // an environment's id, or none
    std::string calendar;    // a calendar's id, or none
    TerrainSettings terrain;
    std::string graph;  // JSON (midgard/worldgen.h's WorldGraph), or empty for none
    bool operator==(const WorldRecipe&) const = default;
};

// Throws std::runtime_error naming what's wrong with JSON that can't be read.
WorldRecipe recipe_from_json(const nlohmann::json& j);
nlohmann::json recipe_to_json(const WorldRecipe& r);
// What the cook refuses in a recipe (its terrain's problems too).
std::vector<std::string> recipe_problems(const WorldRecipe& r);
// Why a world of this recipe can't be on this body (a shape or size it doesn't suit), if so.
std::vector<std::string> recipe_misfits(const WorldRecipe& r, const Body& body);
// The size a recipe's range is about: a bounded area's width, a flat cylinder's circumference,
// or the radius of the others.
double world_size(const Body& body);

struct WorldRef {
    std::string world_type;
    int version = 1;
    std::optional<std::uint64_t> seed;  // none: the body's
    bool operator==(const WorldRef&) const = default;
};

// Whether a scene's "terrain" names a world type (from migration 12) rather than holding the
// settings themselves (packages from before).
bool is_world_ref(const nlohmann::json& j);
WorldRef world_ref_from_json(const nlohmann::json& j);  // throws std::runtime_error
nlohmann::json world_ref_to_json(const WorldRef& r);

// The terrain a world is made with: its recipe's, with the world's seed.
TerrainSettings world_terrain(const WorldRecipe& recipe, const WorldRef& world);

// Every world type version in a game's database (none before migration 12), its recipe as
// stored; and what the cook refuses in them (recipes that can't be read or have problems).
struct WorldTypeVersion {
    std::string world_type;
    int version = 1;
    std::string recipe;  // JSON
    bool shipped = false;
};
std::vector<WorldTypeVersion> load_world_type_versions(const std::filesystem::path& database);
std::vector<std::string> world_type_problems(const std::vector<WorldTypeVersion>& versions);

}  // namespace midgard
