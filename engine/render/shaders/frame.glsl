// SPDX-License-Identifier: Apache-2.0
// The per-frame data every scene shader sees (FrameData in renderer.cpp, which must match).

const int kMaxLocalLights = 8;

struct LocalLight {
    vec4 position_range;   // xyz relative to the camera, w: range (metres)
    vec4 color;            // linear rgb times intensity, w: 0 point, 1 spot
    vec4 direction;        // xyz: where a spot shines, w: cos(outer cone)
    vec4 cone;             // x: cos(inner cone)
};

layout(set = 0, binding = 0) uniform Frame {
    mat4 view_proj;
    vec4 up;              // xyz: "up" at the camera (from the world's shape)
    vec4 sun_direction;   // xyz: the direction sunlight travels
    vec4 sun_color;       // linear rgb times intensity
    vec4 ambient_sky;     // linear rgb: ambient light from above
    vec4 ambient_ground;  // linear rgb: ambient light from below
    ivec4 counts;         // x: local lights
    LocalLight lights[kMaxLocalLights];
} frame;
