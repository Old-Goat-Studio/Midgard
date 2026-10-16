// SPDX-License-Identifier: Apache-2.0
// The debug text overlay: positions arrive in pixels from the top-left corner.
#version 450

layout(push_constant) uniform Push {
    vec2 screen;  // width, height in pixels
} pc;

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec4 in_color;  // sRGB

layout(location = 0) out vec4 v_color;

void main() {
    // The framebuffer is sRGB, so it expects linear colours.
    v_color = vec4(pow(clamp(in_color.rgb, 0.0, 1.0), vec3(2.2)), in_color.a);
    // Vulkan's clip space has y pointing down, as pixel rows do.
    gl_Position = vec4(in_position / pc.screen * 2.0 - 1.0, 0.0, 1.0);
}
