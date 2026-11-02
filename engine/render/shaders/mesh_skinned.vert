// SPDX-License-Identifier: Apache-2.0
// Skinned meshes (milestone 5.13): mesh.vert with each vertex moved by up to four joints. The
// frame's palette holds every skinned draw's joint matrices (each joint's pose times its
// inverse bind matrix, in the mesh's own space); emissive.w is where this draw's start.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(push_constant) uniform Push {
    mat4 model;
    vec4 base_color;  // linear rgb, alpha
    vec4 emissive;    // linear rgb, the draw's first joint in the palette
    vec4 params;      // metallic, roughness, normal scale, occlusion strength
} pc;

layout(set = 0, binding = 2) readonly buffer Palette {
    mat4 joints[];
} palette;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_tangent;
layout(location = 4) in uvec4 in_joints;
layout(location = 5) in vec4 in_weights;

layout(location = 0) out vec3 v_position;  // relative to the camera
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec4 v_tangent;

void main() {
    const uint first = uint(pc.emissive.w);
    const mat4 skin = in_weights.x * palette.joints[first + in_joints.x] +
                      in_weights.y * palette.joints[first + in_joints.y] +
                      in_weights.z * palette.joints[first + in_joints.z] +
                      in_weights.w * palette.joints[first + in_joints.w];
    const mat4 model = pc.model * skin;
    const vec4 world = model * vec4(in_position, 1.0);
    const mat3 normal_matrix = transpose(inverse(mat3(model)));
    v_position = world.xyz;
    v_normal = normal_matrix * in_normal;
    v_tangent = vec4(mat3(model) * in_tangent.xyz, in_tangent.w);
    v_uv = in_uv;
    gl_Position = frame.view_proj * world;
}
