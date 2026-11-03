// SPDX-License-Identifier: Apache-2.0
// How cold or hot it feels (milestone 5.15's survival groundwork; tools/reference/survival.py is
// its Python twin, and schema/tests/feels_like.json holds both to the same numbers).
//
//   feels like = the air's temperature,
//                made colder by the wind (Environment Canada's wind chill, for wind over
//                4.8 km/h, in full at 10 degrees and below and fading out by 16; never warmer),
//                colder still for being wet (up to 4 degrees, and 0.3 more a metre a second of
//                wind: wet and windy is the worst),
//                and warmer by the heat of fires and stoves nearby.
//
// A heat source warms by `heat` degrees at its middle, less further out, and none from `radius`:
// heat * (1 - (d / radius)^2)^2.
#pragma once

namespace midgard::survival {

// The wind chill (degrees Celsius) for air at `air` degrees and wind at `wind` metres a second.
double wind_chill(double air, double wind);
// How warm it feels (degrees Celsius): the air's temperature, the wind where the person is
// (m/s), how wet they are (0 .. 1) and the heat of what warms them (degrees).
double feels_like(double air, double wind, double wetness, double heat);
// A heat source's warmth (degrees) `distance` metres from its middle.
double warmth(double heat, double radius, double distance);

}  // namespace midgard::survival
