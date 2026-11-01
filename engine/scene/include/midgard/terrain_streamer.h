// SPDX-License-Identifier: Apache-2.0
// Streaming terrain (milestone 5.9): which chunks of a world's terrain (midgard/terrain.h) to
// draw from where the camera is, generated on the job system and uploaded to the renderer.
//
// Choosing: every root chunk within range is split while the camera is close to it compared
// with its size, then neighbours are split until no two that touch differ by more than one
// level (so a chunk's edge only ever meets a coarser neighbour's one level up, which
// Terrain::mesh stitches to). A flat-wrap cylinder's roots are drawn on whichever side of the
// camera is nearer, so the world goes round.
//
// Streaming: chunks not yet generated are made by jobs; until every chunk wanted is ready, the
// chunks drawn last stay drawn, so the camera never sees holes. Chunks no longer wanted stay
// cached for a while (up to a limit), then their meshes are freed.
//
// The ends: a cylinder's ends are walls (TerrainSettings::wall): discs closing a curved
// cylinder, and walls across a flat-wrap cylinder.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "midgard/jobs.h"
#include "midgard/render/mesh_data.h"
#include "midgard/render/renderer.h"
#include "midgard/terrain.h"

namespace midgard {

// A chunk to draw: its key, which of its edges meet a coarser neighbour, and (flat-wrap
// cylinders) how many circumferences along x it is drawn from where it is generated.
struct TerrainLeaf {
    ChunkKey key;
    std::array<bool, 4> coarser{};
    int wrap = 0;
};

struct TerrainStreamStats {
    size_t wanted = 0;      // chunks chosen for the camera's place
    size_t drawn = 0;       // chunks being drawn
    size_t generating = 0;  // jobs running
    size_t cached = 0;      // chunk meshes kept (generated or on the GPU)
    bool complete = false;  // everything wanted is drawn
};

class TerrainStreamer {
public:
    // jobs: none generates on the calling thread.
    TerrainStreamer(const Body& body, const TerrainSettings& settings, JobSystem* jobs = nullptr);
    ~TerrainStreamer();  // waits for its jobs
    TerrainStreamer(const TerrainStreamer&) = delete;
    TerrainStreamer& operator=(const TerrainStreamer&) = delete;

    const Terrain& terrain() const { return terrain_; }
    // How far terrain is drawn (metres; a curved cylinder always shows its far side), and how
    // close the camera comes, in chunk sizes, before a chunk is split.
    void set_range(double metres) { range_ = metres; }
    void set_detail(double chunk_sizes) { detail_ = chunk_sizes; }
    // The most chunk meshes kept (wanted ones are always kept).
    void set_cache_limit(size_t chunks) { cache_limit_ = chunks; }

    // The chunks to draw seen from `camera` (pure: the choice alone).
    std::vector<TerrainLeaf> choose(const glm::dvec3& camera) const;

    // Chooses for `camera`, starts jobs for chunks not yet generated, uploads those generated
    // (renderer: none keeps them on the CPU, for tests) and frees chunks long unwanted.
    // wait: finishes every chunk wanted now (a game's first frame, screenshots).
    void update(const glm::dvec3& camera, render::Renderer* renderer, bool wait = false);
    // The chunks and end walls drawn, relative to the camera's origin.
    std::vector<render::DrawItem> draws(const glm::dvec3& origin) const;
    TerrainStreamStats stats() const;
    const std::vector<TerrainLeaf>& drawn() const { return drawn_; }

    // Forgets everything on the GPU (the renderer is going away).
    void release(render::Renderer& renderer);

private:
    struct Entry;
    using MeshKey = std::uint64_t;  // a chunk key and its coarser edges
    static MeshKey mesh_key(const TerrainLeaf& leaf);
    double wrap_length() const;  // a flat-wrap cylinder's circumference, else 0
    void upload_walls(render::Renderer& renderer);

    Terrain terrain_;
    JobSystem* jobs_;
    double range_ = 8000.0, detail_ = 2.5;
    size_t cache_limit_ = 512;
    std::uint64_t tick_ = 0;
    std::unordered_map<MeshKey, std::unique_ptr<Entry>> cache_;
    std::vector<TerrainLeaf> wanted_, drawn_;
    struct Wall {
        render::MeshId mesh = 0;
        glm::dvec3 at{0.0};
    };
    std::vector<Wall> walls_;
    bool walls_uploaded_ = false;
};

// A chunk's mesh for the renderer (uv: metres across the face, over 16).
render::MeshData chunk_mesh_data(const ChunkMesh& mesh);
// The water over a chunk for the renderer (tangent w: the depth below each point).
render::MeshData water_mesh_data(const ChunkMesh& mesh);

}  // namespace midgard
