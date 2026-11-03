// SPDX-License-Identifier: Apache-2.0
// How the weather looks (milestone 5.15): its clouds, the daylight it takes, the colour of its
// light, its fog and its lightning, put on the sky and light of the time of day.
//
// A look is a weather type's (look_of), and the game eases from one to the next as the weather
// changes (blend), so a storm rolls in rather than snapping on. apply() puts it on what the
// environment gives for the hour: clouds across the sky, hiding the sun; the sky greyed and
// darkened; the sunlight and the sky's light dimmed and tinted; fog closing in to the weather's
// visibility (all the way up: rain and snow aren't only in the valleys); and a lightning flash
// lighting everything for a moment.
#pragma once

#include <glm/glm.hpp>

#include "midgard/render/atmosphere.h"
#include "midgard/render/renderer.h"
#include "midgard/weather.h"

namespace midgard {

struct WeatherLook {
    float clouds = 0.0f;    // 0 .. 1
    float darkness = 0.0f;  // 0 .. 1
    glm::vec3 tint{1.0f};
    float fog = 0.0f;       // per metre (3 / visibility: 5% of the light gets through that far)
    float shade = 0.3f;     // the clouds' greyness
    float flash = 0.0f;     // a lightning flash now, 0 .. 1
    glm::vec2 drift{0.0f};  // metres the clouds have moved
};

// A weather type's look (none: a clear sky); the flash and drift are the game's.
WeatherLook look_of(const weather::WeatherType* type);
// Part of the way from one look to another (t 0: from, 1: to); the flash and drift are `to`'s.
WeatherLook blend(const WeatherLook& from, const WeatherLook& to, float t);
// The look on the time of day's sky and light.
void apply(const WeatherLook& look, render::Environment& env, render::Light& light);

}  // namespace midgard
