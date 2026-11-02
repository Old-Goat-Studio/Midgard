// SPDX-License-Identifier: Apache-2.0
// A scene's navigation (milestone 5.12): the triangles agents walk on and round, from its
// static colliders (scenery: no rigid body, or a static one), in the scene's frame.
#pragma once

#include "midgard/collider_outlines.h"
#include "midgard/navmesh.h"
#include "midgard/scene.h"

namespace midgard {

// Whether "up" is +Y everywhere in the scene (no world shape, a bounded area or a flat-wrap
// cylinder): its navigation is one flat mesh.
bool flat_world(const Scene& scene);

// The static colliders as triangles, relative to `origin`: mesh and convex colliders their
// meshes' triangles (from `mesh`), boxes and heightfields their own, spheres and capsules the
// boxes round them (obstacles).
NavGeometry nav_geometry(const Scene& scene, const glm::dvec3& origin, const MeshLookup& mesh);

}  // namespace midgard
