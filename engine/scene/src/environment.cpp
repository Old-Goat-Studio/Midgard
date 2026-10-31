// SPDX-License-Identifier: Apache-2.0
#include "midgard/environment.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace midgard {

const std::vector<CurveName>& environment_curve_names() {
    static const std::vector<CurveName> names = {
        // The sky (render::SkyColors): colours, and the brightness of each part.
        {"zenith", 3},
        {"horizon", 3},
        {"horizon_away", 3},
        {"ground", 3},
        {"sky_brightness", 1},
        {"glow", 3},
        {"glow_brightness", 1},
        {"sun_disc", 3},
        {"sun_disc_brightness", 1},
        // The light.
        {"sunlight", 3},
        {"sunlight_intensity", 1},
        {"ambient", 3},
        {"ambient_ground", 3},
        // The fog.
        {"fog_density", 1},
        {"fog_color", 3},
    };
    return names;
}

std::array<float, 3> Curve::at(double phase) const {
    if (keys.empty()) return {0.0f, 0.0f, 0.0f};
    if (keys.size() == 1) return keys.front().second;
    phase -= std::floor(phase);  // [0, 1)
    const size_t n = keys.size();
    size_t i = n - 1;
    for (size_t k = 0; k < n; ++k)
        if (keys[k].first <= phase) i = k;
    const double start = keys[i].first;
    if (phase < start) phase += 1.0;  // before the first key: from the last, across midnight
    const size_t next = (i + 1) % n;
    const double end = keys[next].first + (next <= i ? 1.0 : 0.0);
    double t = (phase - start) / (end - start);
    t = t * t * (3.0 - 2.0 * t);
    std::array<float, 3> out{};
    for (int c = 0; c < 3; ++c) {
        const double a = keys[i].second[size_t(c)], b = keys[next].second[size_t(c)];
        out[size_t(c)] = float(a + (b - a) * t);
    }
    return out;
}

const Curve* EnvironmentDef::curve(const std::string& curve_name) const {
    const auto it = curves.find(curve_name);
    return it != curves.end() && it->second.set() ? &it->second : nullptr;
}

EnvironmentDef environment_from_json(const std::string& id, const std::string& name,
                                     const std::string& curves_json,
                                     const std::string& settings_json) {
    using json = nlohmann::json;
    EnvironmentDef env;
    env.id = id;
    env.name = name;
    const json curves = json::parse(curves_json.empty() ? "{}" : curves_json);
    const json settings = json::parse(settings_json.empty() ? "{}" : settings_json);
    if (!curves.is_object() || !settings.is_object())
        throw std::runtime_error("environment '" + id + "': curves and settings are objects");
    for (const auto& [key, keys] : curves.items()) {
        Curve c;
        for (const CurveName& n : environment_curve_names())
            if (key == n.name) c.width = n.width;
        if (!keys.is_array())
            throw std::runtime_error("environment '" + id + "': curve '" + key + "' is a list");
        for (const json& k : keys) {
            if (!k.is_array() || k.size() != 2 || !k[0].is_number())
                throw std::runtime_error("environment '" + id + "': curve '" + key +
                                         "' has a key that isn't [phase, value]");
            std::array<float, 3> v{};
            if (k[1].is_number()) {
                v = {k[1].get<float>(), k[1].get<float>(), k[1].get<float>()};
            } else if (k[1].is_array() && k[1].size() == 3) {
                for (size_t i = 0; i < 3; ++i) v[i] = k[1][i].get<float>();
            } else {
                throw std::runtime_error("environment '" + id + "': curve '" + key +
                                         "' has a value that isn't a number or a colour");
            }
            c.keys.emplace_back(k[0].get<double>(), v);
        }
        env.curves[key] = std::move(c);
    }
    env.sun_size = settings.value("sun_size", env.sun_size);
    env.glow_tightness = settings.value("glow_tightness", env.glow_tightness);
    env.fog_height_falloff = settings.value("fog_height_falloff", env.fog_height_falloff);
    env.fog_sun_scatter = settings.value("fog_sun_scatter", env.fog_sun_scatter);
    return env;
}

std::vector<std::string> environment_problems(const EnvironmentDef& env) {
    std::vector<std::string> out;
    const std::string who = "environment '" + env.id + "'";
    for (const auto& [name, curve] : env.curves) {
        bool known = false;
        for (const CurveName& n : environment_curve_names()) known = known || name == n.name;
        if (!known) {
            out.push_back(who + " has a curve '" + name + "' that nothing uses");
            continue;
        }
        double last = -1.0;
        for (const auto& [phase, value] : curve.keys) {
            if (!(phase >= 0.0 && phase < 1.0))
                out.push_back(who + ": curve '" + name + "' has a key outside the day (0 to 1)");
            else if (phase <= last)
                out.push_back(who + ": curve '" + name + "' has keys out of order");
            last = phase;
            for (const float v : value)
                if (!std::isfinite(v) || v < 0.0f)
                    out.push_back(who + ": curve '" + name + "' has a negative value");
        }
    }
    if (env.sun_size <= 0.0f || env.sun_size > 45.0f)
        out.push_back(who + ": the sun's size is between 0 and 45 degrees");
    if (env.glow_tightness < 0.0f || env.glow_tightness >= 1.0f)
        out.push_back(who + ": the glow's tightness is from 0 up to (not including) 1");
    if (env.fog_height_falloff < 0.0f) out.push_back(who + ": fog can't thicken upwards");
    return out;
}

EnvironmentLook evaluate(const EnvironmentDef& env, double phase, float sun_elevation) {
    const render::AnalyticSky analytic = render::analytic_sky(sun_elevation);
    EnvironmentLook look;
    look.sky = analytic.colors;
    look.sunlight = analytic.sunlight;
    look.sunlight_intensity = analytic.sunlight_intensity;
    const auto colour = [&](const char* name, glm::vec3& into) {
        if (const Curve* c = env.curve(name)) {
            const auto v = c->at(phase);
            into = {v[0], v[1], v[2]};
        }
    };
    const auto number = [&](const char* name, float& into) {
        if (const Curve* c = env.curve(name)) into = c->at(phase)[0];
    };
    colour("zenith", look.sky.zenith);
    colour("horizon", look.sky.horizon);
    colour("horizon_away", look.sky.horizon_away);
    colour("ground", look.sky.ground);
    number("sky_brightness", look.sky.brightness);
    colour("glow", look.sky.glow);
    number("glow_brightness", look.sky.glow_brightness);
    colour("sun_disc", look.sky.sun);
    number("sun_disc_brightness", look.sky.sun_brightness);
    look.sky.sun_size = env.sun_size;
    look.sky.glow_tightness = env.glow_tightness;
    colour("sunlight", look.sunlight);
    number("sunlight_intensity", look.sunlight_intensity);
    if (env.curve("ambient")) {
        glm::vec3 a(0.0f);
        colour("ambient", a);
        look.ambient = a;
    }
    if (env.curve("ambient_ground")) {
        glm::vec3 a(0.0f);
        colour("ambient_ground", a);
        look.ambient_ground = a;
    }
    number("fog_density", look.fog_density);
    if (env.curve("fog_color")) {
        glm::vec3 c(0.0f);
        colour("fog_color", c);
        look.fog_color = c;
    }
    return look;
}

}  // namespace midgard
