// SPDX-License-Identifier: Apache-2.0
#version 450

layout(set = 0, binding = 0) uniform Frame {
    mat4 view_proj;
    vec4 light_dir;
    vec4 light_color;
    vec4 ambient;
} frame;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
} pc;

layout(location = 0) in vec3 v_normal;
layout(location = 0) out vec4 out_color;

void main() {
    vec3 n = normalize(v_normal);
    float diffuse = max(dot(n, -normalize(frame.light_dir.xyz)), 0.0);
    vec3 lit = pc.color.rgb * (frame.ambient.rgb + frame.light_color.rgb * diffuse);
    out_color = vec4(lit, pc.color.a);
}
