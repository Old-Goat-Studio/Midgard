// SPDX-License-Identifier: Apache-2.0
// A surface shader for the "surfaces" image test, wrapped as the editor wraps one
// (shader::surface_shader_source): diagonal bands of red gloss over the material's colour.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "atmosphere.glsl"
#include "surface.glsl"

void surface(SurfaceInput i, inout Surface s) {
    float band = step(0.5, fract((i.uv.x + i.uv.y) * 4.0));
    s.color = mix(srgb(vec3(0.85, 0.12, 0.08)), s.color, band);
    s.roughness = mix(0.15, 0.9, band);
}

void main() {
    const SurfaceInput i = surface_input();
    Surface s = material_surface(i);
    surface(i, s);
    if (s.alpha < s.cutout) discard;
    out_color = light_surface(i, s);
}
