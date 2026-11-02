// SPDX-License-Identifier: Apache-2.0
#include "midgard/gltf_skin.h"

#include <cgltf.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

#include <glm/gtc/type_ptr.hpp>

#include "midgard/pak.h"

namespace midgard {

namespace {

struct Freed {
    cgltf_data* data = nullptr;
    ~Freed() { cgltf_free(data); }
};

glm::mat4 world_of(const cgltf_node* node) {
    glm::mat4 m(1.0f);
    if (node) cgltf_node_transform_world(node, glm::value_ptr(m));
    return m;
}

SkinnedModel read(cgltf_data* data) {
    if (data->skins_count == 0) throw AssetError("the glTF file has no skin (no skeleton)");
    const cgltf_skin& skin = data->skins[0];
    SkinnedModel out;
    std::map<const cgltf_node*, int> index;
    for (cgltf_size j = 0; j < skin.joints_count; ++j) index[skin.joints[j]] = int(j);

    // The skeleton: each joint's parent (a joint, or none: a root) and rest pose.
    const cgltf_node* above = nullptr;  // what the roots hang from
    for (cgltf_size j = 0; j < skin.joints_count; ++j) {
        const cgltf_node* n = skin.joints[j];
        std::string name = n->name ? n->name : "";
        if (name.empty()) name = "joint_" + std::to_string(j);
        const auto parent = n->parent ? index.find(n->parent) : index.end();
        out.skeleton.names.push_back(name);
        out.skeleton.parents.push_back(parent == index.end() ? -1 : parent->second);
        if (parent == index.end() && n->parent) above = n->parent;
        JointTransform t;
        if (n->has_matrix) {  // (decomposed: glTF joints are TRS, but matrices are allowed)
            const glm::mat4 m = glm::make_mat4(n->matrix);
            t.translation = glm::vec3(m[3]);
            t.scale = {glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])),
                       glm::length(glm::vec3(m[2]))};
            t.rotation =
                glm::quat_cast(glm::mat3(glm::vec3(m[0]) / t.scale.x, glm::vec3(m[1]) / t.scale.y,
                                         glm::vec3(m[2]) / t.scale.z));
        } else {
            if (n->has_translation) t.translation = glm::make_vec3(n->translation);
            if (n->has_rotation)
                t.rotation =
                    glm::quat(n->rotation[3], n->rotation[0], n->rotation[1], n->rotation[2]);
            if (n->has_scale) t.scale = glm::make_vec3(n->scale);
        }
        out.skeleton.rest.push_back(t);
        glm::mat4 inverse(1.0f);
        if (skin.inverse_bind_matrices)
            cgltf_accessor_read_float(skin.inverse_bind_matrices, j, glm::value_ptr(inverse), 16);
        out.inverse_binds.push_back(inverse);
    }
    out.armature = world_of(above);

    // The clips: the channels that move joints.
    for (cgltf_size a = 0; a < data->animations_count; ++a) {
        const cgltf_animation& anim = data->animations[a];
        RawClip clip;
        clip.name = anim.name ? anim.name : "clip_" + std::to_string(a);
        std::map<int, RawTrack> tracks;
        for (cgltf_size c = 0; c < anim.channels_count; ++c) {
            const cgltf_animation_channel& ch = anim.channels[c];
            const auto it = index.find(ch.target_node);
            if (it == index.end() || !ch.sampler) continue;
            const cgltf_animation_sampler& s = *ch.sampler;
            const bool cubic = s.interpolation == cgltf_interpolation_type_cubic_spline;
            RawTrack& track = tracks[it->second];
            track.joint = out.skeleton.names[size_t(it->second)];
            for (cgltf_size k = 0; k < s.input->count; ++k) {
                float time = 0.0f;
                cgltf_accessor_read_float(s.input, k, &time, 1);
                clip.duration = std::max(clip.duration, time);
                const cgltf_size at = cubic ? k * 3 + 1 : k;  // (a cubic key's value)
                float v[4] = {0, 0, 0, 1};
                switch (ch.target_path) {
                    case cgltf_animation_path_type_translation:
                        cgltf_accessor_read_float(s.output, at, v, 3);
                        track.translations.push_back({time, {v[0], v[1], v[2]}});
                        break;
                    case cgltf_animation_path_type_rotation:
                        cgltf_accessor_read_float(s.output, at, v, 4);
                        track.rotations.push_back({time, glm::quat(v[3], v[0], v[1], v[2])});
                        break;
                    case cgltf_animation_path_type_scale:
                        cgltf_accessor_read_float(s.output, at, v, 3);
                        track.scales.push_back({time, {v[0], v[1], v[2]}});
                        break;
                    default:
                        break;
                }
            }
        }
        if (!(clip.duration > 0.0f)) clip.duration = 1.0f / 30.0f;  // (one pose)
        for (auto& [j, track] : tracks) clip.tracks.push_back(std::move(track));
        out.clips.push_back(std::move(clip));
    }

    // The skinned meshes.
    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        const cgltf_node& node = data->nodes[n];
        if (!node.mesh || node.skin != &skin) continue;
        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive& prim = node.mesh->primitives[p];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor *positions = nullptr, *normals = nullptr, *uvs = nullptr,
                                 *tangents = nullptr, *joints = nullptr, *weights = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& attr = prim.attributes[a];
                if (attr.type == cgltf_attribute_type_position) positions = attr.data;
                if (attr.type == cgltf_attribute_type_normal) normals = attr.data;
                if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uvs = attr.data;
                if (attr.type == cgltf_attribute_type_tangent) tangents = attr.data;
                if (attr.type == cgltf_attribute_type_joints && attr.index == 0) joints = attr.data;
                if (attr.type == cgltf_attribute_type_weights && attr.index == 0)
                    weights = attr.data;
            }
            if (!positions || !joints || !weights) continue;
            const auto base = static_cast<std::uint32_t>(out.mesh.vertices.size());
            for (cgltf_size i = 0; i < positions->count; ++i) {
                render::Vertex v{{0, 0, 0}, {0, 1, 0}, {0, 0}, {0, 0, 0, 0}};
                cgltf_accessor_read_float(positions, i, v.position, 3);
                if (normals) cgltf_accessor_read_float(normals, i, v.normal, 3);
                if (uvs) cgltf_accessor_read_float(uvs, i, v.uv, 2);
                if (tangents) cgltf_accessor_read_float(tangents, i, v.tangent, 4);
                out.mesh.vertices.push_back(v);
                cgltf_uint j[4] = {0, 0, 0, 0};
                cgltf_accessor_read_uint(joints, i, j, 4);
                glm::vec4 w(0.0f);
                cgltf_accessor_read_float(weights, i, glm::value_ptr(w), 4);
                const float sum = w.x + w.y + w.z + w.w;
                w = sum > 0.0f ? w / sum : glm::vec4(1, 0, 0, 0);
                for (int k = 0; k < 4; ++k)
                    if (j[k] >= skin.joints_count) {
                        j[k] = 0;
                        w[k] = 0.0f;
                    }
                out.joints.emplace_back(j[0], j[1], j[2], j[3]);
                out.weights.push_back(w);
            }
            if (prim.indices) {
                for (cgltf_size i = 0; i < prim.indices->count; ++i)
                    out.mesh.indices.push_back(
                        base +
                        static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, i)));
            } else {
                for (cgltf_size i = 0; i < positions->count; ++i)
                    out.mesh.indices.push_back(base + static_cast<std::uint32_t>(i));
            }
        }
    }
    if (out.mesh.indices.empty()) throw AssetError("the glTF file's skin has no triangles");
    return out;
}

}  // namespace

SkinnedModel load_skinned_glb(std::span<const std::uint8_t> bytes) {
    cgltf_options options{};
    Freed f;
    if (cgltf_parse(&options, bytes.data(), bytes.size(), &f.data) != cgltf_result_success)
        throw AssetError("not a valid glTF file");
    if (cgltf_load_buffers(&options, f.data, nullptr) != cgltf_result_success)
        throw AssetError("glTF buffers could not be loaded (external files are not supported)");
    return read(f.data);
}

SkinnedModel load_skinned_gltf_file(const std::filesystem::path& file) {
    cgltf_options options{};
    Freed f;
    const std::string path = file.string();
    if (cgltf_parse_file(&options, path.c_str(), &f.data) != cgltf_result_success)
        throw AssetError("not a valid glTF file: " + path);
    if (cgltf_load_buffers(&options, f.data, path.c_str()) != cgltf_result_success)
        throw AssetError("the glTF file's buffers could not be loaded: " + path);
    return read(f.data);
}

BuiltAnimation build_animation(const SkinnedModel& model) {
    BuiltAnimation out{Skeleton::build(model.skeleton), {}, {}};
    for (const std::string& name : model.skeleton.names)
        out.skin_to_skeleton.push_back(out.skeleton.find(name));
    for (const RawClip& c : model.clips) out.clips.push_back(Clip::build(out.skeleton, c));
    return out;
}

}  // namespace midgard
