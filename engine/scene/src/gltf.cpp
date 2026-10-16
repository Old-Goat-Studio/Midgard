// SPDX-License-Identifier: Apache-2.0
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include "midgard/gltf.h"
#include "midgard/pak.h"

namespace midgard {

render::MeshData load_glb(std::span<const uint8_t> bytes) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse(&options, bytes.data(), bytes.size(), &data) != cgltf_result_success)
        throw AssetError("not a valid glTF file");
    struct Free {
        cgltf_data* d;
        ~Free() { cgltf_free(d); }
    } guard{data};
    // For .glb files, buffers without a URI come from the embedded BIN chunk.
    if (cgltf_load_buffers(&options, data, nullptr) != cgltf_result_success)
        throw AssetError("glTF buffers could not be loaded (external files are not supported)");

    render::MeshData mesh;
    // Tangents are generated (MikkTSpace, as glTF recommends) when the file has uvs but none.
    bool all_tangents = true, all_uvs = true;
    for (cgltf_size m = 0; m < data->meshes_count; ++m) {
        const cgltf_mesh& gm = data->meshes[m];
        for (cgltf_size p = 0; p < gm.primitives_count; ++p) {
            const cgltf_primitive& prim = gm.primitives[p];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor* positions = nullptr;
            const cgltf_accessor* normals = nullptr;
            const cgltf_accessor* uvs = nullptr;
            const cgltf_accessor* tangents = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& attr = prim.attributes[a];
                if (attr.type == cgltf_attribute_type_position) positions = attr.data;
                if (attr.type == cgltf_attribute_type_normal) normals = attr.data;
                if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uvs = attr.data;
                if (attr.type == cgltf_attribute_type_tangent) tangents = attr.data;
            }
            if (!positions) continue;
            all_uvs = all_uvs && uvs;
            all_tangents = all_tangents && tangents;
            const auto base = static_cast<uint32_t>(mesh.vertices.size());
            for (cgltf_size i = 0; i < positions->count; ++i) {
                render::Vertex v{{0, 0, 0}, {0, 1, 0}, {0, 0}, {0, 0, 0, 0}};
                cgltf_accessor_read_float(positions, i, v.position, 3);
                if (normals) cgltf_accessor_read_float(normals, i, v.normal, 3);
                if (uvs) cgltf_accessor_read_float(uvs, i, v.uv, 2);
                if (tangents) cgltf_accessor_read_float(tangents, i, v.tangent, 4);
                mesh.vertices.push_back(v);
            }
            if (prim.indices) {
                for (cgltf_size i = 0; i < prim.indices->count; ++i)
                    mesh.indices.push_back(
                        base + static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i)));
            } else {
                for (cgltf_size i = 0; i < positions->count; ++i)
                    mesh.indices.push_back(base + static_cast<uint32_t>(i));
            }
        }
    }
    if (mesh.indices.empty()) throw AssetError("the glTF file contains no triangles");
    if (!all_tangents && all_uvs) render::generate_tangents(mesh);
    return mesh;
}

}  // namespace midgard
