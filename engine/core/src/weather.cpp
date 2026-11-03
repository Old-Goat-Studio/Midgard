// SPDX-License-Identifier: Apache-2.0
#include "midgard/weather.h"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace midgard::weather {
namespace {

constexpr std::array<const char*, 5> kFalls{"none", "rain", "snow", "ash", "sand"};

float number(const nlohmann::json& j, const char* key, float fallback, float least, float most) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    if (!it->is_number()) throw WeatherError(std::string(key) + " is a number");
    const float v = it->get<float>();
    if (!std::isfinite(v)) throw WeatherError(std::string(key) + " is a number");
    return std::clamp(v, least, most);
}

std::string text(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return {};
    if (!it->is_string()) throw WeatherError(std::string(key) + " is a name");
    return it->get<std::string>();
}

// Written to four decimal places (a float's 0.6 is otherwise 0.6000000238418579).
double tidy(float v) {
    return std::round(double(v) * 10000.0) / 10000.0;
}

}  // namespace

const char* falls_name(Falls f) {
    return kFalls[size_t(f)];
}

std::optional<Falls> falls_from_name(std::string_view name) {
    for (size_t i = 0; i < kFalls.size(); ++i)
        if (name == kFalls[i]) return Falls(i);
    return std::nullopt;
}

WeatherType weather_type_from_json(const std::string& id, const std::string& name,
                                   std::string_view json) {
    WeatherType w;
    w.id = id;
    w.name = name;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json);
    } catch (const nlohmann::json::exception&) {
        throw WeatherError("weather type '" + name + "' isn't JSON");
    }
    try {
        if (!j.is_object()) throw WeatherError("it isn't an object");
        w.clouds = number(j, "clouds", 0.0f, 0.0f, 1.0f);
        w.darkness = number(j, "darkness", 0.0f, 0.0f, 1.0f);
        if (const auto t = j.find("tint"); t != j.end() && !t->is_null()) {
            if (!t->is_array() || t->size() != 3)
                throw WeatherError("tint is three numbers, [red, green, blue]");
            for (size_t i = 0; i < 3; ++i) {
                if (!(*t)[i].is_number())
                    throw WeatherError("tint is three numbers, [red, green, blue]");
                w.tint[i] = std::clamp((*t)[i].get<float>(), 0.0f, 2.0f);
            }
        }
        w.visibility = number(j, "visibility", 0.0f, 0.0f, 1.0e6f);
        const std::string falls = text(j, "falls");
        if (!falls.empty()) {
            const auto f = falls_from_name(falls);
            if (!f) throw WeatherError("what falls is none, rain, snow, ash or sand, not " + falls);
            w.falls = *f;
        }
        // Something falling and no amount given: a moderate amount.
        w.amount = w.falls == Falls::none ? 0.0f : number(j, "amount", 0.5f, 0.0f, 1.0f);
        w.lightning = number(j, "lightning", 0.0f, 0.0f, 60.0f);
        w.wind = number(j, "wind", 0.0f, 0.0f, 100.0f);
        w.gusts = number(j, "gusts", 0.0f, 0.0f, 1.0f);
        w.wetness = number(j, "wetness", 0.0f, 0.0f, 1.0f);
        w.temperature = number(j, "temperature", 0.0f, -100.0f, 100.0f);
        if (const auto s = j.find("sounds"); s != j.end() && !s->is_null()) {
            if (!s->is_array()) throw WeatherError("sounds is a list");
            for (const auto& v : *s) {
                WeatherType::Sound sound;
                if (v.is_string()) {
                    sound.event = v.get<std::string>();
                } else if (v.is_object()) {
                    sound.event = text(v, "event");
                    sound.volume = number(v, "volume", 1.0f, 0.0f, 4.0f);
                } else {
                    throw WeatherError("a sound is a sound event, or {event, volume}");
                }
                if (!sound.event.empty()) w.sounds.push_back(std::move(sound));
            }
        }
        w.thunder = text(j, "thunder");
    } catch (const WeatherError& e) {
        throw WeatherError("weather type '" + name + "': " + e.what());
    } catch (const nlohmann::json::exception& e) {
        throw WeatherError("weather type '" + name + "': " + e.what());
    }
    return w;
}

std::string weather_type_to_json(const WeatherType& w) {
    const WeatherType still;
    nlohmann::json j = nlohmann::json::object();
    auto put = [&](const char* key, float v, float fallback) {
        if (v != fallback) j[key] = tidy(v);
    };
    put("clouds", w.clouds, still.clouds);
    put("darkness", w.darkness, still.darkness);
    if (w.tint != still.tint) j["tint"] = {tidy(w.tint[0]), tidy(w.tint[1]), tidy(w.tint[2])};
    put("visibility", w.visibility, still.visibility);
    if (w.falls != Falls::none) {
        j["falls"] = falls_name(w.falls);
        j["amount"] = tidy(w.amount);
    }
    put("lightning", w.lightning, still.lightning);
    put("wind", w.wind, still.wind);
    put("gusts", w.gusts, still.gusts);
    put("wetness", w.wetness, still.wetness);
    put("temperature", w.temperature, still.temperature);
    if (!w.sounds.empty()) {
        nlohmann::json sounds = nlohmann::json::array();
        for (const auto& s : w.sounds)
            sounds.push_back({{"event", s.event}, {"volume", tidy(s.volume)}});
        j["sounds"] = std::move(sounds);
    }
    if (!w.thunder.empty()) j["thunder"] = w.thunder;
    return j.dump();
}

}  // namespace midgard::weather
