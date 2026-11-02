// SPDX-License-Identifier: Apache-2.0
// Navigation meshes (milestone 5.12; Recast and Detour): where an agent can walk, as polygons
// in square tiles over a flat frame (Y up, metres, single precision round its origin), and the
// paths across them.
//
// A tile is built from the triangles that touch it (walls, floors, the ground) on any thread
// (build_nav_tile), then added to the mesh, which links it to its neighbours. Paths are the
// corners of the shortest way along the polygons ("string pulled"), from a point near the mesh
// to another.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace midgard {

class JobSystem;

// Who walks: their size, and what they can step up and climb.
struct NavAgent {
    float radius = 0.3f;      // metres: they keep this far from walls
    float height = 1.8f;      // metres: they fit under nothing lower
    float max_climb = 0.4f;   // metres: steps they walk up
    float max_slope = 45.0f;  // degrees: slopes they walk up
};

struct NavSettings {
    NavAgent agent;
    float cell_size = 0.2f;    // metres across a voxel (the detail of the outlines)
    float cell_height = 0.1f;  // metres up a voxel
    float tile_size = 32.0f;   // metres across a tile
};

// Triangles in the mesh's frame.
struct NavGeometry {
    std::vector<glm::vec3> vertices;
    std::vector<std::uint32_t> indices;  // three a triangle
    void add_triangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c);
    // The bounds of the vertices (none if there are none).
    std::optional<std::pair<glm::vec3, glm::vec3>> bounds() const;
};

struct NavTileKey {
    int x = 0, z = 0;
    bool operator==(const NavTileKey&) const = default;
};

// A built tile (Detour's data), empty if nothing in it can be walked on.
using NavTileData = std::vector<std::uint8_t>;

// One tile's polygons: the walkable surfaces of `geometry` in tile `key`'s square (tile_size
// metres, the frame's origin at tile (0, 0)'s corner). Any thread.
NavTileData build_nav_tile(const NavSettings& settings, const glm::vec3& origin, NavTileKey key,
                           const NavGeometry& geometry);

class NavMesh {
public:
    NavMesh(const NavSettings& settings, const glm::vec3& origin);
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    const NavSettings& settings() const { return settings_; }
    const glm::vec3& origin() const { return origin_; }
    // The tile a point is in.
    NavTileKey tile_at(const glm::vec3& p) const;

    // Adds (or replaces) a tile; an empty one just removes what was there.
    void add_tile(NavTileKey key, const NavTileData& data);
    void remove_tile(NavTileKey key);
    std::size_t tile_count() const;
    std::size_t polygon_count() const;

    // The nearest point on the mesh within `reach` metres across (and twice that up and down).
    std::optional<glm::vec3> nearest(const glm::vec3& p, float reach = 2.0f) const;

    struct Path {
        std::vector<glm::vec3> points;  // from the start (on the mesh) to the end, corner by corner
        bool complete = true;           // false: the end can't be reached; it stops nearest it
    };
    // The way from one point to another, or none if either is not near the mesh.
    std::optional<Path> find_path(const glm::vec3& from, const glm::vec3& to,
                                  float reach = 2.0f) const;

    // The polygons' edges, for debug drawing.
    std::vector<std::pair<glm::vec3, glm::vec3>> edges() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    NavSettings settings_;
    glm::vec3 origin_;
};

// Builds every tile over the geometry's bounds (in parallel on `jobs`, if given).
std::unique_ptr<NavMesh> build_navmesh(const NavSettings& settings, const NavGeometry& geometry,
                                       JobSystem* jobs = nullptr);

}  // namespace midgard
