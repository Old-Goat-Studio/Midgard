// SPDX-License-Identifier: Apache-2.0
// The debug text overlay: lines of text in the top-left corner of the screen, over a dark panel,
// for frame times, positions and self-test results. It needs no font files (the glyphs are
// built from rectangles by stb_easy_font), so it works long before the in-game menus exist.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace midgard::render {

struct OverlayLine {
    std::string text;       // printable ASCII; anything else shows as '?'
    glm::vec4 color{1.0f};  // sRGB, as a designer picks colours
    float size = 1.0f;      // relative text size (2 = twice as tall)
};

// One corner of a triangle, in pixels from the top-left of the screen.
struct OverlayVertex {
    glm::vec2 position;
    glm::vec4 color;  // sRGB
};

// The pixel size of one font unit for a screen this tall: text stays readable on phones'
// high-resolution screens (about 45 lines of normal text fit the height).
float overlay_scale(uint32_t screen_height);

// Triangles for the lines: first the panel behind them, then the glyphs. Empty when there are
// no lines. Lines that don't fit the screen are cut off.
std::vector<OverlayVertex> layout_overlay(const std::vector<OverlayLine>& lines,
                                          uint32_t screen_width, uint32_t screen_height);

}  // namespace midgard::render
