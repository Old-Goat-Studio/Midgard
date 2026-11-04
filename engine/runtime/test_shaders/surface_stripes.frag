// SPDX-License-Identifier: Apache-2.0
// A surface shader for the "surfaces" image test, wrapped as the editor wraps one
// (shader::surface_shader_source), with two settings as it lays them out: diagonal bands of
// gloss (their colour and count from the draw's settings) over the material's colour.
//
//     setting color stripe_color = vec3(0.85, 0.12, 0.08);
//     setting float bands = 4.0;
#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "atmosphere.glsl"
#include "surface.glsl"

layout(std430, set = 0, binding = 2) readonly buffer MidgardSettings {
    vec4 midgard_values[];
};
vec4 midgard_setting(int k) { return midgard_values[uint(pc.extra.x) + uint(k)]; }
#define stripe_color srgb(midgard_setting(0).xyz)
#define bands (midgard_setting(1).x)

void surface(SurfaceInput i, inout Surface s) {
    float band = step(0.5, fract((i.uv.x + i.uv.y) * bands));
    s.color = mix(stripe_color, s.color, band);
    s.roughness = mix(0.15, 0.9, band);
}

void main() {
    const SurfaceInput i = surface_input();
    Surface s = material_surface(i);
    surface(i, s);
    if (s.alpha < s.cutout) discard;
    out_color = light_surface(i, s);
}
