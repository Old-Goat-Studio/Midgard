// SPDX-License-Identifier: Apache-2.0
// Meshes' surfaces and their lighting, shared by mesh.frag (Midgard's own materials) and surface
// shaders (milestone 5.17: someone's own code, between reading the surface and lighting it).
//
// glTF metal/roughness materials: GGX specular with Smith visibility and Schlick Fresnel, and
// Lambert diffuse, for the sun (with cascaded shadows) and up to 8 point or spot lights (glTF
// KHR_lights_punctual falloff), plus hemisphere ambient light between the sky and the ground
// along "up", what the weather left (snow, wetness), and fog.
// Brightness: a light of intensity 1 head-on lights a white matte surface to 1, so plain
// colours look as picked; that's pi times the physical radiance.
// (Needs frame.glsl and atmosphere.glsl before it.)

layout(push_constant) uniform Push {
    mat4 model;
    vec4 base_color;
    vec4 emissive;
    vec4 params;  // metallic, roughness, normal scale, occlusion strength
} pc;

layout(set = 1, binding = 0) uniform sampler2D base_color_map;          // sRGB
layout(set = 1, binding = 1) uniform sampler2D metallic_roughness_map;  // G roughness, B metal
layout(set = 1, binding = 2) uniform sampler2D normal_map;              // tangent space
layout(set = 1, binding = 3) uniform sampler2D occlusion_map;           // R
layout(set = 1, binding = 4) uniform sampler2D emissive_map;            // sRGB
layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadow_map;   // a layer per cascade

layout(location = 0) in vec3 v_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec4 v_tangent;

layout(location = 0) out vec4 out_color;

const float kPi = 3.14159265358979;

// What a surface shader reads (filled in before it is called).
struct SurfaceInput {
    vec2 uv;               // the mesh's texture coordinates
    vec3 position;         // the point, relative to the camera (metres, world axes)
    vec3 view;             // from the point towards the camera (unit length)
    vec3 geometry_normal;  // the mesh's own normal there, facing the camera (unit length)
    vec3 up;               // "up" at the camera (from the world's shape)
    float time;            // seconds (it repeats every hour)
};

// What a surface shader sets: each starts as the material's own (its colour, maps and
// settings), so a shader changes only what it means to.
struct Surface {
    vec3 color;       // its colour (linear), before light
    float alpha;      // with `cutout`: below it, the point isn't drawn
    float metallic;   // 0 .. 1
    float roughness;  // 0 (a mirror) .. 1 (chalk)
    vec3 normal;      // which way it faces (world axes, unit length): bumps bend it
    vec3 emission;    // light of its own (linear), added after the lighting
    float occlusion;  // 0 .. 1: how much ambient light reaches it
    float cutout;     // 0: everything is drawn
};

SurfaceInput surface_input() {
    SurfaceInput i;
    i.uv = v_uv;
    i.position = v_position;
    i.view = normalize(-v_position);  // the camera is at the origin
    const vec3 n = normalize(v_normal);
    i.geometry_normal = gl_FrontFacing ? n : -n;
    i.up = frame.up.xyz;
    i.time = frame.clock.x;
    return i;
}

// The material as Midgard's own: its colour, metal and roughness, normal, occlusion and
// emissive maps, times its settings.
Surface material_surface(SurfaceInput i) {
    Surface s;
    const vec4 base = texture(base_color_map, i.uv) * pc.base_color;
    const vec4 mr = texture(metallic_roughness_map, i.uv);
    s.color = base.rgb;
    s.alpha = base.a;
    s.metallic = clamp(pc.params.x * mr.b, 0.0, 1.0);
    s.roughness = clamp(pc.params.y * mr.g, 0.03, 1.0);
    // The normal, bent by the normal map where the mesh has tangents.
    vec3 n = normalize(v_normal);
    // (A tangent along the normal, from uvs that don't vary, has no direction left: no map.)
    const vec3 along = v_tangent.xyz - n * dot(n, v_tangent.xyz);
    if (dot(along, along) > 1e-6 * max(dot(v_tangent.xyz, v_tangent.xyz), 1e-8)) {
        const vec3 t = normalize(along);
        const vec3 b = cross(n, t) * v_tangent.w;
        vec3 m = texture(normal_map, i.uv).xyz * 2.0 - 1.0;
        m.xy *= pc.params.z;
        n = normalize(mat3(t, b, n) * m);
    }
    s.normal = gl_FrontFacing ? n : -n;
    s.occlusion = mix(1.0, texture(occlusion_map, i.uv).r, pc.params.w);
    s.emission = pc.emissive.rgb * texture(emissive_map, i.uv).rgb;
    s.cutout = 0.0;
    return s;
}

float distribution_ggx(float n_dot_h, float alpha) {
    const float a2 = alpha * alpha;
    const float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

float visibility_smith(float n_dot_v, float n_dot_l, float alpha) {  // height-correlated
    const float a2 = alpha * alpha;
    const float gv = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    const float gl = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

vec3 fresnel_schlick(float v_dot_h, vec3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - v_dot_h, 5.0);
}

// One light's contribution; `radiance` is the light's colour arriving at the point.
vec3 shade(vec3 n, vec3 v, vec3 l, vec3 radiance, vec3 albedo, vec3 f0, float metallic,
           float alpha) {
    const float n_dot_l = max(dot(n, l), 0.0);
    if (n_dot_l <= 0.0) return vec3(0.0);
    const vec3 h = normalize(v + l);
    const float n_dot_v = max(dot(n, v), 1e-4);
    const vec3 f = fresnel_schlick(max(dot(v, h), 0.0), f0);
    const vec3 specular = f * distribution_ggx(max(dot(n, h), 0.0), alpha) *
                          visibility_smith(n_dot_v, n_dot_l, alpha);
    const vec3 diffuse = (1.0 - f) * (1.0 - metallic) * albedo / kPi;
    return (diffuse + specular) * radiance * kPi * n_dot_l;
}

vec3 hemisphere(vec3 direction) {
    return mix(frame.ambient_ground.rgb, frame.ambient_sky.rgb,
               clamp(dot(direction, frame.up.xyz) * 0.5 + 0.5, 0.0, 1.0));
}

// How much sunlight reaches a point (0 in shadow, 1 lit): its cascade's map, 3 x 3 filtered
// lookups (one on the phone tier). The lookup moves off the surface along its normal by a texel
// and a half, more where the sun grazes it, so surfaces don't shadow themselves (acne). Shadows
// fade out over the last tenth of the shadow distance.
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

// Karis's analytic fit of the split-sum environment BRDF (Unreal Engine 4, mobile).
vec3 environment_brdf(vec3 f0, float roughness, float n_dot_v) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    const vec4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
    const vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

// The surface lit: the sun and its shadows, the lamps, ambient light, what the weather left,
// its own light, and fog.
vec4 light_surface(SurfaceInput i, Surface s) {
    const vec3 n = normalize(s.normal);
    const vec3 surface = i.geometry_normal;  // the geometry's own, for shadow lookups
    const vec3 v = i.view;
    vec3 albedo = s.color;
    float roughness = clamp(s.roughness, 0.03, 1.0);
    float metallic = clamp(s.metallic, 0.0, 1.0);
    // What the weather left: snow settled on what faces up (not on glowing surfaces: lamps,
    // the sun), and wetness, darker and smoother, most on what faces up.
    const float facing_up = smoothstep(0.35, 0.8, dot(surface, frame.up.xyz));
    const float glowing = step(1e-3, dot(s.emission, vec3(1.0)));
    const float snow = clamp(frame.ground.y * 1.2 - 0.1, 0.0, 1.0) * facing_up * (1.0 - glowing);
    albedo = mix(albedo, vec3(0.95, 0.96, 0.98), snow);
    roughness = mix(roughness, 0.75, snow);
    metallic *= 1.0 - snow;
    const float wet = frame.ground.x * (1.0 - snow) * (0.4 + 0.6 * facing_up) * (1.0 - glowing);
    albedo *= 1.0 - 0.35 * wet * (1.0 - metallic);
    roughness = mix(roughness, min(roughness, 0.12), 0.85 * wet);
    const float alpha = roughness * roughness;
    const vec3 f0 = mix(vec3(0.04), albedo, metallic);

    const vec3 l = to_sun();
    vec3 color = shade(n, v, l, frame.sun_color.rgb, albedo, f0, metallic, alpha) *
                 sun_shadow(i.position, surface, l);
    for (int k = 0; k < min(frame.counts.x, kMaxLocalLights); ++k) {
        const LocalLight light = frame.lights[k];
        const vec3 to_light = light.position_range.xyz - i.position;
        const float d2 = max(dot(to_light, to_light), 1e-4);
        const vec3 ll = to_light * inversesqrt(d2);
        // glTF's falloff: inverse square, faded smoothly to zero at the range.
        const float ratio = d2 / (light.position_range.w * light.position_range.w);
        float falloff = clamp(1.0 - ratio * ratio, 0.0, 1.0);
        falloff = falloff * falloff / d2;
        if (light.color.w > 0.5) {  // a spot: between the inner and outer cone
            const float cd = dot(normalize(light.direction.xyz), -ll);
            falloff *= smoothstep(light.direction.w, light.cone.x, cd);
        }
        color += shade(n, v, ll, light.color.rgb * falloff, albedo, f0, metallic, alpha);
    }

    // Ambient: diffuse from the hemisphere, specular reflected off it; occlusion darkens both.
    const float n_dot_v = max(dot(n, v), 1e-4);
    const vec3 diffuse_ambient = hemisphere(n) * albedo * (1.0 - metallic);
    const vec3 specular_ambient =
        hemisphere(reflect(-v, n)) * environment_brdf(f0, roughness, n_dot_v);
    color += (diffuse_ambient + specular_ambient) * clamp(s.occlusion, 0.0, 1.0);

    color += max(s.emission, vec3(0.0));
    return vec4(apply_fog(color, i.position), s.alpha);
}

// ---- helpers for surface shaders -----------------------------------------------------------
// A repeatable pseudo-random number in 0 .. 1 for a point (the same everywhere, every frame).
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

// Smooth value noise in 0 .. 1: hills and valleys about a unit apart.
float noise(vec2 p) {
    const vec2 i = floor(p);
    const vec2 f = fract(p);
    const vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x),
               mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

// Noise in layers (fractal Brownian motion): `octaves` of it, each finer and fainter; in
// 0 .. 1 like noise() (mostly near the middle).
float fbm(vec2 p, int octaves) {
    float sum = 0.0, amplitude = 0.5, total = 0.0;
    for (int k = 0; k < octaves; ++k) {
        sum += amplitude * noise(p);
        total += amplitude;
        p *= 2.0;
        amplitude *= 0.5;
    }
    return total > 0.0 ? sum / total : 0.0;
}

// A colour picked in sRGB (as the editor shows them) made linear, as surfaces use them.
vec3 srgb(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}
