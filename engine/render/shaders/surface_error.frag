// SPDX-License-Identifier: Apache-2.0
// A surface shader that couldn't be used (it didn't compile, or isn't in the package): bright
// magenta, lit dimly so its shape still shows, so the mistake is seen at once and nothing breaks.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "atmosphere.glsl"
#include "surface.glsl"

void main() {
    const SurfaceInput i = surface_input();
    Surface s = material_surface(i);
    s.color = vec3(1.0, 0.0, 1.0);
    s.metallic = 0.0;
    s.roughness = 0.6;
    s.alpha = 1.0;
    s.emission = vec3(0.35, 0.0, 0.35);
    out_color = light_surface(i, s);
}
