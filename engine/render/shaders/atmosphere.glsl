// SPDX-License-Identifier: Apache-2.0
// The sky and fog (render/atmosphere.h), for the sky and mesh shaders. Needs frame.glsl.

vec3 to_sun() {
    return -normalize(frame.sun_direction.xyz);
}

// The sky seen along a direction: a gradient from the horizon (warmer on the sun's side) to the
// zenith, the ground colour below the horizon, the sun's glow (Henyey-Greenstein, 1 looking
// straight at the sun) and its disc.
vec3 sky_radiance(vec3 d) {
    const vec3 up = frame.up.xyz;
    const vec3 s = to_sun();
    const float mu = dot(d, up);
    const vec3 d_level = d - up * mu;
    const vec3 s_level = s - up * dot(s, up);
    float side = 0.5;
    if (dot(d_level, d_level) > 1e-8 && dot(s_level, s_level) > 1e-8)
        side = dot(normalize(d_level), normalize(s_level)) * 0.5 + 0.5;
    const vec3 horizon = mix(frame.sky_horizon_away.rgb, frame.sky_horizon.rgb, side * side);
    // The horizon's colour holds in a band about 10 degrees deep, as in the scattering model.
    vec3 color = mu >= 0.0 ? mix(horizon, frame.sky_zenith.rgb, 1.0 - exp(-6.0 * mu))
                           : mix(horizon, frame.sky_ground.rgb, smoothstep(0.0, 0.1, -mu));
    const float c = dot(d, s);
    const float g = frame.sun_glow.w;
    color += frame.sun_glow.rgb * pow((1.0 - g) * (1.0 - g) / (1.0 + g * g - 2.0 * g * c), 1.5);
    const float above = smoothstep(-0.002, 0.002, mu);  // the ground hides a setting sun
    color += frame.sun_disc.rgb * smoothstep(frame.sky_params.x, frame.sun_disc.w, c) * above;
    return color;
}

// Fog between the camera and a point `offset` from it: the optical depth (render::fog_depth).
float fog_depth(vec3 offset) {
    const float density = frame.fog_params.x;
    if (density <= 0.0) return 0.0;
    const float len = length(offset);
    const float k = frame.fog_params.y;
    if (k <= 0.0) return density * len;
    const float rise = dot(offset, frame.up.xyz) * k;
    const float base = density * exp(clamp(-k * frame.fog_params.z, -80.0, 80.0));
    const float spread =
        abs(rise) < 1e-4 ? 1.0 - 0.5 * rise : (1.0 - exp(min(-rise, 80.0))) / rise;
    return min(base * len * spread, 1e6);
}

// A colour seen through the fog; the fog is brighter towards the sun.
vec3 apply_fog(vec3 color, vec3 offset) {
    const float depth = fog_depth(offset);
    if (depth <= 0.0) return color;
    const float toward = max(dot(normalize(offset), to_sun()), 0.0);
    const vec3 fog =
        frame.fog_color.rgb + frame.sun_color.rgb * frame.fog_color.w * pow(toward, 8.0);
    return mix(fog, color, exp(-depth));
}
