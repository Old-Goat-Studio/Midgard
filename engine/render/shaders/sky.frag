// SPDX-License-Identifier: Apache-2.0
// The sky along each pixel's view direction, with fog in front of it (render/atmosphere.h).
#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "atmosphere.glsl"

layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 out_color;

void main() {
    // The view direction: this pixel on the near plane, relative to the camera.
    const vec4 p = frame.inverse_view_proj * vec4(v_ndc, 0.0, 1.0);
    const vec3 d = normalize(p.xyz / p.w);
    out_color = vec4(apply_fog(sky_radiance(d), d * frame.fog_params.w), 1.0);
}
