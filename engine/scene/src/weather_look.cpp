// SPDX-License-Identifier: Apache-2.0
#include "midgard/weather_look.h"

#include <algorithm>

namespace midgard {
namespace {

float luminance(const glm::vec3& c) {
    return glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

}  // namespace

WeatherLook look_of(const weather::WeatherType* type) {
    WeatherLook look;
    if (!type) return look;
    look.clouds = type->clouds;
    look.darkness = type->darkness;
    look.tint = glm::vec3(type->tint[0], type->tint[1], type->tint[2]);
    look.fog = type->visibility > 0.0f ? 3.0f / std::max(type->visibility, 1.0f) : 0.0f;
    look.shade = std::clamp(0.25f + 0.75f * type->darkness, 0.0f, 1.0f);
    return look;
}

WeatherLook blend(const WeatherLook& from, const WeatherLook& to, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    WeatherLook out = to;
    out.clouds = glm::mix(from.clouds, to.clouds, t);
    out.darkness = glm::mix(from.darkness, to.darkness, t);
    out.tint = glm::mix(from.tint, to.tint, t);
    out.fog = glm::mix(from.fog, to.fog, t);
    out.shade = glm::mix(from.shade, to.shade, t);
    return out;
}

void apply(const WeatherLook& look, render::Environment& env, render::Light& light) {
    const float clouds = std::clamp(look.clouds, 0.0f, 1.0f);
    const float dark = std::clamp(look.darkness, 0.0f, 1.0f);
    const float flash = std::clamp(look.flash, 0.0f, 1.0f);

    // The sky: greyed by the clouds, darkened, tinted; the sun's glow and disc behind them.
    render::SkyColors& sky = env.sky_colors;
    const float grey = luminance(sky.horizon);
    const auto overcast = [&](glm::vec3& c, float strength) {
        c = glm::mix(c, glm::vec3(grey), clouds * strength) * look.tint;
    };
    overcast(sky.zenith, 0.85f);
    overcast(sky.horizon, 0.7f);
    overcast(sky.horizon_away, 0.75f);
    overcast(sky.ground, 0.5f);
    sky.brightness *= (1.0f - 0.75f * dark) * (1.0f + 3.0f * flash);
    sky.glow_brightness *= (1.0f - clouds) * (1.0f - clouds);
    sky.sun_brightness *= (1.0f - clouds) * (1.0f - clouds) * (1.0f - clouds);
    env.clouds.cover = clouds;
    env.clouds.shade = look.shade * (1.0f - flash);
    env.clouds.drift = look.drift;

    // Fog closing in to the weather's visibility, at every height.
    if (look.fog > 0.0f) {
        render::Fog& fog = env.fog;
        const float before = std::max(fog.density, 0.0f);
        fog.density = before + look.fog;
        fog.height_falloff *= before / fog.density;
        if (!fog.color_from_sky) fog.color = glm::mix(fog.color, glm::vec3(grey), clouds);
    }

    // The light: the sun dimmed by the darkness and hidden by thick cloud, the sky's light a
    // little less; both tinted. A flash lights everything from above for a moment.
    light.intensity *= (1.0f - dark) * (1.0f - 0.5f * clouds * clouds);
    light.color *= look.tint;
    light.ambient *= (1.0f - 0.6f * dark) * look.tint;
    light.ambient_ground *= (1.0f - 0.6f * dark) * look.tint;
    light.ambient += glm::vec3(0.85f, 0.9f, 1.0f) * (2.5f * flash);
    light.ambient_ground += glm::vec3(0.85f, 0.9f, 1.0f) * (1.2f * flash);
}

}  // namespace midgard
