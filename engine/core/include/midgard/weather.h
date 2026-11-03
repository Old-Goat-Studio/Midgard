// SPDX-License-Identifier: Apache-2.0
// Weather types (milestone 5.15): kinds of weather, and what each does.
//
// A weather type (the weather_type table, migration 18) says how the weather looks (cloud cover,
// how much daylight it takes, the light's tint, how far you can see, what falls from the sky and
// how much, lightning), how it feels (wind and gusts, how wet uncovered things get, how much
// warmer or colder it is) and how it sounds (sound events that loop while it lasts, and the
// thunder after each flash of lightning). A project starts with ten (clear, cloudy, rain,
// thunderstorm, snow, blizzard, fog, windy, sandstorm, ash fall) and the designer changes them
// or adds their own. Later steps choose the weather from these by season and biome.
#pragma once

#include <array>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace midgard::weather {

class WeatherError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// What falls from the sky.
enum class Falls { none, rain, snow, ash, sand };
const char* falls_name(Falls f);
std::optional<Falls> falls_from_name(std::string_view name);

struct WeatherType {
    bool operator==(const WeatherType&) const = default;
    std::string id, name;

    // How it looks.
    float clouds = 0.0f;                          // 0 a clear sky .. 1 overcast
    float darkness = 0.0f;                        // the daylight it takes: 0 none .. 1 nearly all
    std::array<float, 3> tint{1.0f, 1.0f, 1.0f};  // the light's colour, multiplied
    float visibility = 0.0f;  // metres you can see through its fog, dust or snow; 0: no limit
    Falls falls = Falls::none;
    float amount = 0.0f;     // how much falls: 0 .. 1 (a downpour)
    float lightning = 0.0f;  // flashes a minute

    // How it feels.
    float wind = 0.0f;         // metres a second
    float gusts = 0.0f;        // 0 steady .. 1 very gusty
    float wetness = 0.0f;      // how wet uncovered things get: 0 (they dry) .. 1 (soaked)
    float temperature = 0.0f;  // degrees Celsius added to the climate's

    // How it sounds.
    struct Sound {
        std::string event;  // a sound event, looping while the weather lasts
        float volume = 1.0f;
        bool operator==(const Sound&) const = default;
    };
    std::vector<Sound> sounds;
    std::string thunder;  // a sound event after each flash; empty: none
};

// A weather type from its row (weather_type: id, name, weather JSON). Numbers out of range are
// brought into it; WeatherError if it isn't a weather type (not an object, a value of the
// wrong kind, something falling that isn't none, rain, snow, ash or sand).
WeatherType weather_type_from_json(const std::string& id, const std::string& name,
                                   std::string_view json);
// Its JSON, as the table holds it (only what differs from a clear, still sky).
std::string weather_type_to_json(const WeatherType& type);

// Weather types by id.
using WeatherTypes = std::map<std::string, WeatherType>;

}  // namespace midgard::weather
