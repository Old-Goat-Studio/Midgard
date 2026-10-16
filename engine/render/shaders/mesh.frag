// SPDX-License-Identifier: Apache-2.0
// glTF metal/roughness materials: GGX specular with Smith visibility and Schlick Fresnel, and
// Lambert diffuse, for the sun (with cascaded shadows) and up to 8 point or spot lights (glTF
// KHR_lights_punctual falloff), plus hemisphere ambient light between the sky and the ground
// along "up", and fog.
// Brightness: a light of intensity 1 head-on lights a white matte surface to 1, so plain
// colours look as picked; that's pi times the physical radiance.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "atmosphere.glsl"

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
// lookups (one on the phone tier). The lookup moves off the surface along its normal by a texel and a half, more where
// the sun grazes it, so surfaces don't shadow themselves (acne). Shadows fade out over the last
// tenth of the shadow distance.
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

void main() {
    const vec4 base = texture(base_color_map, v_uv) * pc.base_color;
    const vec4 mr = texture(metallic_roughness_map, v_uv);
    const float metallic = clamp(pc.params.x * mr.b, 0.0, 1.0);
    const float roughness = clamp(pc.params.y * mr.g, 0.03, 1.0);
    const float alpha = roughness * roughness;

    // The normal, bent by the normal map where the mesh has tangents.
    vec3 n = normalize(v_normal);
    const vec3 surface = gl_FrontFacing ? n : -n;  // the geometry's own, for shadow lookups
    if (dot(v_tangent.xyz, v_tangent.xyz) > 1e-8) {
        const vec3 t = normalize(v_tangent.xyz - n * dot(n, v_tangent.xyz));
        const vec3 b = cross(n, t) * v_tangent.w;
        vec3 m = texture(normal_map, v_uv).xyz * 2.0 - 1.0;
        m.xy *= pc.params.z;
        n = normalize(mat3(t, b, n) * m);
    }
    if (!gl_FrontFacing) n = -n;

    const vec3 v = normalize(-v_position);  // the camera is at the origin
    const vec3 albedo = base.rgb;
    const vec3 f0 = mix(vec3(0.04), albedo, metallic);

    const vec3 l = to_sun();
    vec3 color = shade(n, v, l, frame.sun_color.rgb, albedo, f0, metallic, alpha) *
                 sun_shadow(v_position, surface, l);
    for (int i = 0; i < min(frame.counts.x, kMaxLocalLights); ++i) {
        const LocalLight light = frame.lights[i];
        const vec3 to_light = light.position_range.xyz - v_position;
        const float d2 = max(dot(to_light, to_light), 1e-4);
        const vec3 l = to_light * inversesqrt(d2);
        // glTF's falloff: inverse square, faded smoothly to zero at the range.
        const float ratio = d2 / (light.position_range.w * light.position_range.w);
        float falloff = clamp(1.0 - ratio * ratio, 0.0, 1.0);
        falloff = falloff * falloff / d2;
        if (light.color.w > 0.5) {  // a spot: between the inner and outer cone
            const float cd = dot(normalize(light.direction.xyz), -l);
            falloff *= smoothstep(light.direction.w, light.cone.x, cd);
        }
        color += shade(n, v, l, light.color.rgb * falloff, albedo, f0, metallic, alpha);
    }

    // Ambient: diffuse from the hemisphere, specular reflected off it; occlusion darkens both.
    const float n_dot_v = max(dot(n, v), 1e-4);
    const float occlusion = mix(1.0, texture(occlusion_map, v_uv).r, pc.params.w);
    const vec3 diffuse_ambient = hemisphere(n) * albedo * (1.0 - metallic);
    const vec3 specular_ambient =
        hemisphere(reflect(-v, n)) * environment_brdf(f0, roughness, n_dot_v);
    color += (diffuse_ambient + specular_ambient) * occlusion;

    color += pc.emissive.rgb * texture(emissive_map, v_uv).rgb;
    out_color = vec4(apply_fog(color, v_position), base.a);
}
