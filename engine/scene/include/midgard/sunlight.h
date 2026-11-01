// SPDX-License-Identifier: Apache-2.0
// Light models per world shape (roadmap §6, "Day and night on each world shape"; milestone 5.8):
// where the sun is, seen from a point, and that point's own solar phase (0 sunrise, 0.25 noon,
// 0.5 sunset, 0.75 midnight).
//
// The clock gives the time of day at the scene's start (the world's origin) as a fraction of the
// day; the scene's sun, as placed, is where it stands there at solar noon.
//
//   bounded area, flat-wrap cylinder  one sun crossing the sky: the same time everywhere
//   sphere outside, Uniform           a distant sun and a turning planet: each place's time is
//                                     shifted by its longitude, every place has the calendar's
//                                     daylight share, and the sun is as high at noon as at the
//                                     start
//   sphere outside, Latitude          a tilted planet: the sun's declination follows the year,
//                                     day length follows latitude (polar day and night), and the
//                                     start's latitude is set by how high the sun was placed
//   sphere inside, curved cylinder    a central sun (or sun-line) with shades going round: each
//                                     place's time is shifted by its angle round the axis, and
//                                     the light is stylised (low from the side at dawn and dusk,
//                                     overhead at noon)
//
// Each place's season (for the environment's season tints) is the calendar's everywhere, except
// on a tilted planet in Latitude mode: there it follows the sun's declination, opposite in the
// two hemispheres, none at the equator and full from 45 degrees of latitude.
//
// tools/reference/light.py is the reference; schema/tests/light_models.json holds both to it.
#pragma once

#include <optional>
#include <string>

#include <glm/glm.hpp>

#include "midgard/body.h"

namespace midgard {

enum class SunMode { Uniform, Latitude };
const char* sun_mode_name(SunMode mode);  // "uniform", "latitude"
std::optional<SunMode> sun_mode_from_name(const std::string& name);

struct SunClock {
    double time = 0.5;      // the time of day at the scene's start, as a fraction of the day
    double daylight = 0.5;  // the day's daylight share (Uniform, and every other shape)
    double noon = 0.5;      // the middle of daylight, as a fraction of the day
    SunMode mode = SunMode::Uniform;
    double tilt = 0.0;    // the axial tilt, degrees (Latitude)
    double year = 0.0;    // the fraction of the year since the spring equinox (Latitude)
    double season = 0.0;  // how far into summer at the start: 1 summer, -1 winter (calendar.h)
};

struct LocalSun {
    double phase = 0.25;               // the solar phase here
    glm::dvec3 to_sun{0.0, 1.0, 0.0};  // towards the sun (unit)
    double height = 1.0;               // along up here: the sine of the sun's elevation
    double season = 0.0;               // how far into summer here: 1 summer, -1 winter
};

// The solar phase at a time of day (calendar.h's solar_phase, as a fraction of the day), and
// back.
double phase_of_time(double time, double daylight, double noon);
double time_of_phase(double phase, double daylight, double noon);

// The sun at point p. `placed` is the direction towards the sun as placed (unit); `body` is the
// world's shape (none: flat ground, +Y up).
LocalSun local_sun(const std::optional<Body>& body, const glm::dvec3& placed, const SunClock& clock,
                   const glm::dvec3& p);

}  // namespace midgard
