// SPDX-License-Identifier: Apache-2.0
// Terrain (roadmap §7, "One terrain model for every shape"; milestone 5.9): the ground's height
// at every point of a world's faces, and the chunks it is drawn and collided in.
//
// Heights come from noise sampled at 3D positions on the shape's surface (midgard/det.h's fbm,
// so they are identical on every target), which makes joined edges seamless by construction: a
// flat-wrap cylinder samples its noise on the circle it would be if it were curved, so its
// left and right edges meet. Then the shape's edges shape it: a bounded area sinks into a band
// of open ocean all round, and a cylinder rises towards walls at its two ends.
//
// In order (the order of operations is the specification; tools/reference/terrain.py is the
// Python twin, and schema/tests/terrain_chunks.json holds both to the same results):
//   q  = the noise position (above), in metres
//   n  = fbm(seed, q.x / feature_size, q.y / feature_size, q.z / feature_size, octaves) / 65536
//   h  = base + height_scale * n
//   bounded area: d = the distance to the nearest edge (metres, from u and v),
//        t = clamp((d - ocean_band) / ocean_band, 0, 1), s = t t (3 - 2 t),
//        h = -ocean_depth + (h + ocean_depth) s
//   cylinders: e = the distance to the nearer end, t = clamp(1 - e / end_ramp, 0, 1),
//        s = t t (3 - 2 t), h = h + (wall_height - h) s
//
// Chunks: each face is cut into roots about kRootSize metres across, and each root into a
// quadtree: a chunk at level L is a root's 2^L x 2^L part, a grid of kChunkQuads x kChunkQuads
// quads. Grid points are numbered across the whole face, so a point is computed from the same
// (u, v), and so comes out the same, in every chunk that has it, at every level. Where a chunk
// meets a coarser neighbour (one level up), its edge points between the neighbour's are moved
// onto the neighbour's edge, so there are no cracks.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include "midgard/body.h"

namespace midgard {

inline constexpr int kChunkQuads = 32;       // quads along a chunk's side
inline constexpr double kRootSize = 1024.0;  // metres: a root chunk's size, about
inline constexpr int kMaxTerrainLevel = 12;

enum class WallLook { Cliff, Fortification, Barrier };
const char* wall_look_name(WallLook look);  // "cliff", "fortification", "barrier"

struct TerrainSettings {
    std::optional<std::uint64_t> seed;  // none: the body's
    double base = 8.0;                  // metres: the height where the noise is 0
    double height_scale = 60.0;         // metres: the noise's amplitude
    double feature_size = 500.0;        // metres: the largest hills' size
    int octaves = 6;
    double sea_level = 0.0;      // metres: where the water's surface is (5.9's water)
    double ocean_band = 300.0;   // bounded area: open ocean round the land, metres (0: none)
    double ocean_depth = 30.0;   // bounded area: the sea floor's depth there
    double end_ramp = 150.0;     // cylinders: how far from the ends the ground rises (0: none)
    double wall_height = 120.0;  // cylinders: how high it rises, to meet the end walls
    WallLook wall = WallLook::Cliff;
    double spacing = 2.0;  // metres between grid points at the finest level, at most
};

// From the scene settings' "terrain" object. Throws std::runtime_error naming what's wrong with
// JSON that can't be read; terrain_problems() lists what the cook refuses.
TerrainSettings terrain_from_json(const nlohmann::json& j);
nlohmann::json terrain_to_json(const TerrainSettings& t);
std::vector<std::string> terrain_problems(const TerrainSettings& t);

struct ChunkKey {
    int face = 0, level = 0, x = 0, y = 0;  // x along u, y along v, at that level
    bool operator==(const ChunkKey&) const = default;
};

// A chunk's grid, ready to draw: positions and normals relative to `origin` (the chunk's middle,
// on the ground at height 0), row by row along v, (kChunkQuads + 1)^2 points.
struct ChunkMesh {
    glm::dvec3 origin{0.0};
    std::vector<glm::vec3> positions, normals;
    std::vector<glm::vec2> uvs;          // metres across the face, over 16 (for tiling textures)
    std::vector<std::uint32_t> indices;  // triangles, counter-clockwise seen from above
    std::vector<double> heights;         // as generated (before edges were matched)
};

// Edges of a chunk, for ChunkMesh's coarser neighbours.
enum ChunkEdge { kWest = 0, kEast = 1, kSouth = 2, kNorth = 3 };  // -u, +u, -v, +v

class Terrain {
public:
    Terrain(const Body& body, const TerrainSettings& settings);

    const Body& body() const { return body_; }
    const TerrainSettings& settings() const { return settings_; }
    std::uint64_t seed() const { return seed_; }

    // The layout: roots per face along u and v, and the finest level.
    int roots_u() const { return roots_u_; }
    int roots_v() const { return roots_v_; }
    int max_level() const { return max_level_; }
    // Grid points along u (or v) across a whole face at a level.
    std::int64_t points_u(int level) const;
    std::int64_t points_v(int level) const;
    bool valid(const ChunkKey& key) const;
    // The four children of a chunk, and its parent.
    std::array<ChunkKey, 4> children(const ChunkKey& key) const;
    ChunkKey parent(const ChunkKey& key) const;
    // The neighbour across an edge at the same level (a flat-wrap or curved cylinder wraps
    // round along u); none past a face's edge.
    std::optional<ChunkKey> neighbour(const ChunkKey& key, ChunkEdge edge) const;

    glm::dvec3 noise_position(int face, double u, double v) const;  // relative to the body
    double height(int face, double u, double v) const;
    // Where a point is drawn, in the room: a flat-wrap cylinder's u doesn't wrap here, so a
    // chunk at its east edge lies past it (streaming draws it again a circumference over).
    glm::dvec3 position(int face, double u, double v, double h) const;

    // Where a chunk's mesh is placed: its middle, on the ground at height 0.
    glm::dvec3 chunk_origin(const ChunkKey& key) const;
    // grid: kChunkQuads + 1 points a side. coarser[edge]: the neighbour there is a level up.
    ChunkMesh mesh(const ChunkKey& key, const std::array<bool, 4>& coarser = {}) const;
    // A hash of the chunk's key and its generated heights (fnv1a64 over little-endian int32
    // face, level, x, y, then each height's 8 bytes, row by row).
    std::uint64_t chunk_hash(const ChunkKey& key) const;

private:
    double u_at(const ChunkKey& key, int i) const;
    double v_at(const ChunkKey& key, int j) const;

    Body body_;
    TerrainSettings settings_;
    std::uint64_t seed_;
    int roots_u_ = 1, roots_v_ = 1, max_level_ = 0;
};

// A hash over chunk hashes of fixed terrains on four shapes (those of
// schema/tests/terrain_chunks.json): kTerrainFingerprint on every target. The self-test checks
// it on devices.
std::uint64_t terrain_fingerprint();
inline constexpr std::uint64_t kTerrainFingerprint = 0x667639e2e799c3b4ULL;

}  // namespace midgard
