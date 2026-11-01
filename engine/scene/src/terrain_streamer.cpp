// SPDX-License-Identifier: Apache-2.0
#include "midgard/terrain_streamer.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

#include <glm/gtc/matrix_transform.hpp>

namespace midgard {

namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr size_t kUploadsPerUpdate = 24;  // chunk meshes sent to the GPU in one update
constexpr size_t kMaxJobs = 32;           // chunks being generated at once

struct KeyLess {
    bool operator()(const ChunkKey& a, const ChunkKey& b) const {
        return std::tie(a.face, a.level, a.x, a.y) < std::tie(b.face, b.level, b.x, b.y);
    }
};
using KeySet = std::set<ChunkKey, KeyLess>;

render::Vertex vertex(const glm::vec3& p, const glm::vec3& n, const glm::vec2& uv) {
    render::Vertex v{};
    v.position[0] = p.x;
    v.position[1] = p.y;
    v.position[2] = p.z;
    v.normal[0] = n.x;
    v.normal[1] = n.y;
    v.normal[2] = n.z;
    v.uv[0] = uv.x;
    v.uv[1] = uv.y;
    v.tangent[0] = 1.0f;
    v.tangent[3] = 1.0f;
    return v;
}

// A disc of radius r in the XY plane, facing +Z (facing = 1) or -Z (-1).
render::MeshData disc(double r, float facing) {
    constexpr int segments = 128;
    render::MeshData m;
    const glm::vec3 n(0.0f, 0.0f, facing);
    m.vertices.push_back(vertex(glm::vec3(0.0f), n, glm::vec2(0.0f)));
    for (int i = 0; i <= segments; ++i) {
        const double a = kTwoPi * i / segments;
        const glm::vec3 p(float(r * std::sin(a)), float(-r * std::cos(a)), 0.0f);
        m.vertices.push_back(vertex(p, n, glm::vec2(p.x, p.y) / 16.0f));
    }
    for (std::uint32_t i = 1; i <= segments; ++i) {
        if (facing > 0)
            m.indices.insert(m.indices.end(), {0u, i, i + 1});
        else
            m.indices.insert(m.indices.end(), {0u, i + 1, i});
    }
    return m;
}

// A wall across x from -w/2 to w/2 and up from `low` to `high`, facing +Z or -Z.
render::MeshData wall(double w, double low, double high, float facing) {
    render::MeshData m;
    const glm::vec3 n(0.0f, 0.0f, facing);
    const float x0 = float(-w / 2), x1 = float(w / 2), y0 = float(low), y1 = float(high);
    m.vertices = {
        vertex({x0, y0, 0}, n, {x0 / 16, y0 / 16}), vertex({x1, y0, 0}, n, {x1 / 16, y0 / 16}),
        vertex({x0, y1, 0}, n, {x0 / 16, y1 / 16}), vertex({x1, y1, 0}, n, {x1 / 16, y1 / 16})};
    if (facing > 0)
        m.indices = {0, 1, 2, 2, 1, 3};
    else
        m.indices = {0, 2, 1, 1, 2, 3};
    return m;
}

}  // namespace

glm::vec2 octahedral(const glm::vec3& n) {
    const float sum = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    glm::vec2 p = glm::vec2(n.x, n.y) / std::max(sum, 1e-20f);
    if (n.z < 0.0f) {
        const glm::vec2 s(p.x >= 0.0f ? 1.0f : -1.0f, p.y >= 0.0f ? 1.0f : -1.0f);
        p = (glm::vec2(1.0f) - glm::abs(glm::vec2(p.y, p.x))) * s;
    }
    return p;
}

int up_frame(const glm::dvec3& up) {
    int axis = 0;
    for (int a = 1; a < 3; ++a)
        if (std::abs(up[a]) > std::abs(up[axis])) axis = a;
    return axis * 2 + (up[axis] < 0.0 ? 1 : 0);
}

glm::vec3 in_up_frame(const glm::vec3& v, int frame) {
    if (frame < 0) return v;
    const int axis = frame / 2;
    const float sign = frame % 2 == 1 ? -1.0f : 1.0f;
    return {v[(axis + 1) % 3], v[(axis + 2) % 3], sign * v[axis]};
}

render::MeshData ground_mesh_data(const ChunkMesh& mesh, int frame) {
    render::MeshData out;
    out.vertices.reserve(mesh.positions.size());
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        const glm::vec3 b = i < mesh.biome.size() ? mesh.biome[i] : glm::vec3(0.5f, 0.0f, 0.0f);
        render::Vertex v = vertex(mesh.positions[i], mesh.normals[i], glm::vec2(b.x, b.y));
        const glm::vec2 up =
            octahedral(in_up_frame(i < mesh.ups.size() ? mesh.ups[i] : glm::vec3(0, 1, 0), frame));
        v.tangent[0] = up.x;
        v.tangent[1] = up.y;
        v.tangent[2] = b.z;  // painted ground: -1 sand to 1 rock
        v.tangent[3] = float(mesh.heights[i]);
        out.vertices.push_back(v);
    }
    out.indices = mesh.indices;
    return out;
}

// round: on a sphere, where the waves come from each point's place in the body (tangent xyz)
// rather than its face's uv, which doesn't meet the next face's at the edge.
render::MeshData water_mesh_data(const ChunkMesh& mesh, bool round) {
    render::MeshData out = chunk_mesh_data(mesh);
    for (size_t i = 0; i < out.vertices.size(); ++i) {  // the depth below, for the water shader
        render::Vertex& v = out.vertices[i];
        v.tangent[3] = float(mesh.heights[i]);
        if (round) {
            const glm::dvec3 p = mesh.origin + glm::dvec3(mesh.positions[i]);
            v.tangent[0] = float(p.x);
            v.tangent[1] = float(p.y);
            v.tangent[2] = float(p.z);
        }
    }
    return out;
}

render::MeshData chunk_mesh_data(const ChunkMesh& mesh) {
    render::MeshData out;
    out.vertices.reserve(mesh.positions.size());
    for (size_t i = 0; i < mesh.positions.size(); ++i)
        out.vertices.push_back(vertex(mesh.positions[i], mesh.normals[i],
                                      i < mesh.uvs.size() ? mesh.uvs[i] : glm::vec2(0.0f)));
    out.indices = mesh.indices;
    return out;
}

// A chunk's ground, and the water over it (if any of it is below the sea).
struct ChunkMeshes {
    render::MeshData ground;
    std::optional<render::MeshData> water;
};

struct TerrainStreamer::Entry {
    TerrainLeaf leaf;
    JobHandle job;                       // generating
    std::shared_ptr<ChunkMeshes> made;   // written by the job, read once it is done
    std::optional<ChunkMeshes> data;     // generated, not yet on the GPU
    std::optional<render::MeshId> mesh;  // on the GPU
    std::optional<render::MeshId> water;
    bool stale = false;  // the stamps changed it: made again, its old meshes drawn till then
    std::uint64_t last_wanted = 0;
    bool ready(bool gpu) const { return gpu ? mesh.has_value() : data.has_value() || mesh; }
};

TerrainStreamer::TerrainStreamer(const Body& body, const TerrainSettings& settings, JobSystem* jobs)
    : terrain_(body, settings), jobs_(jobs) {}

TerrainStreamer::~TerrainStreamer() {
    for (auto& [key, e] : cache_)
        if (e->job && jobs_) jobs_->wait(e->job);
}

TerrainStreamer::MeshKey TerrainStreamer::mesh_key(const TerrainLeaf& l) {
    std::uint64_t flags = 0;
    for (int i = 0; i < 4; ++i)
        if (l.coarser[size_t(i)]) flags |= std::uint64_t(1) << i;
    return (std::uint64_t(l.key.face) << 60) | (std::uint64_t(l.key.level) << 54) |
           (std::uint64_t(std::uint32_t(l.key.x)) << 31) |
           (std::uint64_t(std::uint32_t(l.key.y)) << 4) | flags;
}

bool TerrainStreamer::sphere() const {
    const Shape::Kind kind = terrain_.body().shape.kind();
    return kind == Shape::Kind::SphereOutside || kind == Shape::Kind::SphereInside;
}

double TerrainStreamer::wrap_length() const {
    return terrain_.body().shape.kind() == Shape::Kind::FlatCylinder
               ? terrain_.body().shape.face_size().x
               : 0.0;
}

std::vector<TerrainLeaf> TerrainStreamer::choose(const glm::dvec3& camera) const {
    const Terrain& t = terrain_;
    const Shape& shape = t.body().shape;
    const glm::dvec2 size = shape.face_size();
    const double circumference = wrap_length();
    double range = range_;
    if (shape.kind() == Shape::Kind::CurvedCylinder)  // the far side is the sky
        range = std::max(range, 2.2 * shape.radius());

    // Where a chunk is drawn, and how big it is.
    const auto centre = [&](const ChunkKey& k, int wrap) {
        const double u = (double(k.x) + 0.5) / double(std::int64_t(t.roots_u()) << k.level);
        const double v = (double(k.y) + 0.5) / double(std::int64_t(t.roots_v()) << k.level);
        glm::dvec3 p = t.position(k.face, u, v, t.height(k.face, u, v));
        p.x += wrap * circumference;
        return p;
    };
    const auto extent = [&](const ChunkKey& k) {
        return std::max(size.x / double(std::int64_t(t.roots_u()) << k.level),
                        size.y / double(std::int64_t(t.roots_v()) << k.level));
    };
    const auto distance = [&](const ChunkKey& k, int wrap) {
        return std::max(0.0, glm::length(camera - centre(k, wrap)) - 0.75 * extent(k));
    };

    // Split while near; roots out of range aren't drawn.
    std::vector<std::pair<ChunkKey, int>> stack;
    for (int f = 0; f < shape.faces(); ++f)
        for (int y = 0; y < t.roots_v(); ++y)
            for (int x = 0; x < t.roots_u(); ++x) {
                const ChunkKey k{f, 0, x, y};
                int wrap = 0;
                if (circumference > 0.0) {
                    const double cx = centre(k, 0).x;
                    wrap = int(std::lround((camera.x - cx) / circumference));
                }
                if (distance(k, wrap) <= range) stack.emplace_back(k, wrap);
            }
    KeySet leaves;
    std::unordered_map<std::uint64_t, int> wrap_of_root;  // root (x, y, face) -> wrap
    const auto root_id = [&](const ChunkKey& k) {
        const int shift = k.level;
        return (std::uint64_t(k.face) << 48) | (std::uint64_t(k.x >> shift) << 24) |
               std::uint64_t(k.y >> shift);
    };
    while (!stack.empty()) {
        const auto [k, wrap] = stack.back();
        stack.pop_back();
        wrap_of_root[root_id(k)] = wrap;
        if (k.level < t.max_level() && distance(k, wrap) < detail_ * extent(k)) {
            for (const ChunkKey& c : t.children(k)) stack.emplace_back(c, wrap);
        } else {
            leaves.insert(k);
        }
    }

    // The leaf covering a chunk's place (it or an ancestor), if any.
    const auto covering = [&](ChunkKey k) -> std::optional<ChunkKey> {
        while (true) {
            if (leaves.contains(k)) return k;
            if (k.level == 0) return std::nullopt;
            k = t.parent(k);
        }
    };
    // No two touching leaves more than one level apart: split the coarser until so.
    for (bool changed = true; changed;) {
        changed = false;
        for (const ChunkKey& leaf : std::vector<ChunkKey>(leaves.begin(), leaves.end())) {
            if (!leaves.contains(leaf)) continue;
            for (int e = 0; e < 4; ++e) {
                const std::optional<ChunkKey> n = t.neighbour(leaf, ChunkEdge(e));
                if (!n) continue;
                const std::optional<ChunkKey> c = covering(*n);
                if (c && c->level < leaf.level - 1) {
                    leaves.erase(*c);
                    for (const ChunkKey& child : t.children(*c)) leaves.insert(child);
                    changed = true;
                }
            }
        }
    }

    std::vector<TerrainLeaf> out;
    out.reserve(leaves.size());
    for (const ChunkKey& k : leaves) {
        TerrainLeaf leaf{k, {}, wrap_of_root[root_id(k)]};
        for (int e = 0; e < 4; ++e) {
            const std::optional<ChunkKey> n = t.neighbour(k, ChunkEdge(e));
            if (!n || n->level == 0 || leaves.contains(*n)) continue;
            const ChunkKey p = t.parent(*n);
            leaf.coarser[size_t(e)] = leaves.contains(p);
        }
        out.push_back(leaf);
    }
    return out;
}

void TerrainStreamer::update(const glm::dvec3& camera, render::Renderer* renderer, bool wait) {
    ++tick_;
    wanted_ = choose(camera);
    const bool gpu = renderer != nullptr;
    if (renderer && !walls_uploaded_) upload_walls(*renderer);

    // Start what's missing, nearest first, with a bounded number of jobs in flight (so fast
    // travel doesn't queue up chunks it has long left behind).
    size_t in_flight = 0;
    for (const auto& [key, e] : cache_)
        if (e->job) ++in_flight;
    std::vector<std::pair<double, Entry*>> missing;
    const double circumference = wrap_length();
    for (const TerrainLeaf& leaf : wanted_) {
        std::unique_ptr<Entry>& e = cache_[mesh_key(leaf)];
        if (!e) {
            e = std::make_unique<Entry>();
            e->leaf = leaf;
        }
        e->last_wanted = tick_;
        if (e->job || (!e->stale && (e->data || e->mesh))) continue;
        const glm::dvec3 at =
            terrain_.chunk_origin(leaf.key) + glm::dvec3(leaf.wrap * circumference, 0.0, 0.0);
        missing.emplace_back(glm::length(at - camera), e.get());
    }
    std::sort(missing.begin(), missing.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [distance, e] : missing) {
        if (jobs_ && !wait && in_flight >= kMaxJobs) break;
        e->made = std::make_shared<ChunkMeshes>();
        auto made = e->made;
        const Terrain* terrain = &terrain_;
        const TerrainLeaf l = e->leaf;
        const bool round = sphere();
        const int frame = round ? up_frame(terrain_.body().up(terrain_.chunk_origin(l.key))) : -1;
        auto make = [terrain, l, made, round, frame] {
            made->ground = ground_mesh_data(terrain->mesh(l.key, l.coarser), frame);
            if (const auto water = terrain->water_mesh(l.key, l.coarser))
                made->water = water_mesh_data(*water, round);
        };
        if (jobs_) {
            e->job = jobs_->submit([make](const JobContext&) { make(); });
            ++in_flight;
        } else {
            make();
            e->data = std::move(*e->made);
            e->made.reset();
        }
    }
    for (const TerrainLeaf& leaf : drawn_)  // kept while the new choice isn't ready
        if (const auto it = cache_.find(mesh_key(leaf)); it != cache_.end())
            it->second->last_wanted = tick_;

    // Collect finished jobs, and upload.
    size_t uploads = 0;
    for (auto& [key, e] : cache_) {
        if (e->job) {
            if (wait && e->last_wanted == tick_) jobs_->wait(e->job);
            if (!e->job->done()) continue;
            jobs_->wait(e->job);  // (rethrows a job's exception)
            e->job.reset();
            e->data = std::move(*e->made);
            e->made.reset();
            e->stale = false;
        }
        if (renderer && e->data && e->last_wanted == tick_ &&
            (wait || uploads < kUploadsPerUpdate)) {
            if (e->mesh) renderer->free_mesh(*e->mesh);  // made again (the stamps changed)
            if (e->water) renderer->free_mesh(*e->water);
            e->water.reset();
            e->mesh = renderer->upload_mesh(e->data->ground);
            if (e->data->water) e->water = renderer->upload_mesh(*e->data->water);
            e->data.reset();
            ++uploads;
        }
    }

    // Draw the new choice once all of it is ready.
    const bool complete = std::all_of(wanted_.begin(), wanted_.end(), [&](const TerrainLeaf& l) {
        return cache_.at(mesh_key(l))->ready(gpu);
    });
    if (complete) drawn_ = wanted_;

    // Forget the longest-unwanted beyond the limit (never what's wanted or drawn).
    if (cache_.size() > cache_limit_) {
        std::vector<std::pair<std::uint64_t, MeshKey>> old;
        for (const auto& [key, e] : cache_)
            if (e->last_wanted < tick_ && !e->job) old.emplace_back(e->last_wanted, key);
        std::sort(old.begin(), old.end());
        for (const auto& [when, key] : old) {
            if (cache_.size() <= cache_limit_) break;
            Entry& e = *cache_.at(key);
            if (e.mesh && renderer) renderer->free_mesh(*e.mesh);
            if (e.water && renderer) renderer->free_mesh(*e.water);
            cache_.erase(key);
        }
    }
}

std::vector<render::DrawItem> TerrainStreamer::draws(const glm::dvec3& origin) const {
    std::vector<render::DrawItem> out;
    const double circumference = wrap_length();
    // The terrain's material: the season's snow, autumn and dryness, and its levels.
    const double summer = std::clamp(summer_, -1.0, 1.0), turn = std::clamp(autumn_, -1.0, 1.0);
    const auto smooth = [](double lo, double hi, double x) {
        const double t = std::clamp((x - lo) / (hi - lo), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    };
    const double snow = smooth(0.25, 0.9, -summer);
    const double autumn = std::max(turn, 0.0) * (1.0 - snow);
    const double dry = smooth(0.3, 1.0, summer);
    const glm::vec4 look(float(snow), float(autumn), float(dry), 1.0f);
    const TerrainSettings& ts = terrain_.settings();
    const glm::vec3 levels(float(ts.snow_line), float(ts.sea_level), 0.0f);
    const bool round = sphere();
    for (const TerrainLeaf& leaf : drawn_) {
        const auto it = cache_.find(mesh_key(leaf));
        if (it == cache_.end() || !it->second->mesh) continue;
        const glm::dvec3 at = terrain_.chunk_origin(leaf.key) +
                              glm::dvec3(leaf.wrap * circumference, 0.0, 0.0) - origin;
        render::DrawItem d;
        d.mesh = *it->second->mesh;
        d.model = glm::translate(glm::mat4(1.0f), glm::vec3(at));
        d.terrain = true;
        d.color = look;
        if (round)  // up in the frame of its own axis (ground_mesh_data's frame)
            d.color.w = float(2 + up_frame(terrain_.body().up(terrain_.chunk_origin(leaf.key))));
        d.emissive = levels;
        out.push_back(d);
        if (it->second->water) {
            render::DrawItem w;
            w.mesh = *it->second->water;
            w.model = d.model;
            w.water = true;
            w.casts_shadow = false;
            if (round) w.emissive.x = 1.0f;  // waves from each point's place (water_mesh_data)
            out.push_back(w);
        }
    }
    // The end walls (a flat-wrap cylinder's on either side of the camera too).
    const TerrainSettings& s = terrain_.settings();
    for (const Wall& w : walls_) {
        render::DrawItem d;
        d.mesh = w.mesh;
        d.roughness = 0.9f;
        switch (s.wall) {
            case WallLook::Cliff:
                d.color = {0.42f, 0.38f, 0.33f, 1.0f};
                break;
            case WallLook::Fortification:
                d.color = {0.66f, 0.63f, 0.57f, 1.0f};
                break;
            case WallLook::Barrier:
                d.color = {0.15f, 0.35f, 0.6f, 1.0f};
                d.emissive = {0.35f, 0.75f, 1.0f};
                d.emissive_strength = 1.5f;
                d.casts_shadow = false;
                break;
        }
        const int copies = circumference > 0.0 ? 1 : 0;
        const int centre =
            circumference > 0.0 ? int(std::lround((origin.x - w.at.x) / circumference)) : 0;
        for (int k = centre - copies; k <= centre + copies; ++k) {
            d.model = glm::translate(
                glm::mat4(1.0f), glm::vec3(w.at + glm::dvec3(k * circumference, 0, 0) - origin));
            out.push_back(d);
        }
    }
    return out;
}

TerrainStreamStats TerrainStreamer::stats() const {
    TerrainStreamStats s;
    s.wanted = wanted_.size();
    s.drawn = drawn_.size();
    s.cached = cache_.size();
    for (const auto& [key, e] : cache_)
        if (e->job) ++s.generating;
    s.complete = !wanted_.empty() && drawn_.size() == wanted_.size() &&
                 std::equal(wanted_.begin(), wanted_.end(), drawn_.begin(),
                            [](const TerrainLeaf& a, const TerrainLeaf& b) {
                                return a.key == b.key && a.coarser == b.coarser && a.wrap == b.wrap;
                            });
    return s;
}

void TerrainStreamer::set_stamps(std::vector<TerrainStamp> stamps) {
    const std::vector<TerrainStamp>& old = terrain_.stamps();
    size_t same = 0;
    while (same < old.size() && same < stamps.size() && old[same] == stamps[same]) ++same;
    if (same == old.size() && same == stamps.size()) return;
    // The stamps from the first that differs on change the ground (order matters).
    std::vector<TerrainStamp> changed(old.begin() + std::ptrdiff_t(same), old.end());
    changed.insert(changed.end(), stamps.begin() + std::ptrdiff_t(same), stamps.end());
    for (auto& [key, e] : cache_)  // jobs read the terrain being replaced
        if (e->job && jobs_) {
            jobs_->wait(e->job);
            e->job.reset();
            e->made.reset();
        }
    terrain_ = Terrain(terrain_.body(), terrain_.settings(), std::move(stamps));
    for (auto& [key, e] : cache_) {
        bool touched = false;
        for (const TerrainStamp& s : changed) touched = touched || terrain_.touches(e->leaf.key, s);
        if (!touched) continue;
        e->stale = true;
        e->data.reset();
    }
}

void TerrainStreamer::release(render::Renderer& renderer) {
    for (auto& [key, e] : cache_) {
        if (e->water) {
            renderer.free_mesh(*e->water);
            e->water.reset();
        }
        if (e->mesh) {
            renderer.free_mesh(*e->mesh);
            e->mesh.reset();
        }
    }
    for (const Wall& w : walls_) renderer.free_mesh(w.mesh);
    walls_.clear();
    walls_uploaded_ = false;
    drawn_.clear();
}

void TerrainStreamer::upload_walls(render::Renderer& renderer) {
    walls_uploaded_ = true;
    const Body& b = terrain_.body();
    const Shape& shape = b.shape;
    const double half = shape.face_size().y / 2.0;
    const TerrainSettings& s = terrain_.settings();
    if (shape.kind() == Shape::Kind::CurvedCylinder) {
        // Discs closing the tube, facing in.
        walls_.push_back({renderer.upload_mesh(disc(shape.radius(), 1.0f)),
                          b.origin + glm::dvec3(0.0, 0.0, -half)});
        walls_.push_back({renderer.upload_mesh(disc(shape.radius(), -1.0f)),
                          b.origin + glm::dvec3(0.0, 0.0, half)});
    } else if (shape.kind() == Shape::Kind::FlatCylinder) {
        const double w = shape.face_size().x;
        const double low = std::min(-s.ocean_depth, s.base - s.height_scale) - 20.0;
        const double high = s.wall_height + 60.0;
        walls_.push_back({renderer.upload_mesh(wall(w, low, high, 1.0f)),
                          b.origin + glm::dvec3(0.0, 0.0, -half)});
        walls_.push_back({renderer.upload_mesh(wall(w, low, high, -1.0f)),
                          b.origin + glm::dvec3(0.0, 0.0, half)});
    }
}

}  // namespace midgard
