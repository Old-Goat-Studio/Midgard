// SPDX-License-Identifier: Apache-2.0
#version 450

layout(set = 0, binding = 0) uniform Frame {
    mat4 view_proj;
    vec4 light_dir;    // xyz: direction the light travels, towards the scene
    vec4 light_color;  // rgb
    vec4 ambient;      // rgb
} frame;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;

layout(location = 0) out vec3 v_normal;

void main() {
    v_normal = mat3(pc.model) * in_normal;
    gl_Position = frame.view_proj * pc.model * vec4(in_position, 1.0);
}
