// SPDX-License-Identifier: Apache-2.0
// Footsteps (milestone 5.14): a step's sound from what is underfoot.
//
// An entity with a footsteps component steps when its character's animation graph passes the
// component's event ("step", placed at each foot's fall in a walk cycle) or, without one, every
// `every` metres it moves. Each step plays the sound event for the surface under it: a ray
// straight down (along the world's "up" there) meets either an entity's collider, whose
// physics_material names its surface ("wood", "stone", "metal"...), or the terrain, whose
// ground is worked out as its look is drawn (terrain.frag): sand on beaches, rock where it's
// steep, painted sand or rock where it's painted, snow above the snow line (or below freezing,
// with a climate), water under the sea, and grass elsewhere. A surface without a sound plays
// the component's "default", if it has one.
#pragma once

#include <map>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "midgard/body.h"
#include "midgard/terrain.h"

namespace midgard {

// The surfaces footsteps know (an object's physics material may name others too).
const std::vector<std::string>& footstep_surfaces();

// The ground at a point on the terrain (its own: without the seasons' snow): "sand", "rock",
// "snow", "water" or "grass".
std::string terrain_ground(const Terrain& terrain, const Body& body, const glm::dvec3& point);

struct Footsteps {
    std::string event = "step";                 // the animation event it steps on
    float every = 0.0f;                         // or metres between steps (0: only on the event)
    std::map<std::string, std::string> sounds;  // surface -> sound event ("default": any)
};

}  // namespace midgard
