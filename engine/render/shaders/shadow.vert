// SPDX-License-Identifier: Apache-2.0
// The shadow pass: positions into one cascade of the sun's shadow map (depth only).
#version 450

layout(push_constant) uniform Push {
    mat4 model_light;  // model, then the cascade's light view and projection
} pc;

layout(location = 0) in vec3 in_position;

void main() {
    gl_Position = pc.model_light * vec4(in_position, 1.0);
}
