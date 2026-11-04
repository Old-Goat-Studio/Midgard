// SPDX-License-Identifier: Apache-2.0
#include "midgard/texture_gen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

#include <nlohmann/json.hpp>

namespace midgard {
namespace {

constexpr double kPi = 3.14159265358979323846;

// A random number in 0 .. 1 from integer coordinates and the seed (integer maths only, so the
// same on every machine).
double hash(int64_t x, int64_t y, uint32_t seed) {
    uint64_t h = uint64_t(x) * 0x9E3779B97F4A7C15ull ^
                 (uint64_t(y) + 0x632BE59BD9B4E019ull) * 0xC2B2AE3D27D4EB4Full;
    h ^= uint64_t(seed) * 0x165667B19E3779F9ull;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return double(h >> 11) * (1.0 / 9007199254740992.0);
}

int64_t wrap(int64_t i, int64_t period) {
    if (period <= 0) return i;
    const int64_t r = i % period;
    return r < 0 ? r + period : r;
}

// Smooth value noise in 0 .. 1 at p, repeating every `period` lattice cells (0: never).
double value_noise(double x, double y, int64_t period, uint32_t seed) {
    const double fx = std::floor(x), fy = std::floor(y);
    const int64_t ix = int64_t(fx), iy = int64_t(fy);
    const double tx = x - fx, ty = y - fy;
    const double sx = tx * tx * (3.0 - 2.0 * tx), sy = ty * ty * (3.0 - 2.0 * ty);
    const auto at = [&](int64_t dx, int64_t dy) {
        return hash(wrap(ix + dx, period), wrap(iy + dy, period), seed);
    };
    const double a = at(0, 0), b = at(1, 0), c = at(0, 1), d = at(1, 1);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

// The recipe's "scale" as a whole number of cells (for tiling), or 0 if it isn't one.
int64_t period_of(double scale) {
    const double r = std::round(scale);
    return std::abs(scale - r) < 1e-9 && r >= 1.0 ? int64_t(r) : 0;
}

double fbm(const TextureRecipe& r, double u, double v, uint32_t seed, bool ridged) {
    double sum = 0.0, amplitude = 1.0, total = 0.0, frequency = r.scale;
    int64_t period = period_of(r.scale);
    for (int k = 0; k < std::max(r.octaves, 1); ++k) {
        double n = value_noise(u * frequency, v * frequency, period, seed + uint32_t(k) * 101u);
        if (ridged) n = 1.0 - std::abs(2.0 * n - 1.0);
        sum += amplitude * n;
        total += amplitude;
        amplitude *= r.roughness;
        frequency *= 2.0;
        period *= 2;
    }
    return total > 0.0 ? sum / total : 0.0;
}

// Worley noise: how near the nearest cell's point is (1 at it), cells repeating.
double cells(const TextureRecipe& r, double u, double v) {
    const double x = u * r.scale, y = v * r.scale;
    const int64_t period = period_of(r.scale);
    const int64_t ix = int64_t(std::floor(x)), iy = int64_t(std::floor(y));
    double nearest = 1e9;
    for (int64_t dy = -1; dy <= 1; ++dy)
        for (int64_t dx = -1; dx <= 1; ++dx) {
            const int64_t cx = ix + dx, cy = iy + dy;
            const int64_t wx = wrap(cx, period), wy = wrap(cy, period);
            const double px = double(cx) + 0.5 + (hash(wx, wy, r.seed) - 0.5) * r.jitter;
            const double py = double(cy) + 0.5 + (hash(wx, wy, r.seed + 7u) - 0.5) * r.jitter;
            nearest = std::min(nearest, std::hypot(x - px, y - py));
        }
    return std::clamp(1.0 - nearest, 0.0, 1.0);
}

// A soft step from 0 to 1 as t passes `edge` (over `soft` either side).
double soft_step(double edge, double soft, double t) {
    if (soft <= 0.0) return t < edge ? 0.0 : 1.0;
    const double s = std::clamp((t - edge + soft) / (2.0 * soft), 0.0, 1.0);
    return s * s * (3.0 - 2.0 * s);
}

// How far into a band (0 at its edge, 0.5 in its middle) a repeating coordinate is.
double band(double t) {
    const double f = t - std::floor(t);
    return std::min(f, 1.0 - f);
}

}  // namespace

const std::vector<std::string>& texture_patterns() {
    static const std::vector<std::string> patterns = {"noise",  "ridged", "cells",   "gradient",
                                                      "radial", "rings",  "checker", "stripes",
                                                      "bricks", "waves"};
    return patterns;
}

TextureRecipe texture_recipe_from_json(const std::string& text,
                                       std::vector<std::string>* problems) {
    TextureRecipe r;
    const auto problem = [&](const std::string& p) {
        if (problems) problems->push_back(p);
    };
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        problem(std::string("the recipe isn't JSON: ") + e.what());
        return r;
    }
    if (!j.is_object()) {
        problem("the recipe isn't a JSON object");
        return r;
    }
    const auto number = [&](const char* key, double fallback, double low, double high) {
        const auto it = j.find(key);
        if (it == j.end()) return fallback;
        if (!it->is_number()) {
            problem(std::string("'") + key + "' isn't a number");
            return fallback;
        }
        const double v = it->get<double>();
        if (v < low || v > high) {
            problem(std::string("'") + key + "' is outside " + nlohmann::json(low).dump() + " .. " +
                    nlohmann::json(high).dump());
            return std::clamp(v, low, high);
        }
        return v;
    };
    r.pattern = j.value("pattern", r.pattern);
    const auto& known = texture_patterns();
    if (std::find(known.begin(), known.end(), r.pattern) == known.end()) {
        problem("'" + r.pattern + "' isn't a pattern");
        r.pattern = "noise";
    }
    r.size = int(number("size", r.size, 16, 4096));
    r.scale = number("scale", r.scale, 0.1, 256);
    r.octaves = int(number("octaves", r.octaves, 1, 10));
    r.roughness = number("roughness", r.roughness, 0, 1);
    r.seed = uint32_t(number("seed", r.seed, 0, 4294967295.0));
    r.warp = number("warp", r.warp, 0, 4);
    r.angle = number("angle", r.angle, -360, 360);
    r.width = number("width", r.width, 0, 1);
    r.softness = number("softness", r.softness, 0, 0.5);
    r.jitter = number("jitter", r.jitter, 0, 1);
    r.variation = number("variation", r.variation, 0, 1);
    r.strength = number("strength", r.strength, 0, 20);
    r.invert = j.value("invert", false);
    r.output = j.value("output", r.output);
    if (r.output != "color" && r.output != "normal") {
        problem("'" + r.output + "' isn't an output (color or normal)");
        r.output = "color";
    }
    if (const auto c = j.find("colors"); c != j.end()) {
        std::vector<std::pair<double, std::array<double, 3>>> stops;
        bool ok = c->is_array() && !c->empty();
        for (const auto& stop : ok ? *c : nlohmann::json::array()) {
            if (!stop.is_array() || stop.size() != 2 || !stop[0].is_number() ||
                !stop[1].is_array() || stop[1].size() < 3) {
                ok = false;
                break;
            }
            std::array<double, 3> rgb{};
            for (size_t k = 0; k < 3; ++k)
                rgb[k] =
                    stop[1][k].is_number() ? std::clamp(stop[1][k].get<double>(), 0.0, 1.0) : 0.0;
            stops.emplace_back(std::clamp(stop[0].get<double>(), 0.0, 1.0), rgb);
        }
        if (ok) {
            std::stable_sort(stops.begin(), stops.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            r.colors = std::move(stops);
        } else {
            problem("'colors' is a list of [position, [r, g, b]]");
        }
    }
    return r;
}

std::string texture_recipe_to_json(const TextureRecipe& r) {
    nlohmann::json colors = nlohmann::json::array();
    for (const auto& [at, rgb] : r.colors) colors.push_back({at, {rgb[0], rgb[1], rgb[2]}});
    return nlohmann::json{
        {"pattern", r.pattern},   {"size", r.size},           {"scale", r.scale},
        {"octaves", r.octaves},   {"roughness", r.roughness}, {"seed", r.seed},
        {"warp", r.warp},         {"angle", r.angle},         {"width", r.width},
        {"softness", r.softness}, {"jitter", r.jitter},       {"variation", r.variation},
        {"invert", r.invert},     {"colors", colors},         {"output", r.output},
        {"strength", r.strength}}
        .dump(2);
}

double texture_height(const TextureRecipe& r, double u, double v) {
    if (r.warp > 0.0) {  // bent by noise (its own, repeating as the pattern does)
        TextureRecipe w = r;
        w.octaves = 3;
        w.roughness = 0.5;
        const double du = fbm(w, u, v, r.seed + 1000u, false) - 0.5;
        const double dv = fbm(w, u, v, r.seed + 2000u, false) - 0.5;
        u += du * r.warp / std::max(r.scale, 1.0);
        v += dv * r.warp / std::max(r.scale, 1.0);
    }
    const double a = r.angle * kPi / 180.0;
    const double along = (u - 0.5) * std::cos(a) + (v - 0.5) * std::sin(a);
    // Stripes and waves repeat a whole number of times across and down (the angle nudged to
    // the nearest that does), so they tile at any angle.
    double kx = std::round(r.scale * std::cos(a)), ky = std::round(r.scale * std::sin(a));
    if (kx == 0.0 && ky == 0.0) kx = 1.0;
    const double repeats = u * kx + v * ky;
    const std::string& p = r.pattern;
    if (p == "noise") return fbm(r, u, v, r.seed, false);
    if (p == "ridged") return fbm(r, u, v, r.seed, true);
    if (p == "cells") return cells(r, u, v);
    if (p == "gradient") return std::clamp(along + 0.5, 0.0, 1.0);
    if (p == "radial") return std::clamp(1.0 - 2.0 * std::hypot(u - 0.5, v - 0.5), 0.0, 1.0);
    if (p == "rings") {
        const double t = std::hypot(u - 0.5, v - 0.5) * r.scale * 2.0;
        return 0.5 + 0.5 * std::cos(2.0 * kPi * t);
    }
    if (p == "checker") {
        const double x = u * r.scale, y = v * r.scale;
        const bool odd = (int64_t(std::floor(x)) + int64_t(std::floor(y))) % 2 != 0;
        const double edge = std::min(band(x), band(y));  // (0 at a square's edge)
        const double soft = soft_step(0.0, r.softness, edge);
        return odd ? 0.5 + 0.5 * soft : 0.5 - 0.5 * soft;
    }
    if (p == "stripes") {
        const double t = repeats;
        const double f = t - std::floor(t);
        // In the band while f < width; soft at both edges.
        const double in =
            std::min(soft_step(0.0, r.softness, f), 1.0 - soft_step(r.width, r.softness, f));
        return r.width >= 1.0 ? 1.0 : std::clamp(in, 0.0, 1.0);
    }
    if (p == "bricks") {
        // "scale" bricks across, twice as many rows (bricks twice as wide as tall).
        const double x = u * r.scale, y = v * r.scale * 2.0;
        const int64_t row = int64_t(std::floor(y));
        const double bx = x + (row % 2 != 0 ? 0.5 : 0.0);
        const int64_t col = int64_t(std::floor(bx));
        const double mortar = r.width * 0.2;
        const double edge = std::min(band(bx) * 2.0, band(y));  // (bricks are 2:1)
        const double brick = soft_step(mortar, r.softness * 0.5, edge);
        const int64_t period = period_of(r.scale);
        const double shade =
            1.0 - r.variation * hash(wrap(col, period), wrap(row, period * 2), r.seed);
        return brick * shade;
    }
    if (p == "waves") return 0.5 + 0.5 * std::sin(2.0 * kPi * repeats);
    return 0.0;
}

std::vector<uint8_t> generate_texture(const TextureRecipe& r) {
    const int n = std::clamp(r.size, 16, 4096);
    std::vector<double> heights(size_t(n) * size_t(n));
    // Rows on every core (each pixel stands alone).
    const unsigned threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t)
        workers.emplace_back([&, t] {
            for (int y = int(t); y < n; y += int(threads))
                for (int x = 0; x < n; ++x) {
                    double h = texture_height(r, (x + 0.5) / n, (y + 0.5) / n);
                    h = std::clamp(h, 0.0, 1.0);
                    heights[size_t(y) * size_t(n) + size_t(x)] = r.invert ? 1.0 - h : h;
                }
        });
    for (std::thread& w : workers) w.join();

    std::vector<uint8_t> out(size_t(n) * size_t(n) * 4);
    const auto byte = [](double c) { return uint8_t(std::lround(std::clamp(c, 0.0, 1.0) * 255)); };
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const size_t i = size_t(y) * size_t(n) + size_t(x);
            std::array<double, 3> c{};
            if (r.output == "normal") {
                // From the heights' slopes (wrapping round, so a tiling pattern's map tiles too).
                const auto h = [&](int px, int py) {
                    return heights[size_t((py + n) % n) * size_t(n) + size_t((px + n) % n)];
                };
                const double s = r.strength * n / 256.0;
                const double dx = (h(x + 1, y) - h(x - 1, y)) * s;
                const double dy = (h(x, y + 1) - h(x, y - 1)) * s;
                const double len = std::sqrt(dx * dx + dy * dy + 1.0);
                // Tangent space, +y up the picture (glTF): heights rise down the rows here.
                c = {(-dx / len) * 0.5 + 0.5, (dy / len) * 0.5 + 0.5, (1.0 / len) * 0.5 + 0.5};
            } else {
                const double h = heights[i];
                const auto& stops = r.colors;
                if (h <= stops.front().first) {
                    c = stops.front().second;
                } else if (h >= stops.back().first) {
                    c = stops.back().second;
                } else {
                    for (size_t k = 1; k < stops.size(); ++k)
                        if (h <= stops[k].first) {
                            const auto& [a0, c0] = stops[k - 1];
                            const auto& [a1, c1] = stops[k];
                            const double t = a1 > a0 ? (h - a0) / (a1 - a0) : 1.0;
                            for (size_t ch = 0; ch < 3; ++ch)
                                c[ch] = c0[ch] + (c1[ch] - c0[ch]) * t;
                            break;
                        }
                }
            }
            out[i * 4 + 0] = byte(c[0]);
            out[i * 4 + 1] = byte(c[1]);
            out[i * 4 + 2] = byte(c[2]);
            out[i * 4 + 3] = 255;
        }
    return out;
}

}  // namespace midgard
