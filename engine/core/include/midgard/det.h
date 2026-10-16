// SPDX-License-Identifier: Apache-2.0
// Deterministic maths: functions that give bit-identical results on every target (x86_64,
// arm64, Android), so a world generated from a seed is the same everywhere (roadmap §7,
// "Procedural generation").
//
// How: integers wherever possible (hashing, random streams, fixed-point noise), and otherwise
// only the floating-point operations IEEE 754 rounds exactly (+ - * / and sqrt), evaluated in
// a fixed order with no fused multiply-add (see the strict-maths guard in this header). System
// maths libraries (std::sin, std::pow, ...) differ between platforms and are never used here.
//
// Every function has a Python twin in tools/reference/det.py, and both are checked against the
// shared tables in schema/tests/det_*.json.
#pragma once

#include <cstdint>

// ---- the strict-maths guard -------------------------------------------------------------------
// Fast-math lets the compiler reorder and fuse operations, which changes results between
// targets. The engine is built with /fp:precise or -ffp-contract=off (engine/CMakeLists.txt);
// this stops a build that drops them. (FMA contraction has no macro; a test catches it.)
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#error "Midgard needs strict floating point: build without fast-math (see midgard/det.h)"
#endif

namespace midgard::det {

// ---- hashing ----------------------------------------------------------------------------------
// SplitMix64's finaliser: a fast, well-mixed 64-bit hash step.
constexpr std::uint64_t mix64(std::uint64_t z) {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// A hash of a seed and up to three integer coordinates (lattice points, chunk numbers).
constexpr std::uint64_t hash3(std::uint64_t seed, std::int64_t x, std::int64_t y = 0,
                              std::int64_t z = 0) {
    std::uint64_t h = mix64(seed);
    h = mix64(h ^ static_cast<std::uint64_t>(x));
    h = mix64(h ^ static_cast<std::uint64_t>(y));
    return mix64(h ^ static_cast<std::uint64_t>(z));
}

// ---- random streams ---------------------------------------------------------------------------
// A seeded random stream (SplitMix64). Streams for different purposes are split off by a tag,
// so a body's terrain, weather and names never draw from the same numbers:
//   det::Rng body = det::Rng(world_seed).split(body_number);
//   det::Rng weather = body.split(det::tag("weather"));
class Rng {
public:
    constexpr explicit Rng(std::uint64_t seed) : state_(seed) {}

    constexpr std::uint64_t next() {
        state_ += 0x9e3779b97f4a7c15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    // A new, independent stream for `tag`; this one is unchanged.
    constexpr Rng split(std::uint64_t tag) const { return Rng(mix64(state_ ^ mix64(tag))); }
    // A number in [0, n), every value equally likely (n > 0).
    constexpr std::uint64_t below(std::uint64_t n) {
        const std::uint64_t threshold = (0 - n) % n;  // 2^64 mod n: rejected, for no bias
        std::uint64_t r = next();
        while (r < threshold) r = next();
        return r % n;
    }
    // A number in [0, 1), in steps of 2^-53.
    double uniform() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

private:
    std::uint64_t state_;
};

// A tag from a name (FNV-1a), for Rng::split and seeds: tag("weather").
constexpr std::uint64_t tag(const char* name) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (; *name; ++name) h = (h ^ static_cast<unsigned char>(*name)) * 0x100000001b3ULL;
    return h;
}

// ---- fixed point --------------------------------------------------------------------------------
// Noise is computed in Q16.16 fixed point (a value times 65536 in an int32), so it uses integer
// arithmetic only.
using Fixed = std::int32_t;
inline constexpr int kFracBits = 16;
inline constexpr Fixed kOne = 1 << kFracBits;

inline double to_double(Fixed v) {
    return static_cast<double>(v) * (1.0 / kOne);
}

// A position along one axis as a lattice cell and the Q16 fraction inside it (0..65535).
struct Lattice {
    std::int64_t cell;
    Fixed frac;
};
Lattice lattice(double x);

// The smooth step 6t^5 - 15t^4 + 10t^3 for t in [0, 1] (Q16).
Fixed fade(Fixed t);
inline Fixed lerp(Fixed a, Fixed b, Fixed t) {
    return a + static_cast<Fixed>((static_cast<std::int64_t>(b) - a) * t >> kFracBits);
}

// ---- noise ----------------------------------------------------------------------------------
// Noise sampled at 3D positions (on a world's surface, positions come from the shape mapping,
// so joined edges are seamless). One unit is one lattice cell; scale positions to choose the
// feature size. Results are Q16, roughly -1..1.
Fixed gradient_noise(std::uint64_t seed, double x, double y, double z);  // Perlin-style
Fixed value_noise(std::uint64_t seed, double x, double y, double z);
// Octaves of gradient noise, each twice the frequency and half the amplitude of the last.
// Not normalised: the sum of n octaves is roughly -(2 - 2^(1-n))..(2 - 2^(1-n)).
Fixed fbm(std::uint64_t seed, double x, double y, double z, int octaves);

// ---- trigonometry ----------------------------------------------------------------------------
// sin and cos from IEEE-exact operations only: reduction by pi/2 in two parts (Cody-Waite),
// then Taylor polynomials on [-pi/4, pi/4]. Accurate to a few units in the last place for
// |x| up to about 1e6; beyond that less accurate, but still identical everywhere.
double sin(double x);
double cos(double x);
double tan(double x);  // sin / cos
// atan: |x| > 1 uses pi/2 - atan(1/x), then values above tan(pi/12) are shifted down by pi/6,
// leaving a Taylor series that converges quickly. atan2 adds the quadrant.
double atan(double x);
double atan2(double y, double x);
// IEEE 754 requires sqrt to be correctly rounded, so the hardware's is already identical
// everywhere; it's here so generation code uses det:: throughout.
double sqrt(double x);

// ---- the build's own check ----------------------------------------------------------------------
// a * b + c, compiled with the same options as everything here. A compiler that fuses it into
// one multiply-add (no rounding in between) gives a different answer for some inputs, so tests
// and the self-test call it to prove the build keeps the two roundings.
double multiply_add_probe(double a, double b, double c);
// True if this build rounds a * b and then adds, as the determinism above needs.
bool strict_floating_point();

// A hash over a sample of everything above (a 64 x 64 fbm field, sin, cos and sqrt of 1000
// values, 100 random numbers): equal to kFingerprint on every target. The self-test checks it
// on devices; schema/tests/det_fingerprint.json holds the value.
std::uint64_t fingerprint();
inline constexpr std::uint64_t kFingerprint = 0x9f42a2ae6f3222d2ULL;

}  // namespace midgard::det
