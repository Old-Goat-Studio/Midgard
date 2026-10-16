// SPDX-License-Identifier: Apache-2.0
#include "midgard/render/overlay.h"

#include <algorithm>
#include <cmath>

// stb_easy_font's functions are static, and the ones not used here would warn.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_easy_font.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

namespace midgard::render {

namespace {

constexpr float kLineUnits = 12.0f;  // stb_easy_font's line height
constexpr float kMargin = 4.0f;      // font units around the text, inside the panel
const glm::vec4 kPanel{0.0f, 0.0f, 0.0f, 0.6f};

// stb_easy_font indexes its tables by character, so only printable ASCII may reach it.
std::string printable(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) out += (c >= 32 && c <= 126) ? c : '?';
    return out;
}

struct Packed {  // stb_easy_font's vertex layout
    float x, y, z;
    unsigned char color[4];
};

void quad(std::vector<OverlayVertex>& out, glm::vec2 a, glm::vec2 b, const glm::vec4& color) {
    const glm::vec2 corners[6] = {a, {b.x, a.y}, b, a, b, {a.x, b.y}};
    for (const glm::vec2& p : corners) out.push_back({p, color});
}

}  // namespace

float overlay_scale(uint32_t screen_height) {
    return std::max(1.0f, std::floor(float(screen_height) / (45.0f * kLineUnits)));
}

std::vector<OverlayVertex> layout_overlay(const std::vector<OverlayLine>& lines,
                                          uint32_t screen_width, uint32_t screen_height) {
    std::vector<OverlayVertex> out;
    if (lines.empty() || screen_width == 0 || screen_height == 0) return out;
    const float scale = overlay_scale(screen_height);

    // Where each line goes, and the panel around all of them.
    struct Placed {
        std::string text;
        glm::vec4 color;
        float size;
        float y;
    };
    std::vector<Placed> placed;
    float y = kMargin * scale;
    float right = 0.0f;
    for (const OverlayLine& line : lines) {
        const float size = std::max(line.size, 0.25f) * scale;
        if (y + kLineUnits * size > float(screen_height)) break;  // off the bottom
        std::string text = printable(line.text);
        right = std::max(right, float(stb_easy_font_width(text.data())) * size);
        placed.push_back({std::move(text), line.color, size, y});
        y += kLineUnits * size;
    }
    if (placed.empty()) return out;
    const float left = kMargin * scale;
    quad(out, {0.0f, 0.0f},
         {std::min(left + right + kMargin * scale, float(screen_width)),
          std::min(y + kMargin * scale, float(screen_height))},
         kPanel);

    std::vector<Packed> buffer;
    for (const Placed& p : placed) {
        // About 270 bytes (17 vertices) per character, per stb_easy_font's notes; with room.
        buffer.resize(p.text.size() * 32 + 32);
        const int quads =
            stb_easy_font_print(0.0f, 0.0f, const_cast<char*>(p.text.c_str()), nullptr,
                                buffer.data(), int(buffer.size() * sizeof(Packed)));
        for (int q = 0; q < quads; ++q) {
            const Packed* v = &buffer[size_t(q) * 4];
            // Each quad's corners go round in order, so 0 and 2 are opposite.
            quad(out, {left + v[0].x * p.size, p.y + v[0].y * p.size},
                 {left + v[2].x * p.size, p.y + v[2].y * p.size}, p.color);
        }
    }
    return out;
}

}  // namespace midgard::render
