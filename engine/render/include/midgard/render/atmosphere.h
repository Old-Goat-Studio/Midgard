// SPDX-License-Identifier: Apache-2.0
// The sky and fog. The sky is drawn after the opaque objects, wherever none was drawn: a gradient
// from the horizon (warmer on the sun's side) to the zenith, a ground colour below the horizon,
// a glow around the sun and its disc. Fog thickens with distance and thins with height along the
// local "up", so it lies in valleys on any world shape.
//
// Everything here is a plain set of colours and numbers: the Environment editor (milestone 5.7)
// will drive them with curves over the day; until then analytic_sky() computes the sky's colours
// from the sun's height.
#pragma once

#include <glm/glm.hpp>

namespace midgard::render {

// The sky's colours, sRGB as picked; each part has its own brightness (the sky is HDR: the sun is
// far brighter than the sky around it).
struct SkyColors {
    glm::vec3 zenith{0.25f, 0.45f, 0.85f};
    glm::vec3 horizon{0.75f, 0.82f, 0.9f};       // on the sun's side
    glm::vec3 horizon_away{0.6f, 0.72f, 0.88f};  // opposite the sun
    glm::vec3 ground{0.3f, 0.32f, 0.34f};        // below the horizon, beyond the world's own ground
    float brightness = 1.0f;                     // of those four
    glm::vec3 glow{1.0f, 0.9f, 0.7f};            // around the sun
    float glow_brightness = 0.3f;
    float glow_tightness = 0.8f;       // 0 spreads the glow over the whole sky, 0.95 hugs the sun
    glm::vec3 sun{1.0f, 0.97f, 0.9f};  // the disc
    float sun_brightness = 50.0f;
    float sun_size = 1.0f;  // degrees across (the real sun is about half a degree)
};

// A clear sky with the sun this far above the horizon (radians; negative below it), from single
// scattering in an Earth-like atmosphere (Rayleigh and Mie, after Nishita et al. 1993) worked out
// for a few directions. Also the sunlight that gets through, for render::Light.
struct AnalyticSky {
    SkyColors colors;
    glm::vec3 sunlight{1.0f};         // sRGB, for Light::color
    float sunlight_intensity = 1.0f;  // for Light::intensity: about 1 with the sun high, 0 below
};
AnalyticSky analytic_sky(float sun_elevation);

struct Fog {
    // Per metre at the fog's base height; 0 is no fog. At 0.01 only 5% of the light from 300 m
    // away gets through.
    float density = 0.0f;
    // Per metre: how fast the fog thins upwards (0: the same at every height).
    float height_falloff = 0.0f;
    // The camera's height above the fog's base, along "up" (Light::up).
    float camera_height = 0.0f;
    glm::vec3 color{0.7f, 0.75f, 0.8f};  // sRGB
    bool color_from_sky = true;          // use the sky's horizon instead (right at any time of day)
    float sun_scatter = 0.5f;            // how much brighter fog is towards the sun
    float sky_distance = 1000.0f;        // metres the sky counts as, for fog in front of it
};

// How much fog lies between the camera and a point `offset` from it (camera-relative), as an
// optical depth: exp(-depth) of the light gets through. The shaders compute the same.
float fog_depth(const Fog& fog, const glm::vec3& offset, const glm::vec3& up);

// How water looks (DrawItem::water surfaces): its colour over shallows and over deep water,
// how far down the ground shows through, and its waves.
struct Water {
    glm::vec3 shallow{0.10f, 0.42f, 0.40f};  // sRGB
    glm::vec3 deep{0.02f, 0.10f, 0.16f};     // sRGB
    float clarity = 3.0f;                    // metres: the ground fades to 1/e this deep
    float wave_strength = 0.12f;             // how much the waves tilt the surface
    float wave_size = 8.0f;                  // metres: the longest waves
    float time = 0.0f;                       // seconds: where the waves are
};

// A layer of cloud across the sky (the weather's; milestone 5.15): procedural, drifting with
// the wind, thinning towards the horizon. Its colour comes from the sky's own, so it is right at
// any time of day; it hides the sun where it is thick.
struct Clouds {
    float cover = 0.0f;     // 0 none .. 1 overcast
    float shade = 0.3f;     // 0 white .. 1 dark grey (storm clouds)
    glm::vec2 drift{0.0f};  // metres the clouds have moved (the wind times the time)
};

// What falls from the sky (the weather's; milestone 5.15): particles made on the GPU in a box
// round the camera, fixed in the world as it moves, carried by the wind. Rain falls as streaks;
// snow, ash and sand as small flakes. How many: `amount` times the quality tier's most.
enum class Falls { None, Rain, Snow, Ash, Sand };
struct Falling {
    Falls kind = Falls::None;
    float amount = 0.0f;       // 0 .. 1
    glm::vec3 velocity{0.0f};  // metres a second (the fall and the wind), in the world's axes
    glm::dvec3 moved{0.0};     // metres carried so far (velocity times time; any size)
    float time = 0.0f;         // seconds, for the flakes' drifting
};
// The box round the camera each falls in (metres across): how far it's seen.
float falling_box(Falls kind);

struct Environment {
    bool sky = false;  // false: the frame's clear colour is the whole sky, flat
    SkyColors sky_colors;
    Fog fog;
    Clouds clouds;
    Falling falling;
    // Radians: how far below level the visible horizon is, from a camera above a curved world
    // (a planet's ground falls away). The sky's horizon, and the sun setting behind it, are there.
    float horizon_dip = 0.0f;
    Water water;
};

}  // namespace midgard::render
