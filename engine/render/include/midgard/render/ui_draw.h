// SPDX-License-Identifier: Apache-2.0
// The in-game UI layer (milestone 6.0): the menus, screens and HUD as 2D triangles, drawn over
// the tone-mapped scene and under the debug overlay. The renderer knows nothing of the UI
// library (RmlUi, in midgard/ui): it draws what it is given, a frame at a time.
#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace midgard::render {

// A UI texture (a font's glyphs, a picture); 0 is none (plain white, for flat colour).
using UiTextureId = uint32_t;
inline constexpr UiTextureId kNoUiTexture = 0;

// One corner, laid out as RmlUi's Rml::Vertex so its geometry is copied as it is: a position in
// pixels from the screen's top-left, a premultiplied-alpha sRGB colour, and a texture place.
struct UiVertex {
    glm::vec2 position{0.0f};
    uint8_t color[4] = {255, 255, 255, 255};  // premultiplied: r, g and b already times alpha
    glm::vec2 uv{0.0f};
};
static_assert(sizeof(UiVertex) == 20, "UiVertex must match Rml::Vertex");

// A run of triangles (indices into the frame's vertices, from first_vertex), with its texture,
// its transform (pixels to pixels: where it is placed, and any CSS transform), and the
// rectangle it is cut to.
struct UiDraw {
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    uint32_t first_vertex = 0;
    UiTextureId texture = kNoUiTexture;
    glm::mat4 transform{1.0f};
    bool clip = false;
    glm::ivec4 clip_rect{0};  // x, y, width, height in pixels (with clip)
};

struct UiFrame {
    std::vector<UiVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<UiDraw> draws;

    bool empty() const { return draws.empty(); }
    void clear() {
        vertices.clear();
        indices.clear();
        draws.clear();
    }
};

// A UI colour as the sRGB framebuffer takes it: premultiplied sRGB (8 bits a channel, as RmlUi
// gives it) to premultiplied linear. Blending then happens in linear light, as the scene's does.
glm::vec4 ui_linear(const uint8_t premultiplied[4]);

// The matrix from a draw's pixels to Vulkan's clip space (y down) for a screen this size.
glm::mat4 ui_projection(uint32_t screen_width, uint32_t screen_height);

}  // namespace midgard::render
