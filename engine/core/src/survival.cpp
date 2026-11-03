// SPDX-License-Identifier: Apache-2.0
#include "midgard/survival.h"

#include <algorithm>
#include <cmath>

namespace midgard::survival {

double wind_chill(double air, double wind) {
    const double v = wind * 3.6;
    if (v <= 4.8) return air;
    const double p = std::pow(v, 0.16);
    return 13.12 + 0.6215 * air - 11.37 * p + 0.3965 * air * p;
}

double feels_like(double air, double wind, double wetness, double heat) {
    double t = air;
    const double chill = std::min(wind_chill(air, wind), air);
    const double k = std::clamp((16.0 - air) / 6.0, 0.0, 1.0);
    t = t + (chill - t) * k;
    const double wet = std::clamp(wetness, 0.0, 1.0);
    t = t - wet * (4.0 + 0.3 * std::max(wind, 0.0));
    return t + heat;
}

double warmth(double heat, double radius, double distance) {
    if (radius <= 0.0 || distance >= radius) return 0.0;
    const double f = 1.0 - (distance / radius) * (distance / radius);
    return heat * f * f;
}

}  // namespace midgard::survival
