// SPDX-License-Identifier: Apache-2.0
// A surface shader for the "surfaces" image test, wrapped as the editor wraps one: rings
// around the uv's centre bend the normal (bumps) and glow faintly blue.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "atmosphere.glsl"
#include "surface.glsl"

void surface(SurfaceInput i, inout Surface s) {
    vec2 c = i.uv - 0.5;
    float r = length(c);
    float wave = sin(r * 60.0);
    vec3 side = normalize(cross(s.normal, i.up + vec3(0.001, 0.0, 0.0)));
    s.normal = normalize(s.normal + side * wave * 0.35);
    s.emission += vec3(0.1, 0.3, 0.9) * max(wave, 0.0) * 0.4;
    s.metallic = 0.6;
    s.roughness = 0.3;
}

void main() {
    const SurfaceInput i = surface_input();
    Surface s = material_surface(i);
    surface(i, s);
    if (s.alpha < s.cutout) discard;
    out_color = light_surface(i, s);
}
