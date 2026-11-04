# SPDX-License-Identifier: Apache-2.0
"""Starter surface shaders (milestone 5.17 step 4): New ▸ in the Shader editor makes a copy of
one in the project's shaders folder, to change as you like. Each compiles as it is (a test
checks), and each has settings, so one shader serves many materials."""

BLANK = """\
// A surface shader: it changes how a material's surface looks. Each value of `s` starts as
// the material's own (colour, metal, roughness, ...), so change only what you want; Midgard
// lights it afterwards (sun, shadows, lamps, fog, snow). Help: "Surface shaders".
setting color tint = vec3(1.0, 1.0, 1.0);   // Tint

void surface(SurfaceInput i, inout Surface s) {
    s.color *= tint;
}
"""

WATER = """\
// Water: a glossy surface whose ripples move (bumps from moving noise).
setting color water_color = vec3(0.04, 0.22, 0.3);   // Water colour
setting float wave_size = 0.12;                       // Wave size, 0.02 .. 2
setting float speed = 0.3;                            // Speed, 0 .. 2
setting float choppiness = 1.0;                       // Choppiness, 0 .. 3

float height(vec2 p, float t) {
    return fbm(p + vec2(t, t * 0.7), 4) + 0.5 * fbm(p * 1.7 - vec2(t * 0.8, t * 0.3), 3);
}

void surface(SurfaceInput i, inout Surface s) {
    vec2 p = i.uv / wave_size;
    float t = i.time * speed;
    float h = height(p, t);
    vec2 slope = vec2(height(p + vec2(0.02, 0.0), t) - h,
                      height(p + vec2(0.0, 0.02), t) - h) / 0.02;
    s.normal = bend_normal(s.normal, slope * choppiness * 0.15);
    s.color = water_color;
    s.metallic = 0.0;
    s.roughness = 0.06;
}
"""

GLOW = """\
// Glow: light of its own that pulses, brightest at the edges.
setting color glow = vec3(0.2, 0.8, 1.0);   // Glow colour
setting float strength = 1.2;               // Strength, 0 .. 20
setting float pulses = 1.0;                 // Pulses a second, 0 .. 5
setting float rim = 2.0;                    // Edge glow, 0.5 .. 8

void surface(SurfaceInput i, inout Surface s) {
    float pulse = 0.6 + 0.4 * sin(i.time * pulses * 6.2832);
    float edge = pow(1.0 - max(dot(s.normal, i.view), 0.0), rim);
    s.emission += glow * strength * pulse * (0.1 + 2.0 * edge);
}
"""

DISSOLVE = """\
// Dissolve: holes eaten into the surface by a noise pattern, their edges glowing.
setting float amount = 0.3;                       // How much is gone, 0 .. 1
setting float edge_width = 0.08;                  // Edge width, 0 .. 0.3
setting color edge_color = vec3(1.0, 0.45, 0.1);  // Edge colour
setting float edge_glow = 8.0;                    // Edge glow, 0 .. 20
setting float scale = 6.0;                        // Pattern size, 1 .. 30
setting bool animate = false;                     // Comes and goes by itself

void surface(SurfaceInput i, inout Surface s) {
    float gone = animate ? 0.5 + 0.5 * sin(i.time * 0.8) : amount;
    float d = fbm(i.uv * scale, 4) - gone;
    s.alpha = d;      // below the cutout (0), the point isn't drawn
    s.cutout = 0.0;
    s.emission += edge_color * edge_glow * (1.0 - smoothstep(0.0, edge_width, d));
}
"""

CARTOON = """\
// Cartoon: flat steps of light from the sun and a dark outline (its own light, so lamps and
// shadows don't change it). The outline is where the surface turns away from the camera, so
// it suits rounded shapes; a flat face seen nearly edge-on darkens too.
setting int steps = 3;                        // Light steps, 1 .. 6
setting float shade = 0.35;                   // Darkest shade, 0 .. 1
setting color ink = vec3(0.04, 0.04, 0.06);   // Outline colour
setting float outline = 0.12;                 // Outline width, 0 .. 0.8

void surface(SurfaceInput i, inout Surface s) {
    float light = max(dot(s.normal, i.sun), 0.0);
    float stepped = floor(light * float(steps) + 0.5) / float(steps);
    vec3 flat_colour = s.color * mix(shade, 1.0, stepped);
    bool edge = dot(s.normal, i.view) < outline;
    s.emission = edge ? ink : flat_colour;
    s.color = vec3(0.0);   // (Midgard's lighting adds nothing to it)
    s.metallic = 0.0;
    s.roughness = 1.0;
}
"""

FORCE_FIELD = """\
// Force field: a glowing honeycomb you can see through, brightest at its edges, with ripples
// running over it.
setting color field_color = vec3(0.3, 0.7, 1.0);   // Colour
setting float strength = 2.5;                       // Strength, 0 .. 10
setting float cells = 12.0;                         // Cells across, 2 .. 40
setting float speed = 0.6;                          // Ripple speed, 0 .. 4

void surface(SurfaceInput i, inout Surface s) {
    vec2 g = i.uv * cells;
    vec2 r = vec2(1.0, 1.732);
    vec2 a = mod(g, r) - r * 0.5;
    vec2 b = mod(g - r * 0.5, r) - r * 0.5;
    vec2 cell = dot(a, a) < dot(b, b) ? a : b;
    float to_edge = max(abs(cell.x) * 0.866 + abs(cell.y) * 0.5, abs(cell.y));
    float lines = smoothstep(0.4, 0.48, to_edge);
    float edge = pow(1.0 - max(dot(s.normal, i.view), 0.0), 2.0);
    float wave = 0.5 + 0.5 * sin(i.uv.y * 20.0 - i.time * speed * 6.2832);
    s.color = vec3(0.0);
    s.emission = field_color * strength * (edge + lines * (0.3 + 0.7 * wave));
    s.alpha = lines + edge;   // see-through inside the cells
    s.cutout = 0.2;
}
"""

# name: (label, code)
STARTERS = {
    "blank": ("Blank", BLANK),
    "water": ("Water", WATER),
    "glow": ("Glow", GLOW),
    "dissolve": ("Dissolve", DISSOLVE),
    "cartoon": ("Cartoon", CARTOON),
    "force_field": ("Force field", FORCE_FIELD),
}
