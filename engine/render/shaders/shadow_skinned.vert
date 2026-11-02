// SPDX-License-Identifier: Apache-2.0
// The shadow pass for skinned meshes: shadow.vert with mesh_skinned.vert's skinning.
#version 450

layout(push_constant) uniform Push {
    mat4 model_light;  // model, then the cascade's light view and projection
    uint first;        // the draw's first joint in the palette
} pc;

layout(set = 0, binding = 2) readonly buffer Palette {
    mat4 joints[];
} palette;

layout(location = 0) in vec3 in_position;
layout(location = 4) in uvec4 in_joints;
layout(location = 5) in vec4 in_weights;

void main() {
    const mat4 skin = in_weights.x * palette.joints[pc.first + in_joints.x] +
                      in_weights.y * palette.joints[pc.first + in_joints.y] +
                      in_weights.z * palette.joints[pc.first + in_joints.z] +
                      in_weights.w * palette.joints[pc.first + in_joints.w];
    gl_Position = pc.model_light * skin * vec4(in_position, 1.0);
}
