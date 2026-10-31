// SPDX-License-Identifier: Apache-2.0
// Environments (roadmap §6, "From clock time to light"; milestone 5.8): how a world looks through
// its day. The sky's colours, the sunlight's colour and strength, the ambient light and the fog,
// each a curve over the solar phase (0 sunrise, 0.25 noon, 0.5 sunset, 0.75 midnight), so a
// different calendar (a 20-hour day, or 70% daylight) never breaks a look already set up. Curves
// that aren't set come from the analytic sky (render/atmosphere.h, an Earth-like atmosphere) at
// the sun's height, so an empty environment already gives a whole day. The scene's Sky entity
// links to one. The editor's Environment editor edits them (through _midgard, so its preview and
// the game agree); tools/reference/environment.py is the reference for the curves.
#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "midgard/render/atmosphere.h"

namespace midgard {

// Keys [phase, value] in increasing phase in [0, 1); a value has 1 number or 3 (a colour).
struct Curve {
    std::vector<std::pair<double, std::array<float, 3>>> keys;
    int width = 1;  // 1 or 3

    bool set() const { return !keys.empty(); }
    // Eases (smoothstep) between keys, round the day: after the last key back to the first.
    std::array<float, 3> at(double phase) const;
};

// The curves an environment may set, and the width of each.
struct CurveName {
    const char* name;
    int width;
};
const std::vector<CurveName>& environment_curve_names();

struct EnvironmentDef {
    std::string id, name;
    std::map<std::string, Curve> curves;  // by name, only those set
    // Not over the day:
    float sun_size = 1.0f;            // the sun's disc, degrees across
    float glow_tightness = 0.8f;      // 0 spreads its glow over the sky, 0.95 hugs the sun
    float fog_height_falloff = 0.0f;  // per metre: how fast fog thins upwards
    float fog_sun_scatter = 0.5f;     // how much brighter fog is towards the sun

    const Curve* curve(const std::string& curve_name) const;
};

// From the environment table's columns. Throws std::runtime_error naming what's wrong with JSON
// that can't be read; problems() lists what the cook refuses.
EnvironmentDef environment_from_json(const std::string& id, const std::string& name,
                                     const std::string& curves_json,
                                     const std::string& settings_json);
std::vector<std::string> environment_problems(const EnvironmentDef& environment);

// What an environment gives at a moment.
struct EnvironmentLook {
    render::SkyColors sky;
    glm::vec3 sunlight{1.0f};                          // sRGB, times the sun light's own colour
    float sunlight_intensity = 1.0f;                   // times the sun light's own intensity
    std::optional<glm::vec3> ambient, ambient_ground;  // none: the scene's own (dimmed at night)
    float fog_density = 0.0f;
    std::optional<glm::vec3> fog_color;  // none: the sky's horizon
};
// At this solar phase, with the sun this high where the camera is (radians; for the curves not
// set, from the analytic sky).
EnvironmentLook evaluate(const EnvironmentDef& environment, double phase, float sun_elevation);

}  // namespace midgard
