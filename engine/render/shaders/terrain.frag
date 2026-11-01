// SPDX-License-Identifier: Apache-2.0
// Terrain (milestone 5.9): the ground's material, from each vertex's data. Grass from dry to
// lush with the moisture, sand on beaches and under shallow water, rock where the ground is
// steep, and snow above the snow line; the seasons dry the grass in summer, turn it in autumn
// and bring the snow down in winter. Fine detail breaks up each layer's edge.
//
// Each point is lit in its own frame: on a curved world the ground far round the curve (the far
// side of a habitat, overhead) faces another way, so the sun is turned with the ground (as if
// it had its own time of day there) and the ambient light comes from its own sky and ground.
// Shadows are looked up near the camera only, where the frame hasn't turned.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "atmosphere.glsl"

layout(push_constant) uniform Push {
    mat4 model;
    vec4 season;  // x snow cover (0 to 1), y autumn (-1 spring to 1), z dry grass (0 to 1),
                  // w up's frame (1 as it is; 2 to 7 an axis's frame, for a sphere)
    vec4 levels;  // x snow line, y sea level (metres)
    vec4 params;
} pc;

layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadow_map;

layout(location = 0) in vec3 v_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;      // x moisture (0 dry, 1 lush), y detail (-1 to 1)
layout(location = 3) in vec4 v_tangent;  // x: the temperature (degrees C, with a climate),
                                         // z: painted ground (-1 sand to 1 rock), w: the
                                         // height (metres)

layout(location = 0) out vec4 out_color;

vec3 linear(vec3 srgb) { return pow(srgb, vec3(2.2)); }

// "Up" at a point (camera-relative) from the world's shape (frame.terrain_body).
vec3 terrain_up(vec3 p) {
    const int shape = int(frame.terrain_body.w + 0.5);
    const vec3 c = frame.terrain_body.xyz;
    if (shape == 1) return normalize(vec3(c.xy - p.xy, 0.0));  // towards a cylinder's axis
    if (shape == 2) return normalize(p - c);                  // away from a planet's centre
    if (shape == 3) return normalize(c - p);                  // towards a hollow world's
    return vec3(0.0, 1.0, 0.0);
}

// How much of a biome's ground is here: 1 inside its ranges, fading out across their edges.
float in_range(float x, float lo, float hi, float edge) {
    return smoothstep(lo - edge, lo + edge, x) * (1.0 - smoothstep(hi - edge, hi + edge, x));
}

float sun_shadow(vec3 position, vec3 normal, vec3 sun) {
    const int count = int(frame.shadow_params.x);
    if (count == 0) return 1.0;
    const float depth = dot(frame.view_depth.xyz, position) + frame.view_depth.w;
    const float distance = frame.shadow_params.y;
    if (depth >= distance) return 1.0;
    int c = 0;
    while (c < count - 1 && depth > frame.shadow_splits[c]) ++c;
    const float grazing = 1.0 - clamp(dot(normal, sun), 0.0, 1.0);
    const vec3 p = position + normal * frame.shadow_texels[c] * (1.5 + 2.0 * grazing);
    const vec4 s = frame.shadow_matrices[c] * vec4(p, 1.0);
    const vec2 uv = s.xy * 0.5 + 0.5;
    const float step = frame.shadow_params.z;
    float lit;
    if (frame.shadow_params.w > 0.5) {
        lit = 0.0;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
                lit += texture(shadow_map, vec4(uv + vec2(x, y) * step, float(c), s.z));
        lit /= 9.0;
    } else {
        lit = texture(shadow_map, vec4(uv, float(c), s.z));
    }
    return mix(lit, 1.0, smoothstep(0.9 * distance, distance, depth));
}

// Turns v by the rotation that takes `from` to `to` (unit vectors).
vec3 turn(vec3 v, vec3 from, vec3 to) {
    const vec3 axis = cross(from, to);
    const float s = length(axis);
    const float c = dot(from, to);
    if (s < 1e-5) return c > 0.0 ? v : -v;
    const vec3 k = axis / s;
    return v * c + cross(k, v) * s + k * dot(k, v) * (1.0 - c);
}

void main() {
    const vec3 n = normalize(v_normal);
    const vec3 up = terrain_up(v_position);
    const float painted = clamp(v_tangent.z, -1.0, 1.0);
    const float slope = 1.0 - clamp(dot(n, up), 0.0, 1.0);  // 0 flat, 1 a wall
    const float height = v_tangent.w - pc.levels.y;           // above the sea
    const float moisture = clamp(v_uv.x, 0.0, 1.0);
    const float detail = clamp(v_uv.y, -1.0, 1.0);

    // Grass, from dry to lush, then the season's: drier in summer, turning in autumn.
    vec3 albedo = mix(linear(vec3(0.55, 0.50, 0.28)), linear(vec3(0.27, 0.42, 0.16)), moisture);
    // The world type's biomes, by temperature and moisture, blended across their edges.
    const float temperature = v_tangent.x + frame.terrain_climate.y;  // with the season
    const int biomes = int(frame.terrain_climate.x + 0.5);
    if (biomes > 0) {
        vec3 sum = vec3(0.0);
        float weight = 0.0;
        for (int i = 0; i < biomes; ++i) {
            const vec4 r = frame.biome_range[i];
            const float w = in_range(v_tangent.x + detail * 0.8, r.x, r.y, 1.5) *
                            in_range(moisture + detail * 0.03, r.z, r.w, 0.04);
            sum += mix(frame.biome_dry[i].rgb, frame.biome_color[i].rgb, moisture) * w;
            weight += w;
        }
        if (weight > 1e-3) albedo = mix(albedo, sum / weight, clamp(weight, 0.0, 1.0));
    }
    albedo = mix(albedo, linear(vec3(0.68, 0.60, 0.33)), pc.season.z * (1.0 - moisture) * 0.8);
    albedo = mix(albedo, linear(vec3(0.62, 0.42, 0.17)),
                 clamp(pc.season.y, 0.0, 1.0) * 0.6 * moisture);
    // Sand on the beach and under shallow water (darker as it gets deeper).
    const vec3 sand = linear(vec3(0.74, 0.68, 0.50));
    albedo = mix(albedo, sand, 1.0 - smoothstep(1.0, 2.5, height + detail * 0.5));
    albedo = mix(albedo, linear(vec3(0.36, 0.34, 0.26)), clamp(-height / 6.0, 0.0, 1.0));
    // Rock where it's steep.
    const vec3 rock = linear(vec3(0.42, 0.40, 0.37)) * (1.0 + detail * 0.15);
    albedo = mix(albedo, rock, smoothstep(0.22, 0.38, slope + detail * 0.04));
    // Painted sand and rock (sculpt and paint).
    albedo = mix(albedo, sand, smoothstep(0.0, 0.6, -painted + detail * 0.05));
    albedo = mix(albedo, rock, smoothstep(0.0, 0.6, painted + detail * 0.05));
    // Snow above the snow line, which comes down to the shore in deep winter; it doesn't lie
    // on the steepest rock.
    // With a climate, snow lies where it is below freezing (colder in winter) instead.
    const float line = mix(pc.levels.x, pc.levels.y + 2.0, clamp(pc.season.x, 0.0, 1.0));
    const float cold = frame.terrain_climate.z > 0.5
                           ? 1.0 - smoothstep(-1.0, 1.0, temperature + detail * 1.5)
                           : smoothstep(line - 3.0, line + 3.0, v_tangent.w + detail * 4.0);
    const float snow = cold * (1.0 - smoothstep(0.45, 0.6, slope)) * step(0.0, height);
    albedo = mix(albedo, linear(vec3(0.92, 0.94, 0.97)), snow);
    albedo *= 1.0 + detail * 0.08;

    // Light in this point's own frame.
    const vec3 camera_up = normalize(frame.up.xyz);
    const vec3 l = turn(to_sun(), camera_up, up);
    const float near_frame = smoothstep(0.97, 0.995, dot(camera_up, up));
    const float lit = mix(1.0, sun_shadow(v_position, n, l), near_frame);
    const float n_dot_l = max(dot(n, l), 0.0);
    vec3 color = albedo * frame.sun_color.rgb * n_dot_l * lit;
    // A little sheen on snow.
    const vec3 v = normalize(-v_position);
    const vec3 h = normalize(v + l);
    color += frame.sun_color.rgb * pow(max(dot(n, h), 0.0), 60.0) * 0.25 * snow * n_dot_l * lit;
    const vec3 ambient = mix(frame.ambient_ground.rgb, frame.ambient_sky.rgb,
                             clamp(dot(n, up) * 0.5 + 0.5, 0.0, 1.0));
    color += albedo * ambient;
    out_color = vec4(apply_fog(color, v_position), 1.0);
}
