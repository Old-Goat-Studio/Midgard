# Entity components

Each row of the `entity` table has a `components` column holding a JSON object. Its keys are
component names, and its values follow the shapes below. The editor writes them; the engine reads
them when it builds a scene. Unknown components are ignored by the engine, so new ones can be
added without breaking older runtimes.

Colours are sRGB values from 0 to 1, as a designer picks them. Positions are in metres (the
engine keeps them in double precision and renders relative to the camera, so worlds can be tens
of kilometres across), angles in degrees. The world is right-handed with **+Y up** (on curved worlds, "up" comes from the shape
mapping instead, from Phase 5).

| Component | Shape | Notes |
|---|---|---|
| `transform` | `{"position": [x, y, z], "rotation": [x, y, z], "scale": [x, y, z]}` | Rotation is Euler degrees combined as `R = Ry · Rx · Rz` (yaw, pitch, roll), and the full local matrix is `T · R · S`. All three keys are optional (defaults: origin, no rotation, scale 1). Relative to the parent entity. |
| `mesh` | `{"asset": "<content id>"}` | A mesh in the package's `content` list (a `.glb`). Ids: `prim_cube`, `prim_sphere`, `prim_plane` (editor primitives), `asset:<path>` (a `.glb` in the project's `objects/` folder; older projects' `assets/` folder is moved there when they open), `builtin:cube` (built into the engine, not in `game.pak`), `builtin:surface` (the world's surface, made by the engine from the scene's `body`: within a few centimetres of the true curve, facing up into the world, placed with the entity at the origin and no rotation or scale; with a `{"shape": "mesh"}` collider it is the ground to walk on; it casts no shadow, so the far side of a cylinder or a hollow world doesn't shade the floor). Optional `"lods": [{"asset", "distance"}]` are simpler versions drawn from `distance` metres from the camera on (the farthest one that applies). |
| `material` | `{"color": [r, g, b, a], "metallic": 0, "roughness": 0.8, "emissive": [r, g, b], "emissive_strength": 1}` | glTF's metal/roughness factors. Alpha is optional (default 1); metallic and roughness are 0 to 1 (defaults: not metal, fairly rough); emissive light is its colour times its strength (default none). Optional `"textures": {"base_color", "metallic_roughness", "normal", "occlusion", "emissive"}` name KTX2 textures in the package by content id (an imported model's own `obj:<object>/tex<n>`, or an image file in the project `tex:<path>`, with `?linear` on the end for the slots that aren't colours: metallic_roughness, normal, occlusion), multiplied by the factors as in glTF, with `normal_scale` (default 1) and `occlusion_strength` (0 to 1, default 1). A texture missing from the package is a warning; the material is drawn without it. |
| `camera` | `{"fov": 55, "near": 0.05, "far": 500, "view": "third", "blend": 0.35, "views": {"first": {…}, "third": {…}, "top": {…}}}` | Vertical field of view in degrees (the third-person view's). The scene's first camera is used. With a `player` in the scene it is the player's camera (`midgard/camera_rig.h`), with three views: `first` person (at the player's eyes), `third` person (orbiting the player, or over its shoulder) and `top`-down (above and behind at a fixed angle). `view` is the one the game starts in (an exported game starts in the player's last view instead, from their profile: see README.md); the view action (V, or the gamepad's Y) steps third → first → top-down → third, zooming all the way in passes from third to first person and zooming out from first back to third, and each change blends over `blend` seconds (0 to 10). Each of `views` may set: `fov` (10 to 150 degrees), `distance` (metres from the player at the start; for third person, without it, where the camera was placed), `min_distance` and `max_distance` (the zoom range), `height` (above the player's centre: what the camera looks at, or the eyes in first person), `shoulder` (third person: metres to the right, negative for left), `min_pitch` and `max_pitch` (degrees up and down; top-down uses `min_pitch` as its fixed angle), `radius` (0 to 5 m: a sphere swept from the player out to the camera, which stops short of anything solid, so it never passes through walls) and `smoothing` (0 to 5: seconds of lag following the player). Defaults: first `{"fov": 70, "height": 0.35, "min_pitch": -80, "max_pitch": 80, "radius": 0.1}`; third `{"fov": 55, "distance": 6, "min_distance": 1.5, "max_distance": 30, "height": 0.5, "shoulder": 0, "min_pitch": -10, "max_pitch": 80, "radius": 0.25, "smoothing": 0}`; top `{"fov": 45, "distance": 18, "min_distance": 8, "max_distance": 40, "height": 0, "min_pitch": 60, "max_pitch": 60, "radius": 0.25}`. `views.third.fov` wins over `fov`. "Up" in every view comes from the scene's `body`, so views stay level on a planet or inside a cylinder. |
| `light` | `{"type": "directional", "color": [r, g, b], "intensity": 1.0}`, or `"point"`/`"spot"` with `"range": 10`, and for spots `"inner_angle": 20, "outer_angle": 30` (degrees) | Directional lights and spots shine along the entity's local −Z axis (after rotation). The scene's first directional light is the sun. With `"follows_clock": true` (directional only) the sun moves with the game's clock (and the editor's Calendar dock scrubber): where it is placed is where it stands at solar noon, and it rises and sets on the great circle through there and the horizon at right angles to it, dimming through twilight, with the ambient light down to a quarter at night. Point and spot lights fade as glTF's KHR_lights_punctual does (inverse square, to zero at the range); the eight nearest the camera are used. Intensity 1 lights a white surface facing the light to white. |
| `script` | `{"module": "door"}` | A Python module in the project's `scripts/` folder that drives the entity (`"door"` is `scripts/door.py`, `"enemies.wolf"` is `scripts/enemies/wolf.py`). Its functions and the `midgard` API are described in `schema/midgard.pyi`. |
| `trigger` | `{"size": [x, y, z]}` | A box centred on the entity (in its local units, so its scale and rotation apply) that reports when the player enters or leaves it: `on_trigger_enter` / `on_trigger_exit` in the entity's script, and in the player's. It is a sensor in the physics world, separate from any `collider` (so a crate can both land on the ground and be a trigger) and moving with its entity: nothing bumps into it, and the player counts as inside once its capsule (radius 0.3 m, 1 m tall, upright along "up") overlaps it. Default size 1 × 1 × 1. |
| `collider` | `{"shape": "box", "size": [x, y, z], "radius": 0.5, "height": 2, "offset": [x, y, z]}` | The entity's shape for physics, in its local units (its scale applies). `shape` is `box` (`size`, default 1 × 1 × 1), `sphere` (`radius`), `capsule` (`radius`, and `height` end to end along local Y, at least twice the radius), `convex` (wraps the entity's mesh, or the content id in `"mesh"`), `mesh` (the mesh's triangles; static or kinematic bodies only) or `heightfield` (`"samples"`: n, `"heights"`: n × n numbers row by row along X then Z, `"spacing"`: metres between them; static only). `offset` moves the shape within the entity. Without a `rigidbody` the entity is static scenery. |
| `rigidbody` | `{"motion": "dynamic", "mass": 0, "layer": 0}` | How the entity moves in the physics world: `dynamic` (gravity, forces and collisions move it; the engine writes its transform), `kinematic` (the game or a script moves it, and it pushes dynamic bodies aside) or `static`. `mass` in kilograms (0: from the collider's volume at the density of water). `layer` is one of the project's collision layers (0 to 15, the `physics_layer` table). Needs a `collider`. |
| `physics_material` | `{"friction": 0.5, "bounciness": 0}` | How the collider's surface behaves: friction from 0 (ice) up, bounciness from 0 (stops dead) to 1 (bounces back as fast as it came). Defaults when absent. |
| `sky` | `{"color": [r, g, b], "ambient": [r, g, b], "environment": "env_earth"}` | The scene's sky colour and ambient light. The first entity with a sky sets them, and the scene settings below are only the fallback for scenes without one. Defaults as in the settings. With `environment` (an id in the environment table, migration 10: every project has the Earth-like `env_earth`), the world's look follows the clock instead: the sky, the sunlight's colour and strength, the ambient light, the fog and the water's colour and clarity are the environment's curves at the solar phase (curves it doesn't set come from an analytic Earth-like sky at the sun's height), and the scene's sun follows the clock (as with `follows_clock`). An environment that isn't in the package is a warning, and the sky stays flat. |
| `group` | `{}` | A group: its child entities are the parts of one thing. Unlike a folder it has its own `transform` (its centre; keep its scale even, the same on all three axes), so moving, turning or scaling it carries all its parts, scripts and components included. With a `dynamic` or `kinematic` `rigidbody`, its parts' colliders (descendants with a `collider`) make **one compound body** that moves as a whole; the parts' own rigid bodies are then ignored, and their triggers move with them. Such a group can't hold `mesh` or `heightfield` colliders (use `convex`). A static group, or one without a rigid body, leaves each part's collider as it is. |
| `object` | `{"id": "obj_crate"}` | An instance of an object: a template in the package's `object` table (migration 6), made once and placed many times. The engine fills in the template's parts as this entity's children (ids `<entity id>/<n>`, not in the database), so a changed template changes every instance. It acts as a `group`: its `transform` places the whole, and with a `dynamic` or `kinematic` `rigidbody` the parts' colliders make one body. An unknown object is a warning and shows nothing. |
| `folder` | `{}` | A folder in the editor's Scene panel that groups its child entities. The engine sees an entity with only a transform (the editor keeps it at the origin). |
| `player` | `{"speed": 4.0, "run_multiplier": 2.0, "jump": 4.5, "radius": 0.3, "height": 1.0, "max_slope": 50, "step_height": 0.3}` | The entity the player controls: a character in the physics world, an upright capsule (`radius`, and `height` end to end) centred on the entity, that walks at `speed` metres per second (times `run_multiplier`, 1 to 5, while the run action is held: Left Shift, or clicking the gamepad's left stick), stands on the ground, slides along walls, walks up steps up to `step_height` metres and slopes up to `max_slope` degrees, and jumps (the jump action) at `jump` metres per second upwards (0: no jumping). "Up" and gravity come from the scene's `body`, so it walks all the way round a planet. Its own `collider` and `rigidbody`, if it has any, are left out. If a scene has a player, its first camera follows it in the camera's views: look turns the view (and the player, in first person), the wheel zooms, and movement is relative to the camera, level with the ground underfoot; the player turns to face where it walks. In first person its own `mesh` isn't drawn. |

## Scene settings

The `scene.settings` column (migration 2) holds a JSON object. A `sky` entity outranks it; the
editor creates one in every scene's World folder.

| Key | Shape | Default |
|---|---|---|
| `sky` | `[r, g, b]` | `[0.08, 0.11, 0.18]` (night blue) |
| `sun_mode` | `"uniform"` or `"latitude"` | `"uniform"`. How day and night work on an outside sphere (`midgard/sunlight.h`): `uniform` shifts each place's time by its longitude (noon on one side, midnight on the other) and gives every place the calendar's daylight share; `latitude` tilts the planet by the calendar's tilt, so the sun's height and the day's length follow latitude and the time of year (polar day and night), with the start's latitude set by how high the sun is placed at noon. Other shapes: one sun crossing the sky (bounded areas, flat-wrap cylinders), or, inside spheres and curved cylinders, a light sphere 10 m across at the centre or a light rod 5 m across down the axis, drawn glowing, with shades going round: the light always comes straight down, and each place's time is shifted by its angle round the axis, the light brightening through its dawn and fading at its dusk. |
| `ambient` | `[r, g, b]` | `[0.18, 0.2, 0.26]` |
| `body` | `{"id": "world", "shape": "sphere_outside", "radius": 1000, "gravity": 9.81, "origin": [x, y, z]}` | none: flat ground, +Y up, gravity 9.81 m/s² down |
| `terrain` | `{"world_type": "wt_custom", "version": 1, "seed": 1}` | none: no terrain. The world type and version its ground is made from, and its own seed (default: the body's); see below. Packages from before migration 12 hold the ground's settings here instead. |

`body` is the scene's world shape (`midgard/body.h`): where "up" points and how gravity pulls.
Shapes and their sizes in metres: `bounded_area` (`width`, `length`), `flat_cylinder`
(`circumference`, `length`), `curved_cylinder` (`radius`, `length`; you live inside it, spin
gravity pulls outwards), `sphere_outside` (`radius`; a planet) and `sphere_inside` (`radius`; a
hollow world). `origin` is where the shape's centre is (default the origin; the editor's World ▸
World Shape… puts the surface through the scene's origin with "up" along +Y there: a planet's
centre at `[0, -radius, 0]`, a hollow world's or a cylinder's at `[0, radius, 0]`), and sizes must lie
within the roadmap's limits (§7, "Size and precision"); the engine refuses others. Physics pulls
every moving body along the shape's gravity where it is, and the sun's shadows use its "up".

`terrain` names a world type (the `world_type` and `world_type_version` tables, migration 12;
`midgard/world_type.h`): a recipe for worlds, kept in versions so a world still generates the
same after its world type changes. A version a shipped game used (an export marks the ones its
scenes use) can't be changed or deleted; editing it saves a new version. A recipe is
`{"shapes": [...], "size": [smallest, largest], "gravity", "atmosphere", "calendar",
"terrain": {"base": 8, "height_scale": 60, "feature_size": 500, "octaves": 6, "sea_level": 0,
"ocean_band": 300, "ocean_depth": 30, "end_ramp": 150, "wall_height": 120, "wall": "cliff",
"spacing": 2, "snow_line": 90}, "graph": {...}}`; the cook refuses a world type made for other
shapes or sizes than the scene's. A recipe's `graph` (`midgard/worldgen.h`) makes the ground's
height from nodes (noise, ridged noise, constants, latitude and distances from an area's edge or a
cylinder's ends, arithmetic, curves, terraces, and the smooth and erode filters, which can't be
nested); without one, the ground is `base` plus `height_scale` times noise as below. Either way
the shape's edges then shape it (the ocean band, the ramps to a cylinder's walls). Filters make a
ground about five times the work: the cook warns when one costs more than four times the plain
ground, which streams more slowly than a chunk's budget.

A recipe's ground can also have a `climate` and `biomes` (milestone 5.11):
`"climate": {"temperature": 14, "latitude_cooling": 40, "lapse_rate": 6.5, "variation": 3,
"season_swing": 12}` gives each point a temperature in degrees Celsius (`temperature` at sea
level, less `latitude_cooling` towards a planet's poles and `lapse_rate` a kilometre up, give or
take `variation`), warmer in summer and colder in winter by half the `season_swing`; snow then
lies where it is below freezing, in place of the snow line. Up to 8 `biomes`,
`{"name", "temperature": [lowest, highest], "moisture": [0 to 1, 0 to 1], "color": [r, g, b],
"dry_color": [r, g, b]}` (sRGB, lush and dry), colour the ground where both are in range,
blended across their edges; sand on beaches, rock on slopes and snow still come by their own
rules. Without them, the ground is the 5.9 grass. Migration 12 made each scene's terrain settings into its own Custom world
type, with the same ground.

The ground is hills and valleys on any shape (`midgard/terrain.h`; spheres from milestone
5.10). Heights come from noise at points on the shape's
surface, so they are the same on every device and a cylinder's join is seamless: `base` plus
`height_scale` times noise whose largest hills are `feature_size` metres across, with `octaves`
layers of detail (1 to 12), from `seed` (default: the body's). A bounded area sinks to
`ocean_depth` below the sea over `ocean_band` metres at its edges; a cylinder rises to
`wall_height` over `end_ramp` metres at its ends, where walls close it (`wall`: `cliff`,
`fortification` or `barrier`). `spacing` is the finest grid's spacing in metres (0.25 to 64).
The ground's material comes from the terrain itself: grass from dry to lush with a moisture
that varies over kilometres, sand on beaches and under shallow water, rock where it's steep, and
snow above `snow_line` metres. The calendar's seasons change it: the snow line comes down to the
shore in deep winter, grass dries in high summer and turns golden-brown in autumn (on a tilted
planet in Latitude mode, by hemisphere).
Sculpting and painting are stored as terrain stamps (the `terrain_stamp` table, migration 11):
each row one brush stroke along points on the face, applied in order on top of the generated
ground: `raise` or `lower` (by `strength` metres along the stroke's line), `smooth`, `flatten`
(to `height`) or `paint` (`lush`, `dry`, `sand`, `rock` or `clear`), each falling off to nothing
at `radius` metres. On a sphere (a planet or a hollow world) a stroke reaches across the cube's
face edges: its fall-off is measured in 3D, and its points may run a little past its `face`'s
edges (`u` or `v` outside 0 to 1, in that face's terms), as the viewport records a stroke dragged
across one. The editor's Terrain dock draws them in the viewport; the cook refuses strokes
without points, a radius outside 0 to 2000 m, or a strength out of range.
Chunks are streamed round the camera (finer near it) and are solid round the player (a player
starting under the ground is lifted onto it). Water lies at `sea_level` wherever the ground is
below it, in the look of the scene's environment (its `water_*` curves, `wave_strength` and
`wave_size`) or a default one. The cook refuses settings outside these ranges, and warns about
terrain without a `body`.

## Physics layers

Migration 5 adds the project's collision layers: `physics_layer` (`id` 0 to 15, `name`) and
`physics_ignore` (pairs `a <= b` of layers that pass through each other; every other pair
collides). New projects have `default`, `player` and `debris`, with the player passing through
debris.
