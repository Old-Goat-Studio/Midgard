// SPDX-License-Identifier: Apache-2.0
#include "midgard/navmesh.h"

#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>

#include "midgard/jobs.h"

namespace midgard {

namespace {

constexpr int kMaxTiles = 1 << 14;  // (with kMaxPolys, the 22 bits of a polygon reference)
constexpr int kMaxPolys = 1 << 8;
constexpr int kMaxVertsPerPoly = 6;
constexpr int kMaxPath = 512;
constexpr unsigned short kWalk = 1;  // the polygons' one flag

// Recast's objects, freed however a build ends.
struct Built {
    rcHeightfield* hf = nullptr;
    rcCompactHeightfield* chf = nullptr;
    rcContourSet* cset = nullptr;
    rcPolyMesh* pmesh = nullptr;
    rcPolyMeshDetail* dmesh = nullptr;
    ~Built() {
        rcFreeHeightField(hf);
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
    }
};

}  // namespace

void NavGeometry::add_triangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const auto first = std::uint32_t(vertices.size());
    vertices.insert(vertices.end(), {a, b, c});
    indices.insert(indices.end(), {first, first + 1, first + 2});
}

std::optional<std::pair<glm::vec3, glm::vec3>> NavGeometry::bounds() const {
    if (vertices.empty()) return std::nullopt;
    glm::vec3 lo = vertices[0], hi = vertices[0];
    for (const glm::vec3& v : vertices) {
        lo = glm::min(lo, v);
        hi = glm::max(hi, v);
    }
    return std::pair{lo, hi};
}

NavTileData build_nav_tile(const NavSettings& s, const glm::vec3& origin, NavTileKey key,
                           const NavGeometry& geometry) {
    rcConfig cfg{};
    cfg.cs = s.cell_size;
    cfg.ch = s.cell_height;
    cfg.walkableSlopeAngle = s.agent.max_slope;
    cfg.walkableHeight = int(std::ceil(s.agent.height / cfg.ch));
    cfg.walkableClimb = int(std::floor(s.agent.max_climb / cfg.ch));
    cfg.walkableRadius = int(std::ceil(s.agent.radius / cfg.cs));
    cfg.maxEdgeLen = int(12.0f / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = 8 * 8;
    cfg.mergeRegionArea = 20 * 20;
    cfg.maxVertsPerPoly = kMaxVertsPerPoly;
    cfg.tileSize = int(std::ceil(s.tile_size / cfg.cs));
    cfg.borderSize = cfg.walkableRadius + 3;
    cfg.width = cfg.height = cfg.tileSize + 2 * cfg.borderSize;
    cfg.detailSampleDist = 6.0f * cfg.cs;
    cfg.detailSampleMaxError = cfg.ch;

    // The tile's square, and the border round it (so walls just outside count).
    const float border = float(cfg.borderSize) * cfg.cs;
    const glm::vec2 lo(origin.x + float(key.x) * s.tile_size - border,
                       origin.z + float(key.z) * s.tile_size - border);
    const glm::vec2 hi = lo + glm::vec2(s.tile_size + 2.0f * border);
    // Only the triangles that reach it.
    std::vector<float> verts;
    std::vector<int> tris;
    float ylo = 1e30f, yhi = -1e30f;
    for (size_t i = 0; i + 2 < geometry.indices.size(); i += 3) {
        const glm::vec3& a = geometry.vertices[geometry.indices[i]];
        const glm::vec3& b = geometry.vertices[geometry.indices[i + 1]];
        const glm::vec3& c = geometry.vertices[geometry.indices[i + 2]];
        if (std::max({a.x, b.x, c.x}) < lo.x || std::min({a.x, b.x, c.x}) > hi.x ||
            std::max({a.z, b.z, c.z}) < lo.y || std::min({a.z, b.z, c.z}) > hi.y)
            continue;
        for (const glm::vec3& v : {a, b, c}) {
            tris.push_back(int(verts.size() / 3));
            verts.insert(verts.end(), {v.x, v.y, v.z});
            ylo = std::min(ylo, v.y);
            yhi = std::max(yhi, v.y);
        }
    }
    if (tris.empty()) return {};
    cfg.bmin[0] = lo.x;
    cfg.bmin[1] = ylo - 1.0f;
    cfg.bmin[2] = lo.y;
    cfg.bmax[0] = hi.x;
    cfg.bmax[1] = yhi + s.agent.height + 1.0f;
    cfg.bmax[2] = hi.y;

    rcContext ctx(false);
    Built b;
    const int nverts = int(verts.size() / 3), ntris = int(tris.size() / 3);
    b.hf = rcAllocHeightfield();
    if (!b.hf || !rcCreateHeightfield(&ctx, *b.hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax,
                                      cfg.cs, cfg.ch))
        return {};
    std::vector<unsigned char> areas(size_t(ntris), 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts.data(), nverts, tris.data(), ntris,
                            areas.data());
    if (!rcRasterizeTriangles(&ctx, verts.data(), nverts, tris.data(), areas.data(), ntris, *b.hf,
                              cfg.walkableClimb))
        return {};
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *b.hf);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *b.hf);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *b.hf);
    b.chf = rcAllocCompactHeightfield();
    if (!b.chf ||
        !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *b.hf, *b.chf) ||
        !rcErodeWalkableArea(&ctx, cfg.walkableRadius, *b.chf) ||
        !rcBuildDistanceField(&ctx, *b.chf) ||
        !rcBuildRegions(&ctx, *b.chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea))
        return {};
    b.cset = rcAllocContourSet();
    if (!b.cset ||
        !rcBuildContours(&ctx, *b.chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *b.cset) ||
        b.cset->nconts == 0)
        return {};
    b.pmesh = rcAllocPolyMesh();
    if (!b.pmesh || !rcBuildPolyMesh(&ctx, *b.cset, cfg.maxVertsPerPoly, *b.pmesh) ||
        b.pmesh->npolys == 0)
        return {};
    b.dmesh = rcAllocPolyMeshDetail();
    if (!b.dmesh || !rcBuildPolyMeshDetail(&ctx, *b.pmesh, *b.chf, cfg.detailSampleDist,
                                           cfg.detailSampleMaxError, *b.dmesh))
        return {};
    for (int i = 0; i < b.pmesh->npolys; ++i)
        b.pmesh->flags[i] = b.pmesh->areas[i] == RC_WALKABLE_AREA ? kWalk : 0;

    dtNavMeshCreateParams p{};
    p.verts = b.pmesh->verts;
    p.vertCount = b.pmesh->nverts;
    p.polys = b.pmesh->polys;
    p.polyAreas = b.pmesh->areas;
    p.polyFlags = b.pmesh->flags;
    p.polyCount = b.pmesh->npolys;
    p.nvp = b.pmesh->nvp;
    p.detailMeshes = b.dmesh->meshes;
    p.detailVerts = b.dmesh->verts;
    p.detailVertsCount = b.dmesh->nverts;
    p.detailTris = b.dmesh->tris;
    p.detailTriCount = b.dmesh->ntris;
    p.walkableHeight = s.agent.height;
    p.walkableRadius = s.agent.radius;
    p.walkableClimb = s.agent.max_climb;
    p.tileX = key.x;
    p.tileY = key.z;
    p.tileLayer = 0;
    rcVcopy(p.bmin, b.pmesh->bmin);
    rcVcopy(p.bmax, b.pmesh->bmax);
    p.cs = cfg.cs;
    p.ch = cfg.ch;
    p.buildBvTree = true;
    unsigned char* data = nullptr;
    int size = 0;
    if (!dtCreateNavMeshData(&p, &data, &size)) return {};
    NavTileData out(data, data + size);
    dtFree(data);
    return out;
}

struct NavMesh::Impl {
    dtNavMesh* mesh = nullptr;
    dtNavMeshQuery* query = nullptr;
    dtQueryFilter filter;
    mutable std::mutex lock;  // (queries share Detour's node pool)
    ~Impl() {
        dtFreeNavMeshQuery(query);
        dtFreeNavMesh(mesh);
    }
};

NavMesh::NavMesh(const NavSettings& settings, const glm::vec3& origin)
    : impl_(std::make_unique<Impl>()), settings_(settings), origin_(origin) {
    dtNavMeshParams params{};
    params.orig[0] = origin.x;
    params.orig[1] = origin.y;
    params.orig[2] = origin.z;
    params.tileWidth = params.tileHeight = settings.tile_size;
    params.maxTiles = kMaxTiles;
    params.maxPolys = kMaxPolys;
    impl_->mesh = dtAllocNavMesh();
    impl_->query = dtAllocNavMeshQuery();
    if (!impl_->mesh || dtStatusFailed(impl_->mesh->init(&params)) || !impl_->query ||
        dtStatusFailed(impl_->query->init(impl_->mesh, 4096)))
        throw std::runtime_error("navigation: Detour couldn't start");
    impl_->filter.setIncludeFlags(kWalk);
    impl_->filter.setExcludeFlags(0);
}

NavMesh::~NavMesh() = default;

NavTileKey NavMesh::tile_at(const glm::vec3& p) const {
    return {int(std::floor((p.x - origin_.x) / settings_.tile_size)),
            int(std::floor((p.z - origin_.z) / settings_.tile_size))};
}

void NavMesh::add_tile(NavTileKey key, const NavTileData& data) {
    std::lock_guard guard(impl_->lock);
    if (const dtTileRef old = impl_->mesh->getTileRefAt(key.x, key.z, 0))
        impl_->mesh->removeTile(old, nullptr, nullptr);
    if (data.empty()) return;
    auto* copy = static_cast<unsigned char*>(dtAlloc(int(data.size()), DT_ALLOC_PERM));
    std::memcpy(copy, data.data(), data.size());
    if (dtStatusFailed(impl_->mesh->addTile(copy, int(data.size()), DT_TILE_FREE_DATA, 0, nullptr)))
        dtFree(copy);
}

void NavMesh::remove_tile(NavTileKey key) {
    add_tile(key, {});
}

std::size_t NavMesh::tile_count() const {
    std::lock_guard guard(impl_->lock);
    const dtNavMesh& m = *impl_->mesh;
    std::size_t n = 0;
    for (int i = 0; i < m.getMaxTiles(); ++i)
        if (const dtMeshTile* t = m.getTile(i); t && t->header) ++n;
    return n;
}

std::size_t NavMesh::polygon_count() const {
    std::lock_guard guard(impl_->lock);
    const dtNavMesh& m = *impl_->mesh;
    std::size_t n = 0;
    for (int i = 0; i < m.getMaxTiles(); ++i)
        if (const dtMeshTile* t = m.getTile(i); t && t->header) n += size_t(t->header->polyCount);
    return n;
}

std::optional<glm::vec3> NavMesh::nearest(const glm::vec3& p, float reach) const {
    std::lock_guard guard(impl_->lock);
    const float extents[3] = {reach, reach * 2.0f, reach};
    dtPolyRef ref = 0;
    float at[3];
    if (dtStatusFailed(impl_->query->findNearestPoly(&p.x, extents, &impl_->filter, &ref, at)) ||
        !ref)
        return std::nullopt;
    return glm::vec3(at[0], at[1], at[2]);
}

std::optional<NavMesh::Path> NavMesh::find_path(const glm::vec3& from, const glm::vec3& to,
                                                float reach) const {
    std::lock_guard guard(impl_->lock);
    dtNavMeshQuery& q = *impl_->query;
    const float extents[3] = {reach, reach * 2.0f, reach};
    dtPolyRef start = 0, end = 0;
    float a[3], b[3];
    if (dtStatusFailed(q.findNearestPoly(&from.x, extents, &impl_->filter, &start, a)) || !start ||
        dtStatusFailed(q.findNearestPoly(&to.x, extents, &impl_->filter, &end, b)) || !end)
        return std::nullopt;
    dtPolyRef polys[kMaxPath];
    int count = 0;
    const dtStatus found = q.findPath(start, end, a, b, &impl_->filter, polys, &count, kMaxPath);
    if (dtStatusFailed(found) || count == 0) return std::nullopt;
    Path out;
    float goal[3] = {b[0], b[1], b[2]};
    if (polys[count - 1] != end) {  // partial: as near the end as the last polygon gets
        out.complete = false;
        q.closestPointOnPoly(polys[count - 1], b, goal, nullptr);
    }
    float corners[kMaxPath * 3];
    unsigned char flags[kMaxPath];
    dtPolyRef refs[kMaxPath];
    int n = 0;
    if (dtStatusFailed(
            q.findStraightPath(a, goal, polys, count, corners, flags, refs, &n, kMaxPath)))
        return std::nullopt;
    for (int i = 0; i < n; ++i)
        out.points.emplace_back(corners[3 * i], corners[3 * i + 1], corners[3 * i + 2]);
    return out;
}

std::vector<std::pair<glm::vec3, glm::vec3>> NavMesh::edges() const {
    std::lock_guard guard(impl_->lock);
    std::vector<std::pair<glm::vec3, glm::vec3>> out;
    const dtNavMesh& m = *impl_->mesh;
    for (int i = 0; i < m.getMaxTiles(); ++i) {
        const dtMeshTile* t = m.getTile(i);
        if (!t || !t->header) continue;
        for (int p = 0; p < t->header->polyCount; ++p) {
            const dtPoly& poly = t->polys[p];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;
            for (int j = 0; j < poly.vertCount; ++j) {
                const float* a = &t->verts[poly.verts[j] * 3];
                const float* b = &t->verts[poly.verts[(j + 1) % poly.vertCount] * 3];
                out.emplace_back(glm::vec3(a[0], a[1], a[2]), glm::vec3(b[0], b[1], b[2]));
            }
        }
    }
    return out;
}

std::unique_ptr<NavMesh> build_navmesh(const NavSettings& settings, const NavGeometry& geometry,
                                       JobSystem* jobs) {
    const auto bounds = geometry.bounds();
    const glm::vec3 origin =
        bounds ? glm::vec3(bounds->first.x, 0.0f, bounds->first.z) : glm::vec3(0.0f);
    auto mesh = std::make_unique<NavMesh>(settings, origin);
    if (!bounds) return mesh;
    const NavTileKey hi = mesh->tile_at(bounds->second);
    std::vector<NavTileKey> keys;
    for (int z = 0; z <= hi.z; ++z)
        for (int x = 0; x <= hi.x; ++x) keys.push_back({x, z});
    std::vector<NavTileData> tiles(keys.size());
    const auto build = [&](std::size_t i) {
        tiles[i] = build_nav_tile(settings, origin, keys[i], geometry);
    };
    if (jobs)
        jobs->parallel_for(keys.size(), build);
    else
        for (std::size_t i = 0; i < keys.size(); ++i) build(i);
    for (std::size_t i = 0; i < keys.size(); ++i) mesh->add_tile(keys[i], tiles[i]);
    return mesh;
}

}  // namespace midgard
