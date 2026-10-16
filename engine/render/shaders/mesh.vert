// SPDX-License-Identifier: Apache-2.0
// Meshes, camera-relative: model matrices and the view are relative to the camera's origin, so
// positions here are small and exact (see render::Camera).
#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(push_constant) uniform Push {
    mat4 model;
    vec4 base_color;  // linear rgb, alpha
    vec4 emissive;    // linear rgb, unused
    vec4 params;      // metallic, roughness, normal scale, occlusion strength
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_tangent;

layout(location = 0) out vec3 v_position;  // relative to the camera
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec4 v_tangent;

void main() {
    const vec4 world = pc.model * vec4(in_position, 1.0);
    // Normals by the inverse transpose, so non-uniform scale keeps them perpendicular.
    const mat3 normal_matrix = transpose(inverse(mat3(pc.model)));
    v_position = world.xyz;
    v_normal = normal_matrix * in_normal;
    v_tangent = vec4(mat3(pc.model) * in_tangent.xyz, in_tangent.w);
    v_uv = in_uv;
    gl_Position = frame.view_proj * world;
}
