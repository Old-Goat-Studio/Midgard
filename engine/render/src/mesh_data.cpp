// SPDX-License-Identifier: Apache-2.0
#include "midgard/render/mesh_data.h"

namespace midgard::render {

MeshData make_cube() {
    // Each face: outward normal, then two axes (u, v) with u × v = normal, so the corners
    // (-u-v, +u-v, +u+v, -u+v) run counter-clockwise when seen from outside.
    struct Face {
        float n[3], u[3], v[3];
    };
    static const Face faces[] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };
    static const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};

    MeshData mesh;
    for (const Face& f : faces) {
        const auto base = static_cast<uint32_t>(mesh.vertices.size());
        for (const auto& c : corners) {
            Vertex vtx{};
            for (int i = 0; i < 3; ++i) {
                vtx.position[i] = 0.5f * (f.n[i] + c[0] * f.u[i] + c[1] * f.v[i]);
                vtx.normal[i] = f.n[i];
            }
            mesh.vertices.push_back(vtx);
        }
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(base + i);
    }
    return mesh;
}

}  // namespace midgard::render
