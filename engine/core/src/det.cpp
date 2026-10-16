// SPDX-License-Identifier: Apache-2.0
// The operations here, and their order, are the specification: tools/reference/det.py repeats
// them step by step in Python, and the tables in schema/tests/ hold both to the same results.
#include "midgard/det.h"

#include <cmath>
#include <cstring>

#include "midgard/hash.h"

namespace midgard::det {

Lattice lattice(double x) {
    const double cell = std::floor(x);
    // x - cell is in [0, 1] and the product by a power of two is exact; truncation then gives
    // the Q16 fraction. Tiny negative x can round x - cell up to exactly 1: stay inside.
    Fixed frac = static_cast<Fixed>((x - cell) * 65536.0);
    if (frac > kOne - 1) frac = kOne - 1;
    return {static_cast<std::int64_t>(cell), frac};
}

Fixed fade(Fixed t) {
    const std::int64_t t1 = t;
    const std::int64_t t2 = (t1 * t1) >> kFracBits;
    const std::int64_t t3 = (t2 * t1) >> kFracBits;
    const std::int64_t inner =
        ((t1 * (6 * t1 - 15 * std::int64_t{kOne})) >> kFracBits) + 10 * std::int64_t{kOne};
    return static_cast<Fixed>((t3 * inner) >> kFracBits);
}

namespace {

// The 12 edge directions of a cube, as in improved Perlin noise.
constexpr int kGradients[12][3] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0},
                                   {1, 0, 1}, {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
                                   {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}};

Fixed gradient_dot(std::uint64_t seed, std::int64_t x, std::int64_t y, std::int64_t z, Fixed dx,
                   Fixed dy, Fixed dz) {
    const int* g = kGradients[hash3(seed, x, y, z) % 12];
    return g[0] * dx + g[1] * dy + g[2] * dz;
}

// A lattice point's value in [-1, 1), Q16.
Fixed corner_value(std::uint64_t seed, std::int64_t x, std::int64_t y, std::int64_t z) {
    return static_cast<Fixed>(hash3(seed, x, y, z) & 0x1ffff) - kOne;
}

}  // namespace

Fixed gradient_noise(std::uint64_t seed, double x, double y, double z) {
    const Lattice lx = lattice(x), ly = lattice(y), lz = lattice(z);
    const Fixed u = fade(lx.frac), v = fade(ly.frac), w = fade(lz.frac);
    Fixed corners[8];
    for (int i = 0; i < 8; ++i) {
        const int ox = i & 1, oy = (i >> 1) & 1, oz = (i >> 2) & 1;
        corners[i] = gradient_dot(seed, lx.cell + ox, ly.cell + oy, lz.cell + oz,
                                  lx.frac - ox * kOne, ly.frac - oy * kOne, lz.frac - oz * kOne);
    }
    const Fixed x00 = lerp(corners[0], corners[1], u), x10 = lerp(corners[2], corners[3], u);
    const Fixed x01 = lerp(corners[4], corners[5], u), x11 = lerp(corners[6], corners[7], u);
    return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
}

Fixed value_noise(std::uint64_t seed, double x, double y, double z) {
    const Lattice lx = lattice(x), ly = lattice(y), lz = lattice(z);
    const Fixed u = fade(lx.frac), v = fade(ly.frac), w = fade(lz.frac);
    Fixed corners[8];
    for (int i = 0; i < 8; ++i)
        corners[i] = corner_value(seed, lx.cell + (i & 1), ly.cell + ((i >> 1) & 1),
                                  lz.cell + ((i >> 2) & 1));
    const Fixed x00 = lerp(corners[0], corners[1], u), x10 = lerp(corners[2], corners[3], u);
    const Fixed x01 = lerp(corners[4], corners[5], u), x11 = lerp(corners[6], corners[7], u);
    return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
}

Fixed fbm(std::uint64_t seed, double x, double y, double z, int octaves) {
    std::int64_t sum = 0;
    double frequency = 1.0;  // powers of two: the products below are exact
    for (int o = 0; o < octaves; ++o) {
        const Fixed n = gradient_noise(hash3(seed, o), x * frequency, y * frequency, z * frequency);
        sum += static_cast<std::int64_t>(n) >> o;  // amplitude 2^-o
        frequency *= 2.0;
    }
    return static_cast<Fixed>(sum);
}

namespace {

// pi/2 in two parts: the first has only 33 significant bits, so k * kPio2Hi is exact for
// |k| < 2^20 and the reduction loses nothing.
constexpr double kTwoOverPi = 0.63661977236758134308;
constexpr double kPio2Hi = 1.57079632673412561417e+00;
constexpr double kPio2Lo = 6.07710050650619224932e-11;

// Taylor series on [-pi/4, pi/4], in Horner form (term to term order is the specification).
double sin_kernel(double r) {
    const double r2 = r * r;
    double p = 1.0 / 1307674368000.0;  // 1/15!
    p = p * r2 - 1.0 / 6227020800.0;   // 1/13!
    p = p * r2 + 1.0 / 39916800.0;     // 1/11!
    p = p * r2 - 1.0 / 362880.0;       // 1/9!
    p = p * r2 + 1.0 / 5040.0;
    p = p * r2 - 1.0 / 120.0;
    p = p * r2 + 1.0 / 6.0;
    // sin r = r - r^3/6 + ...: p holds 1/6 - r^2/120 + ..., with the signs folded in.
    return r - r * r2 * p;
}

double cos_kernel(double r) {
    const double r2 = r * r;
    double p = 1.0 / 20922789888000.0;  // 1/16!
    p = p * r2 - 1.0 / 87178291200.0;   // 1/14!
    p = p * r2 + 1.0 / 479001600.0;     // 1/12!
    p = p * r2 - 1.0 / 3628800.0;       // 1/10!
    p = p * r2 + 1.0 / 40320.0;
    p = p * r2 - 1.0 / 720.0;
    p = p * r2 + 1.0 / 24.0;
    p = p * r2 - 1.0 / 2.0;
    return 1.0 + r2 * p;
}

// x = k * pi/2 + r with r in about [-pi/4, pi/4]; returns r and the quadrant k mod 4.
double reduce(double x, int& quadrant) {
    const double k = std::nearbyint(x * kTwoOverPi);  // round half to even
    quadrant = static_cast<int>(static_cast<std::int64_t>(k) & 3);
    return (x - k * kPio2Hi) - k * kPio2Lo;
}

}  // namespace

double sin(double x) {
    int q = 0;
    const double r = reduce(x, q);
    switch (q) {
        case 0:
            return sin_kernel(r);
        case 1:
            return cos_kernel(r);
        case 2:
            return -sin_kernel(r);
        default:
            return -cos_kernel(r);
    }
}

double cos(double x) {
    int q = 0;
    const double r = reduce(x, q);
    switch (q) {
        case 0:
            return cos_kernel(r);
        case 1:
            return -sin_kernel(r);
        case 2:
            return -cos_kernel(r);
        default:
            return sin_kernel(r);
    }
}

double sqrt(double x) {
    return std::sqrt(x);
}

double multiply_add_probe(double a, double b, double c) {
    return a * b + c;
}

bool strict_floating_point() {
    // (1 + 2^-30)(1 - 2^-30) = 1 - 2^-60, which rounds to 1.0, so adding -1 gives 0. Fused,
    // nothing is rounded before the addition and the answer is -2^-60.
    // The inputs are volatile so the compiler can't work the answer out while compiling (it
    // would round correctly there, whatever it does at run time).
    volatile double a = 1.0 + 0x1p-30, b = 1.0 - 0x1p-30, c = -1.0;
    return multiply_add_probe(a, b, c) == 0.0;
}

std::uint64_t fingerprint() {
    Fnv1a64 hash;  // little-endian values, as on every target Midgard runs on
    for (int j = 0; j < 64; ++j)
        for (int i = 0; i < 64; ++i) {
            const Fixed v = fbm(829174332, -12000.0 + i * 0.173, 4500.0 + j * 0.173, 33.0, 6);
            hash.add(&v, sizeof(v));
        }
    for (int k = 0; k < 1000; ++k) {
        const double x = -500.0 + k * 1.0371;
        const double values[3] = {sin(x), cos(x), sqrt(std::fabs(x))};
        hash.add(values, sizeof(values));
    }
    Rng r = Rng(829174332).split(tag("selftest"));
    for (int k = 0; k < 100; ++k) {
        const std::uint64_t v = r.next();
        hash.add(&v, sizeof(v));
    }
    return hash.value();
}

}  // namespace midgard::det
