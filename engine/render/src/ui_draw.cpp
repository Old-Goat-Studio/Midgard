// SPDX-License-Identifier: Apache-2.0
#include "midgard/render/ui_draw.h"

#include "midgard/render/mesh_data.h"

namespace midgard::render {

glm::vec4 ui_linear(const uint8_t premultiplied[4]) {
    const float a = premultiplied[3] / 255.0f;
    if (a <= 0.0f) return glm::vec4(0.0f);
    glm::vec4 out(0.0f, 0.0f, 0.0f, a);
    for (int i = 0; i < 3; ++i) {
        const float straight = glm::clamp(premultiplied[i] / 255.0f / a, 0.0f, 1.0f);
        out[i] = srgb_to_linear(straight) * a;
    }
    return out;
}

glm::mat4 ui_projection(uint32_t screen_width, uint32_t screen_height) {
    // x: 0..width to -1..1; y: 0..height to -1..1 (Vulkan's clip space has y down, as rows do).
    glm::mat4 m(1.0f);
    m[0][0] = 2.0f / float(screen_width);
    m[1][1] = 2.0f / float(screen_height);
    m[3][0] = -1.0f;
    m[3][1] = -1.0f;
    return m;
}

}  // namespace midgard::render
