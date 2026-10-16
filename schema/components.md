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
| `trigger` | `{"size": [x, y, z]}` | A box centred on the entity (in its local units, so its scale applies; rotation is ignored) that reports when the player enters or leaves it: `on_trigger_enter` / `on_trigger_exit` in the entity's script, and in the player's. Default size 1 × 1 × 1. |
| `sky` | `{"color": [r, g, b], "ambient": [r, g, b]}` | The scene's sky colour and ambient light. The first entity with a sky sets them, and the scene settings below are only the fallback for scenes without one. Defaults as in the settings. |
| `folder` | `{}` | A folder in the editor's Scene panel that groups its child entities. The engine sees an entity with only a transform (the editor keeps it at the origin). |
| `player` | `{"speed": 4.0}` | The entity the player controls (metres per second). If a scene has a player, its first camera follows it in third person: look orbits, the wheel zooms, and movement is relative to the camera. |

## Scene settings

The `scene.settings` column (migration 2) holds a JSON object. A `sky` entity outranks it; the
editor creates one in every scene's World folder.

| Key | Shape | Default |
|---|---|---|
| `sky` | `[r, g, b]` | `[0.08, 0.11, 0.18]` (night blue) |
| `ambient` | `[r, g, b]` | `[0.18, 0.2, 0.26]` |
