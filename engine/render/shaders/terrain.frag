// SPDX-License-Identifier: Apache-2.0
// Terrain (milestone 5.9): the ground's material, from each vertex's data. Grass from dry to
// lush with the moisture, sand on beaches and under shallow water, rock where the ground is
// steep, and snow above the snow line; the seasons dry the grass in summer, turn it in autumn
// and bring the snow down in winter. Fine detail breaks up each layer's edge.
//
// Picture textures (milestone 6.0b): each layer (grass, dry grass, sand, rock, snow, each
// biome's lush and dry ground) can have a texture set's maps, read from texture arrays (a slice
// per set) triplanar: along the world's three axes, blended by how the ground faces, so they lay
// the same on every world shape with no stretching on slopes and no seams at the cube-sphere's
// faces. A textured layer is a material (its colour, normal, metal and roughness, ambient
// occlusion and glow, lit as meshes are: GGX); the layers blend exactly where the colours did.
// Ground with no textures is drawn exactly as before.
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
// The ground's maps (6.0b), a slice per texture set, in a material's order.
layout(set = 1, binding = 0) uniform sampler2DArray ground_color;       // sRGB
layout(set = 1, binding = 1) uniform sampler2DArray ground_mr;          // G rough, B metal
layout(set = 1, binding = 2) uniform sampler2DArray ground_normal;      // tangent space (GL)
layout(set = 1, binding = 3) uniform sampler2DArray ground_occlusion;   // R
layout(set = 1, binding = 4) uniform sampler2DArray ground_emissive;    // sRGB

const float kPi = 3.14159265;
const int kGrass = 0, kDryGrass = 1, kSand = 2, kRock = 3, kSnow = 4;

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

// ---- the ground's layers as materials (6.0b) ------------------------------------------------
struct Ground {
    vec3 albedo;     // linear
    vec3 normal;     // the shading normal (camera-relative world frame)
    float roughness;
    float metallic;
    float occlusion;
    vec3 emission;   // linear
    float pbr;       // 0: lit as the 5.9 ground; 1: as a material (picture textures)
};

Ground mixg(Ground a, Ground b, float t) {
    return Ground(mix(a.albedo, b.albedo, t), mix(a.normal, b.normal, t),
                  mix(a.roughness, b.roughness, t), mix(a.metallic, b.metallic, t),
                  mix(a.occlusion, b.occlusion, t), mix(a.emission, b.emission, t),
                  mix(a.pbr, b.pbr, t));
}

// Set in main, where every pixel is still together: the triplanar weights and the position's
// screen derivatives (the layers are read inside branches, where derivatives aren't defined).
vec3 tri_w;
vec3 dp_dx, dp_dy;

// A map read triplanar: its three projections (onto the planes facing X, Y and Z), weighted.
vec4 tri(sampler2DArray t, vec3 p, float slice, float scale) {
    vec4 c = vec4(0.0);
    if (tri_w.x > 0.0)
        c += textureGrad(t, vec3(p.zy, slice), dp_dx.zy * scale, dp_dy.zy * scale) * tri_w.x;
    if (tri_w.y > 0.0)
        c += textureGrad(t, vec3(p.xz, slice), dp_dx.xz * scale, dp_dy.xz * scale) * tri_w.y;
    if (tri_w.z > 0.0)
        c += textureGrad(t, vec3(p.xy, slice), dp_dx.xy * scale, dp_dy.xy * scale) * tri_w.z;
    return c;
}

// A layer: its colour (and flat, rough ground) or, with a texture set, its maps.
Ground layer(int slot, vec3 colour, vec3 n) {
    Ground g = Ground(colour, n, 0.9, 0.0, 1.0, vec3(0.0), 0.0);
    const vec4 info = frame.ground_layer[slot];
    const int maps = int(info.z + 0.5);
    if (maps == 0) return g;
    const float scale = info.x;  // 1 / the tile
    const vec3 p = (v_position + frame.ground_offset[slot].xyz) * scale;
    const float slice = info.y;
    if ((maps & 1) != 0) g.albedo = tri(ground_color, p, slice, scale).rgb;
    if ((maps & 2) != 0) {
        const vec4 mr = tri(ground_mr, p, slice, scale);
        g.roughness = mr.g;
        g.metallic = mr.b;
    }
    if ((maps & 8) != 0) g.occlusion = tri(ground_occlusion, p, slice, scale).r;
    if ((maps & 16) != 0) g.emission = tri(ground_emissive, p, slice, scale).rgb;
    if ((maps & 4) != 0) {
        // Each projection's bumps along its own axes (u right, the picture's up against v),
        // added to the ground's normal and blended as the colours are.
        vec3 bend = vec3(0.0);
        if (tri_w.x > 0.0) {
            const vec2 t = textureGrad(ground_normal, vec3(p.zy, slice), dp_dx.zy * scale,
                                       dp_dy.zy * scale).xy * 2.0 - 1.0;
            bend += vec3(0.0, -t.y, t.x) * tri_w.x;
        }
        if (tri_w.y > 0.0) {
            const vec2 t = textureGrad(ground_normal, vec3(p.xz, slice), dp_dx.xz * scale,
                                       dp_dy.xz * scale).xy * 2.0 - 1.0;
            bend += vec3(t.x, 0.0, -t.y) * tri_w.y;
        }
        if (tri_w.z > 0.0) {
            const vec2 t = textureGrad(ground_normal, vec3(p.xy, slice), dp_dx.xy * scale,
                                       dp_dy.xy * scale).xy * 2.0 - 1.0;
            bend += vec3(t.x, -t.y, 0.0) * tri_w.z;
        }
        g.normal = normalize(n + bend);
    }
    g.pbr = 1.0;
    return g;
}

// A layer laid over the ground so far by t (none read where t is 0).
Ground over(Ground g, int slot, vec3 colour, vec3 n, float t) {
    return t > 0.0 ? mixg(g, layer(slot, colour, n), t) : g;
}

float distribution_ggx(float n_dot_h, float alpha) {
    const float a2 = alpha * alpha;
    const float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

float visibility_smith(float n_dot_v, float n_dot_l, float alpha) {
    const float a2 = alpha * alpha;
    const float gv = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    const float gl = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

vec3 fresnel_schlick(float v_dot_h, vec3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - v_dot_h, 5.0);
}

vec3 environment_brdf(vec3 f0, float roughness, float n_dot_v) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    const vec4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
    const vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
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
    // Triplanar: sharpened weights by how the ground faces each axis.
    const vec3 aw = pow(abs(n), vec3(4.0));
    tri_w = aw / (aw.x + aw.y + aw.z);
    tri_w = max(tri_w - 0.02, 0.0);  // (none read where a projection hardly counts)
    tri_w /= tri_w.x + tri_w.y + tri_w.z;
    dp_dx = dFdx(v_position);
    dp_dy = dFdy(v_position);

    // Grass, from dry to lush, then the season's: drier in summer, turning in autumn.
    Ground g = moisture < 1.0 ? layer(kDryGrass, linear(vec3(0.55, 0.50, 0.28)), n)
                              : layer(kGrass, linear(vec3(0.27, 0.42, 0.16)), n);
    g = over(g, kGrass, linear(vec3(0.27, 0.42, 0.16)), n, moisture < 1.0 ? moisture : 0.0);
    // The world type's biomes, by temperature and moisture, blended across their edges.
    const float temperature = v_tangent.x + frame.terrain_climate.y;  // with the season
    const int biomes = int(frame.terrain_climate.x + 0.5);
    if (biomes > 0) {
        Ground sum = Ground(vec3(0.0), vec3(0.0), 0.0, 0.0, 0.0, vec3(0.0), 0.0);
        float weight = 0.0;
        for (int i = 0; i < biomes; ++i) {
            const vec4 r = frame.biome_range[i];
            const float w = in_range(v_tangent.x + detail * 0.8, r.x, r.y, 1.5) *
                            in_range(moisture + detail * 0.03, r.z, r.w, 0.04);
            if (w <= 0.0) continue;
            const Ground b =
                mixg(layer(5 + 2 * i + 1, frame.biome_dry[i].rgb, n),
                     moisture > 0.0 ? layer(5 + 2 * i, frame.biome_color[i].rgb, n)
                                    : layer(5 + 2 * i + 1, frame.biome_dry[i].rgb, n),
                     moisture);
            sum.albedo += b.albedo * w;
            sum.normal += b.normal * w;
            sum.roughness += b.roughness * w;
            sum.metallic += b.metallic * w;
            sum.occlusion += b.occlusion * w;
            sum.emission += b.emission * w;
            sum.pbr += b.pbr * w;
            weight += w;
        }
        if (weight > 1e-3) {
            const Ground mean = Ground(sum.albedo / weight, sum.normal / weight,
                                       sum.roughness / weight, sum.metallic / weight,
                                       sum.occlusion / weight, sum.emission / weight,
                                       sum.pbr / weight);
            g = mixg(g, mean, clamp(weight, 0.0, 1.0));
        }
    }
    g.albedo = mix(g.albedo, linear(vec3(0.68, 0.60, 0.33)),
                   pc.season.z * (1.0 - moisture) * 0.8);
    g.albedo = mix(g.albedo, linear(vec3(0.62, 0.42, 0.17)),
                   clamp(pc.season.y, 0.0, 1.0) * 0.6 * moisture);
    // Sand on the beach and under shallow water (darker as it gets deeper).
    const vec3 sand = linear(vec3(0.74, 0.68, 0.50));
    g = over(g, kSand, sand, n, 1.0 - smoothstep(1.0, 2.5, height + detail * 0.5));
    g.albedo = mix(g.albedo, linear(vec3(0.36, 0.34, 0.26)), clamp(-height / 6.0, 0.0, 1.0));
    // Rock where it's steep.
    const vec3 rock = linear(vec3(0.42, 0.40, 0.37)) * (1.0 + detail * 0.15);
    g = over(g, kRock, rock, n, smoothstep(0.22, 0.38, slope + detail * 0.04));
    // Painted sand and rock (sculpt and paint).
    g = over(g, kSand, sand, n, smoothstep(0.0, 0.6, -painted + detail * 0.05));
    g = over(g, kRock, rock, n, smoothstep(0.0, 0.6, painted + detail * 0.05));
    // Snow above the snow line, which comes down to the shore in deep winter; it doesn't lie
    // on the steepest rock.
    // With a climate, snow lies where it is below freezing (colder in winter) instead.
    const float line = mix(pc.levels.x, pc.levels.y + 2.0, clamp(pc.season.x, 0.0, 1.0));
    const float cold = frame.terrain_climate.z > 0.5
                           ? 1.0 - smoothstep(-1.0, 1.0, temperature + detail * 1.5)
                           : smoothstep(line - 3.0, line + 3.0, v_tangent.w + detail * 4.0);
    // And the snow the weather left (milestone 5.15), patchy as it starts and as it melts.
    const float settled = smoothstep(0.0, 0.35, frame.ground.y * 1.3 - 0.3 + detail * 0.25);
    const float snow = max(cold, settled) * (1.0 - smoothstep(0.45, 0.6, slope)) *
                       step(0.0, height);
    g = over(g, kSnow, linear(vec3(0.92, 0.94, 0.97)), n, snow);
    g.albedo *= 1.0 + detail * 0.08;
    // Wet ground after rain: darker, with the sky and sun shining on it.
    const float wet = frame.ground.x * (1.0 - snow) * step(0.0, height);
    g.albedo *= 1.0 - 0.4 * wet;
    const vec3 albedo = g.albedo;

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
    const float fresnel = 0.04 + 0.96 * pow(1.0 - max(dot(n, v), 0.0), 5.0);
    color += (frame.sun_color.rgb * pow(max(dot(n, h), 0.0), 200.0) * 2.0 * n_dot_l * lit +
              frame.sky_horizon.rgb * fresnel * 0.6) * wet;
    const vec3 ambient = mix(frame.ambient_ground.rgb, frame.ambient_sky.rgb,
                             clamp(dot(n, up) * 0.5 + 0.5, 0.0, 1.0));
    color += albedo * ambient;
    // Textured ground: lit as a material, with its own normal, shine, occlusion and glow.
    if (g.pbr > 0.0) {
        const vec3 sn = normalize(g.normal);
        const float roughness = clamp(mix(g.roughness, min(g.roughness, 0.12), 0.85 * wet),
                                      0.03, 1.0);
        const float metallic = clamp(g.metallic, 0.0, 1.0);
        const float alpha = roughness * roughness;
        const vec3 f0 = mix(vec3(0.04), albedo, metallic);
        const float occlusion = clamp(g.occlusion, 0.0, 1.0);
        const float sn_dot_l = max(dot(sn, l), 0.0) * step(0.0, dot(n, l));
        const float sn_dot_v = max(dot(sn, v), 1e-4);
        const vec3 sh = normalize(v + l);
        const vec3 f = fresnel_schlick(max(dot(v, sh), 0.0), f0);
        const vec3 specular = f * distribution_ggx(max(dot(sn, sh), 0.0), alpha) *
                              visibility_smith(sn_dot_v, sn_dot_l, alpha);
        const vec3 diffuse = (1.0 - f) * (1.0 - metallic) * albedo / kPi;
        vec3 pbr = (diffuse + specular) * frame.sun_color.rgb * kPi * sn_dot_l * lit;
        const vec3 sky = mix(frame.ambient_ground.rgb, frame.ambient_sky.rgb,
                             clamp(dot(sn, up) * 0.5 + 0.5, 0.0, 1.0));
        const vec3 r = reflect(-v, sn);
        const vec3 sky_r = mix(frame.ambient_ground.rgb, frame.ambient_sky.rgb,
                               clamp(dot(r, up) * 0.5 + 0.5, 0.0, 1.0));
        pbr += (sky * albedo * (1.0 - metallic) +
                sky_r * environment_brdf(f0, roughness, sn_dot_v)) * occlusion;
        pbr += max(g.emission, vec3(0.0));
        color = mix(color, pbr, clamp(g.pbr, 0.0, 1.0));
    }
    out_color = vec4(apply_fog(color, v_position), 1.0);
}
