// SPDX-License-Identifier: Apache-2.0
#include "midgard/nav_world.h"

#include <algorithm>
#include <cmath>

#include "midgard/scene_nav.h"

namespace midgard {

namespace {

constexpr int kMaxJobs = 8;          // tiles being made at once (unless waiting)
constexpr double kPathStep = 4.0;    // metres between a path's points (it follows the surface)
constexpr double kUnderwater = 1.0;  // metres under the sea the ground stops being walkable

// The ground's triangles over a tile and the border round it, in the face's frame: the
// terrain's heights on a grid `step` metres apart, none of it deeper than kUnderwater under
// the sea.
void add_ground(NavGeometry& g, const Terrain& t, int face, const glm::dvec2& size, double x0,
                double z0, double x1, double z1, double step) {
    x0 = std::max(x0, 0.0);
    z0 = std::max(z0, 0.0);
    x1 = std::min(x1, size.x);
    z1 = std::min(z1, size.y);
    if (x1 <= x0 || z1 <= z0) return;
    const int nx = std::max(1, int(std::ceil((x1 - x0) / step)));
    const int nz = std::max(1, int(std::ceil((z1 - z0) / step)));
    const double floor = t.settings().sea_level - kUnderwater;
    std::vector<glm::vec3> p(size_t(nx + 1) * size_t(nz + 1));
    for (int j = 0; j <= nz; ++j)
        for (int i = 0; i <= nx; ++i) {
            const double x = std::min(x0 + i * step, x1), z = std::min(z0 + j * step, z1);
            const double h = t.height(face, x / size.x, z / size.y);
            p[size_t(j) * size_t(nx + 1) + size_t(i)] = glm::vec3(float(x), float(h), float(z));
        }
    const auto at = [&](int i, int j) { return p[size_t(j) * size_t(nx + 1) + size_t(i)]; };
    for (int j = 0; j < nz; ++j)
        for (int i = 0; i < nx; ++i) {
            const glm::vec3 a = at(i, j), b = at(i + 1, j), c = at(i, j + 1), d = at(i + 1, j + 1);
            // (Counter-clockwise seen from above: Recast's walkable side.)
            if (std::max({a.y, b.y, c.y}) >= floor) g.add_triangle(a, c, b);
            if (std::max({b.y, c.y, d.y}) >= floor) g.add_triangle(b, c, d);
        }
}

}  // namespace

NavSettings terrain_nav_settings() {
    NavSettings s;
    s.cell_size = 0.3f;
    s.cell_height = 0.15f;
    s.tile_size = 32.0f;
    return s;
}

NavWorld::NavWorld(const Terrain& terrain, JobSystem* jobs, NavSettings settings)
    : terrain_(terrain),
      jobs_(jobs),
      settings_(nav_settings_fitted(settings)),
      face_size_(terrain.body().shape.face_size()) {
    for (int f = 0; f < terrain.body().shape.faces(); ++f)
        meshes_.push_back(std::make_unique<NavMesh>(settings_, glm::vec3(0.0f)));
    scenery_ = std::make_shared<std::vector<NavGeometry>>(meshes_.size());
}

NavWorld::~NavWorld() {
    wait();
}

void NavWorld::wait() {
    for (auto& [key, tile] : tiles_)
        if (tile.job && jobs_) jobs_->wait(tile.job);
}

NavWorld::Place NavWorld::place(const glm::dvec3& world) const {
    const Body& body = terrain_.body();
    const SurfacePoint s = body.shape.surface(world - body.origin);
    return {s.face,
            glm::vec3(float(s.u * face_size_.x), float(s.height), float(s.v * face_size_.y))};
}

glm::dvec3 NavWorld::world(int face, const glm::vec3& at) const {
    const Body& body = terrain_.body();
    return body.origin + body.shape.position({face, double(at.x) / face_size_.x,
                                              double(at.z) / face_size_.y, double(at.y)});
}

void NavWorld::set_scenery(const Scene& scene, const MeshLookup& mesh) {
    wait();  // (jobs read the scenery being replaced)
    auto faces = std::make_shared<std::vector<NavGeometry>>(meshes_.size());
    const NavGeometry all = nav_geometry(scene, glm::dvec3(0.0), mesh);
    for (size_t i = 0; i + 2 < all.indices.size(); i += 3) {
        Place p[3];
        for (int k = 0; k < 3; ++k)
            p[k] = place(glm::dvec3(all.vertices[all.indices[i + size_t(k)]]));
        if (p[0].face != p[1].face || p[0].face != p[2].face) continue;  // (on a face edge)
        (*faces)[size_t(p[0].face)].add_triangle(p[0].at, p[1].at, p[2].at);
    }
    scenery_ = std::move(faces);
    ++version_;
}

void NavWorld::ground_changed() {
    ++version_;
}

void NavWorld::collect(bool wait) {
    for (auto& [key, tile] : tiles_) {
        if (!tile.job) continue;
        if (wait && jobs_) jobs_->wait(tile.job);
        if (!tile.job->done()) continue;
        if (jobs_) jobs_->wait(tile.job);  // (rethrows a job's exception)
        tile.job.reset();
        const auto [face, x, z] = key;
        meshes_[size_t(face)]->add_tile({x, z}, *tile.made);
        tile.made.reset();
        tile.version = tile.building_version;
    }
}

void NavWorld::update(const glm::dvec3& around, bool wait) {
    collect(false);
    const Place here = place(around);
    const NavMesh& mesh = *meshes_[size_t(here.face)];
    const NavTileKey centre = mesh.tile_at(here.at);
    const double ts = settings_.tile_size;
    const int reach = int(std::ceil(range_ / ts));
    const int last_x = int(std::ceil(face_size_.x / ts)) - 1;
    const int last_z = int(std::ceil(face_size_.y / ts)) - 1;
    // The tiles wanted: those on this face within range, nearest first.
    std::vector<std::pair<double, NavTileKey>> wanted;
    for (int dz = -reach; dz <= reach; ++dz)
        for (int dx = -reach; dx <= reach; ++dx) {
            const NavTileKey k{centre.x + dx, centre.z + dz};
            if (k.x < 0 || k.z < 0 || k.x > last_x || k.z > last_z) continue;
            const double cx = (k.x + 0.5) * ts - here.at.x, cz = (k.z + 0.5) * ts - here.at.z;
            const double d = std::sqrt(cx * cx + cz * cz);
            if (d <= range_ + ts * 0.71) wanted.emplace_back(d, k);
        }
    std::sort(wanted.begin(), wanted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    int in_flight = 0;
    for (const auto& [key, tile] : tiles_)
        if (tile.job) ++in_flight;
    const NavSettings settings = settings_;
    const double step = std::clamp(terrain_.settings().spacing * 0.5, 0.5, 2.0);
    const float border =
        float(std::ceil(settings_.agent.radius / settings_.cell_size) + 3) * settings_.cell_size +
        float(step);
    complete_ = true;
    for (const auto& [distance, k] : wanted) {
        Tile& tile = tiles_[{here.face, k.x, k.z}];
        if (tile.version == version_ && !tile.job) continue;
        complete_ = false;
        if (tile.job || (jobs_ && !wait && in_flight >= kMaxJobs)) continue;
        tile.building_version = version_;
        tile.made = std::make_shared<NavTileData>();
        auto made = tile.made;
        auto scenery = scenery_;
        const Terrain* terrain = &terrain_;
        const int face = here.face;
        const glm::dvec2 size = face_size_;
        auto make = [=] {
            NavGeometry g = (*scenery)[size_t(face)];
            add_ground(g, *terrain, face, size, k.x * ts - border, k.z * ts - border,
                       (k.x + 1) * ts + border, (k.z + 1) * ts + border, step);
            *made = build_nav_tile(settings, glm::vec3(0.0f), k, g);
        };
        if (jobs_) {
            tile.job = jobs_->submit([make](const JobContext&) { make(); });
            ++in_flight;
        } else {
            make();
            meshes_[size_t(face)]->add_tile(k, *tile.made);
            tile.made.reset();
            tile.version = version_;
        }
    }
    collect(wait);
    if (wait) {
        complete_ = true;
        for (const auto& [distance, k] : wanted)
            if (tiles_[{here.face, k.x, k.z}].version != version_) complete_ = false;
    }
    // Far tiles go (once their jobs are done).
    const double far = range_ * 1.5 + ts;
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        const auto [face, x, z] = it->first;
        const double cx = (x + 0.5) * ts - here.at.x, cz = (z + 0.5) * ts - here.at.z;
        if (!it->second.job && (face != here.face || std::sqrt(cx * cx + cz * cz) > far)) {
            meshes_[size_t(face)]->remove_tile({x, z});
            it = tiles_.erase(it);
        } else {
            ++it;
        }
    }
}

NavWorld::Stats NavWorld::stats() const {
    Stats s;
    for (const auto& m : meshes_) s.tiles += m->tile_count();
    for (const auto& [key, tile] : tiles_)
        if (tile.job) ++s.building;
    s.complete = complete_;
    return s;
}

std::optional<NavMesh::Path> NavWorld::find_path(const glm::dvec3& from, const glm::dvec3& to,
                                                 float reach) const {
    const Place a = place(from), b = place(to);
    if (a.face != b.face) return std::nullopt;
    const NavMesh& mesh = *meshes_[size_t(a.face)];
    const auto path = mesh.find_path(a.at, b.at, reach);
    if (!path) return std::nullopt;
    // Back to the world, with points every few metres so the path follows the surface.
    NavMesh::Path out;
    out.complete = path->complete;
    for (size_t i = 0; i < path->points.size(); ++i) {
        if (i > 0) {
            const glm::vec3 p = path->points[i - 1], q = path->points[i];
            const int steps = int(std::ceil(glm::distance(p, q) / kPathStep));
            for (int s = 1; s < steps; ++s) {
                glm::vec3 m = p + (q - p) * (float(s) / float(steps));
                // On the walkable surface there (the ground, or scenery: a bridge, a floor).
                if (const auto on = mesh.nearest(m, 1.0f)) m.y = on->y;
                out.points.push_back(glm::vec3(world(a.face, m)));
            }
        }
        out.points.push_back(glm::vec3(world(a.face, path->points[i])));
    }
    return out;
}

std::vector<std::pair<glm::dvec3, glm::dvec3>> NavWorld::edges() const {
    std::vector<std::pair<glm::dvec3, glm::dvec3>> out;
    for (int f = 0; f < faces(); ++f)
        for (const auto& [a, b] : meshes_[size_t(f)]->edges())
            out.emplace_back(world(f, a), world(f, b));
    return out;
}

}  // namespace midgard
