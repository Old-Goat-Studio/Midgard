// SPDX-License-Identifier: Apache-2.0
// A surface shader for the "surfaces" image test, wrapped as the editor wraps one: holes cut
// by noise (cutout), their edges glowing (emission).
#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "atmosphere.glsl"
#include "surface.glsl"

void surface(SurfaceInput i, inout Surface s) {
    float edge = fbm(i.uv * 4.0, 3) - 0.4;
    s.alpha = edge;
    s.cutout = 0.0;
    s.emission = vec3(1.0, 0.45, 0.1) * smoothstep(0.06, 0.0, edge) * 3.0;
}

void main() {
    const SurfaceInput i = surface_input();
    Surface s = material_surface(i);
    surface(i, s);
    if (s.alpha < s.cutout) discard;
    out_color = light_surface(i, s);
}
