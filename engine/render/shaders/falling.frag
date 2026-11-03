// SPDX-License-Identifier: Apache-2.0
// What falls from the sky (falling.vert): lit by the sky's light and a little of the sun's.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(location = 0) in vec2 v_corner;
layout(location = 1) in float v_alpha;
layout(location = 0) out vec4 out_color;

void main() {
    const int kind = int(frame.fall.x);
    const vec3 light =
        frame.ambient_sky.rgb * 1.5 + frame.sun_color.rgb * 0.2 + frame.sky_horizon.rgb * 0.3;
    vec3 color;
    float alpha;
    if (kind == 1) {  // rain: a faint streak, brightest down its middle
        color = light;
        alpha = 0.3 * (1.0 - abs(v_corner.x)) * (1.0 - 0.5 * abs(v_corner.y));
    } else {  // flakes: soft round spots
        const float r2 = dot(v_corner, v_corner);
        if (r2 > 1.0) discard;
        alpha = 1.0 - r2;
        if (kind == 2) {  // snow: white, as bright as the sky behind it
            color = light * 1.2 + frame.sky_horizon.rgb * 0.7;
            alpha *= 0.9;
        } else if (kind == 3) {  // ash: dark grey
            color = light * 0.3;
            alpha *= 0.8;
        } else {  // sand: tan
            color = light * vec3(0.95, 0.78, 0.52);
            alpha *= 0.6;
        }
    }
    out_color = vec4(color, alpha * v_alpha);
}
