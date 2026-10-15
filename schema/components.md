# Entity components

Each row of the `entity` table has a `components` column holding a JSON object. Its keys are
component names, and its values follow the shapes below. The editor writes them; the engine reads
them when it builds a scene. Unknown components are ignored by the engine, so new ones can be
added without breaking older runtimes.

Colours are sRGB values from 0 to 1, as a designer picks them. Positions are in metres, angles
in degrees. The world is right-handed with **+Y up** (on curved worlds, "up" comes from the shape
mapping instead, from Phase 5).

| Component | Shape | Notes |
|---|---|---|
| `transform` | `{"position": [x, y, z], "rotation": [x, y, z], "scale": [x, y, z]}` | Rotation is Euler degrees combined as `R = Ry · Rx · Rz` (yaw, pitch, roll), and the full local matrix is `T · R · S`. All three keys are optional (defaults: origin, no rotation, scale 1). Relative to the parent entity. |
| `mesh` | `{"asset": "<content id>"}` | A mesh in the package's `content` list (a `.glb`). |
| `material` | `{"color": [r, g, b, a]}` | Alpha is optional (default 1). |
| `camera` | `{"fov": 55, "near": 0.05, "far": 500}` | Vertical field of view in degrees. The scene's first camera is used. |
| `light` | `{"type": "directional", "color": [r, g, b], "intensity": 1.0}` | A directional light shines along the entity's local −Z axis (after rotation). |
| `player` | `{"speed": 4.0}` | The entity the player controls (metres per second). If a scene has a player, its first camera follows it in third person: look orbits, the wheel zooms, and movement is relative to the camera. |

## Scene settings

The `scene.settings` column (migration 2) holds a JSON object:

| Key | Shape | Default |
|---|---|---|
| `sky` | `[r, g, b]` | `[0.08, 0.11, 0.18]` (night blue) |
| `ambient` | `[r, g, b]` | `[0.18, 0.2, 0.26]` |
