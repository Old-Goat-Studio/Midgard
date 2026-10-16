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
| `mesh` | `{"asset": "<content id>"}` | A mesh in the package's `content` list (a `.glb`). Ids: `prim_cube`, `prim_sphere`, `prim_plane` (editor primitives), `asset:<path>` (a `.glb` in the project's `objects/` folder; older projects' `assets/` folder is moved there when they open), `builtin:cube` (built into the engine, not in `game.pak`). |
| `material` | `{"color": [r, g, b, a], "metallic": 0, "roughness": 0.8, "emissive": [r, g, b], "emissive_strength": 1}` | glTF's metal/roughness factors. Alpha is optional (default 1); metallic and roughness are 0 to 1 (defaults: not metal, fairly rough); emissive light is its colour times its strength (default none). Textures come with the object pipeline. |
| `camera` | `{"fov": 55, "near": 0.05, "far": 500}` | Vertical field of view in degrees. The scene's first camera is used. |
| `light` | `{"type": "directional", "color": [r, g, b], "intensity": 1.0}`, or `"point"`/`"spot"` with `"range": 10`, and for spots `"inner_angle": 20, "outer_angle": 30` (degrees) | Directional lights and spots shine along the entity's local −Z axis (after rotation). The scene's first directional light is the sun. Point and spot lights fade as glTF's KHR_lights_punctual does (inverse square, to zero at the range); the eight nearest the camera are used. Intensity 1 lights a white surface facing the light to white. |
| `script` | `{"module": "door"}` | A Python module in the project's `scripts/` folder that drives the entity (`"door"` is `scripts/door.py`, `"enemies.wolf"` is `scripts/enemies/wolf.py`). Its functions and the `midgard` API are described in `schema/midgard.pyi`. |
| `trigger` | `{"size": [x, y, z]}` | A box centred on the entity (in its local units, so its scale and rotation apply) that reports when the player enters or leaves it: `on_trigger_enter` / `on_trigger_exit` in the entity's script, and in the player's. It is a sensor in the physics world, separate from any `collider` (so a crate can both land on the ground and be a trigger) and moving with its entity: nothing bumps into it, and the player counts as inside once its capsule (radius 0.3 m, 1 m tall, upright along "up") overlaps it. Default size 1 × 1 × 1. |
| `collider` | `{"shape": "box", "size": [x, y, z], "radius": 0.5, "height": 2, "offset": [x, y, z]}` | The entity's shape for physics, in its local units (its scale applies). `shape` is `box` (`size`, default 1 × 1 × 1), `sphere` (`radius`), `capsule` (`radius`, and `height` end to end along local Y, at least twice the radius), `convex` (wraps the entity's mesh, or the content id in `"mesh"`), `mesh` (the mesh's triangles; static or kinematic bodies only) or `heightfield` (`"samples"`: n, `"heights"`: n × n numbers row by row along X then Z, `"spacing"`: metres between them; static only). `offset` moves the shape within the entity. Without a `rigidbody` the entity is static scenery. |
| `rigidbody` | `{"motion": "dynamic", "mass": 0, "layer": 0}` | How the entity moves in the physics world: `dynamic` (gravity, forces and collisions move it; the engine writes its transform), `kinematic` (the game or a script moves it, and it pushes dynamic bodies aside) or `static`. `mass` in kilograms (0: from the collider's volume at the density of water). `layer` is one of the project's collision layers (0 to 15, the `physics_layer` table). Needs a `collider`. |
| `physics_material` | `{"friction": 0.5, "bounciness": 0}` | How the collider's surface behaves: friction from 0 (ice) up, bounciness from 0 (stops dead) to 1 (bounces back as fast as it came). Defaults when absent. |
| `sky` | `{"color": [r, g, b], "ambient": [r, g, b]}` | The scene's sky colour and ambient light. The first entity with a sky sets them, and the scene settings below are only the fallback for scenes without one. Defaults as in the settings. |
| `folder` | `{}` | A folder in the editor's Scene panel that groups its child entities. The engine sees an entity with only a transform (the editor keeps it at the origin). |
| `player` | `{"speed": 4.0, "jump": 4.5, "radius": 0.3, "height": 1.0, "max_slope": 50, "step_height": 0.3}` | The entity the player controls: a character in the physics world, an upright capsule (`radius`, and `height` end to end) centred on the entity, that walks at `speed` metres per second, stands on the ground, slides along walls, walks up steps up to `step_height` metres and slopes up to `max_slope` degrees, and jumps (the jump action) at `jump` metres per second upwards (0: no jumping). "Up" and gravity come from the scene's `body`, so it walks all the way round a planet. Its own `collider` and `rigidbody`, if it has any, are left out. If a scene has a player, its first camera follows it in third person: look orbits, the wheel zooms, and movement is relative to the camera, level with the ground underfoot. |

## Scene settings

The `scene.settings` column (migration 2) holds a JSON object. A `sky` entity outranks it; the
editor creates one in every scene's World folder.

| Key | Shape | Default |
|---|---|---|
| `sky` | `[r, g, b]` | `[0.08, 0.11, 0.18]` (night blue) |
| `ambient` | `[r, g, b]` | `[0.18, 0.2, 0.26]` |
| `body` | `{"id": "world", "shape": "sphere_outside", "radius": 1000, "gravity": 9.81, "origin": [x, y, z]}` | none: flat ground, +Y up, gravity 9.81 m/s² down |

`body` is the scene's world shape (`midgard/body.h`): where "up" points and how gravity pulls.
Shapes and their sizes in metres: `bounded_area` (`width`, `length`), `flat_cylinder`
(`circumference`, `length`), `curved_cylinder` (`radius`, `length`; you live inside it, spin
gravity pulls outwards), `sphere_outside` (`radius`; a planet) and `sphere_inside` (`radius`; a
hollow world). `origin` is where the shape's centre is (default the origin), and sizes must lie
within the roadmap's limits (§7, "Size and precision"); the engine refuses others. Physics pulls
every moving body along the shape's gravity where it is, and the sun's shadows use its "up".

## Physics layers

Migration 5 adds the project's collision layers: `physics_layer` (`id` 0 to 15, `name`) and
`physics_ignore` (pairs `a <= b` of layers that pass through each other; every other pair
collides). New projects have `default`, `player` and `debris`, with the player passing through
debris.
