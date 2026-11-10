// SPDX-License-Identifier: Apache-2.0
// The in-game UI (render/ui_draw.h): positions in pixels, placed by the draw's transform.
#version 450

layout(push_constant) uniform Push {
    mat4 clip_from_pixels;  // the screen's projection times the draw's transform
} pc;

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec4 in_color;  // premultiplied sRGB (RGBA8 unorm)
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;

void main() {
    v_color = in_color;
    v_uv = in_uv;
    gl_Position = pc.clip_from_pixels * vec4(in_position, 0.0, 1.0);
}
