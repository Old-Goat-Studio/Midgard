// SPDX-License-Identifier: Apache-2.0
// Debug lines (render::DebugLine): camera-relative like everything drawn, nudged a little
// towards the camera so a line along a surface's edge isn't hidden by the surface itself.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 0) out vec4 v_color;

void main() {
    v_color = in_color;
    gl_Position = frame.view_proj * vec4(in_position, 1.0);
    gl_Position.z = max(gl_Position.z - 0.0002 * gl_Position.w, 0.0);
}
