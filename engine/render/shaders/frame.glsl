// SPDX-License-Identifier: Apache-2.0
// The per-frame data every scene shader sees (FrameData in renderer.cpp, which must match).

const int kMaxLocalLights = 8;
const int kMaxCascades = 4;

struct LocalLight {
    vec4 position_range;   // xyz relative to the camera, w: range (metres)
    vec4 color;            // linear rgb times intensity, w: 0 point, 1 spot
    vec4 direction;        // xyz: where a spot shines, w: cos(outer cone)
    vec4 cone;             // x: cos(inner cone)
};

layout(set = 0, binding = 0) uniform Frame {
    mat4 view_proj;
    vec4 up;              // xyz: "up" at the camera (from the world's shape)
    vec4 sun_direction;   // xyz: the direction sunlight travels
    vec4 sun_color;       // linear rgb times intensity
    vec4 ambient_sky;     // linear rgb: ambient light from above
    vec4 ambient_ground;  // linear rgb: ambient light from below
    ivec4 counts;         // x: local lights
    LocalLight lights[kMaxLocalLights];
    mat4 shadow_matrices[kMaxCascades];  // camera-relative position to each cascade's map
    vec4 shadow_splits;   // the view depth where each cascade ends
    vec4 shadow_texels;   // metres per texel of each cascade
    vec4 view_depth;      // a position's view depth: dot(xyz, position) + w
    vec4 shadow_params;   // x: cascades (0: none), y: distance, z: 1 / resolution, w: soft
    mat4 inverse_view_proj;  // clip space back to camera-relative positions
    vec4 sky_zenith;         // linear rgb, brightness included
    vec4 sky_horizon;        // on the sun's side
    vec4 sky_horizon_away;
    vec4 sky_ground;
    vec4 sun_disc;           // linear rgb, w: cos of the disc's radius
    vec4 sun_glow;           // linear rgb, w: tightness (Henyey-Greenstein g)
    vec4 sky_params;         // x: cos of the disc's soft outer edge; y: sin of the horizon's dip
    vec4 fog_color;          // linear rgb, w: how much brighter towards the sun
    vec4 fog_params;         // x: density, y: height falloff, z: camera height, w: sky distance
    // The terrain's look (TerrainLook): xyz the world's centre (camera-relative), w its shape
    // (0 flat, 1 curved cylinder, 2 sphere outside, 3 inside); x biomes, y the season's
    // temperature offset, z 1 with a climate; each biome's ranges (temperature min, max,
    // moisture min, max) and colours (linear, lush and dry).
    vec4 terrain_body;
    vec4 terrain_climate;
    vec4 biome_range[8];
    vec4 biome_color[8];
    vec4 biome_dry[8];
    vec4 clouds;             // the weather's: x cover (0 none .. 1 overcast), y shade, zw drift
    // What falls (render::Falling): x its kind (0 none, 1 rain, 2 snow, 3 ash, 4 sand), y how
    // many, z the box's size, w the time (wrapped); the motion so far less the camera's place,
    // wrapped to the box (xyz), w a rain streak's seconds; the velocity (xyz), w the size.
    vec4 fall;
    vec4 fall_motion;
    vec4 fall_velocity;
    vec4 ground;  // what the weather left (render::GroundCover): x wetness, y snow (0 .. 1)
    vec4 stars;   // render::Stars: x brightness (0: none), y density, z seed
    vec4 clock;   // x: seconds of play (repeating every hour), for surface shaders (5.17)
    // The ground's picture textures (6.0b), a slot each (0 grass, 1 dry grass, 2 sand, 3 rock,
    // 4 snow, 5 and 6 the paint layers, then each biome's lush and dry): x 1 / its tile, y its slice, z its maps (bits: 1
    // colour, 2 metal/roughness, 4 normal, 8 occlusion, 16 emissive; 0: its colour); and the
    // camera's place modulo the tile (xyz), so the pattern stays put on the world.
    vec4 ground_layer[23];   // (w: 1 / the larger scale's tile, step 3)
    vec4 ground_offset[23];
    vec4 ground_offset_far[23];  // the camera's place modulo the larger scale's tile
    // x y z: the camera's place modulo 4000 m (the slow variation's period); w: 1 on the
    // phone tier (a cheaper path: one scale, fewer projections).
    vec4 ground_macro;
} frame;
