// SPDX-License-Identifier: Apache-2.0
// The sky: one triangle over the screen at the far plane, so it shows only where nothing was
// drawn.
#version 450

layout(location = 0) out vec2 v_ndc;

void main() {
    v_ndc = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    gl_Position = vec4(v_ndc, 1.0, 1.0);
}
