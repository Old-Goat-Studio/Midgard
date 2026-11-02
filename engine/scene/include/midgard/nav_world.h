// SPDX-License-Identifier: Apache-2.0
// A world's navigation over its terrain (milestone 5.12): one navigation mesh per face of its
// shape, in the face's own frame:
//
//   x  across the face (u times its width, metres)      y  height above the shape's surface
//   z  along the face (v times its length)
//
// so the ground is level and "up" is +Y on every shape (a curved cylinder unrolls exactly; a
// cube-sphere face is stretched a little toward its edges). Tiles are made round a point (the
// player) in its face's frame on the job system as it moves, from the terrain (not below the
// sea) and the scene's scenery, and made again when the ground is sculpted or the scenery
// changes; far ones go, and those of a face the point has left.
//
// The joins: a cylinder's frame is a strip round and round (tiles past its join are made from
// the ground across it), and a cube face's frame goes on past its edges (the cube's projection
// continued onto the faces beyond), so a path near a join or an edge simply crosses it. Paths
// come back as world positions, following the surface.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

#include "midgard/collider_outlines.h"
#include "midgard/jobs.h"
#include "midgard/navmesh.h"
#include "midgard/scene.h"
#include "midgard/terrain.h"

namespace midgard {

// The settings navigation over terrain uses: coarser cells than a room's (open ground).
NavSettings terrain_nav_settings();

class NavWorld {
public:
    // `terrain` and `jobs` (none: tiles are made on the calling thread) must outlive this.
    NavWorld(const Terrain& terrain, JobSystem* jobs,
             NavSettings settings = terrain_nav_settings());
    ~NavWorld();  // waits for its jobs
    NavWorld(const NavWorld&) = delete;
    NavWorld& operator=(const NavWorld&) = delete;

    const NavSettings& settings() const { return settings_; }
    // How far round the point tiles are kept (metres).
    void set_range(double metres) { range_ = metres; }

    // The scene's scenery (its static colliders), in each face's frame. Tiles are made again.
    void set_scenery(const Scene& scene, const MeshLookup& mesh);
    // The ground changed (sculpting): tiles are made again. Call wait() before changing the
    // terrain: tiles being made read it.
    void ground_changed();
    void wait();

    // Makes the tiles round `around` that are missing or out of date (all of them now, with
    // `wait`), adds those done, and drops far ones.
    void update(const glm::dvec3& around, bool wait = false);

    struct Stats {
        std::size_t tiles = 0;     // on the meshes
        std::size_t building = 0;  // jobs running
        bool complete = false;     // every tile wanted round the last point is up to date
    };
    Stats stats() const;
    int faces() const { return int(meshes_.size()); }
    const NavMesh& mesh(int face) const { return *meshes_[size_t(face)]; }

    // A world position in a face's frame, and back.
    struct Place {
        int face = 0;
        glm::vec3 at{0.0f};
    };
    Place place(const glm::dvec3& world) const;
    // In a given face's frame (a cube face's continued past its edges); none if it doesn't
    // reach there.
    std::optional<Place> place_on(int face, const glm::dvec3& world) const;
    glm::dvec3 world(int face, const glm::vec3& at) const;

    // The way between two world positions (each within `reach` metres of the walkable ground),
    // in the start's face's frame (across joins and face edges), or none (too far from the
    // ground made so far). Points every few metres, on the surface.
    std::optional<NavMesh::Path> find_path(const glm::dvec3& from, const glm::dvec3& to,
                                           float reach = 3.0f) const;

    // The meshes' edges in world positions, for debug drawing.
    std::vector<std::pair<glm::dvec3, glm::dvec3>> edges() const;

private:
    struct Tile {
        JobHandle job;
        std::shared_ptr<NavTileData> made;
        int building_version = -1;  // the version the job is making
        int version = -1;           // the version on the mesh (-1: none)
    };
    using Key = std::tuple<int, int, int>;  // face, x, z
    enum class Frame { Area, Strip, Cube };
    struct Bounds {
        double x0 = 0.0, x1 = 0.0, z0 = 0.0, z1 = 0.0;
    };
    void collect(bool wait);
    Bounds bounds() const;
    double ground_height(int face, double x, double z) const;
    NavTileData make_tile(int face, NavTileKey key, const NavGeometry& scenery) const;

    const Terrain& terrain_;
    JobSystem* jobs_;
    Frame frame_ = Frame::Area;
    NavSettings settings_;
    double range_ = 96.0;
    glm::dvec2 face_size_;
    std::vector<std::unique_ptr<NavMesh>> meshes_;
    std::shared_ptr<const std::vector<NavGeometry>> scenery_;  // per face
    std::map<Key, Tile> tiles_;
    int version_ = 0;
    bool complete_ = false;
};

}  // namespace midgard
