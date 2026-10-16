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

struct Environment {
    bool sky = false;  // false: the frame's clear colour is the whole sky, flat
    SkyColors sky_colors;
    Fog fog;
};

}  // namespace midgard::render
