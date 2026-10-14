// SPDX-License-Identifier: Apache-2.0
// CPU-side mesh data, independent of Vulkan.
#pragma once

#include <cstdint>
#include <vector>

namespace midgard::render {

struct Vertex {
    float position[3];
    float normal[3];
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;  // triangles, counter-clockwise when seen from outside
};

// A unit cube centred on the origin (side 1), with flat normals.
MeshData make_cube();

}  // namespace midgard::render
