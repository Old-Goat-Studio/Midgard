// SPDX-License-Identifier: Apache-2.0
// The texture generator (milestone 5.17 step 6): noise, gradients and patterns made into a
// square picture from a recipe (JSON), the same wherever it runs. The editor saves the picture
// (a PNG) with its recipe beside it, so it can be opened and changed again.
//
// A recipe:
//   {"pattern": "noise", "size": 512, "scale": 4, "octaves": 4, "roughness": 0.5, "seed": 1,
//    "warp": 0, "angle": 0, "width": 0.5, "softness": 0.05, "jitter": 1, "variation": 0.3,
//    "invert": false, "colors": [[0, [r, g, b]], [1, [r, g, b]]],
//    "output": "color" | "normal", "strength": 2}
//
// Patterns (each a height from 0 to 1, made into colours by "colors", a ramp of sRGB colours
// from 0 to 1, or into a normal map):
//   noise     smooth random hills (fbm: "octaves" layers, each "roughness" as strong)
//   ridged    sharp ridges (each layer folded)
//   cells     a cell's nearness to its own middle (stone, scales); "jitter" how irregular
//   gradient  from one side to the other ("angle")
//   radial    bright in the middle, dark at the edges
//   rings     rings round the middle (wood: give it some "warp")
//   checker   squares
//   stripes   bands ("width" of each band, 0..1; "angle")
//   bricks    bricks in offset rows ("width": the mortar; "variation": how much bricks differ)
//   waves     smooth waves ("angle")
// "scale" is how many features across the picture: a whole number makes noise, cells, checker
// and bricks tile seamlessly; stripes and waves always tile (their angle is nudged to the
// nearest that repeats exactly). "warp" bends the pattern
// with noise. "softness" softens the edges of stripes, checker squares and bricks.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace midgard {

struct TextureRecipe {
    std::string pattern = "noise";
    int size = 512;  // pixels a side (16 .. 4096)
    double scale = 4.0;
    int octaves = 4;
    double roughness = 0.5;
    uint32_t seed = 1;
    double warp = 0.0;
    double angle = 0.0;  // degrees
    double width = 0.5;
    double softness = 0.05;
    double jitter = 1.0;
    double variation = 0.3;
    bool invert = false;
    std::vector<std::pair<double, std::array<double, 3>>> colors = {{0.0, {0.0, 0.0, 0.0}},
                                                                    {1.0, {1.0, 1.0, 1.0}}};
    std::string output = "color";  // "color" (sRGB) or "normal" (a tangent-space normal map)
    double strength = 2.0;         // a normal map's steepness
};

// The patterns, in the order the editor lists them.
const std::vector<std::string>& texture_patterns();

// A recipe from JSON; problems (unknown pattern, out-of-range values) are returned, and the
// recipe holds what could be used (values clamped).
TextureRecipe texture_recipe_from_json(const std::string& json, std::vector<std::string>* problems);
std::string texture_recipe_to_json(const TextureRecipe& recipe);

// The pattern's height (0 .. 1) at (u, v) in 0 .. 1 (before inverting).
double texture_height(const TextureRecipe& recipe, double u, double v);

// The picture: size x size RGBA, 8 bits a channel, rows top to bottom.
std::vector<uint8_t> generate_texture(const TextureRecipe& recipe);

}  // namespace midgard
