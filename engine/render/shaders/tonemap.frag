// SPDX-License-Identifier: Apache-2.0
// HDR to the screen: exposure, then the highlight compression of Khronos PBR Neutral. Colours up
// to 0.76 stay exactly as authored (a designer's sky colour shows as picked); brighter ones roll
// off smoothly towards white instead of clipping. PBR Neutral's "toe" (a small offset that
// darkens very dark colours) is left out, because it visibly shifted dark sky colours. The
// output is linear; the sRGB framebuffer encodes it.
#version 450

layout(set = 0, binding = 0) uniform sampler2D hdr;
layout(push_constant) uniform Push {
    float exposure;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

// After https://github.com/KhronosGroup/ToneMapping/tree/main/PBR_Neutral (Apache-2.0)
vec3 compress_highlights(vec3 color) {
    const float start_compression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < start_compression) return color;
    const float d = 1.0 - start_compression;
    float new_peak = 1.0 - d * d / (peak + d - start_compression);
    color *= new_peak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return mix(color, new_peak * vec3(1.0), g);
}

void main() {
    vec3 color = texture(hdr, v_uv).rgb * pc.exposure;
    out_color = vec4(compress_highlights(color), 1.0);
}
