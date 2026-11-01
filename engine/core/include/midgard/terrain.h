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
    double snow_line = 90.0;  // metres: snow lies above this all year (lower in winter)
    double spacing = 2.0;     // metres between grid points at the finest level, at most
};

// From the scene settings' "terrain" object. Throws std::runtime_error naming what's wrong with
// JSON that can't be read; terrain_problems() lists what the cook refuses.
TerrainSettings terrain_from_json(const nlohmann::json& j);
nlohmann::json terrain_to_json(const TerrainSettings& t);
std::vector<std::string> terrain_problems(const TerrainSettings& t);

// Sculpting and painting (milestone 5.9): a stamp is one stroke of a brush along points on a
// face, `radius` metres wide, applied in order on top of the generated ground. A stroke counts
// once wherever it passes (its strength falls off with the distance to the stroke's line). On a
// sphere (5.10) that distance is measured in 3D, so a stroke reaches across face edges; its
// points may run a little past its face's edges (u or v outside 0 to 1, cube_face_point), and
// smooth averages round each point along a frame of its own, the same from every face:
//   raise, lower  by `strength` metres along the line
//   smooth        towards the average round each point, by `strength` (0 to 1)
//   flatten       towards `height` metres, by `strength` (0 to 1)
//   paint         the ground's material: lush or dry (its moisture), sand or rock, or clear
//                 (back to as generated), by `strength` (0 to 1)
// World types will place stamps too, and players' changes will use the same mechanism.
enum class StampTool { Raise, Lower, Smooth, Flatten, Paint };
enum class PaintLayer { Clear, Lush, Dry, Sand, Rock };
const char* stamp_tool_name(StampTool tool);     // "raise", "lower", "smooth", "flatten", "paint"
const char* paint_layer_name(PaintLayer layer);  // "clear", "lush", "dry", "sand", "rock"

struct TerrainStamp {
    StampTool tool = StampTool::Raise;
    PaintLayer paint = PaintLayer::Lush;
    double radius = 10.0;   // metres
    double strength = 2.0;  // raise and lower: metres; the others: 0 to 1
    double height = 0.0;    // flatten: to this height (metres)
    int face = 0;
    std::vector<glm::dvec2> points;  // (u, v) on the face, along the stroke
    bool operator==(const TerrainStamp&) const = default;
};
// {"tool", "paint", "radius", "strength", "height", "face", "points": [[u, v], ...]}. Throws
// std::runtime_error naming what's wrong with JSON that can't be read.
TerrainStamp stamp_from_json(const nlohmann::json& j);
nlohmann::json stamp_to_json(const TerrainStamp& s);
std::vector<std::string> stamp_problems(const TerrainStamp& s);

struct ChunkKey {
    int face = 0, level = 0, x = 0, y = 0;  // x along u, y along v, at that level
    bool operator==(const ChunkKey&) const = default;
};

// A chunk's grid, ready to draw: positions and normals relative to `origin` (the chunk's middle,
// on the ground at height 0), row by row along v, (kChunkQuads + 1)^2 points. The origin and
// the points are on a grid of 2^-16 m, so within 256 m of the origin (every chunk at the finest
// levels) origin + position is exact, and a point two chunks share is the same in both.
struct ChunkMesh {
    glm::dvec3 origin{0.0};
    std::vector<glm::vec3> positions, normals;
    std::vector<glm::vec2> uvs;  // metres across the face, over 16 (for tiling textures)
    // Triangles, counter-clockwise seen from above; each square is split along its diagonal from
    // point (i, j) to (i + 1, j + 1), as a physics heightfield splits it.
    std::vector<std::uint32_t> indices;
    std::vector<double> heights;  // as generated (before edges were matched)
    // For the terrain's material: "up" at each point, and its biome: x moisture (0 dry to 1
    // lush, over kilometres), y fine detail (-1 to 1, over metres), z painted ground (-1 sand,
    // 0 as generated, 1 rock).
    std::vector<glm::vec3> ups;
    std::vector<glm::vec3> biome;
};

// Edges of a chunk, for ChunkMesh's coarser neighbours.
enum ChunkEdge { kWest = 0, kEast = 1, kSouth = 2, kNorth = 3 };  // -u, +u, -v, +v

class Terrain {
public:
    Terrain(const Body& body, const TerrainSettings& settings,
            std::vector<TerrainStamp> stamps = {});
    const std::vector<TerrainStamp>& stamps() const { return stamps_; }
    // The part of a face a stamp can change: u and v ranges (any u on a cylinder, which wraps).
    struct StampBounds {
        double u0, u1, v0, v1;
    };
    StampBounds stamp_bounds(const TerrainStamp& s) const;
    bool touches(const ChunkKey& key, const TerrainStamp& s) const;

    const Body& body() const { return body_; }
    const TerrainSettings& settings() const { return settings_; }
    std::uint64_t seed() const { return seed_; }

    // The layout: roots per face along u and v (level 0), and the coarsest and finest levels.
    // A sphere's faces (a power of two of roots a side) also have coarser levels, below 0, up
    // to one chunk a face, for what is far away (the far side of a hollow world, overhead);
    // other shapes start at their roots.
    int roots_u() const { return roots_u_; }
    int roots_v() const { return roots_v_; }
    int min_level() const { return min_level_; }
    int max_level() const { return max_level_; }
    // Chunks, and grid points, along u (or v) across a whole face at a level.
    std::int64_t chunks_u(int level) const;
    std::int64_t chunks_v(int level) const;
    std::int64_t points_u(int level) const;
    std::int64_t points_v(int level) const;
    bool valid(const ChunkKey& key) const;
    // The four children of a chunk, and its parent (a chunk at the coarsest level is its own).
    std::array<ChunkKey, 4> children(const ChunkKey& key) const;
    ChunkKey parent(const ChunkKey& key) const;
    // The neighbour across an edge at the same level (a flat-wrap or curved cylinder wraps
    // round along u); none past a face's edge.
    std::optional<ChunkKey> neighbour(const ChunkKey& key, ChunkEdge edge) const;

    glm::dvec3 noise_position(int face, double u, double v) const;  // relative to the body
    double height(int face, double u, double v) const;              // with the stamps
    double base_height(int face, double u, double v) const;         // as generated
    // How much a stamp applies at a point: 1 on its line, falling to 0 at its radius.
    double stamp_weight(const TerrainStamp& s, int face, double u, double v) const;
    // Where a ray first meets the ground (within `distance` metres), or none.
    std::optional<glm::dvec3> raycast(const glm::dvec3& origin, const glm::dvec3& direction,
                                      double distance) const;
    // The ground under (or over) a point: on the terrain at the point's place, and how high the
    // point is above it along up (negative: below the ground).
    glm::dvec3 ground(const glm::dvec3& p) const;
    double above_ground(const glm::dvec3& p) const;
    // Where a point is drawn, in the room: a flat-wrap cylinder's u doesn't wrap here, so a
    // chunk at its east edge lies past it (streaming draws it again a circumference over).
    glm::dvec3 position(int face, double u, double v, double h) const;

    // The biome at a point: x moisture (0 dry to 1 lush), y fine detail (-1 to 1), z painted
    // ground (-1 sand to 1 rock), with the paint stamps.
    glm::vec3 biome_at(int face, double u, double v) const;
    // Where a chunk's mesh is placed: its middle, on the ground at height 0.
    glm::dvec3 chunk_origin(const ChunkKey& key) const;
    // grid: kChunkQuads + 1 points a side. coarser[edge]: the neighbour there is a level up.
    ChunkMesh mesh(const ChunkKey& key, const std::array<bool, 4>& coarser = {}) const;
    // The water over a chunk: its grid at sea level (the same points, and the same stitching to
    // coarser neighbours), with `heights` holding the water's depth at each point (negative
    // where the ground is above the sea). None if the ground is above the sea everywhere in it.
    std::optional<ChunkMesh> water_mesh(const ChunkKey& key,
                                        const std::array<bool, 4>& coarser = {}) const;
    // A hash of the chunk's key and its generated heights (fnv1a64 over little-endian int32
    // face, level, x, y, then each height's 8 bytes, row by row).
    std::uint64_t chunk_hash(const ChunkKey& key) const;

private:
    // Whether the grid's u then v turn the other way to a flat map's, seen from above (the
    // outside of a sphere): normals and triangles are turned over to face up.
    bool flipped(const ChunkKey& key) const;
    // Spheres' stamps: a stroke's points in 3D, the generated height at a 3D noise position,
    // and smooth's average round a point (along a frame from its direction, not its face).
    std::vector<glm::dvec3> stroke_points(const TerrainStamp& s) const;
    double noise_height(const glm::dvec3& q) const;
    double smooth_average(const glm::dvec3& q, double step) const;
    double u_at(const ChunkKey& key, int i) const;
    double v_at(const ChunkKey& key, int j) const;

    Body body_;
    TerrainSettings settings_;
    std::vector<TerrainStamp> stamps_;
    std::vector<StampBounds> bounds_;
    std::vector<std::vector<glm::dvec3>> strokes_;  // spheres: each stamp's stroke_points
    std::vector<double> reach_;  // spheres: how far each stamp reaches from its first point
    std::uint64_t seed_;
    int roots_u_ = 1, roots_v_ = 1, min_level_ = 0, max_level_ = 0;
};

// A hash over chunk hashes of fixed terrains on four shapes (those of
// schema/tests/terrain_chunks.json): kTerrainFingerprint on every target. The self-test checks
// it on devices.
std::uint64_t terrain_fingerprint();
inline constexpr std::uint64_t kTerrainFingerprint = 0x667639e2e799c3b4ULL;

}  // namespace midgard
