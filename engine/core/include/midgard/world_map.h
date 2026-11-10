// SPDX-License-Identifier: Apache-2.0
// A world's map (milestone 5.11): the whole of its ground as an image, coloured by the sea's
// depth, the ground's biome (or the 5.9 grass and rock) and its snow, shaded by its slopes, in a
// projection that suits its shape:
//   top_down         a bounded area from above (width across, length down)
//   unrolled         a flat or curved cylinder cut along its join and laid flat: round it
//                    across, along it down
//   equirectangular  a sphere's latitude and longitude: longitude across (-180 to 180 degrees,
//                    0 at +Z), the +Y pole along the top
//   cube_faces       a sphere's six cube faces as a cross, 4 x 3 cells: -X, +Z, +X, -Z along the
//                    middle row (each seen from outside, +Y up), +Y above +Z and -Y below it;
//                    the rest of the image is transparent
// Each projection has its own proportions (map_aspect); the editor's World Type editor shows
// them and saves them as images, and the game's map screen (Phase 6) will use them too.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "midgard/terrain.h"

namespace midgard {

enum class MapProjection { TopDown, Unrolled, Equirectangular, CubeFaces };

// The projections a shape's maps can use, its usual one first.
std::vector<MapProjection> map_projections(Shape::Kind kind);
const char* map_projection_name(MapProjection p);
// From its name; none for an unknown name.
std::optional<MapProjection> map_projection_from_name(const std::string& name);

// Width over height of the projection's map of the world (a cylinder 5 km long and 6 km round
// is 1.2; latitude and longitude 2; the cube's cross 4/3).
double map_aspect(const Terrain& terrain, MapProjection p);

// width x height pixels, RGBA8, top row first. Throws std::invalid_argument if the projection
// isn't one of the shape's.
std::vector<std::uint8_t> world_map(const Terrain& terrain, int width, int height, MapProjection p);
// In the shape's usual projection.
std::vector<std::uint8_t> world_map(const Terrain& terrain, int width, int height);

// The pixels of a map whose pixels are these places (none: transparent), `across` and `down`
// metres apart (for the slopes' shading): the colouring every map uses (world_map,
// compass.h's local_map).
std::vector<std::uint8_t> map_pixels(const Terrain& terrain, int width, int height,
                                     const std::vector<std::optional<SurfacePoint>>& places,
                                     double across, double down);

// Where a map pixel's middle is on the world (face, u, v), or none for a pixel off the map (the
// corners of the cube's cross).
std::optional<SurfacePoint> world_map_place(const Terrain& terrain, int width, int height, int x,
                                            int y, MapProjection p);

}  // namespace midgard
