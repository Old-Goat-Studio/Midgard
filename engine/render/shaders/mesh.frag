// SPDX-License-Identifier: Apache-2.0
// Meshes in Midgard's own materials: the material's surface, lit (surface.glsl).
#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "atmosphere.glsl"
#include "surface.glsl"

void main() {
    const SurfaceInput i = surface_input();
    out_color = light_surface(i, material_surface(i));
}
