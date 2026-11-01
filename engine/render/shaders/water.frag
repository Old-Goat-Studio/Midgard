// SPDX-License-Identifier: Apache-2.0
// Water (milestone 5.9): a surface at sea level, blended over the scene. Its colour goes from
// the shallows' to the deep's with how deep the water is (each vertex carries the depth to the
// ground below it), and how much of the ground shows through fades the same way (clarity).
// Gentle waves bend its normal (a few travelling sine waves); the sky reflects off it more at
// grazing angles (Fresnel) and the sun glints. Where the depth comes to nothing, at the shore,
// it fades out over a little foam.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "atmosphere.glsl"

layout(push_constant) uniform Push {
    mat4 model;
    vec4 shallow;  // linear rgb, w: clarity (metres)
    vec4 deep;     // linear rgb, w: time (seconds)
    vec4 waves;    // x: wave strength (slope), y: the longest waves' length (metres)
} pc;

layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadow_map;

layout(location = 0) in vec3 v_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;      // metres across the face, over 16
layout(location = 3) in vec4 v_tangent;  // w: the water's depth here (metres)

layout(location = 0) out vec4 out_color;

const float kPi = 3.14159265358979;

// The waves' slope along the surface's two directions, at p (metres) and time t. A wave fades
// out where a pixel covers more than a sixth of its length (`footprint`: metres per pixel), so
// distant water doesn't shimmer in patterns.
vec2 wave_slope(vec2 p, float t, float footprint) {
    const float length0 = max(pc.waves.y, 0.5);
    vec2 slope = vec2(0.0);
    const vec2 dirs[4] = vec2[](vec2(0.8, 0.6), vec2(-0.45, 0.89), vec2(0.97, -0.24),
                                vec2(-0.7, -0.71));
    float wavelength = length0;
    for (int i = 0; i < 4; ++i) {
        const float k = 2.0 * kPi / wavelength;
        const float speed = sqrt(9.81 / k);  // deep-water waves
        const float phase = k * (dot(dirs[i], p) - speed * t);
        const float visible = 1.0 - smoothstep(wavelength / 12.0, wavelength / 6.0, footprint);
        slope += dirs[i] * cos(phase) * (visible / float(i + 1));
        wavelength *= 0.57;
    }
    return slope * pc.waves.x;
}

float shadow(vec3 position, vec3 normal, vec3 sun) {
    const int count = int(frame.shadow_params.x);
    if (count == 0) return 1.0;
    const float depth = dot(frame.view_depth.xyz, position) + frame.view_depth.w;
    if (depth >= frame.shadow_params.y) return 1.0;
    int c = 0;
    while (c < count - 1 && depth > frame.shadow_splits[c]) ++c;
    const vec3 p = position + normal * frame.shadow_texels[c] * 2.0;
    const vec4 s = frame.shadow_matrices[c] * vec4(p, 1.0);
    return texture(shadow_map, vec4(s.xy * 0.5 + 0.5, float(c), s.z));
}

void main() {
    const float depth = max(v_tangent.w, 0.0);
    const float clarity = max(pc.shallow.w, 0.05);
    vec3 n = normalize(v_normal);
    const vec3 v = normalize(-v_position);
    const bool below = dot(n, v) < 0.0;  // seen from under the water
    if (below) n = -n;

    // The surface's directions from how position and uv change across the screen.
    const vec3 dp1 = dFdx(v_position), dp2 = dFdy(v_position);
    const vec2 duv1 = dFdx(v_uv), duv2 = dFdy(v_uv);
    const float det = duv1.x * duv2.y - duv2.x * duv1.y;
    if (abs(det) > 1e-12 && !below) {
        vec3 t = (dp1 * duv2.y - dp2 * duv1.y) / det;
        vec3 b = (dp2 * duv1.x - dp1 * duv2.x) / det;
        t = normalize(t - n * dot(n, t));
        b = normalize(b - n * dot(n, b));
        const vec2 p = v_uv * 16.0;
        const float footprint = max(length(dFdx(p)), length(dFdy(p)));
        const vec2 slope = wave_slope(p, pc.deep.w, footprint);
        n = normalize(n - t * slope.x - b * slope.y);
    }

    // How much of the ground shows through, and the water's own colour over it.
    const float through = exp(-depth / clarity);
    const vec3 body = mix(pc.deep.rgb, pc.shallow.rgb, through);
    const vec3 l = to_sun();
    const float lit = shadow(v_position, normalize(v_normal), l);
    const float n_dot_l = max(dot(n, l), 0.0);
    const vec3 ambient = mix(frame.ambient_ground.rgb, frame.ambient_sky.rgb, 0.75);
    const vec3 diffuse = body * (frame.sun_color.rgb * n_dot_l * lit * 0.6 + ambient);

    // The sky reflected, more at grazing angles; the sun's glint.
    const float n_dot_v = max(dot(n, v), 0.0);
    const float fresnel = 0.02 + 0.98 * pow(1.0 - n_dot_v, 5.0);
    const vec3 r = reflect(-v, n);
    vec3 reflected = below ? vec3(0.0) : sky_radiance(r);
    const vec3 h = normalize(v + l);
    const float glint = pow(max(dot(n, h), 0.0), 400.0) * 40.0 * n_dot_l * lit;
    reflected += frame.sun_color.rgb * glint;

    vec3 color = mix(diffuse, reflected, fresnel);
    float alpha = max(1.0 - through * (1.0 - fresnel), fresnel);
    // The shore: a thin line of foam where the water meets the ground, fading out.
    const float foam = 1.0 - smoothstep(0.0, 0.25, depth);
    color = mix(color, ambient + frame.sun_color.rgb * 0.8 * lit, foam * 0.6);
    alpha = mix(alpha, 0.85, foam * 0.6) * smoothstep(0.0, 0.04, depth);
    out_color = vec4(apply_fog(color, v_position), clamp(alpha, 0.0, 1.0));
}
