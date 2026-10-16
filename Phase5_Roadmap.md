# Phase 5 Roadmap: World building

**Midgard** · Old Goat Studio · 15 October 2026 · status: **approved**

This document breaks Phase 5 of `midgard_roadmap.md` (§10) into milestones that can be built,
tested and reviewed one at a time. It follows two rules from the brief:

1. **Foundation before features.** The layers everything else stands on (shape mapping,
   deterministic maths, the renderer, physics, the test tools) come first and are finished
   properly, even when that delays the first visible result.
2. **Testing all the way.** Every milestone adds automated tests at each level it touches, and
   it isn't done until they run in CI on every platform they apply to.

§10 of `midgard_roadmap.md` points here for Phase 5 and lists the items moved to Phase 6. Each
milestone becomes a task list of its own when work on it starts.

---

## 1. Decisions for this phase (15 October 2026)

| Question | Decision |
|---|---|
| Phase 5 items that need Phase 6 systems | **Moved to Phase 6** (see §3). Phase 5 builds the foundations they need. |
| Rendering quality by the end of Phase 5 | **glTF PBR materials (metal/roughness) and cascaded shadows**, plus fog, sky and water, with a phone quality tier. |
| Test hardware | **Your Android phone** for device checks (test APKs with on-screen results). No Raspberry Pi 5: CI's arm64 Linux runner stands in for arm64 budgets, and the Pi is measured in Phase 7, when the dedicated server exists. |
| Sample and test assets | **CC0 (public-domain) assets may go in the repository**, for example Quaternius characters and animations, Kenney sounds and ambientCG textures. Each file's source and licence is recorded and checked by `tools/licenses.py`. |

### New libraries (all permissive, all build for Windows, Linux and Android)

| Library | Licence | Used for |
|---|---|---|
| Jolt Physics | MIT | Physics and the character controller, double-precision build |
| ozz-animation | MIT | Animation sampling, blending, skinning data |
| miniaudio | Public domain / MIT-0 | Audio playback on every target |
| libvorbis / libopus (+ libogg) | BSD | Cooking audio to Ogg Vorbis and Opus, and decoding |
| Recast & Detour | zlib | Navigation meshes and path queries |
| Basis Universal | Apache-2.0 | Textures cooked once to KTX2, transcoded per device (BC7 on PCs, ASTC/ETC2 on phones) |
| Mesa lavapipe | MIT | Software Vulkan in Linux CI, so rendering is tested without a GPU (CI only, never shipped) |

Noise and the other generation maths are written in Midgard itself rather than taken from a
library, because they must give bit-identical results on every target (§2.3).

---

## 2. How Phase 5 is tested

Today there are 183 editor and tools tests (pytest) and 36 engine test cases (doctest), run in
CI on Windows, Linux, Linux arm64 and the Android build. Phase 5 adds six kinds of test.
Each milestone below says which ones it adds.

### 2.1 Unit tests (C++ doctest, Python pytest)
Every new engine module gets doctest cases, and every editor dock gets pytest-qt tests that drive
it as a user would (add, edit, undo, redo). This continues the current practice.

### 2.2 Reference tests (C++ against Python)
Formulas with a written specification, such as solar phase, the shape mappings, gravity,
season blending and calendar dates, have a small **Python reference implementation** in
`tools/reference/`. A shared table of inputs and expected outputs (`schema/tests/*.json`) is
checked against both the Python reference and the C++ engine (through `_midgard`). The two
can't drift apart without a test failing.

### 2.3 Determinism tests (identical results on every target)
World generation must give the same chunks on x86_64, arm64 and Android (§7 of the roadmap).
- A deterministic maths layer (§4, milestone 5.1): integer hashing, fixed-point noise, and
  Midgard's own `sin`/`cos`/`sqrt` approximations, since system maths libraries differ.
- **Golden hashes:** CI generates reference chunks for fixed seeds on Windows, Linux x86_64 and
  Linux arm64, and compares their hashes with a list kept in the repository. A changed hash fails
  the build unless the recipe version was raised on purpose.
- **Android:** a test APK generates the same chunks on your phone and shows PASS or FAIL on
  screen (§2.6).

### 2.4 Image tests (rendering without a GPU)
The Linux CI jobs install Mesa's **lavapipe** (software Vulkan). The runtime renders test scenes
headlessly (it already has `--screenshot`) and compares them with reference images in
`engine/tests/images/`. The comparison uses a per-pixel tolerance and a small allowance for
differing pixels, because GPUs round differently. When a reference must change, a tool
regenerates it and the change is reviewed like code. Windows and real GPUs are checked locally
(and on your phone) at milestone reviews.

### 2.5 Integration and live-link tests
These extend the current tests: the editor cooks a project, starts the real runtime, edits over
the live link and checks what the game reports. New systems (time, weather, cameras, audio) get
hot-reload tests the same way.

### 2.6 Performance budgets and device checks
- **Benchmarks** (`midgard_bench`): frame time for the test scenes, chunk generation time,
  navmesh build time and audio mixing time. CI records them on every run and fails when one
  exceeds its budget by a set margin. The budgets are written down in
  `schema/budgets.json`.
- **Self-test mode:** `midgard_runtime --selftest <suite>` runs a suite (generation hashes,
  timing, a render check) and shows the results with the debug text overlay from milestone 5.0.
  The Android template gets a matching **self-test APK**, so a phone check is just: install,
  open, read the result, and tell me what it says.
- **Phone checkpoints** are marked **📱** below. Each needs only an install and a look.

### Definition of done (every milestone)
- All CI jobs green, including the new tests listed for the milestone.
- New tables have migrations with upgrade tests from the previous schema version.
- New components and tables are documented in `schema/components.md` or `schema/README.md`.
- Cook-time checks for any new data, with tests for each error message.
- Script API additions are in `schema/midgard.pyi`, so stubs and completion pick them up.
- Phone checkpoint passed where marked.
- A short review with you: what it does, a screenshot or a short list of steps to try.

---

## 3. Moved to Phase 6

These parts of the roadmap's Phase 5 list depend on systems that Phase 6 builds:

| Item | Needs | Phase 5 still builds |
|---|---|---|
| Cold, heat and soaked **status effects**, rested bonus | Combat's status-effect model | Feels-like temperature, wetness and shelter checks, available to scripts |
| **Clothing survival stats** (insulation, cooling, waterproofing) | Items and inventory | The survival model's inputs, with clothing as a number scripts can set |
| **HUD temperature gauge**, frost and sweat on screen edges | Menu editor and RmlUi | The values the gauge will show, in the debug overlay |
| Campfires and heaters as **craftable, placeable** items | Crafting and bases | A **heat source** component and its range in the feels-like model |
| `survival_scale` from the **difficulty** | Difficulty editor | The scale as a world setting with a default |
| Weather reducing **AI perception** | AI behaviour | Visibility per weather type, in the data and the API |
| **Space weather** (shields, sensors, teleport interference) | Ships and teleporters | Nothing beyond the weather data model |
| **Forecasts** shown by weather stations and ship sensors | Bases and ships | `midgard.weather.forecast()` in scripts |
| Lock-on targeting for cameras | Combat targets | Camera shake and scripted camera moves |
| The in-game **map screen** | Menu editor | Map images per shape, viewable in the editor |
| Full **star field** in system space | Star Field editor | A procedural sky from the seed, built so the star field can replace it |
| Calendar **event** recurrence editor | Already in Phase 6 | Calendar templates without events |
| Saving the whole game | Save system | `world.json` + `world.db` holding `game_time` (and later body changes) |

---

## 4. Milestones

The order follows dependencies. Milestones 5.0 to 5.4 are the foundation, and nothing after
them starts until they're done. From 5.5 on, a few milestones can overlap where the table in §5
allows it. Sizes are relative: **S** small, **M** medium, **L** large, **XL** very large.

### 5.0 Test and tooling foundation · M
Make the testing in §2 possible before there's anything to test.
- Lavapipe in the Linux CI jobs; the image-comparison tool and reference-image workflow
  (`tools/images.py compare | update`).
- `midgard_bench` and `schema/budgets.json`, with results recorded per CI run.
- **Debug text overlay** in the runtime (a built-in bitmap font): frame time, positions and
  self-test results. It's needed long before the Menu editor.
- `--selftest <suite>` and the self-test APK build in CI.
- A **job system** (worker threads with priorities), used later by streaming, generation,
  navmesh building and audio decoding. It has its own stress tests.
- Profiling markers in the engine (on in development builds).
- The `tools/reference/` structure and shared test tables (§2.2).
- **CC0 asset register:** `assets/third_party/` with a `SOURCES.toml` recording each file's
  origin and licence, checked by `tools/licenses.py`.

**Tests:** the image tool compares known-equal and known-different images correctly; job system
stress (thousands of jobs, cancellation, no lost or duplicated work); a budget check that fails
on purpose in a test.
**Exit:** CI renders a test scene on lavapipe and compares it with its reference. A deliberately
broken shader fails the build. 📱 The self-test APK opens on your phone and shows a PASS screen.

### 5.1 Deterministic maths and world coordinates · M
The numbers every later system relies on.
- **World positions in double precision** (`WorldPos`), with conversions to the float
  positions the GPU uses relative to the camera (**camera-relative rendering**).
- Deterministic maths: integer hashes, seeded random streams split per body, chunk and purpose,
  fixed-point value and gradient noise, and Midgard's own `sin`, `cos` and `sqrt`.
- Compiler settings checked in CI: `/fp:precise`, `-ffp-contract=off`, and no fast-math in any
  target, verified by a test that fails if they're missing.

**Tests:** reference tables for every function; golden hashes of noise fields on all three CI
platforms; precision test: a camera 50 km from the origin renders a test object without jitter
(an image test); random streams don't repeat or overlap across splits.
**Exit:** noise hashes match on Windows, Linux x86_64 and Linux arm64. 📱 They also match on the
phone (self-test).

### 5.2 Shape mapping layer · L
The roadmap's rule: **nothing assumes "y is up"**. This milestone builds the layer that answers
"where is this, which way is up, and how strong is gravity?" for every shape.
- One interface, five shapes: **bounded area**, **flat-wrap cylinder**, **curved cylinder**,
  **sphere outside** and **sphere inside** (the cube-sphere projection with its six faces).
- For each shape: surface coordinates (face, u, v, height) ↔ 3D position, local "up", a local
  tangent frame, gravity (constant, or spin gravity `g = g_surface × r / R` on cylinders), and
  the wrap rules for joined edges.
- A **body** in the engine: shape, size, seed and gravity, and the transform that places it.
  For Phase 5, test bodies come from settings; generated bodies arrive with world types (5.10).
- Exposed through `_midgard`, so the editor previews use the same code.

**Tests:** reference tables against the Python versions; round trips (position → surface →
position) for millions of random points per shape; continuity across every seam (the cylinder
join and all 12 cube-sphere face edges); gravity formulas and directions; "up" never flips
between neighbouring points.
**Exit:** an engine test walks a point all the way around each shape in small steps and arrives
where it started, with no jumps in position, "up" or gravity.

### 5.3 Renderer foundation · XL
From flat-coloured meshes to the agreed quality, built on the shape layer.
- **Textures** cooked to KTX2 with Basis Universal and transcoded per device; mipmaps; samplers.
- **glTF PBR materials** (base colour, metal/roughness, normal, occlusion and emissive maps),
  matching the glTF specification's reference renderings.
- **Lights:** the directional sun plus a limited number of point and spot lights.
- **Cascaded shadow maps** computed in the player's local "up" frame (as §6 of the roadmap
  requires for curved worlds).
- **HDR and tone mapping**, sky (an analytic sky model, replaceable by the Environment editor's
  curves), distance and height **fog**, and **camera-relative rendering** throughout.
- **Quality tiers:** desktop and phone (shadow resolution and cascades, texture size, effects).
- A frame structure with clear passes (shadows, opaque, sky, transparent, post), so later
  milestones (water, weather particles, terrain, skinned meshes) slot in without rewrites.
- The scene's **Sky** entity gains a link to an environment (milestone 5.7). Its colours remain
  the fallback.

**Tests:** image tests for each material channel, the glTF sample models (CC0 ones), shadows
on flat and curved test geometry, fog and tone mapping, at both quality tiers; Vulkan
validation layers on in CI, where any validation message fails the test; frame-time benchmarks.
**Exit:** a material test scene matches its references in CI. 📱 It runs on your phone at the
phone tier at a frame rate within budget (self-test).

### 5.4 Physics (Jolt) · L
- Jolt in the **double-precision** build; one physics world per room.
- Global gravity off: gravity applied per body from the shape layer each step.
- Components: **rigid body** (static, dynamic, kinematic), **collider** (box, sphere, capsule,
  convex, mesh, heightfield), **physics material** (friction, bounciness) and collision layers.
- The **character controller** (`CharacterVirtual`) with its "up" taken from the shape every
  frame. It replaces today's simple player movement, keeping the same input actions.
- **Physics editor:** collider editing in the Inspector, layers and materials, debug drawing of
  shapes in the viewport and in the running game.
- Script API: raycasts, overlap queries and forces.

**Tests:** objects fall and settle in the right direction on every shape; the character walks
around a test sphere and inside a test cylinder without sliding or getting stuck; slope limits
and step heights; repeated runs of the same scene give the same result on one machine; the
existing trigger tests keep passing, now on physics shapes.
**Exit:** the player walks all the way around a test sphere and a test cylinder (no terrain yet,
just meshes), with correct gravity, in the runtime and in Play-in-editor.

---
*End of the foundation. The milestones below build features on it.*

### 5.5 Object editor and asset pipeline · L
- **glTF import:** meshes, materials, textures, and skins and animations stored for 5.12. Files
  go in `objects/` (and `textures/`), each with import settings (scale, up axis, collider
  generation, texture compression) kept in the project database.
- **Object editor:** preview a model, set its materials, colliders and LODs, and see a
  thumbnail. An object becomes something you can place from **Add ▸ Object** in the Scene panel.
- **Material editor** (PBR parameters and texture slots), with live preview.
- Re-import when a file changes on disk, and the live link sends the change to the running game.
- Content hashing, so cooking only rebuilds what changed.

**Tests:** an import corpus (CC0 models, including awkward ones: missing textures, odd up axes,
huge and tiny scales); **fuzzing** of the glTF and texture loaders with damaged files (no crash,
a clear error); import → cook → render image tests; undo and redo of every editor action;
re-import over the live link.
**Exit:** you drop a CC0 model into `objects/`, place it from Add ▸ Object, change its material
in the editor while the game runs, and see the change in the game.

### 5.6 Cameras · M
- One camera system with three views, **first person**, **third person** (orbit and
  over-the-shoulder) and **top-down**, switched with an input action (default `V`) and blended.
- **Collision:** the camera never passes through walls (a sphere sweep through physics).
- **Shape-aware "up"**, so views stay level on spheres and inside cylinders.
- **Camera editor:** per view settings (field of view, distance, height, shoulder offset, pitch
  limits, collision radius, smoothing, zoom range, blend time) with live preview.
- Camera shake and short scripted camera moves (`midgard.camera`). The player's last view is
  saved in a small profile file, ready for the Phase 6 profile system.

**Tests:** a camera sweep test around obstacles (never inside geometry); blends are continuous
(no jumps in position or angle); "up" stays aligned around a sphere; switching views keeps the
player's facing; Camera editor undo and live-link tests.
**Exit:** in the game, you switch between the three views while walking around the test sphere,
and the camera never clips through the test walls.

### 5.7 Calendar and time · L
The clock everything time-based follows (§6 of the roadmap).
- **Contract:** migration 0005 with the calendar tables (templates, months, weekdays, seasons,
  moons). Events stay with Phase 6.
- **Calendar dock:** templates (months, weekdays, seasons, moons, day/year/tilt ranges), one
  marked as the **standard calendar**, a **day bar** and a **year strip**, and a **time-of-day
  scrubber** that drives the viewport and the running game.
- **`TimeService`** in C++: the universal clock (int64 seconds) advanced with an accumulator so it
  never drifts, time scale, per-body local date and season, **solar phase**, the smooth daylight
  blend between seasons, and signals (`hour`, `dawn`, `dusk`, `day`, `month`, `season`, `year`).
- **Body calendars** rolled from the seed within the template's ranges.
- **`world.json` and `world.db`** (the start of the world folder from §7 of the roadmap) hold
  `game_time`, and loading restores the date exactly.
- Script API: `midgard.time` (`now`, `standard`, `season`, `is_day`, `on`, `advance`,
  `set_scale`).
- Cook checks: every month has days, season shares add up to 1, daylight is between 1% and
  99%, and the ranges are valid.

**Tests:** solar phase and dates against the Python reference (including leap years, 20-hour
days and odd month lengths); 10 million clock steps with no drift; every signal fires exactly
once at its boundary, including across big time skips; body calendars are identical from the
same seed on all targets; save → load restores the date to the second; Calendar dock tests;
the scrubber moves time in the running game (live link test).
**Exit:** the scrubber sweeps a test scene through a day in the editor and in the running game,
and a 20-hour day with 70% daylight gives the right sunrise and sunset times.

### 5.8 Environment and light per shape · L
- **Environment editor:** sky colours, sun colour and intensity, ambient light, fog and water
  look, all authored as **curves over solar phase** and previewed with the Calendar scrubber.
  The scene's Sky entity links to an environment.
- **Light models per shape** (§6 of the roadmap): a moving sun on bounded areas and flat-wrap
  cylinders; on spheres, phase shifted by **longitude**, with *Uniform* and *Latitude* (axial
  tilt) modes; on inside spheres and curved cylinders, the **central sun or sun-line with
  shades**, dimming and the stylised low-angle sunrise and sunset direction.
- The **inside-world atmosphere** preset (haze by distance, colour of the far side).
- **Seasons' visuals** where they affect lighting and colour (for example a colder light in
  winter). Snow cover and foliage colour come with terrain materials in 5.9.

**Tests:** local solar phase per shape against the Python reference (a sphere's noon moves with
longitude; latitude mode gives polar day and night); image tests of each shape at dawn, noon,
dusk and midnight; curve editing undo and live-link tests.
**Exit:** each test shape shows a full day/night cycle with sunrise and sunset in the right
places, previewed with the scrubber.

### 5.9 Terrain, step 1: rectangular grids · XL
- **Chunks and level of detail:** a quadtree of chunks on a rectangular grid, streamed around
  the player by the job system, with seamless joins between detail levels.
- **Generation** samples noise at 3D positions from the shape layer (so the cylinder join is
  seamless by construction), using the deterministic maths from 5.1.
- In order: **bounded area**, then **flat-wrap cylinder**, then **curved cylinder**, with the
  cylinder **end walls** (cliff, fortification or energy barrier) and the bounded area's ocean
  border.
- **Water** as a shell at sea level, using the renderer's water material.
- **Terrain materials:** splatting by slope, height and biome, with season changes (snow cover,
  foliage colour, dry grass).
- **Collision:** Jolt heightfields on flat shapes, mesh shapes built per chunk on curved ones.
- **Sculpt and paint in the viewport.** Edits are stored as **terrain stamps** in the project (shapes and brush strokes), which world types can place and which the
  bounded-area sample uses directly. Player terrain changes later use the same mechanism, saved
  per body.

**Tests:** chunk hashes for fixed seeds on every target (§2.3); no cracks at LOD joins or the
cylinder join (geometry test: shared edge vertices match exactly); streaming stress (fast
travel across the map with no missing chunks and bounded memory); collision matches the drawn
surface; generation-time budgets per chunk; sculpt and paint undo; image tests.
**Exit:** the player walks across a bounded area and all the way around a curved cylinder with
no seams, with correct spin gravity. 📱 Chunk hashes match and generation time is in budget on
your phone.

### 5.10 Terrain, step 2: cube-sphere · L
- The six faces of the cube-sphere, for the **outside** (planets) and the **inside** (hollow
  worlds) of spheres.
- Seamless joins at face edges and corners at every detail level.
- Inside worlds draw the **far side** overhead at low detail, softened by the haze preset.
- Sculpt and paint across face edges.

**Tests:** all 12 face edges and 8 corners checked for matching vertices at every level of
detail; walking around the sphere along the equator, over both poles and diagonally across
corners; chunk hashes on every target; far-side drawing within frame budget.
**Exit:** the player walks all the way around a planet and inside a hollow world, with no seams
and correct gravity.

### 5.11 World Type editor v1 · XL
- The **shared node-graph widget** (the roadmap lists it in Phase 6, but world types need it
  first; the Animation editor in 5.13 reuses it).
- **World Type editor:** shape and size range, a node graph (noise, combine, curves, terraces,
  erosion-like filters) with **shape-aware inputs** (latitude, distance from the cylinder's ends,
  distance from the bounded area's edge), **biomes** (from temperature and moisture), placement
  rules for objects (scatter, spacing, slope and biome limits), atmosphere kind, gravity, wall
  style and calendar template.
- **Generate preview** through `_midgard`: a map image and a 3D chunk for any seed, exactly as
  the game generates them.
- **Recipe versioning:** worlds record the version they were made with; the cook refuses to drop
  a version a shipped world could use.
- Run-time generation in the game from a seed.
- **Map projections per shape** (top-down, unrolled cylinder, cube-face and equirectangular),
  available as images. The in-game map screen comes in Phase 6.

**Tests:** graph evaluation against hand-computed cases; graph editing undo and redo;
version-locking (an old world still generates identically after its recipe changes);
preview = game (the editor's preview hash equals the runtime's); cook checks; golden hashes for
the sample world types on every target.
**Exit:** you build a world type in the editor, preview it from several seeds, and the game
generates the same world.

### 5.12 Navigation · M
- Recast navigation meshes per chunk, built in the chunk's local frame on the job system, and
  linked across chunk edges, the cylinder join and cube-sphere faces.
- Detour path queries and a script API (`midgard.nav.path`), with debug drawing.

**Tests:** paths across every kind of join; rebuilds when terrain changes; build-time budgets;
a path all the way around each shape.
**Exit:** a test agent walks a path that crosses a cube-sphere face edge and the cylinder join.

### 5.13 Character animation · XL
- **Import** skinned characters and clips from glTF (CC0 humanoid from Quaternius as the sample).
- **Humanoid rig** definition and **retargeting** between characters of different proportions.
- **Runtime** on ozz-animation with **GPU skinning**.
- **Animation editor** (on the shared node-graph widget): **state machines** with conditions and
  blend times, **blend spaces** (1D and 2D) for locomotion, **layers and masks**, optional
  **root motion**, and **animation events** (footsteps, sounds, script calls).
- **IK:** foot placement on slopes against the local "up", head look-at, hand IK.
- **First-person arms** as a separate rig and graph, kept in step with the full body.

**Tests:** sampled poses against reference poses; retargeting bone mapping; state-machine
transitions in fixed-step simulation (same inputs → same states); foot IK error on test slopes
below a set tolerance on every shape; events fire once per loop at the right frame; image tests
of a posed character; skinning frame-time budget.
**Exit:** the sample character walks and runs around the test planet with its feet planted on
slopes, in all three camera views. 📱 It runs within budget on your phone.

### 5.14 Audio · L
- **miniaudio** on every target; audio cooked to Ogg Vorbis (music, ambience) and Opus (effects,
  voice).
- **Sound events** (variants with random pitch and volume), **buses** (master, music, effects,
  voice, ambience, UI), **3D sound** with attenuation curves, **occlusion** through physics, and
  **reverb zones**.
- **Music playlists** by world type, biome and time of day, with crossfades and stingers;
  **ambience** layered by biome and **solar phase**.
- **Audio editor** with preview in the editor.
- Animation events trigger sounds (footsteps from 5.13).

**Tests:** offline mixing through miniaudio's null device, measured levels and timings (an
attenuation curve gives the expected level at each distance, a crossfade takes the set time, no
clipping); the cook converts and the runtime decodes every sample file; Audio editor undo and
live-link tests; mixing-time budget.
**Exit:** walking the test planet plays footsteps per surface, biome ambience that changes at
dusk, and music that crossfades between biomes. 📱 Sound plays correctly on your phone.

### 5.15 Weather · L
- **Weather types** (clear, cloudy, rain, thunderstorm, snow, blizzard, fog, wind, sandstorm,
  ash fall, and the designer's own) with visuals, sounds and their effect data (visibility, wind,
  wetness, temperature change).
- **Weather tables** per season and biome; regions as cells mapped onto each shape; Markov
  transitions with durations; **fronts** drifting between cells. Every roll is seeded, so
  weather can be reproduced and forecast.
- **Controlled weather** schedules for inside worlds, and **scripted weather**
  (`midgard.weather.force`).
- **Visuals:** GPU particles for rain, snow and ash, simple clouds, wet surfaces, snow build-up,
  wind on fog and particles; quality scales on phones.
- **Survival foundation:** feels-like temperature, wetness and shelter checks (rays upward and
  sideways), and a heat-source component, readable from scripts and in the debug overlay (§3).
- Script API: `midgard.weather` (`current`, `forecast`, `on_change`, `force`).
- Weather tab in the Calendar dock.

**Tests:** weather sequences reproduced exactly from the seed; forecasts match what happens;
statistics over long runs match the table's weights (within tolerance); fronts cross cell
borders, including the cylinder join and cube faces; shelter checks in test buildings;
feels-like temperature against the Python reference; particle budget on the phone tier.
**Exit:** a scripted storm rolls in over the test planet, darkens the sky and wets the ground,
then normal weather resumes, and the forecast predicted the natural changes.

### 5.16 Rooms and system space · M
- **Room manager:** one loaded play space at a time per player, and **room changes** that fade,
  save and unload the room being left, carry the character across, and stream the arrival area
  **before** fading in.
- A planet's **atmosphere ceiling** leads to a **system-space** room with a procedural sky from
  the seed (built so Phase 6's star field can replace it); coming back down lands above a chosen
  point. **Habitat docks** and **hollow-world exit sites** lead out too.
- Script API: `midgard.rooms.change`, `on_room_enter`, `on_room_exit`.

**Tests:** room changes never show an unloaded area (the arrival chunks are ready before the
fade-in); state carries across; memory returns to its baseline after many changes; the editor
can Play from any room.
**Exit:** the player rises out of the planet's atmosphere into space and comes back down.

### 5.17 Shader and material authoring · M
- **GLSL editor** with highlighting and compile errors with line numbers (glslang through
  `_midgard`), compiled to SPIR-V at cook time.
- Custom shaders plugged into the material system from 5.3, with hot reload over the live link.
- **Texture generator** (noise, gradients, tiling patterns) in C++, called through `_midgard`,
  so the editor and the game produce the same textures.

**Tests:** compile errors reach the editor with the right line; a shader edit reloads in the
running game; generated textures have golden hashes on every target; image tests of custom
shaders.
**Exit:** you write a small custom shader in the editor and see it change in the running game.

### 5.18 Phase 5 sample and exit · M
- A **World Tour** sample project: one world type per shape (bounded area, cylinder, planet,
  hollow world) at fixed sizes, the animated character, all three cameras, a calendar with
  seasons, weather, environment curves, music and ambience.
- An automated exit run in CI (generation, walking scripted routes around each shape, room
  changes) and a short manual checklist for you.

**Exit (from the roadmap, adjusted for §3):** the game generates, on the player's device, one
world type of each shape at fixed sizes. A player walks all the way around the cylinder and both
spheres with correct gravity, sees no seams, and watches a full day/night cycle on each shape.
They rise out of the planet's atmosphere into space and come back down. The game ships as a
package. An animated character walks the planet in all three camera views, its feet planted on
the slopes, with biome ambience and music. 📱 All of it runs on your phone.

---

## 5. Dependencies and order

| Milestone | Needs | Can overlap with |
|---|---|---|
| 5.0 Test and tooling foundation | — | — |
| 5.1 Deterministic maths and coordinates | 5.0 | — |
| 5.2 Shape mapping layer | 5.1 | — |
| 5.3 Renderer foundation | 5.0, 5.2 | 5.4 |
| 5.4 Physics | 5.2 | 5.3 |
| 5.5 Object editor and assets | 5.3, 5.4 | 5.6, 5.7 |
| 5.6 Cameras | 5.4 | 5.5, 5.7 |
| 5.7 Calendar and time | 5.1 | 5.5, 5.6 |
| 5.8 Environment and light per shape | 5.3, 5.7 | 5.9 |
| 5.9 Terrain: rectangular grids | 5.3, 5.4 | 5.8 |
| 5.10 Terrain: cube-sphere | 5.9 | 5.12 |
| 5.11 World Type editor | 5.9, 5.10 | 5.13 |
| 5.12 Navigation | 5.9 (5.10 for spheres) | 5.10, 5.11 |
| 5.13 Character animation | 5.5, 5.6 | 5.11, 5.14 |
| 5.14 Audio | 5.4, 5.7 | 5.13, 5.15 |
| 5.15 Weather | 5.8, 5.9, 5.14 | 5.16 |
| 5.16 Rooms and system space | 5.10 | 5.15, 5.17 |
| 5.17 Shader and material authoring | 5.3 | almost anything after 5.5 |
| 5.18 Sample and exit | everything | — |

In stages (each stage starts when the one before it is done):

1. **Foundation, one at a time:** 5.0 → 5.1 → 5.2 → 5.3 and 5.4 together.
2. **On the foundation:** 5.5 Objects, 5.6 Cameras, 5.7 Calendar.
3. **The world's surface:** 5.8 Environment, 5.9 Terrain (rectangles), then 5.10 Terrain
   (spheres).
4. **On the terrain:** 5.11 World types, 5.12 Navigation, 5.13 Animation, 5.14 Audio.
5. **Bringing it together:** 5.15 Weather, 5.16 Rooms, 5.17 Shaders.
6. **5.18** Sample and exit.

---

## 6. Risks and how the plan handles them

| Risk | Handling |
|---|---|
| Floating-point results differ between targets, breaking identical generation | Deterministic maths layer first (5.1), golden hashes from the first noise function on, Android checked by self-test APK |
| Software Vulkan (lavapipe) renders slightly differently from real GPUs | Tolerances in image tests; real-GPU checks at milestone reviews and on the phone |
| Phone performance with PBR, shadows, terrain and weather together | Phone quality tier from 5.3; budgets enforced per milestone, not at the end |
| Seams on curved terrain (cylinder join, cube faces, level-of-detail joins) | Shape layer continuity tests (5.2) before any terrain; exact-vertex seam tests in 5.9 and 5.10 |
| Jolt's double-precision build on Android | Built and tested in the Android CI job in 5.4, before anything depends on it |
| The phase is very large | Milestones are reviewed and committed one at a time, each ending with something that runs; the foundation (5.0–5.4) is useful even if later priorities change |
| No Raspberry Pi | CI's arm64 Linux runner covers hashes and budgets until Phase 7 |

---

## 7. Confirmed (15 October 2026)

- **Sculpt and paint (5.9):** stored as **terrain stamps** in the project, placed by world types
  or used directly by bounded-area maps, and reused later for player terrain edits.
- **Sample world sizes** for the exit demo: the smallest allowed sizes (sphere radius 1 km,
  cylinder 5 km long with a 1 km radius) keep walking-around tests short. Larger sizes are
  tested for precision only.
