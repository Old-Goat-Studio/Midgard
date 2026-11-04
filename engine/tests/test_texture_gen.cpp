// SPDX-License-Identifier: Apache-2.0
// The texture generator (milestone 5.17 step 6).
#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <string>

#include "midgard/texture_gen.h"

namespace {

using midgard::TextureRecipe;

TextureRecipe recipe(const std::string& json) {
    std::vector<std::string> problems;
    TextureRecipe r = midgard::texture_recipe_from_json(json, &problems);
    REQUIRE(problems.empty());
    return r;
}

// The largest step between neighbouring pixels across the picture's left/right and top/bottom
// edges, against the largest step inside it: a tiling picture has no seam.
void check_tiles(const TextureRecipe& r) {
    const std::vector<uint8_t> px = midgard::generate_texture(r);
    const int n = r.size;
    const auto red = [&](int x, int y) { return int(px[(size_t(y) * size_t(n) + size_t(x)) * 4]); };
    int inside = 0, across = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x + 1 < n; ++x) {
            inside = std::max(inside, std::abs(red(x + 1, y) - red(x, y)));
            inside = std::max(inside, std::abs(red(y, x + 1) - red(y, x)));
        }
    for (int i = 0; i < n; ++i) {
        across = std::max(across, std::abs(red(0, i) - red(n - 1, i)));
        across = std::max(across, std::abs(red(i, 0) - red(i, n - 1)));
    }
    CHECK(across <= inside);
}

}  // namespace

TEST_CASE("texture generator: every pattern makes a picture, and a recipe the same one twice") {
    for (const std::string& pattern : midgard::texture_patterns()) {
        CAPTURE(pattern);
        TextureRecipe r = recipe(R"({"size": 64, "pattern": ")" + pattern + R"("})");
        const std::vector<uint8_t> a = midgard::generate_texture(r);
        REQUIRE(a.size() == 64u * 64u * 4u);
        CHECK(a == midgard::generate_texture(r));
        // Not all one colour.
        bool varied = false;
        for (size_t i = 4; i < a.size() && !varied; i += 4) varied = a[i] != a[0];
        CHECK(varied);
        for (size_t i = 3; i < a.size(); i += 4) REQUIRE(a[i] == 255);
    }
}

TEST_CASE("texture generator: whole-number scales tile seamlessly") {
    for (const char* pattern :
         {"noise", "ridged", "cells", "checker", "bricks", "waves", "stripes"}) {
        CAPTURE(pattern);
        check_tiles(recipe(std::string(R"({"size": 128, "scale": 5, "pattern": ")") + pattern +
                           R"(", "softness": 0.1})"));
    }
    check_tiles(recipe(R"({"size": 128, "scale": 3, "pattern": "noise", "warp": 1.5})"));
}

TEST_CASE("texture generator: colours come from the ramp, and inverting swaps them") {
    TextureRecipe r = recipe(R"({"size": 32, "pattern": "gradient",
        "colors": [[0, [1, 0, 0]], [1, [0, 0, 1]]]})");
    const std::vector<uint8_t> px = midgard::generate_texture(r);
    // Left (height near 0): red; right: blue.
    CHECK(px[0] > 240);
    CHECK(px[2] < 15);
    const size_t right = 31 * 4;
    CHECK(px[right] < 15);
    CHECK(px[right + 2] > 240);
    r.invert = true;
    const std::vector<uint8_t> inverted = midgard::generate_texture(r);
    CHECK(inverted[0] < 15);
    CHECK(inverted[2] > 240);
}

TEST_CASE("texture generator: a normal map from the heights") {
    // Flat (one height everywhere): every normal points straight out (128, 128, 255).
    TextureRecipe flat = recipe(R"({"size": 32, "pattern": "stripes", "width": 1,
                                     "output": "normal"})");
    const std::vector<uint8_t> px = midgard::generate_texture(flat);
    for (size_t i = 0; i < px.size(); i += 4) {
        REQUIRE(std::abs(int(px[i]) - 128) <= 1);
        REQUIRE(std::abs(int(px[i + 1]) - 128) <= 1);
        REQUIRE(px[i + 2] == 255);
    }
    // A slope rising to the right leans the normals left (red below the middle).
    TextureRecipe ramp =
        recipe(R"({"size": 32, "pattern": "gradient", "output": "normal", "strength": 20})");
    const std::vector<uint8_t> lean = midgard::generate_texture(ramp);
    const size_t middle = (16 * 32 + 16) * 4;
    CHECK(lean[middle] < 120);
    CHECK(std::abs(int(lean[middle + 1]) - 128) <= 1);
}

TEST_CASE("texture generator: a recipe's problems are named, and it round-trips") {
    std::vector<std::string> problems;
    const TextureRecipe r = midgard::texture_recipe_from_json(
        R"({"pattern": "plaid", "size": 99999, "scale": "lots", "output": "height",
            "colors": [1, 2]})",
        &problems);
    REQUIRE(problems.size() == 5);
    CHECK(problems[0].find("'plaid' isn't a pattern") != std::string::npos);
    CHECK(r.pattern == "noise");
    CHECK(r.size == 4096);  // (clamped)
    CHECK(r.output == "color");

    TextureRecipe custom = recipe(R"({"pattern": "bricks", "size": 256, "scale": 6, "seed": 9,
        "width": 0.4, "variation": 0.5, "colors": [[0, [0.2, 0.2, 0.2]], [1, [0.7, 0.3, 0.2]]]})");
    std::vector<std::string> again;
    const TextureRecipe back =
        midgard::texture_recipe_from_json(midgard::texture_recipe_to_json(custom), &again);
    CHECK(again.empty());
    CHECK(midgard::generate_texture(back) == midgard::generate_texture(custom));
}
