// SPDX-License-Identifier: Apache-2.0
#include "midgard/nav_world.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "midgard/scene_nav.h"

namespace midgard {

namespace {

constexpr int kMaxJobs = 8;          // tiles being made at once (unless waiting)
constexpr double kPathStep = 4.0;    // metres between a path's points (it follows the surface)
constexpr double kUnderwater = 1.0;  // metres under the sea the ground stops being walkable
constexpr double kFacing = 0.3;      // a cube face's frame reaches directions this near it (cos)
constexpr double kInf = std::numeric_limits<double>::infinity();

double wrap01(double u) {
    return u - std::floor(u);
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
    switch (terrain.body().shape.kind()) {
        case Shape::Kind::BoundedArea:
            frame_ = Frame::Area;
            break;
        case Shape::Kind::FlatCylinder:
        case Shape::Kind::CurvedCylinder:
            frame_ = Frame::Strip;
            break;
        default:
            frame_ = Frame::Cube;
    }
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

// ---- the faces' frames --------------------------------------------------------------------

NavWorld::Place NavWorld::place(const glm::dvec3& world) const {
    const Body& body = terrain_.body();
    const SurfacePoint s = body.shape.surface(world - body.origin);
    return {s.face,
            glm::vec3(float(s.u * face_size_.x), float(s.height), float(s.v * face_size_.y))};
}

std::optional<NavWorld::Place> NavWorld::place_on(int face, const glm::dvec3& world) const {
    if (frame_ != Frame::Cube) {
        const Place p = place(world);
        return p.face == face ? std::optional(p) : std::nullopt;
    }
    const Body& body = terrain_.body();
    const glm::dvec3 d = glm::normalize(world - body.origin);
    if (glm::dot(d, cube_direction(face, 0.5, 0.5)) < kFacing) return std::nullopt;
    const glm::dvec2 uv = cube_face_uv(face, d);
    const double height = body.shape.surface(world - body.origin).height;
    return Place{face,
                 glm::vec3(float(uv.x * face_size_.x), float(height), float(uv.y * face_size_.y))};
}

glm::dvec3 NavWorld::world(int face, const glm::vec3& at) const {
    const double u = double(at.x) / face_size_.x, v = double(at.z) / face_size_.y;
    if (frame_ == Frame::Cube) {  // (past the face's edges too: onto the faces beyond)
        SurfacePoint s = cube_surface(cube_direction(face, u, v));
        s.height = double(at.y);
        return terrain_.body().position(s);
    }
    // (A flat-wrap cylinder's u doesn't wrap here: a path across its join stays in one piece.)
    return terrain_.position(face, u, v, double(at.y));
}

double NavWorld::ground_height(int face, double x, double z) const {
    const double u = x / face_size_.x, v = z / face_size_.y;
    switch (frame_) {
        case Frame::Area:
            return terrain_.height(face, u, v);
        case Frame::Strip:
            return terrain_.height(face, wrap01(u), v);
        case Frame::Cube: {
            const SurfacePoint s = cube_surface(cube_direction(face, u, v));
            return terrain_.height(s.face, s.u, s.v);
        }
    }
    return 0.0;
}

// The part of a face's frame tiles may cover: the face itself (a bounded area), a strip round
// and round (cylinders), or the face and a margin past its edges (a cube's faces).
NavWorld::Bounds NavWorld::bounds() const {
    switch (frame_) {
        case Frame::Area:
            return {0.0, face_size_.x, 0.0, face_size_.y};
        case Frame::Strip:
            return {-kInf, kInf, 0.0, face_size_.y};
        case Frame::Cube: {
            const double m = range_ + 2.0 * settings_.tile_size;
            return {-m, face_size_.x + m, -m, face_size_.y + m};
        }
    }
    return {};
}

// ---- tiles --------------------------------------------------------------------------------

void NavWorld::set_scenery(const Scene& scene, const MeshLookup& mesh) {
    wait();  // (jobs read the scenery being replaced)
    auto faces = std::make_shared<std::vector<NavGeometry>>(meshes_.size());
    const NavGeometry all = nav_geometry(scene, glm::dvec3(0.0), mesh);
    for (size_t i = 0; i + 2 < all.indices.size(); i += 3) {
        glm::dvec3 w[3];
        for (int k = 0; k < 3; ++k) w[k] = glm::dvec3(all.vertices[all.indices[i + size_t(k)]]);
        if (frame_ == Frame::Cube) {  // on every face whose frame reaches it
            for (int f = 0; f < faces->size(); ++f) {
                const auto a = place_on(f, w[0]), b = place_on(f, w[1]), c = place_on(f, w[2]);
                if (a && b && c) (*faces)[size_t(f)].add_triangle(a->at, b->at, c->at);
            }
            continue;
        }
        const Place a = place(w[0]), b = place(w[1]), c = place(w[2]);
        glm::vec3 p[3] = {a.at, b.at, c.at};
        if (frame_ == Frame::Strip) {
            // In one piece (not split by the join), and once a way round each side of it.
            const float round = float(face_size_.x);
            for (glm::vec3& q : p)
                if (q.x - p[0].x > round * 0.5f)
                    q.x -= round;
                else if (p[0].x - q.x > round * 0.5f)
                    q.x += round;
            for (const float shift : {-round, 0.0f, round}) {
                const glm::vec3 s(shift, 0.0f, 0.0f);
                (*faces)[0].add_triangle(p[0] + s, p[1] + s, p[2] + s);
            }
            continue;
        }
        (*faces)[size_t(a.face)].add_triangle(p[0], p[1], p[2]);
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

NavTileData NavWorld::make_tile(int face, NavTileKey k, const NavGeometry& scenery) const {
    // The ground over the tile and the border round it: the terrain's heights on a grid a
    // little finer than its own, none of it deeper than kUnderwater under the sea.
    const double ts = settings_.tile_size;
    const double step = std::clamp(terrain_.settings().spacing * 0.5, 0.5, 2.0);
    const double border =
        double(std::ceil(settings_.agent.radius / settings_.cell_size) + 3) * settings_.cell_size +
        step;
    const Bounds b = bounds();
    const double x0 = std::max(k.x * ts - border, b.x0),
                 x1 = std::min((k.x + 1) * ts + border, b.x1);
    const double z0 = std::max(k.z * ts - border, b.z0),
                 z1 = std::min((k.z + 1) * ts + border, b.z1);
    NavGeometry g = scenery;
    if (x1 > x0 && z1 > z0) {
        const int nx = std::max(1, int(std::ceil((x1 - x0) / step)));
        const int nz = std::max(1, int(std::ceil((z1 - z0) / step)));
        const double floor = terrain_.settings().sea_level - kUnderwater;
        std::vector<glm::vec3> p(size_t(nx + 1) * size_t(nz + 1));
        for (int j = 0; j <= nz; ++j)
            for (int i = 0; i <= nx; ++i) {
                const double x = std::min(x0 + i * step, x1), z = std::min(z0 + j * step, z1);
                p[size_t(j) * size_t(nx + 1) + size_t(i)] =
                    glm::vec3(float(x), float(ground_height(face, x, z)), float(z));
            }
        const auto at = [&](int i, int j) { return p[size_t(j) * size_t(nx + 1) + size_t(i)]; };
        for (int j = 0; j < nz; ++j)
            for (int i = 0; i < nx; ++i) {
                const glm::vec3 a = at(i, j), bb = at(i + 1, j), c = at(i, j + 1),
                                d = at(i + 1, j + 1);
                // (Counter-clockwise seen from above: Recast's walkable side.)
                if (std::max({a.y, bb.y, c.y}) >= floor) g.add_triangle(a, c, bb);
                if (std::max({bb.y, c.y, d.y}) >= floor) g.add_triangle(bb, c, d);
            }
    }
    return build_nav_tile(settings_, glm::vec3(0.0f), k, g);
}

void NavWorld::update(const glm::dvec3& around, bool wait) {
    collect(false);
    const Place here = place(around);
    const NavMesh& mesh = *meshes_[size_t(here.face)];
    const NavTileKey centre = mesh.tile_at(here.at);
    const double ts = settings_.tile_size;
    const int reach = int(std::ceil(range_ / ts));
    const Bounds b = bounds();
    // The tiles wanted: those of this face's frame within range, nearest first.
    std::vector<std::pair<double, NavTileKey>> wanted;
    for (int dz = -reach; dz <= reach; ++dz)
        for (int dx = -reach; dx <= reach; ++dx) {
            const NavTileKey k{centre.x + dx, centre.z + dz};
            if ((k.x + 1) * ts <= b.x0 || k.x * ts >= b.x1 || (k.z + 1) * ts <= b.z0 ||
                k.z * ts >= b.z1)
                continue;
            const double cx = (k.x + 0.5) * ts - here.at.x, cz = (k.z + 0.5) * ts - here.at.z;
            const double d = std::sqrt(cx * cx + cz * cz);
            if (d <= range_ + ts * 0.71) wanted.emplace_back(d, k);
        }
    std::sort(wanted.begin(), wanted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    int in_flight = 0;
    for (const auto& [key, tile] : tiles_)
        if (tile.job) ++in_flight;
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
        const int face = here.face;
        auto make = [this, made, scenery, face, k] {
            *made = make_tile(face, k, (*scenery)[size_t(face)]);
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
    // Far tiles go (once their jobs are done), and other faces' (the player left them).
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

// ---- paths --------------------------------------------------------------------------------

std::optional<NavMesh::Path> NavWorld::find_path(const glm::dvec3& from, const glm::dvec3& to,
                                                 float reach) const {
    const Place a = place(from);
    // The end in the start's frame: past a cube face's edge, or the nearer way round a strip.
    std::optional<Place> b = place_on(a.face, to);
    if (!b) return std::nullopt;
    if (frame_ == Frame::Strip) {
        const float round = float(face_size_.x);
        b->at.x += round * std::round((a.at.x - b->at.x) / round);
    }
    const NavMesh& mesh = *meshes_[size_t(a.face)];
    const auto path = mesh.find_path(a.at, b->at, reach);
    if (!path) return std::nullopt;
    // Back to the world, with points every few metres so the path follows the surface. (A
    // flat-wrap cylinder's room repeats: the path is put beside the start, wherever it is.)
    glm::dvec3 shift(0.0);
    if (terrain_.body().shape.kind() == Shape::Kind::FlatCylinder)
        shift = from - world(a.face, a.at);
    const auto out_point = [&](const glm::vec3& p) { return glm::vec3(world(a.face, p) + shift); };
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
                out.points.push_back(out_point(m));
            }
        }
        out.points.push_back(out_point(path->points[i]));
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
