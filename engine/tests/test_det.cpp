// SPDX-License-Identifier: Apache-2.0
// Deterministic maths: the engine against the shared tables (schema/tests/det_*.json, which the
// Python reference matches too), so CI on Windows, Linux x86_64 and Linux arm64 checks that
// every target gets the same bits. Then properties the tables can't show.
#include <doctest/doctest.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "midgard/det.h"
#include "midgard/hash.h"

namespace det = midgard::det;

namespace {

nlohmann::json table(const std::string& name) {
    std::ifstream in(std::filesystem::path(MIDGARD_SCHEMA_DIR) / "tests" / (name + ".json"));
    REQUIRE(in);
    return nlohmann::json::parse(in).at("cases");
}

std::string hex(std::uint64_t v) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016" PRIx64, v);
    return text;
}

std::string bits(double x) {
    std::uint64_t b = 0;
    std::memcpy(&b, &x, sizeof(b));
    return hex(b);
}

std::uint64_t u64(const nlohmann::json& decimal) {
    return std::strtoull(decimal.get<std::string>().c_str(), nullptr, 10);
}

}  // namespace

TEST_CASE("hash3 matches its table") {
    for (const auto& c : table("det_hash"))
        CHECK_MESSAGE(
            hex(det::hash3(u64(c["seed"]), c["x"].get<std::int64_t>(), c["y"].get<std::int64_t>(),
                           c["z"].get<std::int64_t>())) == c["hash"].get<std::string>(),
            c.dump());
}

TEST_CASE("random streams match their table") {
    for (const auto& c : table("det_rng")) {
        if (c.contains("tag")) {
            CHECK(hex(det::tag(c["tag"].get<std::string>().c_str())) ==
                  c["value"].get<std::string>());
            continue;
        }
        const std::uint64_t seed = u64(c["seed"]);
        det::Rng r(seed);
        for (const auto& want : c["next"]) CHECK(hex(r.next()) == want.get<std::string>());
        det::Rng s = det::Rng(seed).split(det::tag("weather"));
        for (const auto& want : c["split_weather"]) CHECK(hex(s.next()) == want.get<std::string>());
        det::Rng b(seed);
        const std::uint64_t ns[] = {1, 2, 3, 10, 1000, (std::uint64_t{1} << 63) + 5};
        for (size_t i = 0; i < 6; ++i)
            CHECK(std::to_string(b.below(ns[i])) ==
                  c["below_1_2_3_10_1000_2p63p5"][i].get<std::string>());
        det::Rng u(seed);
        for (const auto& want : c["uniform_bits"])
            CHECK(bits(u.uniform()) == want.get<std::string>());
    }
}

TEST_CASE("sin, cos and sqrt match their table bit for bit") {
    for (const auto& c : table("det_trig")) {
        const double x = std::strtod(c["x"].get<std::string>().c_str(), nullptr);  // hex float
        CHECK_MESSAGE(bits(det::sin(x)) == c["sin"].get<std::string>(), c.dump());
        CHECK_MESSAGE(bits(det::cos(x)) == c["cos"].get<std::string>(), c.dump());
        CHECK_MESSAGE(bits(det::sqrt(std::fabs(x))) == c["sqrt"].get<std::string>(), c.dump());
    }
}

TEST_CASE("noise fields match their table on every target") {
    for (const auto& c : table("det_noise")) {
        const std::uint64_t seed = u64(c["seed"]);
        const double ox = c["origin"][0], oy = c["origin"][1], oz = c["origin"][2];
        const double step = c["step"];
        const int n = c["n"], octaves = c["octaves"];
        const std::string kind = c["kind"];
        std::vector<det::Fixed> values;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const double x = ox + i * step, y = oy + j * step;
                values.push_back(kind == "gradient" ? det::gradient_noise(seed, x, y, oz)
                                 : kind == "value"  ? det::value_noise(seed, x, y, oz)
                                                    : det::fbm(seed, x, y, oz, octaves));
            }
        for (int k = 0; k < 4; ++k) CHECK(values[k] == c["first"][k].get<int>());
        midgard::Fnv1a64 hash;  // little-endian int32s: every target Midgard runs on
        hash.add(values.data(), values.size() * sizeof(det::Fixed));
        CHECK_MESSAGE(hex(hash.value()) == c["hash"].get<std::string>(), kind);
    }
}

TEST_CASE("streams split for different purposes don't share numbers") {
    const det::Rng world(829174332);
    std::set<std::uint64_t> seen;
    int total = 0;
    for (const char* purpose : {"terrain", "weather", "names", "quests"}) {
        det::Rng r = world.split(det::tag(purpose));
        for (int i = 0; i < 10000; ++i, ++total) seen.insert(r.next());
    }
    CHECK(seen.size() == size_t(total));  // 40,000 draws, no repeats across the four streams
    det::Rng a = world.split(1), b = world.split(1);
    CHECK(a.next() == b.next());  // the same tag gives the same stream
}

TEST_CASE("below() is unbiased and in range") {
    det::Rng r(5);
    int counts[6] = {};
    for (int i = 0; i < 60000; ++i) {
        const std::uint64_t v = r.below(6);
        REQUIRE(v < 6);
        ++counts[v];
    }
    for (int c : counts) CHECK(std::abs(c - 10000) < 400);  // about 4 standard deviations
    det::Rng u(9);
    for (int i = 0; i < 10000; ++i) {
        const double x = u.uniform();
        REQUIRE(x >= 0.0);
        REQUIRE(x < 1.0);
    }
}

TEST_CASE("fade and lattice behave at the edges") {
    CHECK(det::fade(0) == 0);
    CHECK(det::fade(det::kOne) == det::kOne);
    CHECK(det::fade(det::kOne / 2) == det::kOne / 2);
    CHECK(det::lattice(2.75).cell == 2);
    CHECK(det::lattice(2.75).frac == 49152);
    CHECK(det::lattice(-0.25).cell == -1);
    CHECK(det::lattice(-0.25).frac == 49152);
    CHECK(det::lattice(-1e-20).frac == det::kOne - 1);  // x - floor(x) rounds to 1: stays inside
}

TEST_CASE("noise is continuous and in range") {
    for (const std::uint64_t seed : {1ULL, 77ULL, 829174332ULL}) {
        det::Fixed previous = det::gradient_noise(seed, -5.0, 1.3, 2.7);
        for (int i = 1; i < 20000; ++i) {
            const double x =
                -5.0 + i * 0.0005;  // crossing many lattice cells, positive and negative
            const det::Fixed g = det::gradient_noise(seed, x, 1.3, 2.7);
            REQUIRE(std::abs(g - previous) < det::kOne / 64);  // no jumps
            REQUIRE(std::abs(g) <= det::kOne);
            REQUIRE(std::abs(det::value_noise(seed, x, 1.3, 2.7)) <= det::kOne);
            previous = g;
        }
    }
    // At lattice points gradient noise is zero.
    CHECK(det::gradient_noise(3, 4.0, -7.0, 2.0) == 0);
}

TEST_CASE("sin and cos are sin and cos") {
    for (int i = -2000; i <= 2000; ++i) {
        const double x = i * 0.01237;
        REQUIRE(std::fabs(det::sin(x) - std::sin(x)) < 1e-15);
        REQUIRE(std::fabs(det::cos(x) - std::cos(x)) < 1e-15);
        const double s = det::sin(x), c = det::cos(x);
        REQUIRE(std::fabs(s * s + c * c - 1.0) < 1e-15);
    }
}

TEST_CASE("the engine is built with strict floating point (no fused multiply-add)") {
    // Fast-math is refused when compiling (midgard/det.h); contraction can only be seen at run
    // time. engine/CMakeLists.txt sets /fp:precise and -ffp-contract=off for every target.
    CHECK(det::strict_floating_point());
    // volatile, so no compiler can work these out while compiling (even across files).
    volatile double a = 1.0 + 0x1p-30, b = 1.0 - 0x1p-30, c = -1.0;
    CHECK(det::multiply_add_probe(a, b, c) == 0.0);  // fused, it would be -2^-60
    volatile double two = 2.0, three = 3.0, four = 4.0;
    CHECK(det::multiply_add_probe(two, three, four) == 10.0);
}

TEST_CASE("the fingerprint over all of it matches its table") {
    const std::string want = table("det_fingerprint")[0]["fingerprint"].get<std::string>();
    CHECK(hex(det::fingerprint()) == want);
    CHECK(hex(det::kFingerprint) == want);  // the value the self-test compares with
}

TEST_CASE("tan, atan and atan2 match their table bit for bit") {
    const auto hexf = [](const nlohmann::json& v) {
        return std::strtod(v.get<std::string>().c_str(), nullptr);
    };
    for (const auto& c : table("det_atan")) {
        double got = 0.0;
        if (c.contains("atan"))
            got = det::atan(hexf(c["atan"]));
        else if (c.contains("tan"))
            got = det::tan(hexf(c["tan"]));
        else
            got = det::atan2(hexf(c["atan2"][0]), hexf(c["atan2"][1]));
        CHECK_MESSAGE(bits(got) == c["bits"].get<std::string>(), c.dump());
    }
}

TEST_CASE("atan and atan2 are atan and atan2, and odd where they should be") {
    for (int i = -3000; i <= 3000; ++i) {
        const double x = i * 0.0137 * (1 + (i % 7));
        REQUIRE(std::fabs(det::atan(x) - std::atan(x)) < 1e-15 * std::max(1.0, std::fabs(x)));
        REQUIRE(det::atan(-x) == -det::atan(x));  // exactly: seams depend on it
        REQUIRE(det::tan(-x * 0.01) == -det::tan(x * 0.01));
        const double y = (i % 13) - 6.5;
        REQUIRE(std::fabs(det::atan2(y, x) - std::atan2(y, x)) < 1e-15 * 4);
    }
    CHECK(det::atan2(0.0, -1.0) == doctest::Approx(3.141592653589793));
    CHECK(det::atan2(0.0, 0.0) == 0.0);
}
