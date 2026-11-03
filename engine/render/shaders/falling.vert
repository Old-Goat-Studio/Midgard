// SPDX-License-Identifier: Apache-2.0
// What falls from the sky (render::Falling; milestone 5.15): rain, snow, ash or sand, made here
// from each particle's number, with no buffers. Each particle has a place in a box round the
// camera, fixed in the world (the box moves with the camera, wrapping), carried by the wind and
// its fall; rain is a streak along its motion, the others small flakes facing the camera.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(location = 0) out vec2 v_corner;
layout(location = 1) out float v_alpha;

float rand(uint n) {
    n = (n << 13u) ^ n;
    n = n * (n * n * 15731u + 789221u) + 1376312589u;
    return float(n & 0x7fffffffu) / 2147483647.0;
}

const vec2 kCorners[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

void main() {
    const uint id = uint(gl_VertexIndex) / 6u;
    const vec2 corner = kCorners[uint(gl_VertexIndex) % 6u];
    const int kind = int(frame.fall.x);
    const float box = frame.fall.z;
    const float t = frame.fall.w;
    const float r = rand(id * 4u + 4u);
    vec3 p = vec3(rand(id * 4u + 1u), rand(id * 4u + 2u), rand(id * 4u + 3u)) * box;
    // Camera-relative: the box round the camera, the particle where the world has it.
    vec3 rel = mod(p + frame.fall_motion.xyz, box) - 0.5 * box;
    if (kind != 1) {  // flakes drift and turn as they fall
        rel.x += 0.25 * sin(t * (0.9 + r) + r * 31.0);
        rel.z += 0.25 * cos(t * (0.7 + r) + r * 17.0);
    }
    const vec3 up = frame.up.xyz;
    const float dist = max(length(rel), 1e-3);
    const vec3 toward = rel / dist;
    vec3 along, side;
    float half_length, half_width;
    const float size = frame.fall_velocity.w;
    if (kind == 1) {  // rain: a streak as long as the drop moves in an instant
        const vec3 v = frame.fall_velocity.xyz;
        along = length(v) > 1e-3 ? normalize(v) : -up;
        vec3 s = cross(toward, along);
        side = dot(s, s) > 1e-6 ? normalize(s) : normalize(cross(along, vec3(0.3, 0.5, 0.8)));
        half_length = 0.5 * length(v) * frame.fall_motion.w;
        half_width = 0.5 * size;
    } else {  // a flake facing the camera
        vec3 s = cross(toward, up);
        side = dot(s, s) > 1e-6 ? normalize(s) : normalize(cross(toward, vec3(1.0, 0.0, 0.0)));
        along = cross(side, toward);
        half_length = half_width = 0.5 * size * (0.7 + 0.6 * r);
    }
    const vec3 position = rel + side * corner.x * half_width + along * corner.y * half_length;
    v_corner = corner;
    // Gone near the camera (no drops on the lens) and towards the box's edge (no wall of rain).
    v_alpha = smoothstep(0.25, 1.2, dist) * (1.0 - smoothstep(0.3 * box, 0.5 * box, dist));
    gl_Position = frame.view_proj * vec4(position, 1.0);
}
