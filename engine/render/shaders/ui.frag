// SPDX-License-Identifier: Apache-2.0
// The in-game UI: the vertex colour times the texture (both premultiplied sRGB, as RmlUi makes
// them; textures are uploaded unorm so that sampling leaves them so), made linear for the sRGB
// framebuffer (render/ui_draw.h's ui_linear does the same) and blended premultiplied.
#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

vec3 srgb_to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

void main() {
    vec4 c = v_color * texture(tex, v_uv);
    if (c.a <= 0.0) {
        out_color = vec4(0.0);
        return;
    }
    vec3 straight = clamp(c.rgb / c.a, 0.0, 1.0);
    out_color = vec4(srgb_to_linear(straight) * c.a, c.a);
}
