<p align="center"><img src="midgard.png" alt="Midgard — Yggdrasil" width="320"></p>

# Midgard Roadmap

## A game engine with an action-focused editor

*Companion to [midgard_plan.md](midgard_plan.md) · 16 October 2026 · revision 4*

This document answers the two questions in the plan, sets out an architecture for the editor (Python + PySide6) and the engine (C++), and lays out a phased roadmap. It also adapts two existing guides to Midgard: the **Theme Scheme** (View ▸ Themes) and the **About Scheme** (version, build number and About box). **Revision 3** records the answers to every open question: worlds and space, ships, action RPG systems, survival, multiplayer and services, platforms and open-source licensing. Every decision is listed in §13. **Revision 4** (16 October 2026) brings the document up to date with what has been built: §0 says where the project stands, §2–§5 describe the code as it is (with what is still to come marked *planned*), §10 gives each phase's status, and §13 records the decisions made while building Phase 5.

---

## 0. Where things stand (16 October 2026)

**Phases 0–4 are done, and Phase 5 is under way**, following [`Phase5_Roadmap.md`](Phase5_Roadmap.md). Every commit runs CI: the engine on Windows, Linux x86_64, Linux arm64 and Android; the editor's tests and packaged editor on Windows and Ubuntu; and the licence, format and lint checks.

| Phase | Status |
|---|---|
| 0 Foundations | **Done** (14 October): monorepo, Apache-2.0 housekeeping, CI on every target, contract v0. |
| 1 Editor shell | **Done** (14 October). |
| 2 Engine core and the vertical slice | **Done** (14 October): a cube placed in the editor runs on Windows, Linux and Android, all exported from one editor host. |
| 3 Live link and engine bindings | **Done** (14 October): Play, Pause and Stop, hot reload, the `_midgard` module, the embedded Vulkan viewport. |
| 4 Scripting | **Done** (14 October): games embed CPython 3.14 (Android too), the `midgard` API v1, the Script editor, Assistant v1. |
| 5 World building | **In progress:** milestones 5.0–5.5 are done (below). |
| 6–9 | Not started. |

**Phase 5 milestones**

| Milestone | Status | What it brought |
|---|---|---|
| 5.0 Test and tooling foundation | **Done** | Image tests on software Vulkan (lavapipe) in CI, `midgard_bench` with budgets, the debug text overlay, `--selftest` and the self-test APK (passing on the developer's phone), the job system, profiling markers, the CC0 asset register. |
| 5.1 Deterministic maths and coordinates | **Done** | Double-precision world positions, camera-relative rendering, deterministic hashes, random streams and noise that match on every target. |
| 5.2 Shape mapping layer | **Done** | Bounded area, flat-wrap and curved cylinders, sphere outside and inside: positions, "up", gravity and wrap rules, shared with the editor through `_midgard.Body`. |
| 5.3 Renderer foundation | **Done** | HDR and tone mapping, glTF PBR materials, KTX2/Basis textures, point and spot lights, cascaded shadows in the local "up" frame, analytic sky and fog, desktop and phone quality tiers. |
| 5.4 Physics (Jolt) | **Done** | Jolt in double precision with gravity from the shape, rigid bodies, colliders, physics materials, layers, triggers, the character controller, the script API (raycasts, overlaps, forces), collider outlines, the world's surface mesh. The player walks round a planet and inside a cylinder. |
| 5.5 Object editor and asset pipeline | **Done** | glTF import, objects as templates placed as instances, the Objects panel (Object editor), the Material editor, re-import on change, live objects in the running game, content-hashed cooking, Save Group as Object, and the first CC0 model (Avocado) with an import corpus test. The exit check runs as a test with the real game. |
| 5.6–5.18 | Not started | Next are 5.6 Cameras and 5.7 Calendar (stage 2 in `Phase5_Roadmap.md` §5). |

**Also built along the way** (asked for while using the editor): **groups** (complex objects made of primitives, moved as one, and one compound physics body when they move), **viewport gizmos** (move, rotate and scale in the view's corner, following the dock onto any monitor), a viewport toolbar (Colliders, Gizmos), **cylinder and wedge** primitives, layouts saved as they change (tab order included), delete by key and right-click, **Git from the editor** (put a project under Git, commit, change markers in the Script editor), the **Project Page** (tagline, description, logo and the game's icon), opening the last project on start, and picture buttons for Play, Pause and Stop.

**What a game can do today:** a scene made in the editor (primitives, imported glTF objects with textured PBR materials, lights, sky and fog) runs as an exported game on Windows, Linux and Android. The player walks in third person with physics, on flat ground, all the way round a planet or inside a cylinder. Scripts react to triggers and change the world, and Play-in-editor shows edits in the running game within a second.

---

## 1. Summary of the key decisions

| Area | Decision |
|---|---|
| Editor ↔ engine | The editor **cooks** a project into a versioned *Midgard package* (manifest + SQLite + assets). The engine only ever loads packages. A live TCP link adds play-in-editor and hot reload, and a pybind11 module lets the editor call engine code directly. |
| The contract | One `schema/` folder owns the SQL migrations, the package manifest's JSON Schema and the live-link message definitions. Both sides check a `format_version`. |
| Docking | [Qt Advanced Docking System](https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System) via `PySide6-QtAds`, with floating dock containers on any monitor, named layouts, and restore that checks the monitors still exist. |
| Engine stack | C++20, CMake presets, vcpkg. **In use:** SDL3 (windows, input, Android), Vulkan 1.1 (volk + VMA), EnTT, Jolt Physics (double precision), cgltf and MikkTSpace, stb_image, KTX-Software (Basis Universal), miniz, SQLite, nlohmann-json, glm, embedded CPython 3.14 via pybind11, and a built-in debug text overlay. **Planned:** ozz-animation, miniaudio, Recast & Detour (Phase 5), RmlUi for player menus (Phase 6). |
| Platforms | The **editor** runs on Linux and Windows. **Games** target Linux, Windows and Android, and the **dedicated server** targets Linux x86_64, Linux arm64 and Windows. Every target is export-only. Midgard's CI prebuilds a runtime template per target, so any editor host exports to any target without compiling. **macOS is out of scope** for both the editor and games. |
| Android | Never an editor host. Exporting puts the package into a prebuilt APK/AAB template and signs it. A full Gradle/NDK build is the advanced path. |
| Python in games | The engine embeds CPython 3.14 (on Android too) and exposes a `midgard` module. The Script editor uses its stubs (`schema/midgard.pyi`) for completion. Game scripts are pure Python, so the same package runs on every target. |
| Genre | **3D RPG-style action games.** First-person, third-person and top-down cameras are in every game, and **players switch between them freely** (§8). |
| RPG systems | **Combat** (co-op only, no PvP), **AI behaviour**, **companions** (with approval: they react to the player's choices and can refuse orders or leave), character animation, audio, dialogue trees (recorded voice, text-to-speech for generated words, optional subtitles, group conversations where the starter chooses), input mapping (keyboard, gamepad and touch, rebindable by players), localisation (English original, translation tables for users) and save/load UI are all in 1.0, each with its own editor subsystem (§8). |
| Bases | **Base building, crafting, resource gathering and base raids are all in 1.0**, in both world modes, each with its own editor (Base, Crafting and Resources editors). Bases hold storage, crafting stations, power, beds, teleport pads, landing pads and defences (§7, *Bases*; §8). |
| Game time | One universal clock per universe, a **standard calendar** for things not tied to a place, and **a calendar for every body**: its own day length, year, months, **seasons** and **weather**. Weather is tied to the season and biome, can be forecast, and affects gameplay, including **survival effects**: cold, heat and wet (countered by clothing and shelter), **hunger and thirst** (food and drink also ease status effects), and **hostile atmospheres** (suits, air supply and life support). Their strength varies with the world's difficulty (§6). |
| Open source | **Midgard is open source under Apache-2.0**: editor, engine, runtime templates and services. Games made with it can use any licence, including commercial (§10, *Open source*). |
| Projects | **One person per project** in 1.0. Multi-user projects come in a later version. |
| AI console | An **LLM assistant** dock in the editor (Claude API). It reads the project and proposes changes, which go through undo after the user approves them. **Each user provides their own API key.** It never ships inside games, and **games have no AI-generated NPC dialogue**. |
| Wallet/banking | In-game currency only. No real-money purchases. |
| Worlds | **Generated by the game at run time from a seed.** The editor only describes **world types** and a **star field recipe**. Each game project chooses one **world mode**: a **system map** (a universe of 9 systems with 2–13 bodies each, all **cylinders** or **spheres**, with terrain outside or inside) or a **bounded surface map** (a single area of land surrounded by **ocean**, with 1–3 continents plus archipelagos and islands, and no space). A player can own several worlds, each with a name, an editable description and a difficulty level. |
| Space and travel | **9 star systems per universe.** A planet's sky is its boundary: leaving the atmosphere is a **room change** to space. Habitats and hollow worlds have docks and exit sites. **Players fly spacecraft** within a system, and use **aircraft, airships, sailing ships, small craft, land vehicles and mounts** on bodies with air, water and ground. Travel between bodies and systems is **instant teleportation** (like Star Trek's transporter) through a teleporter attached to a game object: a machine, a ship module or an artifact. Players can be on different bodies at the same time. |
| Quests | **Hand-written** quests (placed through generator anchors) and **templates** filled from each generated world. |
| Characters | **Characters move between worlds.** They live in the player's profile, and the host checks them against the game rules when they join. |
| Multiplayer | **Small co-op, 2–16 players per world.** One authoritative host, either a player's game (listen server) or a headless dedicated server, over ENet (UDP). No MMO infrastructure. |

---

## 2. Answers to the plan's questions

### Q1. How should the game designed in the editor be communicated to the C++ engine?

Use **three channels**. Each one does a different job, and only the first is needed to ship a game.

```
            ┌──────────────────────── Midgard Editor (Python / PySide6) ───────────────────────┐
            │  {project}.config (JSON)   {project}.db (SQLite)   objects/  textures/  scripts/      │
            └───────┬───────────────────────────────┬───────────────────────────────┬─────────┘
                    │ 1. Cook (Build ▸ Cook)        │ 2. Live link (TCP, JSON)      │ 3. In-process
                    ▼                               ▼                               ▼
          ┌────────────────────┐         ┌──────────────────────┐        ┌───────────────────────┐
          │  Midgard package   │ loads   │ midgard_runtime      │        │ _midgard (pybind11)   │
          │  manifest.json     │────────▶│ (game executable,    │        │ validation, world     │
          │  {project}.db      │         │  C++)                │        │ shapes, model import, │
          │  game.pak (assets) │         │  play / server modes │        │ texture encoding,     │
          └────────────────────┘         └──────────────────────┘        │ embedded viewport     │
                                                    ▲                    └───────────────────────┘
                                                    └──── same C++ library: libmidgard ─────┘
```

#### Channel 1: the cooked package (the product)

The editor's **Build ▸ Cook** step turns the project into a package the engine can load with no editor present. This package *is* "the product that the game engine consumes".

```
MyGame/                       (Windows example; Linux is the same without .exe)
  MyGame.exe                  midgard_runtime, renamed and given the game's icon
  data/
    manifest.json             what the package is, what built it, where to start
    MyGame.config             runtime part of {project}.config
    MyGame.db                 read-only copy of {project}.db (editor-only tables removed)
    game.pak                  zip of cooked content:
                                meshes/      primitives and model files (.glb)
                                models/<id>/ imported objects: parts (.glb), textures (.ktx2)
                                images/      image files materials use (.ktx2)
    scripts/                  the game's scripts (.py)
    icon.png                  the game's icon (Project Page)
  python/                     embedded CPython 3.14 runtime + stdlib zip
```

The engine's own shaders are compiled to SPIR-V when the engine is built and live inside it. *Planned:* project shaders (5.17), audio (5.14) and player menus (`.rml`/`.rcss`, Phase 6) join `game.pak`.

`manifest.json`:

```json
{
  "format": "midgard-package",
  "format_version": "1.0",
  "game": { "name": "MyGame", "version": "0.3.0" },
  "built_with": { "editor": "0.1.0+2", "engine_abi": 0 },
  "target": { "os": "windows", "arch": "x86_64", "role": "game" },
  "entry": { "scene": "scn_main" },
  "database": "MyGame.db",
  "schema_version": 6,
  "content_hash": "sha256:…",
  "content": [
    { "id": "prim_cube", "path": "meshes/prim_cube.glb", "sha256": "…" },
    { "id": "obj:obj_crate/0", "path": "models/obj_crate/0.glb", "sha256": "…" },
    { "id": "tex:textures/bricks.png", "path": "images/textures/bricks.png.ktx2", "sha256": "…" }
  ]
}
```

Rules that keep the contract stable:

- **SQLite is the shared data model.** Inventory items, currencies, quests, skills, progression curves, menus and scenes live in tables. The engine reads them with the SQLite C library, so there is no second serialisation format to keep in sync. `PRAGMA user_version` holds the schema version.
- **The schema has one owner.** `schema/migrations/NNNN_*.sql` are applied by the editor when a project opens. The cook step refuses to run if the engine's supported range (`engine_abi`) does not cover the project's `schema_version`.
- **Assets are cooked into runtime formats** by the editor, through the `_midgard` module where the work is C++: models → glTF binary parts, one per material (`import_model`), and images → KTX2 with Basis Universal (`encode_texture`). Results are kept in the project's `build/cache/` under a hash of what they're made from, so cooking only redoes what changed. *Planned:* project shaders, GLSL → SPIR-V (5.17).
- **Everything is referenced by stable ID**, never by display name, so renaming a quest in the editor never breaks a script.
- **The shipped database is read-only.** Player saves go to a separate `saves/{profile}.db` in the user's data folder. This is required on Android, where APK contents can't be written to, and it keeps content and save data separate everywhere.
- **Scripts ship as `.py`.** Ship `.pyc` only if it is compiled by the runtime's own interpreter (`midgard_runtime --compile-scripts`), because bytecode is tied to the Python version.

Example of the kind of tables the contract defines:

```sql
CREATE TABLE item      (id TEXT PRIMARY KEY, name TEXT, stack_max INTEGER, data JSON);
CREATE TABLE currency  (id TEXT PRIMARY KEY, name TEXT, decimals INTEGER);
CREATE TABLE quest     (id TEXT PRIMARY KEY, name TEXT, giver TEXT, data JSON);
CREATE TABLE quest_goal(quest TEXT REFERENCES quest(id), seq INTEGER, kind TEXT, target TEXT, count INTEGER);
CREATE TABLE quest_prereq(quest TEXT REFERENCES quest(id), requires TEXT, kind TEXT);   -- quest|skill|level|item
CREATE TABLE skill     (id TEXT PRIMARY KEY, tree TEXT, name TEXT, max_rank INTEGER, data JSON);
CREATE TABLE skill_prereq(skill TEXT REFERENCES skill(id), requires TEXT, rank INTEGER);
CREATE TABLE skill_modifier(skill TEXT REFERENCES skill(id), stat TEXT, op TEXT, per_rank REAL);
```

#### Channel 2: the live link (play-in-editor)

**Play** launches `midgard_runtime --package <cook dir> --editor-link 127.0.0.1:<port>` as a **separate process**, so an engine crash never takes the editor down. Messages are length-prefixed JSON over TCP:

```json
{"type": "asset.changed", "id": "obj:obj_crate/0"}
{"type": "entity.set", "entity": "e000042", "component": "transform", "value": {"position": [1, 0, 4]}}
{"type": "log", "level": "warn", "text": "texture 'tex:textures/bricks.png': it is not in the package"}
{"type": "stats", "fps": 144, "draw_calls": 812, "paused": false}
```

The messages are defined in `schema/live_link.schema.json` (protocol 0):

| Message | Direction | What it does |
|---|---|---|
| `hello` | both | Version and protocol check when the game connects. |
| `play`, `pause`, `resume`, `stop` | editor → game | Play controls (F6 in the game window pauses too, reported back as `state`). |
| `entity.put`, `entity.set`, `entity.delete` | editor → game | An entity added or replaced, one component changed, or an entity removed. |
| `row.changed` | editor → game | A database row changed (scene settings, an entity's row). |
| `asset.changed` | editor → game | A mesh or texture in `game.pak` changed; the editor has cooked it first. |
| `object.put` | editor → game | An object's template changed (settings, materials, a re-imported model); every instance is filled in again. |
| `script.changed` | editor → game | A script was edited; the game reloads it. |
| `script.error` | game → editor | A script raised an error; the Script editor marks the line. |
| `log`, `state`, `stats` | game → editor | Log lines, Play state, and frame figures about once a second. |
| `scene.dump`, `scene` | editor → game, and back | What the game has now (entities, their places and colours, the sky), for tests and tools. |

Edits reach the running game in well under a second. New or changed content (a mesh, a model, an image) is cooked into `game.pak` before the message that uses it is sent.

TCP is used instead of named pipes because it lets the same editor drive a game running on an **Android device** through `adb forward tcp:<port> tcp:<port>`.

#### Channel 3: in-process engine library (pybind11)

The engine is built as a library, `libmidgard`, with three front ends: the game runtime, a headless dedicated server, and a Python extension module `_midgard` that the editor imports. The editor uses it for anything that must match the game exactly:

- **`version()`**: the engine's version, shown in the About box;
- **`validate(package_dir)`**: package validation, run at the end of every cook;
- **`Body(settings)`** and `shape_fingerprint()`: the world shapes (positions, "up" and gravity), exactly as the game computes them;
- **`import_model(path, settings)`**: a glTF model as parts (`.glb`), KTX2 textures and materials, for the cook and the editor's views;
- **`encode_texture(path, srgb)`**: an image file as KTX2 with mipmaps;
- **`Viewport`**: the engine's renderer drawing the scene being edited, into a native window or offscreen. The editor creates a `QWindow`, passes its native handle (`winId()`) to C++, and the engine creates the Vulkan surface (Win32 on Windows, XCB on Linux). The window is wrapped with `QWidget.createWindowContainer()` and put in a dock. Offscreen viewports draw the object and material previews and the thumbnails; each renderer in the editor's process keeps its own Vulkan functions.
- *Planned:* deterministic terrain and texture generation (5.9 onwards, one implementation so previews equal the game), and shader compilation and validation (5.17).

All three channels are built.

### Q2. How can I handle building for Android?

**Decision: Android is a target only.** The editor never runs on Android. The same rule now covers every platform: Linux, Windows and Android game packages are all **export targets**, produced from prebuilt runtime templates (see *Target-only exports* below). The plan's overview should read: *"The editor runs on Linux and Windows. Games are exported to Linux, Windows and Android from either editor host."*

What the Android runtime needs:

| Piece | How |
|---|---|
| Native code | `libmidgard` + runtime built with the **Android NDK** through CMake. Targets `arm64-v8a`, plus `x86_64` for the emulator. vcpkg has `arm64-android` triplets for the dependencies. |
| App shell | **SDL3's Android project template** (`SDLActivity`). It provides the activity, surface, input, touch, lifecycle and audio, so the C++ code stays almost the same as on desktop. |
| Graphics | Vulkan. Set `minSdk 29` (Android 10), where 64-bit devices must support Vulkan 1.1. |
| Python | CPython is an official Android platform from **3.13** (PEP 738). Build it with the scripts in CPython's `Android/` folder. Ship `libpython3.x.so` in `lib/arm64-v8a/` and the stdlib as a zip in `assets/`. |
| Content | `data/` goes in the APK's `assets/`. `game.pak` is read through `AAssetManager` (store it uncompressed). `MyGame.db` is copied to internal storage on first launch or after a version change, because SQLite needs a real file. |
| Networking | `INTERNET` permission. Android builds **join only** and never host. Players join a PC host or a dedicated server, directly or through Midgard's relay on carrier networks (§4, *Co-op networking*). |
| Distribution | `.aab` for Google Play, `.apk` for sideloading. Sign with the game author's keystore. |

**Two export paths, the same way Godot handles it:**

1. **Template export (default, fast).** Midgard's own CI builds a *runtime template APK* on Linux for each engine release. On export, the editor copies the template, adds `data/` to `assets/`, sets the package name, label and version by editing the compiled binary manifest, aligns the zip and signs it with APK Signature Scheme v2, all in the editor's own Python code. The game author needs no JDK, no Android SDK, no NDK and no Gradle.
2. **Gradle export (advanced).** The editor writes a complete Gradle project (`platforms/android/` template + `data/`) for authors who add native plugins or need custom manifests. It is built with the NDK and `./gradlew bundleRelease`.

#### Target-only exports (all platforms)

The plan's rule that "final deployment build will be on each OS needed" moves to **Midgard's own release pipeline**. Each runtime template is compiled on its own OS in CI: the Windows template on Windows, the Linux template on Linux, and the Android template on Linux with the NDK. Templates are versioned with the editor and installed with it, or downloaded on first export. Exporting a game never compiles anything. It copies a template and adds the cooked package, so **either editor host can export to all three targets.**

| Target | Template | What the editor does on export | Works from |
|---|---|---|---|
| Windows x86_64 | `midgard_runtime.exe` + embedded CPython (`python3x.dll`, stdlib zip) | Copy, rename to `<Game>.exe`, add `data/`, set the window icon from `data/icon.png`. Optionally embed the Explorer icon and version info in the `.exe` resources. **Unsigned in 1.0** (see *Code signing* below). | Linux, Windows |
| Linux x86_64 | Runtime + embedded CPython, built against an old glibc (for example in a manylinux container) so it runs on most distributions | Copy, add `data/`, write a `.desktop` file and icon, pack as `.tar.gz`. **`.tar.gz` is the only Linux format** (decided). AppImage and Flatpak are not planned. | Linux, Windows |
| Android arm64 | Template APK/AAB (SDL3 shell + runtime + CPython) | Add `data/` to `assets/`, set package name, label and version, align, sign (v2) | Linux, Windows (nothing extra to install) |
| **Dedicated server**, Linux x86_64 | `midgard_server`: headless runtime + embedded CPython (no Vulkan, windowing or UI) | Add the *server* cook of `data/`, write `server.json`, a `systemd` unit and a `Dockerfile`, pack as `.tar.gz`. Build the Docker image too if Docker is installed. | Linux, Windows (`.tar.gz` + `Dockerfile`); image build where Docker is available |
| **Dedicated server**, Linux arm64 | `midgard_server` built for aarch64 + embedded CPython for aarch64 | Same as Linux x86_64. The `Dockerfile` names the platform, and the editor can build one multi-architecture image (`docker buildx`, amd64 + arm64). | Linux, Windows (`.tar.gz` + `Dockerfile`); image build where Docker with buildx is available |
| **Dedicated server**, Windows x86_64 | `midgard_server.exe` + embedded CPython | Add the *server* cook of `data/`, write `server.json` and start scripts, pack as `.zip`. Runs as a console program. | Linux, Windows |

What makes this possible, and the rules it imposes:

- **The package is platform-neutral.** `manifest.json`, `{project}.db`, `game.pak` (KTX2, glTF, SPIR-V, RmlUi, audio) and `.py` scripts are identical for every target. Only the runtime template differs. Per-target asset variants, such as smaller textures for Android, come from cook profiles, not separate projects.
- **Game scripts must be pure Python** (decided). Compiled extension modules (`.pyd`/`.so`) would need a build per target and break cross-export. Pure-Python libraries can be vendored into `scripts/lib/`. Anything that needs native speed belongs in the engine and is exposed through the `midgard` module.
- **No per-game C++.** Games extend the engine with Python only. A future native-plugin system would have to ship prebuilt binaries for every target.
- **Explorer icon from Linux needs checking.** Rewriting a Windows `.exe`'s icon resource without Windows tools (for example with LIEF) is a Phase 8 spike. If it isn't reliable, the window icon still comes from `data/icon.png` at runtime, and only the file icon in Explorer stays generic.
- **Code signing:** Windows builds are **unsigned in 1.0**. Players will see a SmartScreen "unknown publisher" warning the first time they run a game, so the export folder includes a short note explaining how to continue. A **Windows code-signing certificate** is planned **after 1.0**. The export step is ready for it: a *Sign* option will call `signtool` on Windows or `osslsigncode` on Linux once a certificate is configured. Android builds are always signed, because Android requires it.

#### Dedicated server export

The dedicated server is the same engine with rendering, audio, UI and input left out. It loads the same kind of package as the game, cooked with a **server profile**. That profile keeps the database, `.py` scripts, collision meshes, navigation meshes and terrain heights, and drops textures, shaders, audio and UI. Server packages are much smaller and need no GPU, so they run on a cheap cloud VM (including ARM instances), a home PC, a NAS or a **Raspberry Pi**.

**Linux arm64 notes**
- **Supported hardware:** 64-bit ARM Linux with glibc, such as Raspberry Pi 4 or 5 running a 64-bit OS, and ARM cloud VMs. 32-bit ARM (armhf), including 32-bit Raspberry Pi OS, is not supported.
- **Build:** the template is compiled in CI on native arm64 Linux runners. The fallback is cross-compiling with an aarch64 toolchain in a `manylinux_2_28` aarch64 container, so it runs on most distributions. vcpkg's `arm64-linux` triplet provides the dependencies. CPython for aarch64 comes from the same source as the x86_64 template.
- **Same package.** The server cook is platform-neutral, so one export can be copied to x86_64 and arm64 machines, and only the server binary differs. *Export ▸ Dedicated Server* can produce both at once.
- **Performance budget:** a Raspberry Pi 5 must run a 16-player session of the vertical slice at the full 30 Hz simulation rate. Physics and AI costs are measured on a Pi in CI or on a test device before each release. The editor's bandwidth warning gets a matching *CPU budget (Pi 5)* estimate.

```
MyGame-Server/
  midgard_server               the server runtime (midgard_server.exe on Windows)
  server.json                  the operator's settings (below)
  data/                        server cook: manifest.json, MyGame.db, server.pak, scripts
  python/                      embedded CPython
  saves/                       world saves and rotating backups (created on first run)
  logs/
  mygame-server.service        systemd unit (Linux)
  Dockerfile                   container build (Linux)
```

`server.json` holds the operator's settings. Any of them can also be set on the command line, such as `--port 7777`:

```json
{
  "world": { "name": "Northreach", "description": "Our winter world.", "difficulty": "hard", "seed": null },
  "port": 7777,
  "max_players": 16,
  "password": "",
  "admins": ["player-id-1"],
  "pause_when_empty": true,
  "autosave_minutes": 10,
  "backups_kept": 5,
  "join_code": true,
  "motd": "Welcome to Northreach"
}
```

Rules:
- **Matching content.** The manifest carries a content hash. A client whose package doesn't match the server's is refused with a clear message ("Server runs MyGame 0.3.0 build 17. You have 0.3.0 build 15.") instead of failing in strange ways later.
- **Players can't exceed the game's limit.** `max_players` in `server.json` can't be higher than the project's `max_players` (2–16).
- **One universe per server.** Each dedicated server process runs exactly one universe. To host several, run several servers on different ports, for example several Docker containers.
- **Administration.** Commands can be typed in the server console, sent through stdin under `systemd`/Docker, or used in game by players listed in `admins`: `save`, `kick`, `ban`, `say`, `time set`, `time scale`, `players`, `shutdown`. Every admin action is logged.
- **Saves.** The server autosaves on a timer and on shutdown (it handles SIGTERM, which Docker and `systemd` send). It keeps rotating backups, and a world save can be copied between a listen-server host and a dedicated server in either direction.
- **Same scripts.** Gameplay scripts already run on the host only (§4, *Co-op networking*), so they run unchanged on a dedicated server. `midgard.net.is_dedicated()` lets a script skip anything that assumes a local player.

**Start an Android spike early (Phase 2),** with a rotating cube from a cooked package on a real device. SDL3 + Vulkan + embedded CPython + SQLite on Android is where surprises hide, and it is cheaper to find them before 14 feature systems depend on the engine.

---

## 3. Editor architecture (Python / PySide6)

### Project and settings files

| File | Location | Contents |
|---|---|---|
| `midgard.json` | `QStandardPaths.AppDataLocation`, which resolves to `~/.local/share/Midgard/` on Linux and `%APPDATA%\Midgard\` on Windows (application name "Midgard", organisation name left empty) | Editor settings: theme, recent projects, dock layouts, monitor geometry |
| `{project_name}.config` | Project root | Project settings (JSON) |
| `{project_name}.db` | Project root | All structured game data (SQLite) |
| `objects/` | Project root | Models (`.gltf` with its files, or `.glb`) for objects and meshes. Projects from before it existed have their `assets/` folder moved here when they open. |
| `textures/` | Project root | Image files (PNG, JPEG) that materials use |
| `scripts/` | Project root | Game scripts (`.py`) |
| `.midgard/` | Project root | Editor-only: automatic backups and Assistant history (not committed) |
| `build/` | Project root | Cooked output, the content-hashed cook cache (`build/cache/`) and object thumbnails (not committed) |

The main window title is `Midgard - {project_name}`.

### Package layout

As built (`editor/midgard_editor/`):

```
__init__.py          THEMES_DIR, ICON_PATH, ICONS_DIR, SCHEMA_DIR (see Theme and About sections)
version.py           generated by bump_build.py
app.py               QApplication, high-DPI policy, settings, startup (opens the last project)
main_window.py       menus, dock manager, layouts, Play toolbar, title bar, About box
settings.py          midgard.json read/write (atomic)
project.py           open/create, {project}.config, {project}.db, migrations, change signals
undo.py              every edit is a QUndoCommand
scene_model.py       entities, folders, groups; transforms.py: placement maths for groups and gizmos
cook.py              Build ▸ Cook; exporter.py, export_dialog.py, android_apk.py, winicon.py: Export
live_link.py         Play-in-editor: the game process and the TCP link
engine.py            finds and loads the _midgard module
objects.py           objects: import, LODs, colliders, material changes, previews
textures.py          image textures; content_cache.py: the cook cache; watch.py: re-import on change
meshes.py            the editor's primitives as .glb
scripting.py, assistant.py, vcs.py (Git), backup.py, layouts.py, themes.py, about.py, branding.py,
physics_dialogs.py (World Shape, Physics Layers), dialogs.py
docks/               Project browser, Scene panel, Inspector (entity_form.py), Log, Viewport (with
                     gizmos.py), Script editor, Assistant, Project Page, Objects panel, Material editor
```

*Planned for Phase 6*, when the data editors arrive: **each Main Function becomes a plugin** that registers its dock widgets, menu actions, schema migrations, cook step and runtime API docs. With 14 systems, that keeps every system the same shape:

```python
class EditorModule(Protocol):
    id: str                                    # "quests"
    def migrations(self) -> list[Path]: ...    # schema/migrations/quests/*.sql
    def docks(self, ctx) -> list[DockSpec]: ... # Quest list, Quest graph, Goal inspector
    def actions(self, ctx) -> list[QAction]: ...
    def cook(self, ctx, out: CookOutput): ...   # validate + write runtime rows/assets
```

Two shared widgets will do much of the work (*planned*):

- **Node-graph view** (`QGraphicsView`, built for world types in 5.11): quest prerequisites, skill trees, progression, and later the shader graph.
- **Property grid** (Phase 6): the inspector used by every system.

Undo and redo have used `QUndoStack` from the first commit, and every edit goes through it (the History panel lists them; a slider dragged in the Material editor is one step).

### Dockable windows across multiple monitors

**Use `PySide6-QtAds`** (Qt Advanced Docking System). Plain `QDockWidget` can float panels onto a second monitor, but each floating panel is its own small tool window. With ADS, a floating window is a full **dock container**: you can drag several panels into one window on monitor 2 and tab or split them there, and save the arrangement as a named layout ("perspective").

```python
import PySide6QtAds as QtAds

class MainWindow(QMainWindow):
    def __init__(self, project):
        super().__init__()
        self.setWindowTitle(f"Midgard - {project.name}")
        QtAds.CDockManager.setConfigFlag(QtAds.CDockManager.OpaqueSplitterResize, True)
        QtAds.CDockManager.setConfigFlag(QtAds.CDockManager.FocusHighlighting, True)
        self.docks = QtAds.CDockManager(self)

    def add_panel(self, object_name, title, widget, area=QtAds.DockWidgetArea.LeftDockWidgetArea):
        dock = QtAds.CDockWidget(title)
        dock.setObjectName(object_name)      # stable key for saved layouts
        dock.setWidget(widget)
        self.docks.addDockWidget(area, dock)
        self.window_menu.addAction(dock.toggleViewAction())
        return dock
```

Layout features:

- **Window ▸ Layouts**: *Default* and *Two Monitors*, plus the user's own (*Save Current Layout As…*) and *Reset Layout*. More built-in layouts (*Scripting*, *World Building*) come with the editors that need them. Each layout is `CDockManager.saveState()` (bytes, stored base64) plus the main window geometry, saved in `midgard.json` under `layouts`.
- **The current layout is kept as it changes** (every few seconds, not only on a tidy exit): docked and floating panels, and the order of tabs dragged along a tab strip.
- **Restore safely.** After restoring, check every floating container and move it to the primary screen if its monitor has gone (for example, a laptop taken away from its dock):

  ```python
  def keep_on_screen(win: QWidget):
      if not any(s.availableGeometry().intersects(win.frameGeometry())
                 for s in QGuiApplication.screens()):
          win.move(QGuiApplication.primaryScreen().availableGeometry().topLeft() + QPoint(40, 40))
  ```

- **Mixed-DPI monitors.** Qt 6 scales per monitor by default. Set `QGuiApplication.setHighDpiScaleFactorRoundingPolicy(Qt.HighDpiScaleFactorRoundingPolicy.PassThrough)` before creating the app, so 150% screens aren't rounded to 200%.
- **The Viewport dock** handles `QWindow.screenChanged` and resize events by recreating the swapchain, because moving it to another monitor can change its DPI and refresh rate. Its gizmos float in a small see-through window of their own that follows the view onto any monitor.
- **Linux/Wayland caveat.** Wayland doesn't let applications position their own windows, so floating panels can't be restored onto a chosen monitor, and native child windows (viewports) are harder to embed. Offer a setting that starts the editor with `QT_QPA_PLATFORM=xcb` (XWayland) for full multi-monitor behaviour. Windows and X11 work as expected.

**Fallback:** if ADS is ever unavailable on a platform, `QMainWindow.setDockOptions(AllowTabbedDocks | AllowNestedDocks | GroupedDragging)` with `QDockWidget` gives basic multi-monitor floating with `saveState()` and `restoreState()`.

---

## 4. Engine architecture (C++)

**As built:**

```
engine/
  core/       version, double-precision world positions, deterministic maths and hashes, job system,
              profiling markers, package manifest, world shapes and bodies (the shape mapping layer)
  input/      action mapping on SDL3 input (keyboard, mouse, gamepad)
  render/     Vulkan via volk + VMA: HDR, PBR materials, KTX2/Basis textures, cascaded shadows, sky
              and fog, quality tiers, debug lines, the debug text overlay
  physics/    Jolt Physics (double precision): bodies, shapes, materials, layers, triggers,
              per-body gravity, the character controller
  scene/      package and pak loading, the scene (EnTT) from the database, objects and groups,
              glTF import, materials, the third-person camera, validation, the world's surface,
              collider outlines, the game loop
  script/     embedded CPython (pybind11/embed.h); the `midgard` Python API
  link/       the live link (length-prefixed JSON over TCP)
  runtime/    main(): the game executable (also --selftest, --screenshot, --dump-scene)
  server/     main(): the headless dedicated server (checks a package; networking is Phase 7)
  bindings/   the _midgard pybind11 module for the editor
  bench/      midgard_bench: timings checked against schema/budgets.json
  tests/      doctest unit tests
```

**The full plan** (what is still to come is added beside these as it is built):

```
engine/
  core/       ECS (EnTT), IDs, jobs, logging (spdlog), package + pak loader (miniz), SQLite access
  platform/   SDL3: window, input (keyboard, mouse, gamepad, touch), file system, Android assets
  render/     Vulkan via volk + VMA; frame graph; terrain clipmaps; sky, fog, water; KTX2 textures
  physics/    Jolt Physics (double precision): bodies, shapes, materials, character controller, triggers, per-body gravity
  world/      star field and world type generators, shape mappings (bounded, cylinder, sphere outside/inside),
              chunk grids and quadtrees, anchors, rooms and room changes, teleporters
  ships/      craft classes, hulls and modules, flight (space and aero), buoyancy, sails, wheeled and tracked vehicles,
              crew stations, interiors in the craft's frame, boarding, docking/landing/mooring
  mounts/     riding, rider IK and animation, taming, draft harnesses, mount stamina and needs
  water/      sea-level water on every world shape, wind-driven waves (identical on all clients), buoyancy queries
  script/     embedded CPython (pybind11/embed.h); the `midgard` Python API
  ui/         RmlUi: runtime player menus (generated from Menu Editor data), localised strings (ICU)
  camera/     first-person, third-person and top-down cameras, collision, lock-on
  anim/       ozz-animation: graphs, blend spaces, layers, IK, animation events, GPU skinning
  audio/      miniaudio: sound events, buses, 3D positioning, music, ambience, reverb zones; on-device TTS (Piper) for slot words
  input/      action mapping, contexts, rebinding, touch layouts (on SDL3 input)
  combat/     weapons, abilities, hit/hurt boxes, projectiles, damage and status effects, lag compensation
  ai/         behaviour trees, utility scoring, perception, factions, schedules, steering, companions
  net/        transports behind one interface (ENet direct, Midgard relay, Steam Networking Sockets); snapshots + prediction; join codes; listen/dedicated server; optional Steamworks layer
  game/       inventory, wallet/banking, quests, dialogue, skills, progression, bases and raids, gathering, crafting, saves: data-driven from the DB
  runtime/    main(): the game executable
  server/     main(): headless dedicated server (no render/ui)
  bindings/   _midgard pybind11 module for the editor
```

Notes:

- **One codebase, three front ends.** The dedicated server is the same engine without rendering, which is how "self-hosted in the game" and "dedicated server" share one implementation.
- **Server authority.** Wallet, banking, inventory and quest completion are decided on the server and replicated to clients, never trusted from the client, even when the server is a player's own game.
- **Python API.** Scripts reach engine systems only through the `midgard` module. Today it has entities (find, move, forces), raycasts and overlaps, gravity and "up", input actions, time, the sky, logging, and the `on_start`, `on_update`, `on_trigger_enter`/`exit` and `on_reload` callbacks. Later systems add their own (`midgard.quests.complete("q_wolves")`, `midgard.wallet.add(player, "gold", 50)`). The stubs in `schema/midgard.pyi` are checked against the engine (`midgard_runtime --dump-api`) and give the Script editor its completion.
- **Version.** CMake reads the root `VERSION` file into `midgard/version.h`, so the engine and the editor report the same release (see the About Scheme section).

### Co-op networking (2–16 players per world)

Small co-op keeps networking simple. There is one authoritative host per world, there are no shards or databases on a server farm, and every player can see the whole world.

| Topic | Design |
|---|---|
| **Topology** | Client/server with one authoritative host. The host is either a player's own game (**listen server**, *Host Game* in the menu) or the **headless dedicated server** built from the same engine. Clients never talk to each other directly. |
| **Transport** | **ENet** (UDP, reliable and unreliable channels). It is small, builds for Windows, Linux and Android, and easily handles 16 players. Add encryption later if needed (for example with libsodium). |
| **Simulation** | The host simulates at 30 Hz, including Jolt physics, AI, quests, economy and the calendar. It sends snapshots at 20 Hz, delta-compressed against the last snapshot each client acknowledged. |
| **Feel (action)** | Each client **predicts its own character** (movement, jumping, attack start) and reconciles with the host's result. Other players and NPCs are **interpolated** about 100 ms behind. Hits are confirmed on the host, with lag compensation (rewinding hitboxes to the shooter's view time) if combat needs it. |
| **Relevance** | With 16 players a simple distance-based relevance filter is enough. Spatial partitioning only becomes necessary for very large worlds. |
| **Scripts** | Gameplay scripts run **on the host only**. Client-side scripts are limited to UI and cosmetic effects. `midgard.net.is_host()` lets one script handle both cases, and `midgard.net.rpc()` sends player requests (such as "buy item") to the host, which checks them. |
| **Joining** | LAN discovery (broadcast), direct `address:port`, a **join code** from the **Midgard join-code service** (below), or a **Steam invite** in Steam builds. Text chat is built in. Voice chat is not. |
| **Join in progress** | A player who joins late gets a full world snapshot, including calendar time, then deltas. Players can drop in and out at any time. |
| **Host leaving** | **No host migration.** If a listen-server host quits, the session ends for everyone and the universe is saved on the host. If the host crashes, the others get a short **rejoin window** to reconnect to the same host. Groups that want an always-on universe use a dedicated server. |
| **Saves** | The **world save** (`worlds/{id}/`) lives with the host. **Characters live in each player's profile** and travel between worlds. The host checks them on join and sends them back on autosave and when the player leaves (§7). |
| **Cheating** | This is co-op among friends, so there is no anti-cheat. The host still checks every client request, which also prevents bugs from corrupting the economy. |
| **Limits** | `max_players` in `{project}.config`, from 2 to 16, checked at cook time. The editor warns if a game's replicated entity count would push a 16-player snapshot over the bandwidth budget. |

#### Join codes: the Midgard service
**Midgard provides the join-code service.** PHP is a good fit for half of the job:

| Part | Job | Technology |
|---|---|---|
| **Directory API** | Hosts register a session and get a code. Players look a code up. Heartbeats and expiry. | **PHP 8.3** (plain PHP or a micro-framework such as Slim) with SQLite or MySQL. Runs on the same server as the relay. |
| **Rendezvous and relay** | Tells the host and the joining player each other's public UDP address so they can connect directly (**hole punching**), and forwards traffic when a direct connection fails | A small long-running UDP program, **`midgard_relay`**, in C++ and reusing the engine's ENet code. Midgard's CI builds it for Linux x86_64 and arm64. PHP **can't** do this part on normal web hosting, because PHP runs once per web request and can't keep UDP sockets open. |

**How a join works:**
1. **Host:** *Host Game* calls `POST /v1/sessions` with the game ID, version, content hash, universe name, description, difficulty and player counts. It receives a **join code** and a secret host token. Codes are 6 characters from an alphabet without look-alikes (no 0/O, 1/I/L), shown as `K7Q-4ZP`. The game sends a heartbeat every 30 seconds with the player count. The code expires 90 seconds after the last heartbeat, and is deleted when the host closes the session.
2. **Joining player:** enters the code. `GET /v1/sessions/K7Q4ZP` returns the universe's name, description, difficulty and player count, checks the content hash (a version mismatch is refused with a clear message), and returns a one-time rendezvous ticket.
3. **Connection:** both sides contact `midgard_relay` with the ticket. The relay tells each side the other's public address, and they try to connect directly. If that fails within about 3 seconds, which is common on mobile networks, traffic flows through the relay instead.

**Running it:**
- **No accounts.** Only what a live session needs is stored. IP addresses are personal data under GDPR, so they're kept only while a session is live and deleted when it expires. The service publishes a short privacy note.
- **Protection:** HTTPS only, rate limits per IP address, random (not sequential) codes, and the host token is required to update or close a session.
- **Capacity:** most sessions connect directly. A fully relayed 16-player session uses roughly 0.5 MB/s, so one modest server can relay dozens of sessions.
- **Where it runs:** Midgard's public instance runs on a **private server** or with a **dedicated game-server hosting provider**. Either way it needs inbound UDP for the relay, which ordinary shared web hosting doesn't allow. Both parts ship as Docker images (one `docker compose` file), so moving between providers is simple. A game's `{project}.config` can also point at a studio's own instance.
- **Versioned API** (`/v1/`, JSON), so older games keep working when the service changes.

#### Steam
Games **may be published on Steam** (Windows and Linux builds; Android is not on Steam). An optional **Steam layer** uses Steamworks for:
- **friends and invites:** *Join game* from the Steam friends list, and rich presence showing the universe and system;
- **Steam Networking Sockets** with Valve's relay network between players who are all on Steam;
- **Steam Cloud** for character profiles and owned universes between Steam PCs. Midgard Sync covers phones (§7, *Syncing between devices*);
- **Achievements:** designers define achievements in an **Achievements** dock in the Progression editor (name, description, icon, hidden or visible). Each one is unlocked by a quest, progression, combat, discovery or script event (`midgard.achievements.unlock(id)`). Steam builds report them to Steam. Other builds show the same achievements in an in-game list, so every player has them.
- **Steam Workshop** for sharing **language packs** and **ship blueprints**. Subscribed items are downloaded into the same folders as hand-installed packs and blueprints, so non-Steam players can still share them as files. Workshop content gets no special trust: language packs fall back to English for missing keys, and blueprints are checked by the shipyard and the host like any other.

**Join codes stay the universal key.** A Steam invite carries the session's join code, so Steam players, non-Steam PC players and Android players can all be in the same session. The engine picks the transport per connection: Steam relay when both ends are on Steam, otherwise Midgard's direct or relayed connection.

The Steamworks SDK is proprietary, so it isn't in Midgard's repository. Developers download it themselves and point the editor at it. **Build ▸ Export** then offers a *Steam* option for the Windows and Linux targets: it adds the Steam API library and app ID and lays the build out for SteamPipe uploads.

#### Cross-play
**Cross-play is on.** Windows, Linux and Android players, with or without Steam, share the same universes.
- **Everyone runs the same package** (same content hash) over the same transport.
- **Fairness:** combat is co-op only, so differences between touch, gamepad and mouse can't disadvantage anyone against another player. Touch players get stronger lock-on and aim assist by default.
- **The join list and player list show each player's platform.** Text chat works everywhere, using the on-screen keyboard on phones.
- **Phones join but never host.** Android builds have no *Host Game* option. Android players join PC hosts or dedicated servers, through Midgard's relay when they are on a carrier network.

#### Host migration: what it would take
Today, when a listen-server host quits, the session ends. **Host migration** means another player takes over as host and the session continues. It would involve:

1. **A warm standby host.** Clients normally receive only their own room's state, but every room the host runs must be preserved. So one chosen **standby** client (the strongest device with the best connection, PCs before phones) continuously receives a compact copy of the host's full state: every active room, AI, quests, ships, companions, and unsaved universe changes. A full copy every few seconds plus the changes in between.
2. **Detecting the loss.** Either the host says it is leaving, or its heartbeats stop for about 3 seconds (a crash).
3. **Promotion.** The standby loads the latest state, becomes the authority, and takes over the join code in the directory. Every other client reconnects through the rendezvous service, with hole punching or relay again.
4. **Resync.** Clients get full snapshots and resume prediction. Anything after the last state copy (a few seconds) is rolled back: a jump, a hit or a purchase may need to be repeated.
5. **Universe ownership.** The universe belongs to the player who created it. After migration the new host has the latest state and the owner's copy is out of date. The updated universe has to be returned to the owner later, or it **forks** into two diverging copies.

**Advantages**
- The session survives the host quitting or crashing. The group keeps playing.
- Drop-in, drop-out groups don't need to rent or run a dedicated server.
- A host crash costs only a few seconds of progress, not everything since the last autosave.

**Disadvantages**
- **Complexity and risk.** It is one of the hardest networking features to get right. Edge cases include a host leaving mid-teleport, mid-room-change, with ships in flight, or with players spread over many rooms. These are hard to test, and bugs can desynchronise sessions or corrupt saves.
- **A constant cost** even when nothing goes wrong: the standby receives full-state traffic and does extra work all session. That is hard on phones.
- **A visible hitch:** a short freeze and a few seconds of rollback when migration happens.
- **Save forks:** the universe's ownership and the "real" copy must be reconciled, or players lose progress or get confused.
- **The new host may be weaker,** such as a phone, so performance or the player cap may drop.
- **Schedule:** it would noticeably lengthen Phase 7.

**Lighter alternatives**
- **Graceful handoff** (*not adopted*): a host who chooses to leave would hand the session to another player, with the universe state saved and transferred. This covers planned exits but not crashes.
- **Rejoin window** (*adopted*): if the host crashes, clients wait on a *Reconnecting…* screen and rejoin automatically, with the same join code, when the host restarts within a few minutes.
- **Dedicated server** (*adopted*): already supported, and there is no host to lose.

**Decision: Midgard has no host migration, including graceful handoff.** The analysis above is kept for the record. When a listen-server host leaves, the session ends and the universe stays with its owner, so universes are never forked. Two things soften this: the **rejoin window**, which reconnects everyone to the *same* host after a crash and so is not a migration, and **dedicated servers**, which have no host to lose.

---

## 5. Feature notes for the Main Functions

| Function | Editor (Python) | Engine (C++) | Notes |
|---|---|---|---|
| Script editor + snippets | **Built (Phase 4):** code editor dock (`QPlainTextEdit` + highlighter) with line numbers, completion from the `midgard` stubs, error markers from the running game and Git change markers. Snippets in a `snippet` table with tags; a project-level and a user-level library. | Embedded CPython, hot reload over the live link | Ship a starter snippet set: "give item", "start quest", "on trigger enter"… |
| Menu editor | WYSIWYG canvas with the widgets from the plan (text entry, buttons, drop boxes, lists, trees, radio buttons, checkboxes, panels, tabs, progress bars) and window controls | RmlUi documents generated at cook time | Window move, resize, minimize and close are behaviours of a `MenuWindow` element; player-chosen sizes and positions go in the save DB. Trees need a custom RmlUi element. |
| World environment | Skybox, sun/moon, lighting, fog, water panels with live preview | Sky, fog and water passes | **Sky and fog are built** (5.3); the Environment editor and water are 5.8. |
| Physics editor | Body/shape/material inspector, collision layers matrix | Jolt | **Built (5.4):** collider, rigid body and physics material rows in the Inspector, *World ▸ Physics Layers* and *World ▸ World Shape*, collider outlines in the viewport and the game (F4). |
| Vulkan shaders + texture generation | GLSL editor first, node graph later; texture generator (noise, gradients, tiling) | shaderc → SPIR-V; generators in C++, called via `_midgard` | One generator implementation for editor preview and runtime. Image textures (KTX2) are built (5.3, 5.5); shaders and generators are 5.17. |
| Terrain | Procedural parameters (FastNoise2 graph) + sculpt/paint brushes | Chunked heightmaps, clipmap LOD, splat materials | Sculpting needs the embedded viewport, so it comes late. |
| Object editor | Primitives, groups, glTF import, the Objects panel and Material editor | glTF loader, objects as templates | **Built (5.5):** cube, sphere, plane, cylinder and wedge primitives; groups; imported glTF objects with scale, up axis, collider (box, convex, mesh or none), levels of detail and thumbnails; the Material editor; re-import on change; Save Group as Object. |
| Inventory | Item table, categories, stacking, containers | Server-authoritative inventory | |
| Wallet + banking | Currencies, exchange rates, **Banking editor**: bank templates with interest, fees, insurance, withdrawal rules and risk profiles (see §7, *Banks*) | Host-authoritative ledger (append-only), monthly risk events from the Calendar | **In-game currency only.** The wallet travels with the character. Banks stay in each universe, with several banks of different risk and reward. |
| Quests | Quest list + goal editor + prerequisite graph | Quest state machine, events from game systems | Validate at cook time: unknown targets, impossible prerequisites, cycles. |
| Skill trees | Tree graph, ranks, modifiers, prerequisites | Stat modifier pipeline | Reuses the node-graph widget. |
| Progression | XP curves (curve editor), levels, unlocks | Progression service | |
| Calendar | Calendar templates (months, weekdays, seasons, moons, events, day and year ranges), standard calendar, year strip (see §6) | `TimeService`: universal clock, per-body local dates and seasons, solar phase, event dispatch per body | Drives lighting, seasons, weather, shops, NPC schedules, banks, raids and regrowth on each body. |
| Weather | Weather tab: weather types, tables per season and biome, transitions, previews (see §6) | Weather per region with drifting fronts, forecasts, gameplay effects, space weather, scripted weather | Tied to the calendar's seasons. |
| Survival | Survival settings: temperature model, status thresholds, clothing and suit stats, heat sources, hunger and thirst rates, food and drink effects, atmosphere kinds, preview (see §6) | Feels-like temperature, wetness, shelter checks, hunger and thirst meters, food relief, suits, air supply, life support, depressurisation, spacewalks | All effects scale with the world's difficulty. |
| Worlds & characters | World Type editor (graph with shape-aware inputs), Star Field editor, Difficulty editor, anchors and quest templates, world/character screen templates (see §7) | Run-time generation of star fields and bodies, shape mappings and gravity, chunk streaming from the seed, quest instancing, character validation and transfer | Generation must give identical results on every target. |
| Space & teleport | Teleport editor (range, targets, capacity, cost, cooldown, effects), star map, destination menu and loading screen templates (see §7) | Room manager, flyable system-space rooms, procedural skies, teleporter component and teleport events, interference zones | Teleporting is instant and always comes from a game object. |
| Ships and other craft | Ship editor for every class (hull frames with class and slots, modules as items, ship designs, movement tuning) (see §7) | Spacecraft flight, aerodynamics, buoyancy (air and water), waves and wind, sail physics, wheeled and tracked vehicles, crewed decks and turrets, combat and **boarding**, landing, mooring and docking, prediction, shipyard checks per class, salvage | Spacecraft, aircraft, airships, sailing ships, small craft and land vehicles. Bought, earned from quests, or built at the matching shipyard (small craft also at base boatyards). |
| Mounts | Mounts dock (AI Behaviour editor): species, stats and stat limits, inheritance, taming rules, equipment, mounted and aerial combat animations, draft harnesses | Riding and flying, stamina, saddlebags, mounted combat, taming, **breeding**, survival needs, calling, stables | Up to 21 per character. All are available in the player's own universes; one can be brought to someone else's. |
| Cameras | Camera editor: per-view settings, previews (see §8) | First-person, third-person and top-down cameras, switchable by the player, collision, lock-on, shape-aware "up" | |
| Combat | Combat editor: weapons, abilities, damage types, status effects, hit boxes on bones, combat sandbox | Hit detection from animation windows, projectiles, host-checked hits with lag compensation | Co-op only: players never damage each other. |
| AI behaviour | AI Behaviour editor: behaviour trees, utility scoring, perception, factions, schedules, spawn tables | Behaviour runtime, perception, navigation, live debugging | Runs on the host only. |
| Companions | Companions dock: recruit rules, hire wages, commands, traits, approval tags and levels, approval simulator, companion skill trees, comments | Following, commands weighed against approval and safety, refusals, leaving, downed/revive, travel through teleports, ships and between universes | Up to 2 per player. They belong to the character. They don't follow orders blindly. |
| Base building | Base editor: piece catalogue, snap points, costs, functions, build rules, piece budgets, no-build zones, prefab layouts, raid settings (see §7, *Bases*) | Snap placement aligned to local "up", support checks, permissions, storage, power, pads, defences, raids | Bases stay in their universe. Both world modes. |
| Gathering | Resources editor: resource items, node types, tools, distribution, regrowth, heat-map preview (see §8) | Resource nodes from the seed, depletion and regrowth, mining lasers, scanning | |
| Crafting | Crafting editor: recipes, stations, production-chain graph, quality, balance checker (see §8) | Station queues, quality rolls, host-checked recipes | Ship modules can be crafted. |
| Character animation | Animation editor: import, retargeting, state machines, blend spaces, layers, IK, events | ozz-animation sampling and blending, GPU skinning, parameter replication | |
| Audio | Audio editor: sound events, buses, music playlists, ambience, reverb zones | miniaudio, 3D positioning, occlusion | |
| Dialogue | Dialogue editor: trees of lines and player responses, conditions, actions, role templates, voice profiles, recording status, recording script export, lip-sync generation | Conversation runtime, group join/watch (the starter chooses), subtitles, TTS for slot words, lip sync, host-checked actions | Voice is English. Subtitles are optional. |
| Input mapping | Input editor: actions, contexts, default bindings, touch layouts | SDL3 input, player rebinding, device-aware prompts | Bindings live in the player's profile. |
| Localisation | Localisation editor: string table, status, pseudo-localisation, translation kit export | ICU MessageFormat, font fallback, in-game *Export strings*, user-installed language packs, English fallback | English original; players translate. No right-to-left languages. |
| Save/load UI | Save editor: autosave triggers, slots, slot details | Save slots (single-player), autosave only on listen servers, named manual saves on dedicated servers, thumbnails, save migrations | |
| AI console | LLM assistant dock (see §9) | None. It is editor-only. | Uses the Claude API with the user's own key. It is off until the user turns it on. **v1 built (Phase 4)**: read-only tools. |

---

## 6. Calendar: game time, seasons, day/night and weather

The Calendar controls how time passes in a game. **There is one clock per universe, and every body has its own calendar**, with its own day length, year, months, **seasons** and **weather**. Lighting, scripts, quests, shops, NPC schedules, banks, raids and regrowth on a body all follow that body's calendar.

### One clock, a calendar for every body
- **The universal clock** counts standard seconds since the universe was created (int64). It never differs between bodies, so co-op players on different bodies stay in step.
- **The standard calendar** is the project's main calendar. It is used for anything not tied to a place: companion wages, weekly limits on joint bank accounts, quest deadlines that span bodies, and timestamps in menus and saves.
- **Body calendars:** each world type links to a **calendar template**. When a body is generated, its calendar gets values from the template's ranges (day length, year length, axial tilt, season lengths), picked from the seed, so every body has its own **day length**, **year** and **seasons**. Month and weekday names come from the template (or its name generator).
- **What follows the body's calendar:** day and night, seasons, weather, NPC schedules, shop hours, calendar events such as festivals, bank interest and risk events (for banks on that body), raid schedules, and resource regrowth.
- **In a bounded-surface-map game**, the one map has the one calendar, and the standard calendar is the same as the map's.
- **On screen:** the HUD shows the current body's local date, time and season. The star map shows each discovered body's local time and season.

### What a calendar template defines

| Setting | Example | Notes |
|---|---|---|
| Hours per day / minutes per hour | 24 / 60 | Any whole numbers, such as a 20-hour day. |
| Months | 12 months × 30 days, each with a name | Months can have different lengths. Year length is the sum of the months. |
| Leap rule (optional) | Every 4 years, add a day to month 2 | |
| Weekdays | 7 names (Sunnudagr, Mánadagr, Týsdagr, Óðinsdagr, Þórsdagr, Frjádagr, Laugardagr) | Week length is independent of month length. |
| Era / year label | "Year {n} of the Ash" | Used when dates are shown to players. |
| Start date and time | Year 1, month 4, day 1, 06:00 | Where a new game begins. |
| **Real minutes per game day** | 48 | Designers think in "how long a day lasts". The editor shows the derived time scale (here, 30 game seconds per real second). |
| **Daylight** | 60% day / 40% night | The day/night split as a percentage of the day's hours. |
| **Seasons** | Spring, summer, autumn, winter (or any set) | Each season has a length, daylight percentage, temperature range and weather table (see *Seasons*). Daylight changes smoothly between seasons. |
| Day and year ranges | Day 18–36 hours, year 200–500 local days, tilt 0–35° | The generator picks each body's values inside these ranges. |
| Solar noon | 12:00 | The middle of the daylight period. |
| Twilight | 4% of the day | Dawn and dusk blend length. |
| Moons (optional) | "Máni", period 29.5 days | Phases for night lighting and events, such as a full-moon spawn. |
| Pause rules | Pause in single-player menus. Never pause in multiplayer. | |
| Time skips | Sleep/wait/fast travel: fire skipped events *all*, *latest only*, or *none* | |

### From clock time to light: solar phase

The clock is an integer count of **game seconds since the epoch** (int64). Dates are derived from it. Lighting never uses clock hours directly. It uses a **solar phase** from 0 to 1: 0 is sunrise, 0.5 is sunset, and back to 1 at the next sunrise. Daylight always fills 0 to 0.5 and night 0.5 to 1, whatever the percentage. The Environment editor authors sky colours, sun intensity, fog and ambient light as curves over solar phase, so changing the calendar (a 20-hour day, or 70% daylight) never breaks the lighting a designer has already set up.

```python
import math

def solar_phase(seconds_into_day: float, day_length: float, day_fraction: float,
                noon: float = 0.5) -> float:
    """0 = sunrise, 0.5 = sunset, 1 = next sunrise. day_fraction is the daylight share (0.6 = 60%)."""
    d = min(max(day_fraction, 0.01), 0.99)
    t = (seconds_into_day / day_length - noon + d / 2) % 1.0   # 0 at sunrise
    if t < d:
        return 0.5 * t / d                                      # day: 0 → 0.5
    return 0.5 + 0.5 * (t - d) / (1 - d)                        # night: 0.5 → 1

def sun_elevation(phase: float, max_elevation_deg: float) -> float:
    return max_elevation_deg * math.sin(2 * math.pi * phase)    # positive by day, negative at night
```

Between seasons, the daylight percentage blends smoothly: `day_fraction = base + amplitude × cos(2π × (day_of_year − longest_day) / days_per_year)`, where the base and amplitude come from the seasons' daylight percentages.

The real implementation lives in C++ (`TimeService`), and `_midgard` exposes it to the editor, so the editor's preview and the game always agree. The Python above is the reference for tests.

### Day and night on each world shape

**Each body has its own day length** from its calendar, so a planet may have a 30-hour day while a habitat cycles every 16 hours. **Solar phase** (0 = sunrise, 0.5 = sunset) can depend on where a player is standing, and each world shape has a light model that maps the clock and a position to a local solar phase:

| Shape | Light source | Local solar phase |
|---|---|---|
| Bounded area, flat-wrap cylinder | A sun moving across the sky | The same everywhere, as in the formula above |
| Sphere, outside | A distant sun; the planet turns | Shifted by **longitude**, so it is noon on one side while it is midnight on the other. *Uniform* mode uses the Calendar's daylight percentage at every latitude. *Latitude* mode varies day length with latitude and an axial tilt, giving polar summers and winters. |
| Sphere, inside | A **central sun** with **dimming** and orbiting **shades** | Set by where the shades are relative to the player. The shades cover (1 − daylight percentage) of the orbit, so the Calendar's percentage holds everywhere. As a shade's edge passes, the sun **dims and warms** over the twilight period, and the light comes in at a low angle from the side the shade is moving towards, giving **sunrise and sunset** effects. |
| Cylinder (curved) | A **sun-line** along the axis, with **dimming** and **shades** sweeping around the axis | The same model as the inside sphere: shades sweep around the axis, so different parts of the cylinder have different times of day, and the sun-line dims, warms and lights from a low angle as a shade's edge passes, giving **sunrise and sunset**. |

Because lighting curves are authored over solar phase, the Environment editor's sky, fog and light settings work unchanged on every shape. For shadows, the sun's direction is computed in the player's local frame from the local solar phase. Within shadow range the frame is nearly constant, so normal cascaded shadow maps still work, even with a central sun or a sun-line. Inside worlds use this **stylised sun direction** to make long sunrise and sunset shadows, even though the real light source is overhead. Their twilight colour, brightness and fog come from the same solar-phase curves as on planets.

Inside worlds draw no sky dome: the far side of the world is overhead, seen through haze, with the central sun or sun-line in view. The Environment editor has a separate **inside world atmosphere** preset (haze density by distance, colour of the far side).

### Seasons
Every body's calendar has **seasons**, which the calendar template defines:
- **Per season:** name, share of the year, **daylight percentage**, temperature range, **weather table**, visual changes (snow cover, foliage colour, ice on water, dry grass), music and ambience, and gameplay effects: resource regrowth rates, which plants can be gathered, NPC schedules, and creature spawn tables.
- **Planets (sphere, outside):** seasons come from the body's **axial tilt**. The two hemispheres have opposite seasons, the equator changes least and the poles most, so latitude matters (the *Latitude* lighting mode).
- **Hollow worlds and habitats:** seasons are an **artificial climate cycle** run by the world's makers. Designers can give them seasons like a planet's, a short cycle (such as a rainy week every month), or none.
- **Bounded surface maps:** one set of seasons for the whole map.
- **Transitions blend over days**, not instantly, for daylight, temperature, visuals and weather odds.
- **Scripts and quests** can use seasons, for example a harvest quest in autumn or a creature that only appears in winter.

### Weather
**Weather is tied to the calendar:** each season has its own weather table per biome.
- **Weather types** include clear, cloudy, rain, thunderstorm, snow, blizzard, fog, wind, sandstorm and ash fall. Designers can add their own. Each type sets clouds, precipitation, wind, fog, light tint, wetness and snow build-up, sounds, and gameplay effects.
- **How weather is chosen:** for each region of a body (cells a few kilometres across, so a large planet has different weather in different places), the weather follows the season's table for that biome. It moves from state to state with set probabilities and durations, and **fronts drift** between neighbouring cells. Every roll uses a random seed from the universe seed, the body, the cell and the local date, so weather can be reproduced and **forecast**.
- **Forecasts:** a weather station in a base, a ship's sensors or a handheld scanner shows the forecast for the next few local days.
- **Gameplay effects:** rain and fog reduce sight for players and AI perception; storms add turbulence and drag for ships; lightning can strike tall structures; snow slows movement; weather changes regrowth and which creatures appear; NPCs take shelter. A storm can also raise a base's attention for raids, or delay them. **Wind drives sailing ships and pushes airships**, and storms raise the sea state.
- **Inside worlds:** habitats and hollow worlds have **controlled weather**, such as scheduled rain cycles, mist and occasional "climate system faults" that quests can use.
- **Space weather:** each system can have **solar or ion storms** that weaken shields, blur sensors and cause teleport interference while they last.
- **Bounded surface maps:** weather crosses the map, and the storm wall at the edge is a permanent storm.
- **Scripted weather:** quests and scripts can force weather for a while, for example a storm during a boss fight, and normal weather then resumes.
- **Co-op:** the host decides the weather in every active room and sends it to clients. Clients render the effects themselves.
- **Performance:** precipitation and cloud quality scale with the device. Phones use lighter particle and cloud settings.
### Survival effects: temperature, wetness, clothing and shelter
**Weather and seasons affect characters directly** (decided).
- **Feels-like temperature** at a character's position comes from the season's temperature range, the time of day, altitude, biome, weather (wind chill, rain, sun) and nearby heat sources.
- **Clothing** is equipment with three survival stats: **insulation** (against cold), **cooling** (against heat) and **waterproofing**. Each body slot (head, torso, legs, feet, hands, outer layer) adds to the total. Clothing is crafted, bought or looted like any other equipment, and quality tiers improve its stats.
- **Wetness** builds up in rain, snow and water. It reduces insulation and dries out near heat, in shelter, or in warm, dry weather.
- **Shelter:** a character is sheltered under a roof and between walls, in base pieces, NPC buildings, caves, ship interiors and station interiors. The engine checks for this with a few short rays upward and sideways. Shelter blocks wind and precipitation.
- **Heat sources:** campfires (crafted from a few materials and placeable anywhere outside no-build zones), heaters and stoves in bases (which use power), hot springs, and warm drinks or food items for a short boost.
- **Status effects** use the Combat system's status model:

  | Effect | Cause | Result |
  |---|---|---|
  | **Chilled** → **Freezing** | Cold beyond the clothing's insulation | Slower stamina regeneration, then slower movement, then gradual health loss |
  | **Hot** → **Heatstroke** | Heat beyond the clothing's cooling | Faster stamina drain, then blurred vision and gradual health loss |
  | **Soaked** | Rain, swimming, snow melting | Insulation reduced until dry |
  | **Warm and dry** | Shelter and heat | Faster stamina and health regeneration (a rested bonus) |

- **Warnings come early:** a HUD temperature gauge, character comments, frost or sweat on the screen edges, and companions saying they're cold. Health loss only starts after a clear warning.
- **Companions** are affected by the same rules through their own equipment, so dressing them for the climate matters. Companions in base quarters are always sheltered.
- **Inside worlds** are mostly mild by design, so survival matters most on planets and bounded maps, and during climate faults.
- **The difficulty's `survival_scale`** sets how strong the effects are. At 0, effects are only cosmetic.
- **Co-op:** the host calculates every character's temperature and wetness.
- **Survival settings in the editor:** the feels-like temperature model (weights for wind, wetness, altitude and sun), status thresholds and effects, clothing stats in the item tables, and heat sources and their ranges. A preview shows a character's status for any season, time and weather on a chosen body.
### Hunger and thirst
- **Two meters**, **hunger** and **thirst**, drain over time. They drain faster when sprinting, fighting, carrying heavy loads, in the heat (thirst) and in the cold (hunger).
- **Effects as they run low:** first, stamina regenerates more slowly and health stops regenerating. Then come *Hungry* and *Thirsty* status effects with slower movement. At empty, *Starving* and *Dehydrated* cause gradual health loss, always after clear warnings.
- **Food and drink** are items: gathered (berries, fish, water from rivers and rain), hunted, bought, or **cooked** with Crafting recipes at campfires, stoves and kitchens in bases. Cooked meals restore more and last longer than raw food. Unsafe water and raw meat can cause sickness unless boiled or cooked.
- **Food and drink ease status effects** (decided):

  | Item (examples) | Eases |
  |---|---|
  | Hot soup, stew, hot tea | **Chilled** and **Freezing** (warms for a while), and speeds drying when **Soaked** |
  | Cold water, fruit, chilled drinks | **Hot** and **Heatstroke** |
  | Herbal remedies, broths | Sickness from bad food or water, **Poisoned** |
  | Hearty meals | A **Well fed** buff: better stamina and health regeneration, and a little extra cold resistance |

  Each food or drink item lists the effects it eases and for how long, using the same status model as combat and survival.
- **Companions** need food and water too. They eat from the base's food storage when in their quarters, and from their own inventory on the road.
- **Ships and bases** can store provisions. A kitchen in a base cooks from a queue like other crafting stations.
- **Food never spoils** (decided). Food and drink keep indefinitely in inventories, storage and ships, so there is no spoilage timer or preservation system.

### Difficulty sets the strength of every effect
Every survival effect scales with the world's difficulty (decided):

| Difficulty field | Controls |
|---|---|
| `survival_scale` | Strength of cold, heat and wet effects (0 = cosmetic only) |
| `hunger_scale`, `thirst_scale` | How fast the meters drain, and how hard the *Hungry*, *Thirsty*, *Starving* and *Dehydrated* effects hit (0 = meters off) |
| `food_effect_scale` | How strongly food and drink ease status effects, and how long the effects last |
| `air_scale` | How fast suits use air, and how quickly suffocation and toxic damage build (see below) |

The difficulty is fixed when the universe is created, so these never change mid-game.

### Hostile atmospheres
Some world types have air that characters can't simply breathe (decided). Each world type sets an **atmosphere kind**, and the generator can vary it per body within the type's options:

| Atmosphere | What players need | Notes |
|---|---|---|
| **Breathable** | Nothing | The **starting body is always breathable**. |
| **Thin** | **Breathing gear** with an air supply (or only at high altitude) | Stamina drains faster without it. |
| **Toxic** | A **filter mask**, plus a **sealed suit** if the air is corrosive | Unfiltered air builds a *Toxic exposure* effect. Corrosive air damages unsealed equipment. |
| **None (vacuum)** | A **sealed suit** with an **air supply** | Airless moons and asteroids, and **spacewalks** in system space. |

- **Suits and breathing gear** are equipment for the head and outer layer, with stats for **seal**, **air capacity**, **filter rating** and **temperature rating**. The temperature rating protects against the extreme heat and cold of airless worlds, using the survival model.
- **Air supply:** tanks run down while worn in hostile air. Players refill them at **life support** in bases, ships and stations, or with air canisters (crafted or bought). Low air gives repeated warnings, then *Suffocating* causes rapid health loss.
- **Sealed spaces:** ships have a **life support** module and are pressurised inside. Bases on hostile bodies need **airlocks** and a **life support** piece (which uses power) to make their rooms breathable. A hull breach in ship combat, or a raid that breaks a base wall, **depressurises** the affected rooms until they are repaired.
- **Spacewalks:** in system space, players in sealed suits can step outside ships and stations, use **magnetic boots** on hulls and a small **thruster pack** for zero-g movement, and repair or salvage from outside.
- **What the air changes:** campfires won't burn without oxygen, so heaters are needed. Sound is muffled in thin air and near silent in vacuum. Ships get no lift or drag in vacuum (already part of flight).
- **Teleporting and landing** show the destination's atmosphere, and warn if the player's or a companion's gear isn't enough.
- **Companions** need their own suits and air, and complain early when their air runs low.
### Contract tables

```sql
CREATE TABLE calendar_template (
  id TEXT PRIMARY KEY, name TEXT, is_standard INTEGER,       -- exactly one template is the standard calendar
  minutes_per_hour INTEGER, real_minutes_per_standard_day REAL,
  day_hours_min REAL, day_hours_max REAL, year_days_min INTEGER, year_days_max INTEGER,
  tilt_min REAL, tilt_max REAL, solar_noon REAL, twilight REAL,
  leap_every_years INTEGER, leap_month INTEGER, era_format TEXT, month_names JSON,
  pause_in_menus INTEGER, skip_policy TEXT);
CREATE TABLE calendar_month   (template TEXT, seq INTEGER, name TEXT, share REAL, PRIMARY KEY (template, seq));
CREATE TABLE calendar_weekday (template TEXT, seq INTEGER, name TEXT, PRIMARY KEY (template, seq));
CREATE TABLE calendar_season  (template TEXT, seq INTEGER, name TEXT, share REAL, day_fraction REAL,
  temp_min REAL, temp_max REAL, visuals JSON, effects JSON, PRIMARY KEY (template, seq));
CREATE TABLE calendar_moon    (template TEXT, id TEXT, name TEXT, period_days REAL, offset_days REAL,
  PRIMARY KEY (template, id));
CREATE TABLE calendar_event   (id TEXT PRIMARY KEY, template TEXT, name TEXT,
  recurrence TEXT CHECK (recurrence IN ('once','yearly','seasonal','monthly','weekly','daily')),
  year INTEGER, month INTEGER, season INTEGER, day INTEGER, weekday INTEGER, hour INTEGER, minute INTEGER,
  duration_minutes INTEGER, script TEXT);
CREATE TABLE weather_type     (id TEXT PRIMARY KEY, name TEXT, visuals JSON, audio JSON, effects JSON);
CREATE TABLE weather_table    (template TEXT, season INTEGER, biome TEXT, weather TEXT, weight REAL,
  min_hours REAL, max_hours REAL, next JSON);                 -- next = transition weights
-- world_type.calendar_template links each world type to its template.
-- A body's calendar values (day length, year length, tilt) are generated from the seed and never saved.
```

Other systems refer to calendar events by ID, and they fire on each body by that body's local date. For example, `quest_prereq.kind = 'calendar_event'` makes a quest available only during a body's Harvest Festival. Shop opening hours and NPC schedules are local hour ranges, and a bank's interest is posted on its body's monthly event.

### Editor: the Calendar dock
- **Calendar templates**: months, weekdays, **seasons**, moons, events (with a recurrence editor) and the ranges for day length, year length and tilt. One template is marked as the **standard calendar**.
- A **weather** tab: weather types and the weather table per season and biome, with transition weights and durations.
- A **day bar** showing day, twilight and night for a selected date, and a **year strip** showing day length, seasons and weather odds across the whole year.
- A **time-of-day scrubber**, plus **season** and **weather** pickers, that drive the viewport and Environment preview through `_midgard`, and the running game through the live link. A **body preview** rolls a body's calendar from any seed.
- Cook-time checks: every month has at least one day, season shares add up to 1, daylight is between 1% and 99%, ranges are valid, every biome has weather in every season, and events point at real months, seasons and days.

### Engine: `TimeService`
- Advances the clock by `real dt × scale` with a fractional accumulator, so time never drifts.
- Keeps the universal clock and computes each active body's local date, season and solar phase from it.
- Emits `hour`, `dawn` (phase passes 0), `dusk` (phase passes 0.5), `day`, `month`, `season`, `year`, and `event_start`/`event_end` **per body**, plus `weather_change` per region. There are no per-minute events, which would be too frequent.
- **Multiplayer:** the host owns the clock. Time runs while the host or dedicated server is running and stops when the world is closed. A dedicated server can be set to pause when no players are connected (the default) or to keep running. Clients get `{game_time, scale, server_timestamp}` on join and whenever the scale changes, extrapolate locally, and resync smoothly every few seconds.
- The save database stores `game_time`. Loading a save restores the date exactly.

### Script API
```python
now = midgard.time.now()        # local date on the player's body: GameTime(year=3, month=4, day=12, hour=18, weekday="Óðinsdagr", season="Autumn")
std = midgard.time.standard()   # the standard calendar's date
midgard.time.season()           # "Autumn"
midgard.weather.current()       # Weather(type="rain", intensity=0.6, wind=(3.1, 0.4))
midgard.weather.forecast(days=3)
midgard.weather.on_change(close_shutters)
midgard.weather.force("thunderstorm", hours=2)   # scripted weather, then normal weather resumes
midgard.time.is_day()
midgard.time.on("dusk", light_torches)
midgard.time.on_event("harvest_festival", open_market)
midgard.time.advance(hours=8)   # sleeping; skipped events follow the skip policy
midgard.time.set_scale(0)       # freeze time for a cutscene
```

---

## 7. Worlds, space and characters

Worlds are **procedurally generated by the game, at run time, from a seed**. The editor only *describes* what kinds of places can be generated. A player can own **several worlds**, each with its own **name**, **editable description** and **difficulty level**. **Characters belong to the player, not the world**, so the same character can play in their own worlds and in friends' worlds.

### Terms

| Term | Meaning |
|---|---|
| **World** | What the player creates and sees in their world list: name, editable description, difficulty, seed. A world is a whole generated **universe**: a procedural **star field** of star systems containing **bodies**. |
| **Body** | One place to stand on, in a system-map game: a **planet** (sphere, terrain outside), a **hollow world** (sphere, terrain inside) or a **habitat** (cylinder). Systems contain only cylinders and spheres, and all three kinds can be mixed. |
| **World type** | A designer's recipe in the editor for one kind of body: shape, size range, terrain graph, biomes, atmosphere, lighting, gravity and exits. Examples: *Temperate Planet*, *Ice Moon*, *O'Neill Habitat*, *Hollow World*. |
| **Star field recipe** | A designer's recipe for the universe: how many systems, the kinds of stars, and which world types appear where. |
| **Room** | One loaded play space: a body's surface, the space around a star system, or an interior. Moving between rooms is a **room change** (below). |

**World mode is a decision for each game project**, set in `{project}.config` and fixed for that game:

| World mode | What a world is | Space, systems and ships |
|---|---|---|
| **System map** | A universe: 9 star systems with **2–13 bodies** each, all cylinders and spheres | System space, star map, teleporting between bodies and systems, ships flying through space and atmospheres |
| **Bounded surface map** | One **bounded area**: a rectangle of generated land and sea, **surrounded by ocean**, with **1–3 continents** and a chosen number of **archipelagos** and **islands** | No star field and no system space. Teleporting works within the map. **Craft work here too**: **sailing ships and small craft** carry players between continents and islands, and **aircraft and airships** fly under a height ceiling. Everything stays inside the storm wall at the map's edge. Spacecraft aren't used in this mode. |

Everything else (calendar, quests, combat, companions, banks, co-op, sync) works the same in both modes.

### Generated in the game, described in the editor
- **The editor never generates players' worlds.** Designers describe world types and a star field recipe. The cooked package carries those recipes, and the **game** generates everything from the seed the player chooses or rolls on the New World screen.
- **Previews are exact.** The editor's previews call the same engine code through `_midgard`, so a designer previewing seed 829174332 sees what a player with that seed gets.
- **What is generated when:**

  | When | What is generated |
  |---|---|
  | **World creation** (seconds) | The star field: systems, stars, each system's bodies, their world types, sizes and names, which body hosts each quest anchor, and the starting body |
  | **First visit to a body** | That body's layout: biomes, points of interest, anchors, NPC settlements |
  | **As players move** | Terrain chunks streamed around each player |
  | **Never saved** | Anything that can be generated again from the seed |
  | **Saved** | Only changes, per body |

- **Every target generates.** Android phones, PCs and Raspberry Pi dedicated servers all run the generator, so generation time is a release budget. For example, a new body must be playable within a few seconds on a mid-range phone, and this is measured in CI on each target.

### What a world is

A world is a seed plus the recipes, along with everything that has changed since generation. The generated landscape is never saved. It is rebuilt from the seed, so saves stay small.

| Part | Contents |
|---|---|
| Identity | World ID (UUID), **name**, **description** (plain text, up to 500 characters, editable at any time by the world's owner), owner, created and last-played dates, total play time |
| Generation | **Seed**, star field recipe and version, world type versions in use, options chosen at creation (for example number of systems, "more water") |
| Difficulty | The difficulty level chosen at creation (below) |
| State | Discovered systems and bodies, per-body changes (terrain edits, placed and removed objects, depleted resources, NPC state), quest instances, bank accounts, calendar time |
| Rules | Owner's settings: password, whether outside characters may join, optional character level range |

On disk (player's data folder, or the dedicated server's `saves/`):

```
worlds/
  7f3c…/                       one folder per world, named by ID (renaming a world never moves files)
    world.json                 identity, generation, difficulty, rules: read by the world list without opening a DB
    world.db                   universe state: discovered systems/bodies, quests, banks, calendar time (SQLite)
    bodies/
      b_0412.db                changes on one body, created on its first visit
    backups/                   rotating copies of the databases
```

```json
{
  "id": "7f3c2a9e-…",
  "name": "Northreach",
  "description": "Our winter world. Don't build near the river: it floods in spring.",
  "seed": 829174332,
  "star_field": { "id": "sf_main", "version": 2, "options": { "water": 1.2 } },
  "start_body": "b_0007",
  "difficulty": "hard",
  "game": { "name": "MyGame", "version": "0.3.0" },
  "created": "2026-10-14T19:02:00Z",
  "last_played": "2026-10-20T21:40:00Z",
  "play_time_s": 45210,
  "rules": { "allow_outside_characters": true, "level_range": null }
}
```

### Procedural generation
- **World Type editor:** one node graph per world type. It combines the Terrain system's noise, biome rules, and placement rules for points of interest (villages, dungeons, resources, NPC camps, exits). It also sets the shape, size range (within the limits for that shape), atmosphere, lighting (dimming and shade settings for inside worlds), surface gravity and wall style. It has a seed field and a **Generate preview** that renders a map and a 3D chunk through `_midgard`.
- **Star Field editor:**
  - the layout of the **9 systems** (positions, distances, which system the game starts in);
  - the mix of star classes and nebula settings;
  - the **body mix**: **2–13 bodies per system**, drawn from cylinder and sphere world types by weight, plus rules such as "habitats only orbit G-class stars" or "the starting system has a Temperate Planet";
  - **name generators** for systems and bodies (syllable tables or word lists).

  A **Generate preview** shows a seed's star map and lists the bodies it would contain.
- **Recipes are versioned.** Each world records the star field and world type versions it was made with. If a game update changes a recipe, existing worlds keep generating with their old version, so they don't change under the player. The engine keeps every recipe version that any shipped world could use, and the cook step refuses to drop one.
- **The same seed gives the same world on every target.** In co-op, clients rebuild chunks from the seed rather than downloading them, so generation must give identical results on x86_64, arm64 and Android. Generators use integer or fixed-point noise and hashing where possible, and are compiled with strict floating-point settings (`/fp:precise`, `-ffp-contract=off`, no fast-math). CI generates reference chunks on every target and compares their hashes. As a safety net, a client checks each chunk's hash against the host's and downloads the chunk if they differ.
- **Generation is off the main thread** and streams chunks around the players, so a new world is playable within seconds.

### World shapes

Every body is **fixed in size**, and its shape comes from its world type. **In a system-map game, a universe mixes cylinders and spheres**: planets, hollow worlds and habitats in the same star field, with travel between them (*Space, star fields and travel*, below). **Bounded areas exist only in bounded-surface-map games**, where the whole world is one bounded area.

| Shape | Terrain | "Up" and gravity | Edges | Example |
|---|---|---|---|---|
| **Bounded area** (bounded surface map games only) | A rectangle of land and sea | Constant, straight down | **Ocean** on every side, ending in a storm wall (see *Edges*) | Continents, archipelagos, island chains |
| **Cylinder** | A rectangle whose two opposite edges **join seamlessly**. Shown as the inside surface of a cylinder (*curved*) or as a flat map that wraps around (*flat wrap*) | Curved: outward from the axis, so "up" points at the axis. Flat wrap: straight down | The two edges that don't join are the cylinder's ends | A space habitat, a ring valley, an endless-feeling strip of land |
| **Sphere, outside** | The outer surface of a sphere | Toward the centre | None | A small planet or moon |
| **Sphere, inside** | The inner surface of a hollow sphere | Outward from the centre, so "up" points at the centre | None | A hollow world with a central sun |

#### One terrain model for every shape
- **Terrain is always a set of rectangular grids.** A bounded area and a cylinder each use **one** rectangle. A sphere uses a **cube-sphere**: six square faces whose grids are projected onto the sphere, which avoids the crowding and seams a latitude/longitude grid has at its poles. A cylinder is literally the rectangle the bounded area uses, with its left and right edges joined (chunk columns are numbered modulo the circumference). It is stored and generated that way, then bent into a cylinder only when it is drawn.
- **A shape mapping** turns surface coordinates (face, u, v, height) into a 3D position, and gives the local "up" and the gravity at any point. Rendering, physics, AI, object placement and networking all ask the mapping and never assume "y is up". Height is displacement along "up": outward on a sphere's outside, inward on the inside surfaces.
- **The generator samples noise at 3D positions on the shape's surface**, not at grid coordinates. This makes the joined cylinder edge and the sphere's face edges seamless automatically: walking around a cylinder or across a cube face never reveals a seam. Generator nodes also get shape-aware inputs, such as latitude on a sphere, distance from the cylinder's ends, and distance from a bounded area's edge, so designers can add polar ice, mountains at the ends, or a coast around the edge.
- **Level of detail** uses a quadtree of chunks on each grid. The far side of an inside world is drawn too: on a cylinder or inside sphere, the terrain overhead *is* the sky. It is drawn at low detail and softened by distance haze.
- **Water** is a shell at sea level: a height on a bounded area, a radius on a sphere, a distance from the axis on a cylinder.
- **Collision** comes from mesh shapes built for the chunks around each player, because Jolt's heightfield shape can't follow a curved surface. A bounded area or flat-wrap cylinder can use Jolt's heightfield directly.

#### Gravity
- Jolt's single global gravity is turned off. Each physics step applies gravity per body from the shape mapping. The character controller (`CharacterVirtual`) gets its "up" vector every frame, so players walk all the way around a sphere or cylinder.
- **Gravity per shape** (decided):
  - **Spheres, outside and inside: constant gravity.** It has the same strength everywhere on the surface, pointing to the centre (outside) or away from it (inside).
  - **Cylinders: realistic spin gravity.** Strength is proportional to the distance from the axis: full at the surface (`g = g_surface × r / R`) and zero at the axis. The world type sets the surface gravity, and the spin rate follows from the radius (`ω = √(g_surface / R)`). Hilltops are slightly lighter than valleys, and gravity fades towards the axis, so ships near the axis are almost weightless. The Coriolis effect is not simulated for characters, because it makes movement feel wrong. Projectiles and ships follow the radial gravity only.
  - **Bounded areas and flat-wrap cylinders:** constant, straight down.
- Trees, buildings and spawned objects are placed upright along the local "up".

#### Edges
- **Bounded area: surrounded by ocean.** The generator always leaves a band of open ocean around the land, so no coast touches the map's edge. At the very edge of the map is a **storm wall**: heavy weather, fog and rough seas that turn swimmers, boats and ships back gently. It is the only hard limit, and it needs no message.
- **Land masses:** the **New World** screen asks the player for:
  - **Continents: 1–3** large land masses with noise-shaped coastlines, mountain ranges and rivers;
  - **Archipelagos:** how many clusters of islands (for example 0–6);
  - **Islands:** how many separate single islands (for example 0–20).

  The world type sets the maximum for each count from the map size, and the land-to-ocean ratio. The generator places the continents first, then archipelagos in the open ocean between them, then lone islands, keeping minimum gaps so sea routes exist. The choices are part of the world's generation and **never change after creation**.
- **Cylinder ends:** the end caps are **walls**. The world type chooses their look (natural cliff face, built fortification, or energy barrier), the generator raises the terrain to meet them, and they are impassable on foot and in ships. **Docks** on the end caps, at the axis, are the habitat's exits to space.
- **Planets (sphere, outside) are bounded by the sky.** The atmosphere has a top height set in the world type. Climbing or flying above it triggers a **room change** to the space around the planet's system. Coming back down returns the player to the sky above a chosen landing site.
- **Hollow worlds (sphere, inside)** have a ceiling below the central sun, which players can't reach on foot. Ships may fly across the open interior but not into a **no-fly zone** around the sun. In **habitats**, ships cross the open middle freely, past the axis. Their exits are **exit sites** placed by the generator, such as polar shafts or docks, and each one triggers a room change to space.

#### Size and precision
- **Size ranges** (decided): **spheres** (outside and inside) have a **radius of 1–20 km**. **Cylinders** are **5–50 km long**, with a radius of 1–5 km (a circumference of about 6–31 km; an assumption). Bounded areas are set per world type. Each world type picks a range within these limits, and the star field generator picks each body's size within the world type's range. The star map shows each body's size. These limits set the precision, streaming and Raspberry Pi budgets.
- Large worlds lose floating-point precision far from the origin. The host simulates in **double precision** (Jolt's `JPH_DOUBLE_PRECISION` build) and rendering is **camera-relative**, so a world can be tens of kilometres across without jitter.
- Positions sent over the network are chunk-relative, and wrap correctly across a cylinder's joined edge.

#### Navigation and maps
- AI navigation meshes (Recast) are built per chunk in the chunk's local frame, where "up" is nearly constant, and linked at chunk edges, including across a cylinder's joined edge and between cube-sphere faces.
- The in-game map uses one projection per shape: top-down for a bounded area, an unrolled rectangle that scrolls around for a cylinder, and a cube-face or equirectangular map for spheres. The star map shows a small preview image of each body, made the same way.

### Space, star fields and travel

#### Star fields
- **Every universe has 9 star systems**, generated from the world seed when the world is created. The recipe arranges them (layout, distances, star classes, which world types each system holds). Each star has a class (colour, brightness, size), and each system's bodies get cosmetic orbits that advance with the calendar. There is no orbital mechanics simulation.
- **One sky everywhere.** From a planet at night, from inside a habitat, or in space, the sky shows the star field as seen from that system. Each star system's sky is built the same way:
  - the nearest bright stars are drawn individually from the generated catalogue;
  - the faint background (distant stars and noise-based nebulae) is rendered once per system into a cubemap at run time and cached;
  - other bodies in the system are visible: a habitat as a bright line, a hollow world as a dark shell, planets and moons as discs.
- **Names** for systems and bodies come from the recipe's name generators and appear on the star map. **Players can rename bodies** and give them a description. A new name is shown to everyone in the universe, the generated name is kept as a subtitle, and only the universe's owner (or the dedicated server's admins) can rename a body that someone else has already renamed.
- **The star map** (a Menu Editor template) shows the 9 systems and their **discovered** bodies with names, world types and sizes.
- **Discovery: everything starts hidden** (decided), except the body the player starts on. Systems and bodies are revealed only by **exploration and scanning**: visiting, flying close, and using a ship's **sensor** module or a handheld scanner. Sensor range and quality depend on the module. Quests can point to a system or a direction, but the player still has to scan to reveal the body. Discoveries are shared by everyone in the universe.

#### Rooms and room changes
A **room** is one loaded play space. There are three kinds:
- the **surface** of a body;
- **system space** around a star system: a flyable room with the star, its bodies and the star field;
- **interiors** that designers build by hand, such as stations or dungeons.

Ship interiors are **not** separate rooms. A ship's decks travel with the ship (see *Ships*).

| Trigger | From → to |
|---|---|
| Flying or rising above a planet's atmosphere | Planet surface → system space, above the point left (with the ship, if any) |
| Flying a ship into a planet's atmosphere in system space | System space → that planet's sky, above the matching point on the surface |
| Habitat docks and hollow-world exit sites (on foot or by ship) | Body ↔ system space, at that exit |
| A **teleport** to another room (below) | Any teleporter → its destination |
| Doors and portals placed by designers | Any room → any room |

A room change:
1. Fades out, or shows a loading screen (a Menu Editor template).
2. Saves the room being left. If no players remain in it, it is unloaded.
3. Carries the character across, with their inventory and state, and their ship with its crew if they are flying.
4. Loads the target room and streams the chunks around the arrival point **before** fading in, so the player never arrives in empty terrain.

Scripts use `midgard.rooms.change(target, arrival)`, `on_room_enter` and `on_room_exit`.

**Co-op: players can be on different bodies, in different systems, at the same time.** The host simulates every room that has a player in it, so there are at most 16 active rooms, one per player. Snapshots only cover each player's own room, and the calendar clock is shared by all rooms. Weak hardware can set an optional `max_active_rooms` in `server.json`. When that limit is reached, a teleport or flight into a new room is refused with a message. The Raspberry Pi 5 budget is measured with 16 players and 32 companions spread over 8 rooms.

#### Teleportation: instant travel
Players travel between bodies and systems **instantly**, by teleport, in the style of Star Trek's transporter. No calendar time passes.

**Teleporting is always done by a game object.** A teleporter is a component that designers attach to any object:

| Kind | Examples | Behaviour |
|---|---|---|
| **Machine** | A transporter pad in a station, an ancient gate, a booth in a city | Fixed in place. Can send everyone standing on it, up to its capacity. |
| **Ship module** | A ship's transporter | Beams crew between the ship and a surface or another ship within range. |
| **Player-placed pad** | A teleport pad a player builds or places **in a base or anywhere on a surface** | Placed from an inventory item. It becomes a named destination on the owner's star map, and the owner sets who may use it (only themselves, their party, or everyone in the universe). It can't be placed inside an interference zone. |
| **Artifact / item** | A relic, a wrist device, a one-use crystal | Carried in the inventory. Usually personal, with charges or a cooldown. |

Each teleporter type is set up in the **Teleport editor**:
- **Range:** the same body, the same system, or anywhere in the 9 systems.
- **What it can lock on to:** paired machines; any discovered landing site, dock or exit; the ship it's installed in; a party member; or a **player-placed teleport pad**.
- **Capacity:** how many people one use can send.
- **Cost and requirements:** wallet currency, energy, charges or items; and skills, quests or items needed to use it.
- **Cooldown.**
- **Effects:** the beam shader, particles and sound.
- **Recall:** whether it can beam up or send other players. It always asks them first.

Using a teleporter opens a **destination menu**: the star map, filtered to what that device can reach. Choosing a destination fires a **teleport event**:
1. Scripts can react or cancel (`on_teleport_start`, `on_teleport_arrive`), for example on a malfunction or a quest condition.
2. The beam effect plays.
3. A room change follows if the destination is in another room. Otherwise the player is moved instantly within the same room.

Other rules:
- **Safe arrival:** the arrival point is a valid standing spot near the target. The game finds it by looking down along the local "up" and keeping clear of geometry.
- **Interference zones:** designers mark areas, such as dungeons, shielded bases or storms, where teleporting in or out is blocked or needs a stronger device. Quests can raise or lower these zones.
- **Destinations must be discovered,** unless the target is a paired machine or a pad the player is allowed to use.
- **Teleports move people and what they carry. They never move ships.**

#### Ships and other craft
**"Ship" in this document means any craft**: spacecraft, aircraft, airships, sailing ships, small craft and land vehicles. They share one system of hulls, modules, crews, building, ownership, wrecks and combat. Each **craft class** has its own way of moving.

| Class | Moves through | Movement model | Examples |
|---|---|---|---|
| **Spacecraft** | Space, atmospheres, inside habitats and hollow worlds | Thrusters, with flight assist or Newtonian flight | Fighters, freighters, landers, shuttles |
| **Aircraft** | Atmospheres only | **Aerodynamic flight:** lift from wings or rotors, drag, stall, banking turns. Runways, or vertical take-off for rotorcraft and VTOL designs. | Biplanes, jets, gyrocopters, helicopters |
| **Airships** | Atmospheres only | **Buoyancy:** lift from gas cells, which depends on air density, with ballast and venting to climb and descend. Slow propellers, and **strongly pushed by the wind**. | Zeppelins, cargo blimps, sky-galleons |
| **Sailing ships** | Water surfaces | **Buoyancy on waves**, driven by **sails and the weather's wind** (points of sail, tacking, reefing in storms), steered by the rudder. Oars or engines are optional modules. | Sloops, brigantines, galleons, junks |
| **Small craft** | Water or air, close to the surface | Light versions of the boat and aircraft models, for one to four people. Some are **paddled or rowed by the characters** (using stamina). | Rowboats, canoes, rafts, motor launches, gliders, ultralights |
| **Land vehicles** | Ground: roads, open terrain, inside habitats and hollow worlds | **Wheeled or tracked vehicle physics** (Jolt's vehicle constraints: suspension, traction, gears). Grip changes with terrain and weather (mud, snow, ice, sand). Engines, or **draft animals** that pull carts and wagons. | Carts, wagons, buggies, rovers, trucks, tanks |

**Every craft is a hull plus modules.** Designers create both in the **Ship editor**, which is the same editor for every class. Players get craft ready-made or build their own from the same parts.

| Part | What designers define |
|---|---|
| **Hull frame** | The **craft class**, model (from the Object editor or glTF), size class, base mass and armour, crew stations, a walkable interior or deck, and **slots**: named mount points, each taking a module size and kind. Crew stations depend on the class: pilot, co-pilot, gunner and engineer for spacecraft and aircraft; helm, sail trim, lookout and gun crew for sailing ships; pilot and ballast master for airships; a rower or paddler seat for small craft; driver, gunner and passenger seats for land vehicles. |
| **Module** | Kind, size and the **classes it fits**, mass, power use or output, stats (thrust, lift, sail area, fuel, shield strength, cargo volume, teleporter range), model, and the **inventory item** that represents it, so modules can be bought, looted, given as quest rewards and carried |
| **Ship design (prefab)** | A hull with a complete set of modules, a name and a price. These are the craft sold in shops and given by quests. |

**Module kinds by class**

| Class | Modules |
|---|---|
| All | Cargo, crew quarters, sensors (for discovery and scanning), **weapons**, armour plating, lights, a **hangar** for carrying smaller craft |
| Spacecraft | Engines, manoeuvring thrusters, power, fuel, shields, life support, landing gear, a **teleporter** (ship transporter) |
| Aircraft | Wings or rotors, jet or propeller engines, fuel, landing gear or floats, flaps, parachutes for the crew |
| Airships | Gas cells, ballast tanks, propeller engines, rudders and elevators, mooring gear, a gondola |
| Sailing ships | Masts, sails and rigging, rudder, keel, oars or auxiliary engines, anchors, cannons and broadside gun decks, lifeboats |
| Small craft | Oars or paddles, a small sail or motor, outriggers, wings and a small engine (gliders and ultralights) |
| Land vehicles | Wheels or tracks, suspension, engines and fuel, a harness for draft animals, turrets, winches, cargo beds, passenger seats |

#### Ship combat
**Ships carry weapons and fight in 1.0.**
- **Weapons and shields** use the Combat system's model: damage types, resistances, critical hits and status effects (for example ion damage that disables modules). Weapon modules include fixed guns, **turrets** that a gunner (player or companion) controls, and missiles with lock-on.
- **Damage to modules:** hits damage specific modules, so engines, sensors or weapons can fail during a fight. A destroyed ship becomes a wreck, as described above.
- **Enemies:** pirate ships, patrols, defence turrets on stations and bodies, and boss ships. They fly with **ship behaviours** from the AI Behaviour editor (dogfight, strafing run, flee, call reinforcements, guard a point). Spawn tables per system and difficulty, and quest templates, place them.
- **Co-op only:** like all combat, ships never damage other players' ships.
- **Networking:** the host decides every hit, with the same lag compensation as personal combat. Pilots' movement is predicted on their own machine.
- **In atmospheres and in space:** the same rules apply, and atmospheric drag affects projectiles and missiles.
- **Every class can fight:** dogfights between aircraft, **naval battles** with broadsides and chain shot between sailing ships, airship gun decks and bombing, and small craft with deck guns or boarding crews. Sailing ships take **hull and sail damage**: holes below the waterline take on water (pumps and repairs at sea), and torn sails cost speed.
- **Enemies by class:** sky pirates in aircraft and airships, corsairs and sea monsters at sea, and coastal forts, each with matching AI behaviours (broadside approach, crossing the T, ramming, boarding).
- **Boarding (1.0):** crews can board enemy craft and fight on their decks.
  - **Getting aboard:** grappling hooks and boarding planks between sailing ships, grapples between airships, **docking clamps and boarding tubes** between spacecraft (or a spacewalk in a sealed suit), and jumping across between land vehicles or small craft.
  - **The fight** happens on the craft's walkable decks and interiors, which already move in the craft's own frame of reference, using the normal combat and AI systems. Companions fight alongside.
  - **Capture:** a craft is taken when its captain is defeated or its crew **surrenders**. Surrender depends on the NPC crew's morale, set by AI behaviours. The winners can **claim it as a prize** (it becomes the boarding player's craft, with whatever damage and modules it has), **salvage** it, or **scuttle** it.
  - **Enemies board too:** pirates and corsairs can board players' craft, so crews and companions may have to repel boarders.
  - **Co-op only:** players never board each other's craft. Boarding targets NPC craft only.
  - **Quests** can use boarding, for example capturing a pirate flagship or rescuing hostages from a raider.

#### Getting a ship
Players can get ships in three ways:
- **Buy one.** Shipyards and ship dealers of the right kind (shop NPCs or machines, placed as anchors or by templates) sell ship designs and loose modules for the in-game wallet. Harbours sell sailing ships, airfields sell aircraft, and so on. Prices follow the difficulty's `price_scale`. Players can also sell craft and modules back.
- **Earn one from a quest.** A quest reward can be a ship design, a hull or a module. Hand-written quests name a specific design. Templates pick from a reward table scaled by level and difficulty. The ship is delivered to a dock or pad near the quest giver.
- **Build one from modules.** At a **shipyard**, the player chooses a hull frame and fits modules from their inventory into its slots. Changing modules on an existing craft (refitting) works the same way. Each kind of shipyard serves certain classes:

  | Facility | Builds and refits |
  |---|---|
  | **Spaceport shipyard** (stations, docks, city pads) | Spacecraft, and small craft that fit their hangars |
  | **Airfield and hangar** | Aircraft, and small air craft |
  | **Airship dock** (mooring tower and construction shed) | Airships |
  | **Harbour and dry dock** | Sailing ships |
  | **Boatyard** (on any coast or river settlement) | Small craft |
  | **Vehicle workshop** (garages in settlements and outposts) | Land vehicles |
  | **Base boatyard** (a base piece on a shoreline, see *Bases*) | Small craft only |
  | **Base garage** (a base piece, see *Bases*) | Land vehicles only |

#### Building and refitting
- **Slot-based.** Modules snap into a hull's slots and must fit the slot's size and kind. A slot-based design keeps building simple on a phone, keeps ships valid, and lets designers control what each hull can become.
- **Live checks while building**, by class:
  - **all:** the right crew stations for the class, power or propulsion that covers the load, and modules that fit the class;
  - **spacecraft:** thrust enough for their mass, and to lift off against the gravity of the world types they should land on, plus landing gear;
  - **aircraft:** enough lift for their mass at take-off speed, and a stall speed that suits runways or VTOL;
  - **airships:** enough gas-cell lift for their mass at their working altitude, and ballast to trim;
  - **sailing ships and small craft:** **buoyancy and stability**, so they float with a safe freeboard and don't capsize under their own sail or gun weight.

  The shipyard shows the resulting stats and warns about what's missing, such as "can't take off from Ice Moon: needs 20% more thrust" or "capsizes in a strong breeze: add keel weight".
- **Blueprints.** A finished build can be saved as a **blueprint** in the character's profile, so it travels with the character to other universes. Building from a blueprint still needs the modules, or the money to buy the missing ones at that shipyard. **Blueprints can be shared**: given to another player in game, exported as a file, or published on Steam Workshop. Shared blueprints are checked by the shipyard like any other.
- **Craft are built and refitted only at shipyards** of the matching kind (see the table above). A **base boatyard** counts as a boatyard for small craft, and a **base garage** as a vehicle workshop for land vehicles, so players can build canoes, rafts, rowboats, carts and rovers at home. There is no portable build pad.
- **Co-op:** the host checks every build and refit against the same rules and the player's inventory, then saves the result. Two players can't refit the same ship at once.

**Scripts** can hook into acquisition (`on_ship_acquired`, `on_ship_refit`) and grant hulls, modules or designs.

**Movement**
- **Spacecraft:** each is a Jolt rigid body driven by its thrusters. **Both flight modes are in 1.0** (decided): **flight assist** (automatic damping and speed limits, the default) and **Newtonian** flight (momentum is kept and thrusters do all the work). Players toggle between them at any time with an input action.
- In an atmosphere, the world type's air density adds drag and lift, and gravity comes from the shape mapping. Space has no gravity, and neither does the axis of a habitat.
- **Aircraft:** an aerodynamic model (lift, drag, stall, control surfaces) with an **assisted** mode (auto-trim, stall protection, the default) and a **realistic** mode. Air density from the world type and altitude limits each design's **service ceiling**, and aircraft can't leave the atmosphere.
- **Airships:** buoyancy from gas cells against the air density, so they rise until they balance and must vent gas or take on ballast to come down. They drift with the **weather's wind**, and storms make them hard to hold, so they moor at towers in bad weather.
- **Sailing ships:** a **water and wave simulation** (Gerstner-style waves driven by the wind, identical on every client) with buoyancy sample points along the hull. Sail force comes from the **wind direction and strength in the weather system**, so routes, tacking and timing matter. Storms raise the sea state. Sail trim has an **assisted** mode (automatic trim to the wind) and a **manual** mode.
- **Small craft** use the same water and air models, scaled down. Rowing and paddling are character actions that use stamina, and small craft can be carried in larger craft's **hangars** or on davits.
- **Land vehicles:** wheeled or tracked vehicle physics on the terrain's collision mesh, with grip from the surface type and the weather (rain makes mud, snow and ice reduce traction), and fuel use from the engine. Animal-drawn carts move at the animals' pace and use their stamina. Vehicles follow the local "up" on curved worlds, so a rover can drive all the way around a planet or a habitat.
- **On every world shape:** water is the sea-level shell (a height on bounded maps, a radius on spheres, a distance from the axis in cylinders), and gravity, buoyancy and wind follow the local "up". Spin gravity in habitats makes high-flying aircraft and airships lighter, and near the axis there is almost no lift or weight at all.
- **System space uses a compressed scale.** A cruise mode scales speed with the distance to the nearest body, so flying between bodies takes minutes, not hours.

**Crew**
- Several players can crew one craft, each at a station for its class (for example helm, sail trim and gun crew on a galleon). The interior is simulated in the ship's own frame of reference, so people walk around inside a moving ship normally.
- The pilot's input is predicted on their own machine. Everyone else sees the ship interpolated.

**Landing and parking**
- **Spacecraft** land on planets anywhere flat enough, or on pads, and enter habitats and hollow worlds through docks and exit sites. **Aircraft** use runways, airstrips, flat ground or water (with floats), and rotorcraft land almost anywhere. **Airships** moor at towers or anchor to the ground. **Sailing ships** anchor offshore or tie up at harbours and jetties. **Small craft** can be beached on any shore. **Land vehicles** park anywhere on the ground.
- **Only spacecraft cross the top of an atmosphere** into system space. Every other class stays on its body.
- A parked ship is saved with the body or system it is in.

**Ownership: ships belong to the character, and travel with them between universes.** A character can own several ships. Each ship records its hull, fitted modules, cargo, condition and crew permissions.
- **In a universe**, a ship exists physically wherever it is parked or flying. Its live state is in that universe's `world.db` while the character is there, and the star map shows where each ship is.
- **Leaving a universe:** the character's ships go with them. The host sends their current state back with the character, and it is stored in the character's profile.
- **Joining a universe:** each ship remembers **where it was last parked in every universe it has been in**. The host validates each ship as it does characters: the hull and modules must exist, the fit must pass the shipyard's rules, and the cargo must be valid items. Then:
  - **Rejoining a universe the ship has been in:** it is placed **back where it was parked**, in the same system, room and spot. If that spot is now blocked, it goes to the nearest free pad or dock in the same system. The character may arrive in a different system and need to teleport to their ship.
  - **First time in a universe:** the craft is delivered to the matching facility (spaceport, airfield, airship dock, harbour or boatyard) nearest to where the character arrives, on a body that has water for sailing ships and an atmosphere for aircraft and airships. That becomes its parking spot in this universe.
- **Wrecks don't travel.** A wrecked ship stays where it was destroyed, in its universe, until it is repaired (or towed and repaired). After that it travels again.

#### Mounts
**Players can ride mounts** (1.0): horses and other riding beasts, and **flying mounts**, designed in the **Mounts dock** of the AI Behaviour editor.
- **Getting a mount:** buy one at **stables**, **tame** a wild creature (a taming skill, the right food and some patience), earn one as a quest reward, or **breed** one (below).
- **Up to 21 mounts per character** (decided). A full roster must sell, release or retire a mount before taking a new one.
- **Riding:** mounts are faster than walking and can jump, swim (if the species can) and carry extra cargo in **saddlebags** (an inventory). They use their own **stamina**, and gallop until it runs out.
- **Equipment:** saddles, armour, saddlebags and harnesses are equipment items, crafted or bought, with quality tiers.
- **Mounted combat:** weapons with mounted attack animations, such as lances, swords and bows, using a rider-and-mount animation graph with IK so the rider sits correctly at every pace.
- **Draft animals:** some mounts can be harnessed to **carts and wagons** (land vehicles) and pull them.
- **Flying mounts** (decided), such as griffons, wyverns and great gliding beasts, use the **aircraft movement model** with wing-beat **stamina**: flapping climbs and costs stamina, and gliding and diving recover it. They take off from open ground and land anywhere, have a **height ceiling** from the air density, can't leave the atmosphere or fly in vacuum, and allow **aerial mounted combat**. Storms and strong wind make them harder to control, and spin gravity in habitats makes them lighter.
- **Breeding** (decided): two compatible mounts at a stable (in a town or a base) can produce a foal, hatchling or cub after a gestation period in the body's **local Calendar days**. The young mount **inherits** stats (speed, stamina, strength, carrying capacity, flight agility) and coat or plumage from both parents, with random variation from a seed, capped by the species' limits. It grows up over local days before it can be ridden. Breeding has a cooldown per parent, and the 21-mount limit counts young mounts too.
- **Care:** mounts need food and water and feel cold and heat, using the survival rules. Fed and rested mounts recover faster. A base **stable** piece shelters and feeds them.
- **Defeat:** a defeated mount is **downed** and can be revived. Otherwise it limps back to the last stable, and is never lost permanently (there is no permadeath).
- **Calling:** a whistle or call action brings a nearby mount to the player.
- **Mounts belong to the character** and are kept in their profile, so the whole roster is available in **every universe the player owns**: each mount waits at the stable it was last left at, like a parked craft.
- **Visiting another player's universe** (decided): any universe the player doesn't own, whether **another player hosts it or it runs on a dedicated server** someone else operates. There the character can bring **only one mount**, chosen on the join screen. The limit never applies in the player's **own** universes, even while friends are visiting. The rest wait at home. To swap mounts, the player leaves and rejoins with a different one.
- **On the move:** mounts **teleport** with their rider (counting towards the device's capacity), and can ride in a craft's hangar or a ship's hold.
- **Co-op:** the host simulates every mount. A mount carries its owner, and the owner's party if it has extra seats.
#### Wrecks and repair
A destroyed ship is **not lost. It becomes a wreck** that can be repaired.
- **The wreck stays where the craft was destroyed**: on a body's surface, in system space, or **on the seabed** for craft that sink. It is saved in `world.db` and is marked on the owner's star map with a **wreck beacon**. The beacon is also a teleport target, so the owner can teleport to it with any device that has the range.
- **Damage:** each fitted module survives *intact*, is *damaged* (works at reduced stats until repaired) or is *destroyed*. The chances depend on how the ship was destroyed and on the difficulty level. The hull drops to a structural minimum.
- **Cargo:** part of the cargo spills into a loot container next to the wreck.
- **Salvage is always allowed, on every wreck** (decided). Anyone can salvage intact or damaged modules and cargo from any wreck: their own, another player's, or an NPC ship destroyed in combat. Salvaging takes time with a salvage tool. The owner is notified when their wreck is salvaged, and the wreck remains repairable: missing modules simply need replacing. NPC wrecks are an important source of modules.
- **Repairing at the wreck:** a player with a **repair tool** (an item) repairs the hull and damaged modules. This costs repair materials or kits (items) and time, and higher skill makes it faster and cheaper. Destroyed modules must be replaced from the inventory. Once the ship passes the shipyard's flight checks, it can be flown away.
- **Sunken wrecks:** salvaging or repairing a wreck on the seabed means **diving** with breathing gear (the survival system's air supply). A sunken craft must be **refloated** (with lifting bags, or by a salvage tow) before it can sail again.
- **Salvage tow:** for a wallet fee, a shipyard of the matching kind can have the wreck towed or raised and brought in for repair. This is the fallback when the wreck is somewhere dangerous or the player lacks materials.
- **Crew always escape.** When a craft is destroyed, everyone aboard gets away by the class's means: **escape pods** (spacecraft), **parachutes** (aircraft and airships), **lifeboats** (sailing ships; small-craft crews simply swim) or **bailing out** (land vehicles), and reaches the nearest safe spot: a shore, a dock, or a station in system space. The difficulty's death rules don't apply to crew who escape this way, and the players can teleport or travel back to the wreck to repair it.
- **Wrecks stay until they are repaired.** They never decay or despawn, however long they are left.
- **Difficulty** sets the damage chances, repair cost and tow fee. Every wreck can be repaired.
- **Scripts:** `on_ship_wrecked` and `on_ship_repaired`. Quest templates can use wrecks, for example "recover the wreck of the *Sundrake*".

**Scripts** use `midgard.ships` for spawning, ownership, damage and docking events.

**Where craft can go:** spacecraft stay in their **system**, and every other class stays on its **body**. There are no jump drives or ship transfers, so people cross between bodies and systems by teleport. A smaller craft, such as a rover, a small aircraft or a boat, can travel to another body **inside a larger spacecraft's hangar**, if its size class fits. Because rejoining a universe puts each craft back where it was parked, leaving and rejoining can't be used to move a craft to another system.

```sql
-- project DB
CREATE TABLE world_type  (id TEXT, version INTEGER, name TEXT,
  shape TEXT CHECK (shape IN ('bounded','cylinder','sphere_outside','sphere_inside')),
  size JSON, graph JSON, atmosphere JSON, lighting JSON, gravity JSON, edges JSON, exits JSON,
  PRIMARY KEY (id, version));
CREATE TABLE star_field  (id TEXT, version INTEGER, name TEXT,
  systems INTEGER CHECK (systems = 9), layout JSON,
  star_classes JSON, body_mix JSON, start_rules JSON, PRIMARY KEY (id, version));
CREATE TABLE name_list   (id TEXT PRIMARY KEY, kind TEXT, parts JSON);        -- syllables or words
CREATE TABLE teleporter  (id TEXT PRIMARY KEY, name TEXT,
  kind  TEXT CHECK (kind  IN ('machine','ship_module','artifact')),
  range TEXT CHECK (range IN ('body','system','universe')),
  targets JSON, capacity INTEGER, cost JSON, requirements JSON, cooldown_s REAL,
  recall INTEGER, effects JSON, script TEXT);
CREATE TABLE ship_hull   (id TEXT PRIMARY KEY, name TEXT,
  class TEXT CHECK (class IN ('spacecraft','aircraft','airship','sailing','small_craft','land_vehicle')),
  model TEXT, size_class TEXT, mass REAL,
  armour REAL, slots JSON, crew_stations JSON, interior JSON, flight JSON, data JSON);
CREATE TABLE ship_module (id TEXT PRIMARY KEY, name TEXT, kind TEXT, classes JSON, size INTEGER, mass REAL,
  power REAL, stats JSON, model TEXT, item TEXT REFERENCES item(id));
CREATE TABLE ship_design (id TEXT PRIMARY KEY, name TEXT, hull TEXT REFERENCES ship_hull(id),
  fit JSON, price JSON);                                                       -- slot → module
-- world.db
CREATE TABLE body_state  (body TEXT PRIMARY KEY, discovered INTEGER, visited INTEGER, discovered_by TEXT,
  name TEXT, description TEXT, renamed_by TEXT);                               -- NULL = generated name
CREATE TABLE ship_here   (ship TEXT PRIMARY KEY, owner TEXT, room TEXT, position JSON,
  wrecked INTEGER, live_state JSON);                                           -- ships present in this universe
-- character profile (travels between universes)
CREATE TABLE ship        (id TEXT PRIMARY KEY, character TEXT, hull TEXT, fit JSON, name TEXT,
  cargo JSON, condition JSON, crew_permissions JSON,
  parked JSON, version INTEGER);                                              -- universe ID → system, room, position
CREATE TABLE companion   (id TEXT PRIMARY KEY, character TEXT, template TEXT, name TEXT, level INTEGER,
  xp INTEGER, skills JSON, equipment JSON, inventory JSON, wage JSON, state JSON, version INTEGER);
CREATE TABLE blueprint   (id TEXT PRIMARY KEY, character TEXT, name TEXT, hull TEXT, fit JSON);
CREATE TABLE mount       (id TEXT PRIMARY KEY, character TEXT, species TEXT, name TEXT, level INTEGER,
  stats JSON, genes JSON, parents JSON, born INTEGER, equipment JSON, saddlebags JSON,
  stabled JSON, state JSON, version INTEGER);                                 -- stabled: universe ID → stable location
```

### Difficulty levels
Designers define any number of difficulty levels in a **Difficulty** editor. Each is a named set of modifiers that game systems read:

```sql
CREATE TABLE difficulty (
  id TEXT PRIMARY KEY, seq INTEGER, name TEXT, description TEXT,
  enemy_health REAL, enemy_damage REAL, enemy_density REAL,   -- combat and generation
  xp_rate REAL, loot_rate REAL, resource_abundance REAL,       -- progression and generation
  price_scale REAL, bank_interest_scale REAL,                  -- economy
  raid_scale REAL,                                             -- base raid frequency and strength (0 = no raids)
  survival_scale REAL,                                         -- strength of cold/heat/wet effects (0 = cosmetic only)
  hunger_scale REAL, thirst_scale REAL,                        -- meter drain and effect strength (0 = off)
  food_effect_scale REAL, air_scale REAL,                      -- food/drink relief strength; suit air use and suffocation speed
  death_penalty TEXT,                                          -- none | drop_wallet | drop_inventory (no permadeath)
  data JSON);                                                  -- game-specific extras for scripts
```

- **A universe's difficulty never changes after creation.** Some modifiers shape generation (enemy density, resource abundance), and the rest (combat and economy) stay fixed too, so a universe always plays the way it was created. Combat and economy modifiers are applied at run time.
- **There is no permadeath.** Death penalties range from none to dropping the wallet or inventory, which can be recovered from where the character fell.
- Scripts read the current level with `midgard.world.difficulty()` (for example, `.enemy_damage` or a key from `.data`).
- The difficulty is shown in the world list, the join list and on the join-code preview.

### Quests in generated worlds

Generated worlds use **both hand-written quests and quest templates**. Both run on the same quest system (goals, prerequisites, rewards and calendar conditions from §5 and §6).

- **Hand-written quests need anchors.** A hand-written quest refers to **anchors**: named places and NPCs such as `site_hollowmere` or `npc_village_elder`. Designers give each anchor placement rules: which world types can host it, biome, distance from the start, distance from other anchors, and system rules such as "in the starting system". **Every anchor is placed in every world:** the star field generator chooses a host body of an allowed world type, and that body's generator places the anchor when the body is first visited. The cook step checks that every star field recipe can satisfy every anchor a quest uses. At run time the quest finds its anchors wherever they were placed, and the star map can mark the bodies a quest leads to.
- **Templates fill slots from the world.** A quest template has slots, such as *a site of kind `wolf_den` within 2 km of the giver*, *an NPC with role `merchant`*, *an item from loot table `herbs`*, *a body of type `hollow_world` in another system*, or *a count between 5 and 12, scaled by difficulty*. The world's **quest generator** fills them from what is actually in the world, using a random seed based on the world seed and the time, so results can be reproduced. Rewards scale with difficulty and character level.
- **How often templates appear** is set per template: a bounty board that refreshes on a weekly calendar event, a once-per-site quest, a chain of up to N steps, or a limit per region.
- **Where they are stored:** hand-written quests, anchors and templates live in the project database. Instances created in a world, with their filled-in slots, live in `world.db`.

```sql
-- project DB
CREATE TABLE world_anchor   (id TEXT PRIMARY KEY, name TEXT, kind TEXT, rules JSON);   -- placement rules
CREATE TABLE quest_template (id TEXT PRIMARY KEY, name TEXT, text TEXT, slots JSON, goals JSON,
                             rewards JSON, frequency JSON, data JSON);
-- world.db
CREATE TABLE quest_instance (id TEXT PRIMARY KEY, template TEXT, filled JSON, state TEXT,
                             created INTEGER, expires INTEGER);
```

In the **Quest editor**, a quest is either *hand-written* or a *template*. Templates have a slot editor and a **Preview in seed…** button that generates a world and lists the quest instances it would produce. Text uses slot placeholders such as "Clear the {site} near {giver.home}".

### Banks
**Banks and bank accounts belong to each universe, while the wallet and inventory travel with the character.** Every universe has **several banks**, each offering a different balance of interest and risk.

| Bank style (examples) | Interest | Risk | Notes |
|---|---|---|---|
| **Vault** | None (or a storage fee) | None | Safe storage. Withdraw any time. |
| **Savings bank** | Low | Very low | Insured: losses are covered up to a limit. |
| **Merchant bank** | Medium | Medium | Can freeze withdrawals briefly during "bad months". |
| **Frontier or venture bank** | High | High | Can lose part of its deposits in a crisis, or collapse and repay only a share. |

- **Banks are designed in the Banking editor:** name, interest rate (per Calendar month), fees, insurance limit, minimum deposit, withdrawal rules (instant, notice period, locked term), and a **risk profile** (chance and size of loss events, freezes and collapse, and how much is recovered).
- **Where banks are:** hand-written banks are anchors (a guild bank in the starting city). Generated banks come from bank templates that the world generator places in settlements and stations, so different bodies and systems have different banks.
- **Risk is resolved by the Calendar.** On each month's bank event the host rolls each bank's risk, using a random seed from the universe seed and the date, so results are reproducible. Players see each bank's **risk rating** (A to E) and a short history of past events, and banks may hint at trouble beforehand ("rumours of bad loans").
- **The difficulty's `bank_interest_scale`** applies to all interest. Every transaction goes in an append-only ledger in `world.db`, recording which character made it.
- **Shared accounts:** in co-op, players can open **joint accounts** (for example a guild account) with any characters that have played in that universe. The character who opens the account manages it: they add and remove members, and can set a **withdrawal limit** per member per Calendar week. Every member can deposit and see the ledger. Closing the account pays out the balance to its manager, or splits it as the manager chooses. A joint account stays in the universe even when its members are offline.
- **Losses can be won back through a quest.** When a bank suffers a loss event or collapses, it starts a **recovery quest** from a quest template linked to its risk profile, for example tracking down an embezzler, retaking a vault from pirates, or recovering a lost cargo ship. Every character with money in that bank, including joint-account members, can take part. Success returns a designer-set share of the lost money (up to 100%) to each affected account, recorded in the ledger as a *recovery*. A collapsed bank can reopen afterwards if its template allows it. Other scripts and quests can also react to bank events.

```sql
-- project DB
CREATE TABLE bank_template (id TEXT PRIMARY KEY, name TEXT, style TEXT, interest_month REAL, fees JSON,
  insurance_limit INTEGER, min_deposit INTEGER, withdrawal JSON, risk JSON, placement JSON,
  recovery_quests JSON);                                       -- quest templates and recovery shares per event kind
-- world.db
CREATE TABLE bank         (id TEXT PRIMARY KEY, template TEXT, name TEXT, body TEXT, site TEXT,
  state TEXT, history JSON);                                   -- state: open | frozen | collapsed
CREATE TABLE bank_account (id TEXT PRIMARY KEY, bank TEXT, manager TEXT, joint INTEGER, currency TEXT,
  balance INTEGER);                                            -- manager = character ID
CREATE TABLE bank_member  (account TEXT, character TEXT, weekly_limit INTEGER, PRIMARY KEY (account, character));
CREATE TABLE bank_recovery (bank TEXT, event_time INTEGER, quest_instance TEXT, lost INTEGER,
  share REAL, state TEXT);                                     -- open | recovered | failed
CREATE TABLE bank_ledger  (id INTEGER PRIMARY KEY, account TEXT, game_time INTEGER, kind TEXT,
  amount INTEGER, note TEXT);                                  -- deposit | withdrawal | interest | fee | loss | recovery
```
### Bases
**Players build bases in 1.0**, in both world modes, on any body's surface (outside and inside spheres, inside cylinders) and on bounded surface maps.
- **Building pieces** snap together: foundations, floors, walls, roofs, doors, stairs, windows and ramps. Pieces align to the local "up", so bases sit correctly on curved worlds. Foundations flatten the terrain beneath them.
- **Functional pieces:** storage containers, crafting stations, power generators, lights, beds (respawn points), **teleport pads**, **landing pads** (ship parking), a **boatyard** on a shoreline (builds and refits small craft), a **garage** (builds and refits land vehicles), **stables** for mounts (shelter, feeding and breeding), defences (turrets, gates), and quarters where companions wait.
- **Costs:** pieces are built from inventory items and materials, and some also cost currency. Pieces can be dismantled for part of their cost back.
- **Rules:** a piece needs a supporting foundation (a simple support check, not a full physics simulation). Players can't build in settlements, in interference zones, or in **no-build zones** that designers mark around anchors and quest sites. Each base has a **piece budget**, so 16-player sessions stay within the Raspberry Pi budget.
- **Ownership and permissions:** a base belongs to the character who placed its first foundation. The owner sets who may build, use storage, and use teleport pads: only themselves, their party, or everyone. Bases belong to the universe and stay there, while the character travels.
- **Saving:** bases are stored as changes in the per-body file (`bodies/{body}.db`), or in `world.db` on a bounded surface map.
- **Base editor (in the Midgard editor):** the piece catalogue (model, snap points, cost, health, function), build rules, piece budgets, no-build zones, prefab base layouts for NPC outposts, and **raid settings** (attention weights, warning time, wave composition, raider ship types).
- **Co-op:** the host checks every placement against the rules and the player's inventory.
- **Raids (1.0):** NPC enemies attack bases, which is what gives defences a purpose.
  - **What triggers a raid:** a Calendar schedule plus the base's **attention**, which rises with its size, the value stored in it, and noisy activity such as mining and crafting. The difficulty's `raid_scale` sets how often and how strong raids are (0 turns raids off).
  - **Warning:** players get a warning beforehand, such as scouts spotted, a radio message or a sensor contact, so they can get home in time to defend.
  - **Attackers:** waves of ground enemies, and **raider ships** at landing pads and in orbit. They use raid behaviours from the AI Behaviour editor: siege, breach walls and gates, target generators and storage, retreat.
  - **Damage:** every piece has health. Destroyed pieces drop part of their materials. If raiders break into storage, they carry off part of its contents, which players can win back by catching the raiders as they retreat.
  - **Defending:** turrets (which need power), walls and gates, traps, companions on guard, and the players themselves. Defeated raiders drop loot, and raids can lead into quest templates, such as tracking the raiders back to their camp.
  - **Repairs** use materials from storage, and a repair tool fixes damaged pieces.
  - **When raids happen:** only while at least one character with build rights on that base is in the universe, so bases aren't destroyed while everyone is offline. **There are no offline raids**, on dedicated servers as well (decided).
### Characters travel between worlds
A character is stored in the **player's profile**, not in a world:

```
profiles/{profile}/characters.db     all of this player's characters (SQLite), with backups
```

| Travels with the character | Stays in each world |
|---|---|
| Appearance, level and XP, skill-tree choices, progression unlocks, inventory, **wallet**, **companions** (up to 2), **mounts**, **ships and other craft** (except wrecks), ship blueprints | Position, quest progress, **bank accounts** (banks are places in a world), reputation, owned buildings, wrecks |

**Joining a world with a character:**
1. The client sends the character to the host, with their companions, mounts and craft.
2. The host **validates it against the game's data**. XP must fit the progression curve, skill points spent can't exceed those earned at that level, items and currencies must exist, and stacks can't exceed their limits. Derived stats (health, damage) are **recalculated on the host** from base choices and are never taken from the client.
3. The host validates the **companions** the same way (level against XP, skills, equipment) and the **ships** (hull, modules, fit, cargo). Companions arrive beside the character. Ships return to where they were parked in this universe, or, on a first visit, are delivered to a dock in the arrival system.
4. The host applies its world rules: whether outside characters are allowed, and the level range.
5. While the character is in the world, **the host owns it**, and its companions and ships. Autosaves and leaving the world send everything back to the client, which stores it with a version number and keeps a backup of the previous version.

**Rules:**
- **A character can be in one world at a time.** The profile marks it as *in use* while a session is running. If the game crashes, the last version the host confirmed is kept.
- **A character can't be edited in another world's favour.** Characters on the player's own machine could be edited by hand, but the host's validation catches anything the game rules don't allow. Real cheat-proofing isn't a goal for co-op among friends.
- On a **dedicated server**, characters work the same way. Clients bring their characters and the server returns them.

### Syncing between devices
**A player's characters and worlds follow them between their devices**, for example PC and phone.
- **What syncs:** the whole profile (characters, companions, ships, blueprints, input bindings, settings and language choice) and the **universes the player owns**: `world.json`, `world.db`, per-body files and save slots. Universes on dedicated servers don't need syncing, because they live on the server.
- **Midgard Sync:** a service that runs next to the join-code service, on the same private server or hosting provider, with Docker images for studios who self-host. It is needed because Steam Cloud can't reach Android.
- **Signing in:** there are three ways, and none of them uses a password:
  - **Pairing code:** one device shows a short code, and the other enters it.
  - **Email:** the player enters an address and types the one-time code sent to it. The service needs an outgoing mail provider for this.
  - **Steam:** in Steam builds, a Steam session ticket checked by the service with Valve's Web API. On Android and other non-Steam builds, Steam sign-in in the browser (Steam OpenID).

  Google sign-in is **not** offered. A player can link email and Steam to the same sync profile.
- **Recovery:** signing in with a linked email or Steam account on a new device restores everything, even if every old device is lost. Pairing-only profiles can't be recovered that way.
- **Encryption:** pairing-only profiles are **encrypted on the device**, so the server can't read them. Profiles linked to email or Steam are **encrypted at rest on the server with keys the service holds**, which is what makes recovery possible (decided). The keys are stored apart from the data, the service's privacy note says the operator can technically access synced data, and access is limited to recovery and erasure requests.
- **Upgrading a profile:** when a pairing-only player links email or Steam, the game asks them to confirm the switch to server-held keys, then re-uploads the profile with them.
- **Privacy:** an email address is personal data. It is used only to send sign-in codes, and is deleted along with everything else when the player asks for erasure (GDPR). Players can unlink devices and accounts at any time.
- **When it syncs:** at game start and exit, and after each autosave, in the background. Files are versioned, and only changed files are uploaded.
- **No double play:** the *in use* lock on characters is held by the sync service while the player is online, so a character can't be played on two devices at once.
- **Conflicts:** if two devices changed the same universe while offline, the newer version wins. The other is kept as a **conflict copy** in the world list, so nothing is lost.
- **Steam builds** use Steam Cloud between Steam PCs, and Midgard Sync when the player also plays on a phone.
### Player-facing screens
The game's Menu Editor gets ready-made templates for these screens:
- **World list:** each world's name, description, difficulty, last played, play time and **size on disk**. Actions: *Play*, *Host*, *Edit name/description*, *Duplicate*, *Restore backup*, *Delete* (with confirmation). **There is no limit on the number of worlds.** Players decide what to keep and delete the rest.
- **New World:** name, description, difficulty (with its description), optional seed (blank = random). In system-map games, star field options. In bounded-surface-map games, the **number of continents (1–3), archipelagos and islands**. The world is generated on the player's device when they press *Create*.
- **Star map / teleport destination:** the 9 systems and their discovered bodies. When opened from a teleporter, it is filtered to what that device can reach and shows its cost and requirements; *Teleport* fires the teleport event.
- **Ship screens:** ship status (fuel, power, shields, cargo), crew permissions and the docking/landing prompt.
- **Shipyard:** buy ship designs and modules; build or refit by fitting modules into a hull's slots, with live stats and warnings; save and load blueprints; sell ships and modules; order a salvage tow for a wreck.
- **Wreck repair:** shown when using a repair tool at a wreck. It lists the hull and each module (intact, damaged, destroyed), the materials needed, and progress.
- **Loading / room change:** a fade or loading screen with the destination's name and a preview image.
- **Character select:** characters with level and where each was last played. Actions: *Create*, *Delete* (with confirmation; the character's companions, ships and blueprints are deleted with it). **There is no limit on the number of characters.**
- **Join list:** LAN and join-code worlds, showing name, description, difficulty, players and the host's level range.

### Dedicated servers
`server.json` names an existing world by ID or describes one to create on first start:

```json
"world": { "name": "Northreach", "description": "Our winter world.", "difficulty": "hard", "seed": null }
```

The world's owner and the server admins can change the description at any time, in game or with the admin command `world describe "…"`. A world can be copied between a player's machine and a dedicated server by copying its folder.

### Editor
- **World Type** and **Star Field** editors (each with a seed preview), the **Teleport** and **Ship** editors (a ship can be test-flown in the viewport), and the **Difficulty** editor (table + live preview of modifiers on sample enemies and prices).
- **Play ▸ Play in World…** chooses a seed and difficulty, optionally a starting body, or reuses a saved test world, so designers can reproduce a bug in a specific world.
- Cook-time checks: at least one difficulty level, every recipe version still present, every anchor satisfiable, every world type that has bodies to leave also has exits, modifiers within sensible ranges.

---

## 8. Action RPG systems

**Midgard is built for 3D, RPG-style action games**: real-time movement and combat, characters that grow through skills and progression, quests and conversations. Each system below has its own **editor subsystem** (a set of docks) and a runtime part in the engine, and all of them are in version 1.0.

### Cameras
- **Three views, switchable by the player:** **first person**, **third person** (orbit or over-the-shoulder) and **top-down** (fixed-angle isometric with zoom). Every game has all three, and players switch between them at any time with an input action (default: `V`, or the mouse wheel between first and third person). Designers can only take over the camera briefly, for scripted moments such as conversations and cutscenes, and the player's chosen view comes back afterwards.
- **Camera editor:** settings per view: field of view, distance, height and shoulder offset, pitch limits, collision radius (the camera never clips through walls), smoothing, zoom range, and blend time between views. Previews run live in the viewport. The player's last view is saved in their profile.
- **Works on every world shape.** The camera's "up" comes from the shape mapping, so third-person and top-down views stay level when walking around a sphere or inside a cylinder.
- **Action support:** lock-on targeting, camera shake, and short scripted camera moves for dialogue and cutscenes.
- **Ships:** cockpit (first person), chase (third person) and a tactical overhead view, using the same camera system.

### Character animation (Animation editor)
- **Import:** skinned characters and animation clips from glTF. A standard **humanoid rig** lets clips be retargeted between characters with different proportions.
- **Animation graph:** a node-graph editor (reusing the shared node-graph widget) with:
  - **state machines** (idle, locomotion, jump, attack, hit, death, swim, climb) with conditions and blend times;
  - **blend spaces** for locomotion by speed and direction;
  - **layers and masks**, such as upper-body attacks while running;
  - optional **root motion** for attacks and dodges.
- **IK:** foot placement on uneven terrain and slopes (against the local "up" on curved worlds), look-at for heads, and hand IK for weapons and ladders.
- **Animation events:** clips mark frames that fire footsteps, hit windows (when an attack can deal damage), visual effects, sounds and script calls.
- **First-person arms** are a separate rig and graph, kept in sync with the full body that other players see.
- **Runtime:** ozz-animation (MIT licence) for sampling and blending, with skinning on the GPU. In co-op, only the graph's parameters are replicated, not the poses, so animation costs very little bandwidth.

### Combat (Combat editor)
- **Weapons and abilities** are data: damage, damage type, range, cost (stamina, mana, energy, ammo), cooldown, and the **animation** that performs them. Melee hits come from the animation's hit windows. Ranged and spell attacks spawn **projectiles** (speed, gravity along the local "up", homing, area effect).
- **Hit boxes and hurt boxes** are attached to skeleton bones in the Combat editor and previewed on the animated character, frame by frame.
- **Damage model:** damage types (physical, fire, frost, energy…), resistances and armour, critical hits, and **status effects** (burning, slowed, stunned, poisoned) with durations and stacking rules. All values can be modified by skill-tree modifiers and difficulty.
- **Action moves:** dodge with invulnerability frames, block and parry with timing windows, combos as chains of attack states in the animation graph, and lock-on targeting through the camera.
- **Co-op:** the host decides every hit. Clients play their own attack animations immediately (prediction), and the host rewinds hit boxes to the attacker's view time (lag compensation) so hits feel fair at 150 ms. **Combat is co-op only:** players can never damage each other. Area attacks, projectiles and status effects pass through allies, and there is no PvP mode.
- **Combat editor docks:** weapon and ability tables, damage types and status effects, a hit box editor on the skeleton, and a **combat sandbox** in the viewport that spawns enemies at any difficulty and shows damage numbers and timelines.
- **Ships** use the same weapons, damage and status model for ship weapons and shields. Ship combat is in 1.0 (§7, *Ship combat*).

### AI behaviour (AI Behaviour editor)
- **Behaviour trees** built in a node-graph editor: sequences, selectors, conditions and actions (patrol, chase, attack, flee, use ability, call for help, return home), plus **utility scoring** to choose between options, such as heal or attack.
- **Perception:** sight cones, hearing radius (from audio events such as footsteps and combat) and memory of the last known position. All of it works with the local "up" on curved worlds.
- **Navigation:** the per-chunk navigation meshes from §7, with jump, climb and swim links. Flying enemies use 3D steering.
- **Groups and factions:** packs with leaders and flanking roles, factions with hostility tied to the player's reputation, and NPCs that call for help.
- **Daily life:** NPC **schedules** tied to the Calendar (work, eat, sleep, open the shop at dawn), so settlements feel alive by day and night.
- **Generated content:** enemy spawn tables per biome, world type and difficulty, used by the world generator and quest templates.
- **Debugging:** behaviour trees light up live in the editor while the game runs (through the live link), and the viewport can show perception cones, paths and the current target. AI runs on the host only.
### Companions
Players can recruit **NPC companions** who follow them, fight beside them and help them. Companions use the AI Behaviour system, and designers set them up in the AI Behaviour editor's **Companions** dock.
- **Recruiting:** through dialogue (a dialogue action *recruit*), as a quest reward, or by **hiring** for a wage from the wallet, paid on a Calendar schedule. Hand-written companions are anchor NPCs with their own story. Generated companions come from role templates (mercenary, scout, medic, engineer).
- **Commands** through a quick menu or input actions: *follow*, *wait here*, *attack my target*, *defend me*, *hold back*, *use ability*. Each command is a branch of the companion's behaviour tree. **Companions don't follow orders blindly.** They weigh each order against their approval of the player and their own safety (see *Approval* below).
- **Growth:** companions have equipment slots, an inventory, levels and their own skill tree, using the same data as player characters.
- **Getting hurt:** a defeated companion is **downed** and can be revived by a player during a short window. After that, the companion retreats and waits at the last safe spot, where its leader can collect it. Companions are never lost permanently through death.
- **Travel:** companions go wherever their leader goes. They walk with them, **teleport** with them (each companion counts towards the device's capacity), crew **ships**, where they can take a gunner or engineer station, and **travel between universes** with their character. They are stored in the character's profile, like ships, and validated by each host.
- **Talking:** companions have voiced dialogue trees and short comments on places, quests and combat, triggered by conditions and limited by cooldowns.
- **Limit: 2 companions per player.** In a full 16-player session there can be up to 32 companions, and the Raspberry Pi 5 budget is measured with that load.
- **Co-op:** the host simulates every companion. A companion follows the player who recruited it, and only that player gives it commands.
#### Approval
Each companion has an **approval** of the player who recruited them, from −100 to +100, shaped by the companion's **personality**.
- **Personality traits** are values such as mercy, honour, greed, curiosity, lawfulness and loyalty to friends. Hand-written companions have designed traits. Generated companions roll traits from their role template (a mercenary leans towards greed, a medic towards mercy).
- **What changes approval:**
  - **Approval tags** that designers put on dialogue responses, quest outcomes and actions, such as *spared an enemy*, *stole*, *harmed a non-hostile*, *kept a promise* or *chose profit over people*.
  - **How the companion is treated:** wages paid or missed, being healed or left downed, being ordered into hopeless fights.
  - Each companion's traits turn every tag into a gain or loss. The same choice can please one companion and anger another. Companions react with short voiced comments.
- **Effects by approval level:**

  | Approval | Behaviour |
  |---|---|
  | **Devoted** (high) | Unlocks their personal quest and extra dialogue, a loyalty branch in their skill tree, and a morale bonus in combat. Will defend the player without being told. |
  | **Friendly** | Follows orders readily. |
  | **Neutral** | Follows orders, but questions risky ones in comments. |
  | **Unhappy** | **May refuse** dangerous or distasteful orders and says why ("I'm not charging into that, not at this health"). Asks for a higher wage. Slower to respond. |
  | **Leaving** (low) | After a warning conversation, the companion **leaves** and returns to where they were recruited. They can be won back only through dialogue there, if approval has recovered. |

- **Deciding on orders:** the behaviour tree's utility scoring combines the order with approval, the companion's health and the odds of the fight. A devoted companion follows almost any order. An unhappy one picks the safer option and explains.
- **Co-op:** approval is towards the companion's owner. The companion also reacts, more weakly, to what other players in the group do in front of them.
- **Travels with the companion.** Approval and traits are saved in the companion's record in the character's profile, so they carry over between universes.
- **Editor:**
  - the Companions dock has traits, the tag-to-approval table, level thresholds and refusal rules;
  - a **simulator** shows how each companion's approval would change for a list of player actions;
  - the Dialogue and Quest editors gain an **approval tags** field on responses and outcomes.
- **Scripts:** `midgard.companions.approval(companion)`, `on_approval_changed` and `on_order_refused`.
### Gathering resources (Resources editor)
- **Resource nodes** are placed by the world generator from the seed: ore veins, trees, plants, crystals, sand and clay on surfaces, and **asteroid fields and gas clouds** in system space. Each world type and biome has its own resource table, scaled by the difficulty's `resource_abundance`.
- **Tools:** each node needs a tool kind and tier, such as a pickaxe, laser drill, axe or harvester. Ships can fit a **mining laser** module to mine asteroids and large surface deposits. Gathering takes time, and gathering skills raise the yield and speed.
- **Depletion and regrowth:** a gathered node is recorded as a change in the body's file. Plants and trees **regrow** over Calendar time, and ore veins and asteroids **slowly respawn** nearby, so even a bounded surface map never runs out.
- **Scanning** with ship sensors or handheld scanners reveals deposits, which ties gathering into discovery.
- **Co-op:** nodes are shared. The host decides each yield, and everyone in the universe sees the same depletion.
- **Resources editor:** resource items, node types (model, tool, yield, time, regrowth), distribution rules per world type, biome and depth, and a **heat-map preview** of a seed's resource spread.

### Crafting (Crafting editor)
- **Recipes:** input items and amounts, outputs, the **station** needed (workbench, forge, refinery, chemistry bench, module fabricator), crafting time, power use, and the skill or unlock needed. Recipes are unlocked by skills, quests or recipe items found in the world.
- **Production chains:** ore → ingots → components → equipment, weapons, base pieces and **ship modules**. Players can make the modules they fit at shipyards, which links crafting, ships and the economy.
- **Quality:** crafted items have quality tiers (common to masterwork), which affect their stats. Quality depends on the crafter's skill and the quality of the materials.
- **Queues:** stations in a base run a crafting queue while they have power and their owner's party is in the universe.
- **Co-op:** stations follow base permissions, and the host checks every recipe and inventory change.
- **Crafting editor:** a recipe table, a **production-chain graph** (built on the shared node-graph widget) that shows what feeds what, and a **balance checker** that compares crafting cost with shop prices and loot value and flags recipes that can be crafted and sold at a profit. The AI assistant can draft recipe sets for review.
### Audio (Audio editor)
- **Sound events:** one event, such as `footstep_grass`, plays one of several variants with random pitch and volume, so repeated sounds don't feel mechanical.
- **3D sound:** positioning, attenuation curves, occlusion behind walls, and **reverb zones** for caves, interiors and ship decks.
- **Mixer buses:** master, music, effects, voice, ambience and UI, each with its own volume in the player's settings.
- **Music system:** playlists by world type, biome, combat state and time of day, with crossfades and short **stingers** for events such as levelling up or completing a quest.
- **Ambience:** layered beds by biome and solar phase (dawn chorus, night insects), and the rumble of habitats and engines.
- **Runtime:** miniaudio (public domain / MIT-0) on every target, including Android. Audio is cooked to Ogg Vorbis or Opus.

### Dialogue and NPC conversation (Dialogue editor)
- **Dialogue trees:** a node graph of NPC lines and **player responses**. The conversation branches on the response chosen.
- **Conditions** on any line or response: quest state, skills, items, wallet balance, reputation, calendar time, difficulty, previous answers.
- **Actions** from any node: start or advance a quest, give or take items and currency, change reputation, open a shop or shipyard, use a teleporter, or run a script.
- **Hand-written and generated NPCs:** hand-written dialogue belongs to anchor NPCs. **Role dialogue** (merchant, guard, quest giver, shipwright) is written once with slots that the world generator fills, such as "Welcome to {settlement}. The {site} to the north has been trouble lately." This matches the quest templates.
- **Recorded voice for every line, with optional text.** Each line has a recorded English voice clip. **Subtitles** (the line's text, with the speaker's name) appear when the player turns them on in settings, with size and background options. Response choices are always shown as text.
- **Voice production:** the Dialogue editor lists every line with its recording status (missing, recorded, approved) and exports a **recording script** (line ID, character, context, text) for voice actors. Imported clips are matched by line ID.
- **Lip sync:** when a clip is imported, the editor generates a **mouth-shape track** from the audio (for example with Rhubarb Lip Sync, MIT licence). TTS slot words get mouth shapes from the engine's phoneme timings. At run time the track drives the character's facial blend shapes.
- **Generated slots use text-to-speech.** A role line with slots ("Welcome to {settlement}") is recorded with the slots left as gaps. At run time, the slot words (place names, NPC names, counts) are spoken by an on-device **text-to-speech** engine in a voice matched to the NPC's voice profile, and placed into the gaps. A designer can mark a line *TTS whole line* if splicing sounds wrong for it.
- **TTS engine:** a local neural engine such as **Piper**, which runs offline on Windows, Linux, Android and Raspberry Pi. Check the licence of the exact version and of each voice model: the original Piper is MIT, but newer forks are GPL. Generated names give the engine pronunciation hints from the name generator's syllables. Slot audio is generated in the background when a body is first visited, cached in the universe folder, and generated on each client, so nothing extra is sent over the network. **Stock TTS voices** are used for now. Custom voices trained to match the recorded actors may be added later.
- **Voice profiles:** the Dialogue editor gives each NPC voice (actor, gender, age, accent) a matching TTS voice and pitch/speed settings, with a **preview** button that plays a slotted line with sample words.
- **Presentation:** a Menu Editor template for the conversation window, conversation camera framing and talk animations.
- **Group conversations in co-op:** when a player starts a conversation, nearby players get a prompt to **join** or **watch**. Everyone in the conversation hears the voice, sees the same lines, and sees the same subtitles if they have them on. Joined players can propose a response, which the others see highlighted, and **the player who started the conversation chooses**. Watchers only listen. The host checks every action. Quest progress applies to all joined players, and items and money go to the chooser unless the node says otherwise.
- **AI assistant:** can draft dialogue trees and alternative lines for designers to review (§9).

### Input mapping (Input editor)
- **Actions, not keys.** Designers define named actions (`move`, `jump`, `attack`, `interact`, `inventory`, `throttle`…) grouped into **contexts**: on foot, in a ship, in menus, in dialogue. Scripts and systems read actions and never read raw keys.
- **Default bindings** for each action on **keyboard and mouse**, **gamepad** (SDL3's gamepad support covers Xbox, PlayStation and Switch layouts, with matching button icons) and **touch**.
- **Touch layout editor:** designers place on-screen sticks, buttons and gesture areas for each context. Players can move and resize them.
- **Players can rebind everything** in a rebinding screen (a Menu Editor template), with conflict warnings, reset to defaults, sensitivity, invert axes, dead zones, and hold-or-toggle options. Bindings are saved in the player's profile, so they apply in every universe.
- **Button prompts** follow the last device used: a keyboard key, a gamepad button icon, or a touch hint.

### Localisation (Localisation editor)
- **English is the original language.** All player-facing text uses string keys from the start: items, quests, dialogue, menus, the Calendar's month and weekday names, difficulty names. The cook step fails on hard-coded player-facing strings.
- **Editor table:** keys × languages, with status (missing, needs review, done) and a **pseudo-localisation** preview that lengthens and accents text to show layout problems early.
- **Translation tables for users:** every exported game includes a **translation kit**: the English string table as CSV and PO files, with context notes for each key. Anyone can translate it into their own language and install it as a **language pack** (the translated file plus an optional font) in the game's `languages/` folder in the user data directory. The game lists installed packs in its language settings. Missing keys fall back to English, and the game reports keys a pack is missing or no longer needs after a game update. Game makers can also bundle finished packs in the export. **Players can do this themselves:** the game's language settings have an **Export strings** button that writes the translation kit to the user data folder. The player translates it, and it appears in the language list next time the game starts.
- **Plurals, gender and placeholders** use ICU MessageFormat, so slot text such as "{count} wolves" or "{site}" translates correctly.
- **Fonts:** fallback fonts per language (for example CJK). **Right-to-left languages (such as Arabic and Hebrew) are not supported.** The language pack loader warns when a pack's text is mostly right-to-left script.
- **Generated names** (systems, bodies, NPCs) come from the name generators and are not translated. **Voice stays in English.** Other languages are offered through subtitles.
- **The AI assistant** can draft translations, which are marked *needs review*.
- **Players** choose their language in settings, from English plus any installed or bundled language packs.

### Save and load UI (Save editor)
- **What is saved** is already defined: the universe folder (`world.json`, `world.db`, per-body files) and the character profile. The Save system adds the player-facing part.
- **Save slots:** in single-player, each universe has an **autosave** chain and **manual save slots** (snapshots of the universe folder).
- **Co-op on a listen server** (a player hosting): **autosave only**, plus the rotating backups. There are no manual slots, and clients see the host's last save time.
- **Co-op on a dedicated server:** **manual saves** are available to admins through the console and in game: `save <name>`, `saves` (list), `load <name>` (restarts the session) and `delete <name>`. Autosave and backups continue as well.
- **Save editor:** autosave triggers (interval, room change, quest completion, before teleporting), number of slots, whether saving is allowed in combat, quicksave and quickload (single-player only), and the details each slot shows.
- **Save/load screen** (a Menu Editor template): each slot shows a **thumbnail screenshot**, universe name, character, location (body and system), calendar date, play time and difficulty. Actions: load, overwrite, delete.
- **Save migrations:** each game update can change the save schema. The package carries migration scripts, so saves from older versions are upgraded on load, and a copy of the old save is kept.

---
## 9. AI console: LLM assistant in the editor

The AI console is an **Assistant** dock where the designer talks to an LLM (Claude, through the Anthropic Python SDK) about the open project. It runs **in the editor only**. Nothing from it is cooked into games, and the editor works the same when it is turned off. **Games never use an LLM at run time:** there is no AI-generated NPC dialogue. All NPC dialogue is written by designers in the Dialogue editor (with generator slots filled from the world), as described in §8.

### What it does
- Answers questions about Midgard and the `midgard` scripting API. The API reference and `.pyi` stubs are in its prompt.
- Writes, explains and fixes game scripts. The Script editor has *Ask Assistant about selection* on its context menu.
- Drafts content in bulk: items, quest chains, skill trees, calendars, dialogue text, shop inventories.
- Explains cook and validation errors, and reviews quest and skill logic for impossible prerequisites or cycles.

### How it reaches the project (tools)
The assistant can only act through a small set of editor functions:

| Tool | Effect |
|---|---|
| `get_schema()` | Returns the project's table definitions. |
| `query_project(sql)` | One read-only `SELECT` on a connection opened with `PRAGMA query_only = ON`. |
| `read_script(path)`, `list_assets(kind)` | Read-only access to scripts and the asset list. |
| `validate_project()` | Runs the cook validator and returns its messages. |
| `propose_changes(summary, changes)` | Does **not** edit anything. It shows a diff in the dock with **Apply** and **Reject**. Apply runs the whole change set as one `QUndoCommand` named "Assistant: {summary}", so one Ctrl+Z reverts it. |

The assistant has no shell, no direct file writes and no network access beyond the API call itself.

### Sketch
```python
import anthropic
import keyring
from anthropic import beta_tool

client = anthropic.Anthropic(api_key=keyring.get_password("Midgard", "anthropic"))

@beta_tool
def query_project(sql: str) -> str:
    """Run one read-only SELECT against the open project's database.

    Args:
        sql: A single SELECT statement.
    """
    return project.read_only_query(sql)

@beta_tool
def propose_changes(summary: str, changes: list[dict]) -> str:
    """Propose edits. The user sees a diff and applies or rejects them.

    Args:
        summary: One line describing the change set.
        changes: Row inserts/updates/deletes and script edits in Midgard change-set format.
    """
    pending_changes.put(ChangeSet(summary, changes))
    return "Shown to the user for approval."

runner = client.beta.messages.tool_runner(
    model="claude-opus-5-5",
    max_tokens=16000,
    system=[{"type": "text", "text": MIDGARD_SYSTEM_PROMPT,          # API reference + schema; stable, so cached
             "cache_control": {"type": "ephemeral"}}],
    tools=[get_schema, query_project, read_script, list_assets, validate_project, propose_changes],
    messages=history,
    output_config={"effort": "medium"},
    betas=["server-side-fallback-2026-07-01"],
    fallbacks="default",            # retry on another model if a request is declined
)
for message in runner:              # runs on a worker QThread; each message is posted to the dock
    worker.message_ready.emit(message)
```

The sketch shows the shape only. Check the tool-runner and fallback parameters against the installed `anthropic` SDK version when building it.

### Rules
- **Off by default, one key per user** (decided). Each user turns it on in Preferences and provides **their own** Anthropic API key. There is no shared team key or proxy. The key is stored in the OS credential store (`keyring`) or read from `ANTHROPIC_API_KEY`. It is never stored in `midgard.json` or the project.
- **The user sees what is sent.** Each message can be expanded to show the context that went with it (schema, selected rows, script). A project can forbid the assistant with `"assistant": {"allowed": false}` in `{project}.config`.
- **Model and effort** are preferences. The defaults are `claude-opus-5-5` at `medium` effort. The dock shows token usage for each conversation.
- **The UI never freezes.** Requests run on a worker thread, replies stream into the dock, and a Stop button cancels.
- **History stays local**, in `.midgard/assistant/` in the project. It is not cooked and is ignored by version control.

---

## 10. Roadmap

Each phase ends with something that **runs**. Phases 0–3 are in strict order because everything later depends on them. From Phase 4 on, work can overlap. §0 has the current status of every phase and milestone.

### Phase 0: Foundations
- Monorepo: `editor/`, `engine/`, `schema/`, `platforms/`, `Themes/`, `tools/`, root `VERSION`.
- **Open-source setup:** `LICENSE`, `CONTRIBUTING.md`, code of conduct, issue templates, a third-party licence checker in CI, and `THIRD_PARTY_NOTICES.txt` generation for exports.
- Python ≥ 3.12 venv, `PySide6>=6.8`, `PySide6-QtAds`; ruff and pytest.
- CMake + vcpkg manifest; C++20; clang-format.
- CI (GitHub Actions or similar): Linux and Windows builds of editor and engine, plus an Android NDK build job and a Linux arm64 engine build job (native ARM runner or cross-compile).
- Write **contract v0**: `manifest.schema.json`, first migration, live-link message list.

**Exit:** CI is green on Linux and Windows, and both halves build from a clean checkout.
**Status: done** (14 October 2026).

### Phase 1: Editor shell
- Main window `Midgard - {project}`, New/Open/Recent project, `{project}.config`, `{project}.db` with migrations.
- `midgard.json` settings in the app data folder.
- ADS docking, Window menu, saved layouts, multi-monitor restore, high-DPI policy.
- Generic docks: Project browser, Inspector, Log.
- Undo/redo infrastructure.
- Automatic project backups in `.midgard/backups/` (one person per project).
- **Localisation keys from day one:** every player-facing text field in the data model stores a string key, with the string table in `{project}.db`. The Localisation editor comes later, but nothing has to be retrofitted.
- **View ▸ Themes** (Theme Scheme) and **Help ▸ About Midgard** (About Scheme).
- PyInstaller packaging through `tools/package.py`.

**Exit:** a packaged editor opens a project, arranges panels across two monitors, restarts with the same layout and theme, and shows the right build number in About.
**Status: done** (14 October 2026).

### Phase 2: Engine core and the vertical slice
- SDL3 window, Vulkan renderer (mesh, material, camera, directional light).
- Package loader: manifest, read-only SQLite, `game.pak`.
- Minimal ECS scene from the `scene`/`entity` tables.
- **Input actions layer** (keyboard, mouse and gamepad defaults; nothing reads raw keys) and a basic **third-person camera**.
- Editor **Build ▸ Cook** produces a package. `midgard_runtime` runs it.
- First runtime templates built in CI (Windows, Linux, Android) and a minimal **Build ▸ Export** that copies a template and adds the package.
- **Android spike:** the same cooked package runs on a phone.

**Exit:** a cube placed in the editor appears in the game on Windows, Linux and Android, all three exported from a single editor host. **This is the most important milestone in the roadmap.**
**Status: done** (14 October 2026).

### Phase 3: Live link and engine bindings
- Play / Stop in the editor (subprocess + TCP link), log streaming, hot reload of assets and DB rows.
- `_midgard` pybind11 module, starting with package validation.
- Embedded Vulkan viewport dock (move between monitors without crashing).

**Exit:** editing a value in the inspector updates the running game in under a second.
**Status: done** (14 October 2026); a test checks the one-second limit with the real engine.

### Phase 4: Scripting
- Embedded CPython in the engine, `midgard` API v1, generated stubs.
- Script editor dock with highlighting, completion, error markers from the running game, snippet library.
- **Assistant v1** (§9): the dock, API key in `keyring`, read-only tools (`get_schema`, `query_project`, `read_script`), and help with scripts and the `midgard` API.

**Exit:** a script reacts to a trigger and changes the world, edited and reloaded without restarting.
**Status: done** (14 October 2026).

### Phase 5: World building
**Status: in progress.** 5.0–5.5 are done (§0 lists what each milestone brought).

**The detailed plan is in [`Phase5_Roadmap.md`](Phase5_Roadmap.md)** (approved 15 October 2026):
19 milestones (5.0–5.18), foundation first (test tooling, deterministic maths and world
coordinates, the shape mapping layer, the renderer, physics), each with its own tests and exit
check. The list below is the phase's scope; where they differ, `Phase5_Roadmap.md` wins.
Survival status effects, clothing stats, the HUD gauge, weather's effect on AI and space weather
moved to Phase 6 (see there); Phase 5 builds their foundations.

- ~~Object editor (primitives, glTF import).~~ Built in 5.5, with objects as templates, the Material editor and groups.
- **Calendar core** (§6): calendar templates and the standard calendar, per-body calendars generated from the seed, `TimeService`, solar phase, **seasons** (daylight, temperature, visuals), time scale, save/restore of `game_time`.
- **Weather** (§6): weather types and tables per season and biome, regional weather with fronts, forecasts (script API), controlled and scripted weather, visuals. (Effects on AI perception, forecast devices and space weather are in Phase 6.)
- **Survival foundation** (§6): feels-like temperature, wetness, shelter checks and heat sources, readable from scripts. (Status effects, clothing stats, placeable campfires and heaters, the HUD gauge and `survival_scale` from the difficulty are in Phase 6.)
- Environment editor (sky, lighting, fog, water), with lighting curves authored over **solar phase** and previewed with the Calendar's time scrubber.
- ~~Physics editor (Jolt).~~ Built in 5.4.
- **Cameras** (§8): first-person, third-person and top-down views, the Camera editor, collision and shape-aware "up".
- **Character animation** (§8): glTF import, humanoid retargeting, the Animation editor (state machines, blend spaces, layers, events), foot IK on every world shape, first-person arms.
- **Audio** (§8): the Audio editor, sound events, buses, 3D sound, reverb zones, music playlists, ambience by biome and solar phase.
- Renderer foundation: textures (KTX2 / Basis Universal), glTF PBR materials, cascaded shadows in the local "up" frame, HDR, sky, fog, water, desktop and phone quality tiers. *Built in 5.3, except water (5.8).*
- Shader system: GLSL editor → SPIR-V, material editor, texture generator.
- **Shape mapping layer** (§7, *World shapes*) built before any terrain feature, so nothing assumes "y is up": per-body gravity, character "up" vector, double-precision simulation, camera-relative rendering. *Built in 5.1, 5.2 and 5.4.*
- Terrain in two steps. First **rectangular grids**: bounded area, then cylinder (flat wrap, then curved), with edges and cylinder ends. Then **cube-sphere** for the outside and inside of spheres. Sculpt/paint in the viewport works on all shapes.
- **World Type editor** v1 (§7): seed + node graph with shape-aware inputs, recipe versioning, run-time generation in the game, chunk streaming, and the cross-target check (identical chunk hashes on x86_64, arm64 and Android in CI), plus generation-time budgets measured on a phone and a Raspberry Pi.
- **Rooms:** the room manager and room changes. A planet's atmosphere ceiling, habitat docks and hollow-world exit sites lead to a **system-space** room with a procedural sky.
- **Light models per shape** (§6, *Day and night on each world shape*), plus the inside-world atmosphere preset.
- Per-chunk navigation meshes linked across chunk edges, cylinder joins and cube-sphere faces; map projections per shape as images (the in-game map screen is in Phase 6).
- Testing foundation: image tests on software Vulkan in CI, cross-target generation hashes, performance budgets, and a self-test APK for phone checks. *Built in 5.0 and 5.1.*

**Exit:** a game generates, on the player's device, one world type of each shape at fixed sizes. A player walks all the way around the cylinder and both spheres with correct gravity, sees no seams, and watches a full day/night cycle on each shape. They rise out of the planet's atmosphere into space and come back down. The game ships as a package. An animated character walks the planet in all three camera views, its feet planted on the slopes, with biome ambience and music.

### Phase 6: Gameplay data systems
- Property-grid widget (the shared node-graph widget is built in Phase 5, for world types).
- **Moved from Phase 5** (they need this phase's systems): cold, heat and soaked **status effects** and the rested bonus; **clothing survival stats**; craftable, placeable **campfires and heaters**; the **HUD temperature gauge** and screen-edge frost and sweat; `survival_scale` from the difficulty; weather reducing **AI perception**; **space weather**; **forecast devices** (weather stations, ship sensors, scanners); camera **lock-on**; the in-game **map screen**.
- Menu editor + RmlUi runtime (with player-sizable, movable windows).
- **Combat** (§8): the Combat editor, weapons and abilities, hit boxes on bones, damage types and status effects, dodge, block and parry, the combat sandbox.
- **AI behaviour** (§8): the AI Behaviour editor, perception, navigation links, groups and factions, Calendar schedules, spawn tables, live behaviour-tree debugging.
- **Companions** (§8): recruiting by dialogue, quest and hire, commands, companion skill trees, downed and revive, following through teleports and onto ships. **Approval**: traits, approval tags in the Dialogue and Quest editors, approval levels, refusals, leaving and winning back, personal quests, the approval simulator.
- **Dialogue** (§8): the Dialogue editor, response trees with conditions and actions, role dialogue with generator slots, recorded voice with optional subtitles, voice profiles, on-device TTS for slot words, recording scripts, lip-sync tracks, the conversation window and camera.
- **Input mapping** (§8): the Input editor, contexts, touch layout editor, the player's rebinding screen and device-aware prompts.
- **Localisation** (§8): English string table, the Localisation editor, pseudo-localisation, the **translation kit** in every export, user-installed language packs with English fallback, ICU plurals and placeholders, font fallback. The in-game *Export strings* button for players.
- **Save and load** (§8): the Save editor, autosave triggers, manual slots with thumbnails in single-player, the save/load screen, save migrations.
- Inventory → wallet → **banks** (Banking editor, several banks per universe with interest and risk profiles, monthly risk events, ratings and history, joint accounts with member limits, recovery quests after losses) → quests → skill trees → progression, each with cook-time validation and a `midgard` script API.
- **Calendar events**: recurrence editor, `calendar_event` prerequisites for quests, shop hours, NPC schedules, monthly bank interest, time skips.
- **Space and teleportation** (§7): Star Field editor and run-time generation of the 9 systems (stars, body mix, names), skies built from the star field on every body and in space, discovery, the star map, the **teleporter component** on machines, ship modules and artifacts, the Teleport editor, teleport events, interference zones and safe arrival. **World mode** (system map or bounded surface map) in `{project}.config`, 2–13 bodies per system, per-body day lengths, discovery by exploration and scanning, player-placed teleport pads, renaming bodies.
- **Base building** (§7, *Bases*): the Base editor, snap placement on every world shape, functional pieces (storage, crafting stations, power, beds, teleport and landing pads, defences), permissions, piece budgets, no-build zones.
- **Gathering** (§8): the Resources editor, resource nodes from the seed, tools and mining lasers, depletion and regrowth, deposit scanning.
- **Crafting** (§8): the Crafting editor, recipes and stations, production chains up to ship modules, quality tiers, station queues, the balance checker.
- **Hunger and thirst** (§6): meters, food and drink items, cooking recipes and kitchens, food relief for status effects, sickness, companions eating, difficulty scales.
- **Hostile atmospheres** (§6): atmosphere kinds per world type, suits and breathing gear, air supply and refills, life support and airlocks in ships and bases, depressurisation, spacewalks with magnetic boots and thruster packs, atmosphere warnings on teleport and landing.
- **Base raids** (§7, *Bases*): attention and schedules, warnings, ground waves and raider ships, piece damage and repair, storage theft and recovery, defences, the difficulty's `raid_scale`.
- **Bounded surface maps:** ocean boundary and storm wall, 1–3 continents, archipelagos and islands from the New World screen, ships in both world modes.
- **Ships** (§7): Ship editor, flight model with flight assist, atmosphere drag and lift, compressed system space with cruise mode, flying between system space and planet skies, docks into habitats and hollow worlds, landing and parking, crewed interiors in the ship's frame, ship ownership. **Getting ships**: hull frames, modules as inventory items, ship designs sold at shipyards, ships, hulls and modules as quest rewards, and the shipyard's slot-based building and refitting with live checks and blueprints. **Wrecks**: module damage, wreck beacons, loot containers, repair tools and materials, salvage tows. **Ship combat**: weapon and shield modules, turrets, missiles, module damage, ship behaviours for enemies, spawn tables. Salvage from any wreck, and blueprint sharing (in game, as files, and on Workshop). Flight assist and Newtonian flight.
- **Other craft classes** (§7): aircraft (aerodynamics, runways and VTOL), airships (buoyancy, ballast, wind drift, mooring), sailing ships (water and wave simulation, sails driven by the weather's wind, hull and sail damage, naval combat, harbours), small craft (rowing and paddling, hangars and davits). Shipyard kinds per class, diving salvage and refloating for sunken wrecks. **Land vehicles** (wheeled and tracked physics, terrain and weather grip, animal-drawn carts, vehicle workshops). **Base boatyards.** **Boarding** (grapples, docking clamps, deck fights, surrender and prizes, enemy boarders).
- **Mounts** (§7): stables, taming, riding and stamina, saddlebags, mount equipment, mounted combat, draft animals, survival needs, calling, base stables. **Flying mounts** (aircraft movement model, wing-beat stamina, aerial combat). **Breeding** (inheritance, local-day gestation, growth). The 21-mount roster, and bringing one mount into another player's universe.
- **Base garages** for land vehicles.
- **Quests in generated worlds** (§7): anchors with placement rules (including host world types and systems), quest templates with slots, the quest generator, *Preview in seed…*, and the cook check that every anchor is placed.
- **Worlds and characters** (§7): world folders and `world.json`, shape and size chosen on the New World screen, several worlds per player, name and editable description, Difficulty editor and modifiers, character profiles, and menu templates for the world list, New World, character select and join list.
- **Assistant v2**: `propose_changes` with diff, Apply/Reject and a single undo step, so the assistant can draft items, quests and skill trees.

**Exit:** a playable loop: take a quest, kill the wolves, earn gold, bank it (and earn interest at month end), buy an item from a shop that is only open by day, spend a skill point. Then fly a ship from the planet into space and dock inside a habitat, teleport with an artifact to a hollow world in another system, and finally take that character into a second universe, newly generated on a harder difficulty. Along the way, get the quest through a voiced dialogue tree whose answers change the outcome, fight a wolf pack whose behaviour tree flanks and calls for help alongside a hired companion, switch between all three camera views mid-fight, rebind the attack key, install a community language pack, and save to a manual slot and load it again. Finally, mine ore, refine and craft a turret, build a small base with a teleport pad, and defend it against a raid with a companion.

### Phase 7: Networking
- ENet transport, snapshots with delta compression, client prediction and reconciliation for the player's character, interpolation for everything else.
- Host-authoritative gameplay systems and RPCs, including the host-owned game clock and client time sync.
- Listen server (*Host Game*) and headless dedicated server; LAN discovery and direct connect.
- **Dedicated server export target** (§2, *Dedicated server export*): server cook profile, `server.json` (create or load a world), admin commands including `world describe`, autosave and backups, content-hash check on join. The editor's *Play* menu gains **Run Dedicated Server** for local testing.
- **Midgard join-code service:** the PHP directory API, the `midgard_relay` UDP rendezvous and relay, Docker images, the public instance, and the privacy note.
- **Midgard Sync:** sign-in by pairing code, email one-time code and Steam (session tickets and OpenID), account recovery, encryption on the device or at rest, versioned upload and download of profiles and owned universes, the online *in use* lock, conflict copies, unlinking and erasure.
- **Steam layer** (optional): invites, rich presence, Steam Networking Sockets between Steam players, Steam Cloud for profiles, **achievements** (with an in-game list for non-Steam builds), and **Steam Workshop** for language packs and ship blueprints. Steam invites carry join codes.
- **Cross-play:** mixed Windows, Linux and Android sessions with and without Steam, platform icons, touch aim assist, and Android as join-only.
- **Rejoin window** after a host crash (there is no host migration, see §4).
- **Combat and conversations in co-op**: host-decided hits with lag compensation, allies immune to each other's attacks, group conversations (join or watch), named manual saves on dedicated servers.
- **Rooms in co-op**: players spread over different bodies and systems. The host simulates every occupied room (up to 16), with snapshots per room, an optional `max_active_rooms`, and the shared universal clock. Party teleport from machines, and recall with consent.
- **Ships in co-op**: several crew aboard one ship, pilot prediction and reconciliation, and ships carried with their crew through room changes.
- **Character transfer**: send the character with their **companions and ships** on join and validate all of them on the host. Ships return to their parking spot in a universe they've been in, or go to a dock in the arrival system on a first visit. Return everything on autosave and on leaving, with the *in use* lock and versioned backups. World rules (outside characters, level range), and name/description/difficulty in the join list.
- Editor: network settings and `max_players` in `{project}.config`. **Play with N clients** launches a host and N−1 clients in separate windows, which can be spread across monitors, with simulated latency and packet loss.

**Exit:** 16 players (a mix of Windows, Linux and Android) and a dedicated server play the vertical slice across the internet using a join code, spread across several bodies in different systems, with four of them crewing one ship. Quests, economy and time of day stay consistent, and movement feels responsive at 150 ms latency. The same session also runs on an arm64 dedicated server (Raspberry Pi 5).

### Phase 8: Platforms and distribution
- Production runtime templates for every target, including all three server templates (Linux x86_64, Linux arm64, Windows), built on their own OS in CI, versioned with the editor and downloaded on demand.
- Server packaging extras: `systemd` unit, `Dockerfile` and optional image build, a Windows start script.
- **Build ▸ Export…** dialog: Windows (zip/installer, unsigned in 1.0), Linux (`.tar.gz` from any host), Android (APK/AAB, signing UI, keystore management, Gradle export for advanced users). Plus the **Steam** option for Windows and Linux (Steam API library, app ID, SteamPipe layout), using the developer's own Steamworks SDK.
- Cook profiles for per-target asset variants (for example, smaller textures on Android).
- Spike: Explorer icon and version resources written into the Windows `.exe` from a Linux host.
- Platform handoff documents (Appendix A) kept current.

**Exit:** the released editor on Linux exports the sample game for Windows, Linux and Android, and the editor on Windows does the same. All six builds run on their targets.

### Phase 9: Polish and 1.0
- Assistant v3: context actions throughout the editor (Quest, Skill, Calendar and Menu editors), conversation history and token-usage display.
- Localisation spike: CJK font fallback on Android.
- Documentation, sample projects, performance passes, crash reporting, 1.0 release.

### Open source
**Midgard is open source** (decided). This affects the whole project:
- **Licence: Apache-2.0** (decided) for the editor, engine, runtime templates and services. It is permissive and includes an explicit patent grant, so game makers can ship games under any licence, including commercial ones, even though every game contains the runtime.
- **Apache-2.0 housekeeping:** a `LICENSE` file with the full text, a `NOTICE` file for Midgard's attribution, an `SPDX-License-Identifier: Apache-2.0` header in every source file (checked in CI), and contributions accepted under the same licence (inbound = outbound, stated in `CONTRIBUTING.md`). Exported games carry Midgard's `NOTICE` text inside `THIRD_PARTY_NOTICES.txt`.
- **Dependencies stay compatible:**
  - **Qt/PySide6 and Qt-ADS are LGPL.** They are used only by the **editor**, never by games. The packaged editor uses PyInstaller's *one-folder* mode, so users can replace the Qt libraries as the LGPL requires.
  - **Text-to-speech:** use the **MIT-licensed original Piper**, not the GPL fork, so games aren't forced under the GPL. Choose **voice models with licences that allow commercial use**, because some are non-commercial only.
  - **The Steamworks SDK is proprietary** and never committed. Developers download it themselves (already the plan).
  - Everything else in the engine is permissive (MIT, BSD, zlib, Apache-2.0, public domain, the Python and Unicode licences).
- **Third-party notices:** every export automatically includes a `THIRD_PARTY_NOTICES.txt` listing the licences of everything bundled with the game (CPython, SDL3, Jolt, RmlUi, and so on), and the editor's About box links to its own.
- **Repository:** a public repository with `LICENSE`, `CONTRIBUTING.md`, a code of conduct, issue templates, and a CI pipeline that runs on pull requests. The **Licensing Scheme** artifact (licence keys, trials, subscriptions) is for commercial software, so it **does not apply to Midgard**.
- **Services:** the join-code service, relay and Midgard Sync are open source too, so studios can audit and self-host them.

### One person per project
In 1.0 a project is edited by **one person** (decided). `{project}.db` can still live in git as a binary file for history and backups, because nobody needs to merge it. The editor also keeps **automatic project backups** (rotating copies of the database and config) in `.midgard/backups/`.

### After 1.0
Planned for later versions:
- **Multi-user projects:** a text export of the project database (one JSON file per record, so git can merge it), or a project server with locking.
- **Windows code signing** with a code-signing certificate.
- **Custom TTS voices** matched to the recorded actors.
- **Submarines and diving craft** as a new craft class.
---

## 11. Theme Scheme in the Midgard editor

Source guide: [Theme Scheme](https://claude.ai/artifact/4TxyztohhWytP22EMKEV1b). Midgard follows it closely. A **View ▸ Themes** menu offers *System Light* and *System Dark* (Qt's own color schemes), then every `.qss` file in a `Themes` folder as a further theme, named after its file. The chosen theme is saved and reapplied at startup.

### What is the same
- `THEMES`, `QSS_PREFIX`, `ICON_SEARCH_PREFIX = "renoir"`, `qss_themes()`, `theme_title()`, `set_theme()` and `_apply_theme()` are copied as written in the guide.
- `<name>-icons/` folders next to `.qss` files, loaded as `url(renoir:…)`.
- `THEMES_DIR` lives in `midgard_editor/__init__.py`, with the `source_dir.txt` mechanism so packaged builds see the project's live `Themes` folder.
- Requires `PySide6>=6.8`.
- **No fixed colors in widget code.** This matters even more in Midgard, where every editor module is written separately. Take colors from `self.palette()`. Node-graph views and curve editors should draw with `palette().highlight()`, `mid()`, `text()` and so on.

### What changes for Midgard
1. **Settings store.** The theme key is saved in `midgard.json`, not SQLite or `QSettings`:

   ```python
   # midgard_editor/settings.py
   import json, os
   from pathlib import Path
   from PySide6.QtCore import QStandardPaths

   class Settings:
       """Editor settings in midgard.json: ~/.local/share/Midgard on Linux,
       %APPDATA%\\Midgard on Windows (application name "Midgard", no organisation name)."""

       def __init__(self):
           base = Path(QStandardPaths.writableLocation(QStandardPaths.StandardLocation.AppDataLocation))
           base.mkdir(parents=True, exist_ok=True)
           self._path = base / "midgard.json"
           try:
               self._data = json.loads(self._path.read_text(encoding="utf-8"))
           except (OSError, ValueError):
               self._data = {}

       def get(self, key, default=None):
           return self._data.get(key, default)

       def set(self, key, value):
           self._data[key] = value
           tmp = self._path.with_suffix(".json.tmp")
           tmp.write_text(json.dumps(self._data, indent=2), encoding="utf-8")
           os.replace(tmp, self._path)          # atomic, so a crash never leaves half a file
   ```

2. **A second, per-user Themes folder.** `qss_themes()` also scans `<AppData>/Midgard/Themes`, so users can add themes without touching the install. If both folders contain a theme with the same name, the user's file wins.
3. **Dock styling.** Every theme must also style the docking system. Floating dock containers on other monitors are top-level windows, but they still take the application-wide stylesheet, so one `.qss` covers every monitor:

   ```css
   /* Themes/yggdrasil.qss: palette taken from the Midgard logo */
   QWidget { background-color: #141b2d; color: #e8e1cf;
             selection-background-color: #6f9a3a; selection-color: #0c1220; }
   QWidget:disabled { color: #8a8574; }
   .QWidget { background-color: transparent; }

   ads--CDockAreaTitleBar { background: #1d2740; }
   ads--CDockWidgetTab { background: #1d2740; padding: 2px 8px; }
   ads--CDockWidgetTab[activeTab="true"] { background: #2b3550; border-bottom: 2px solid #c9a227; }
   ads--CDockSplitter::handle { background: #2b3550; }
   ads--CFloatingDockContainer { background: #141b2d; }
   QMainWindow::separator { background: #2b3550; width: 4px; height: 4px; }   /* QDockWidget fallback */
   QMainWindow::separator:hover { background: #c9a227; }
   ```

   Ship **Yggdrasil** (night blue, bark brown, leaf green, knotwork gold from the logo) as Midgard's signature dark theme.
4. **Viewports.** Vulkan viewports aren't drawn by Qt, so QSS can't reach them. When the theme changes, the editor sends the palette's `window` color to the engine (live link or `_midgard`) to use as the empty-viewport background.
5. **Editor themes are not game themes.** The Menu Editor's styles for *player* menus belong to the project (stored in `{project}.db` and cooked to `.rcss`). They never come from the editor's theme.
6. **Packaging** is `tools/package.py` (Python, so it runs on Windows without bash). PyInstaller 6 accepts `:` as the `--add-data` separator on every OS: `--add-data "Themes:Themes"`.

**Checklist:** follow the guide's checklist, plus: Settings class on `midgard.json` · user Themes folder · ADS selectors in every theme · viewport background hook · test each theme with a floating dock container on a second monitor.

---

## 12. About Scheme in the Midgard editor

Source guide: [About Scheme](https://claude.ai/artifact/JduRbCFwUs3eTXpfBVCjme). Midgard uses its numbering and dialog as written. A hand-edited `VERSION` file (`major.minor.patch`), a generated `midgard_editor/version.py` (`VERSION`, `BUILD`, `BUILD_DATE`), and `bump_build.py`, which raises the build number on every package and resets it to 1 when the major or minor version changes. The About box opens from **Help ▸ About Midgard** and is fixed at the 1.6:1 golden ratio so its title is never cut off.

### Midgard's About box (top to bottom, centred)
1. **Midgard**, bold, 1.8× font size
2. *A game engine with an action-focused editor*
3. The Yggdrasil icon scaled smoothly to 128 × 128. Bundle `midgard512.png` as `ICON_PATH` rather than the 2.5 MB `midgard.png`.
4. `v0.4.0 (build 17)`
5. `Engine v0.4.0 · package format 1.0`, which Midgard adds (see below)
6. `October 14, 2026`
7. **OK**

Margins of 32/24/32/16 px, 10 px spacing, and width = max(content width, 1.6 × height), all as in the guide's code.

### What changes for Midgard
1. **One `VERSION` for both halves.** CMake reads the same file:

   ```cmake
   file(READ "${CMAKE_SOURCE_DIR}/VERSION" MIDGARD_VERSION)
   string(STRIP "${MIDGARD_VERSION}" MIDGARD_VERSION)
   configure_file(engine/core/version.h.in "${CMAKE_BINARY_DIR}/midgard/version.h")
   ```

   The About box reads the engine version from `_midgard.version()` (or `midgard_runtime --version` before the bindings exist) and shows line 5. If the two don't match, line 5 is shown in the theme's disabled color with "(mismatch)". It uses the palette, not a fixed red, following the Theme Scheme.
2. **The build number travels with the product.** The cook step writes `"built_with": {"editor": "0.4.0+17", …}` into every package's `manifest.json`, so a bug report from a player identifies the exact editor build.
3. **Cross-platform package script.** The guide's bash `trap` becomes Python:

   ```python
   # tools/package.py
   import shutil, subprocess, sys
   from pathlib import Path

   ROOT = Path(__file__).resolve().parent.parent
   VERSION_PY = ROOT / "editor" / "midgard_editor" / "version.py"

   def main():
       backup = VERSION_PY.with_name("version.py.bak")
       shutil.copy2(VERSION_PY, backup)
       try:
           subprocess.run([sys.executable, str(ROOT / "bump_build.py")], check=True)
           build_engine()      # cmake --build, per platform
           build_editor()      # pyinstaller midgard.spec (bundles Themes, midgard512.png, runtime templates)
       except BaseException:
           shutil.move(backup, VERSION_PY)   # a failed build never uses up a build number
           raise
       backup.unlink()
   ```

   In `bump_build.py`, set `OUT = DIR / "editor" / "midgard_editor" / "version.py"`.
4. **Games get the same scheme.** A game project has its own `game.version` in `{project}.config`. Every export bumps its build number, and the runtime can show it in a player-facing "About" menu built in the Menu Editor.

**Checklist:** follow the guide's checklist, plus: CMake reads `VERSION` · engine and format version lines in the About box · `built_with` in the manifest · `tools/package.py` instead of `package.sh`.

---

## 13. Decisions and open questions

### Decided (14 October 2026)
- **AI console** = an LLM assistant in the editor (§9). It is editor-only and off by default.
- **All game packages are target-only.** The editor runs on Linux and Windows. Windows, Linux and Android games are exported from runtime templates, and either host can export to every target (§2, *Target-only exports*).
- **Android** never hosts the editor.
- **Wallet and banking** are in-game currency only.
- **Calendar** added as a Main Function (§6).
- **Multiplayer** is small co-op with 2–16 players per world and one authoritative host (§4, *Co-op networking*). Calendar time runs only while a world is being hosted.
- **Worlds** are procedurally generated. A player can own several worlds, each with a name, an editable description and a difficulty level (§7).
- **Worlds are fixed in size** and come in four shapes: bounded area, cylinder (a rectangle whose two opposite edges join; curved inside surface or flat wrap), sphere outside and sphere inside (§7, *World shapes*).
- **Quests** use both hand-written quests (through generator anchors) and templates filled from the world (§7).
- **Worlds are generated by the game at run time** from a seed. The editor only describes world types and the star field recipe (§7).
- **A world is a universe:** a procedural star field whose bodies can be planets, hollow worlds and cylinder habitats (plus bounded areas) in the same game.
- **Planets are bounded by the sky.** Leaving the atmosphere is a **room change** to space (§7, *Space, star fields and travel*).
- **"World" means the universe.** Each planet, habitat or hollow world in it is a body.
- **Every universe has 9 star systems.**
- **Players can be on different bodies at once.** The host simulates every occupied room.
- **Travel is instant teleportation**, like Star Trek's transporter. It always comes from a game object: a machine, a ship module or an artifact. No calendar time passes.
- **Midgard is for 3D RPG-style action games**, with first-person, third-person and top-down cameras all available (§8).
- **Six more systems are in 1.0**, each with its own editor subsystem: character animation, audio, dialogue trees, input mapping (keyboard, gamepad and touch, rebindable by players), localisation and save/load UI (§8).
- **A Combat editor and an AI Behaviour editor are in 1.0** (§8).
- **Players switch camera views freely** in every game.
- **English is the original language.** Every game ships a translation kit, so users can translate it into their own language as installable language packs. Voice stays in English.
- **Dialogue has recorded voice for every line**, with subtitles if the player turns them on.
- **All players can join or watch conversations.**
- **Manual saves:** single-player has manual slots. A co-op listen-server host has autosave only. A dedicated server has named manual saves for admins.
- **In group conversations, the player who started the conversation chooses.**
- **Combat is co-op only.** Players never damage each other, and there is no PvP.
- **Companions are in 1.0** (§8, *Companions*).
- **Generated slot words in voiced lines use on-device text-to-speech** (§8, *Dialogue*).
- **Up to 2 companions per player.**
- **Companions and ships belong to the character** and travel with them between universes. Wrecks stay behind until repaired (§7, *Ships*).
- **Rejoining a universe puts each ship back where it was parked** there. On a first visit, craft are delivered to the matching facility near where the character arrives. Spacecraft never change system, and other craft never leave their body except inside a spacecraft's hangar.
- **Companions have approval** and don't follow orders blindly. They react to the player's choices, can refuse orders, and can leave (§8, *Approval*).
- **Stock TTS voices** are used for now. Custom voices may come later.
- **Players can export the English strings** from the game and translate them into their own language.
- **Right-to-left languages are not supported.**
- **Midgard provides the join-code service:** a PHP directory API plus a small C++ UDP rendezvous and relay (§4, *Join codes*).
- **Games may be published on Steam.** There is an optional Steam layer, and Steam invites carry join codes (§4, *Steam*).
- **Cross-play is on** across Windows, Linux and Android, with or without Steam (§4, *Cross-play*).
- **No host migration**, not even graceful handoff. A crashed host gets a rejoin window, and always-on universes use dedicated servers.
- **Phones never host.** Android builds join only.
- **Steam builds use achievements and Steam Workshop** (language packs and ship blueprints). Non-Steam builds show achievements in game.
- **Midgard's public join-code service runs on a private server or a dedicated game-server hosting provider.**
- **A universe never changes after creation**, including its difficulty.
- **Banks and quests stay in each universe.** The wallet and inventory travel with the character. Each universe has **several banks** with different interest and risk (§7, *Banks*).
- **No permadeath.**
- **No limits on characters or worlds.** Players delete what they don't want to keep.
- **Players sync between devices** through Midgard Sync, signing in with a pairing code, email or Steam (no Google) (§7, *Syncing between devices*).
- **One universe per dedicated server.**
- **Co-op players can share bank accounts** (joint accounts with a manager and member withdrawal limits).
- **Bank losses can be won back** through recovery quests.
- **Recovery by email or Steam uses server-held keys.** Pairing-only profiles stay encrypted on the device.
- **Sizes:** sphere radius 1–20 km, cylinder length 5–50 km.
- **Gravity:** realistic spin gravity in cylinders, constant gravity on spheres.
- **Inside lighting:** a central sun (spheres) or an axis sun-line (cylinders), both with dimming and shades for sunrise and sunset.
- **Bounded surface maps are surrounded by ocean**, with 1–3 continents and a player-chosen number of archipelagos and islands. A storm wall at the map's edge is the only hard limit. **Cylinder ends remain walls.**
- **Ships cross a habitat's open middle freely.** Hollow worlds have a no-fly zone around the sun.
- **World mode is chosen per game project:** a **system map** (9 systems of 2–13 cylinders and spheres) or a **bounded surface map** (one ocean-bounded area of continents, archipelagos and islands). Bounded areas never appear as bodies in a system.
- **Each body has its own day length** (later extended: each body has its own full calendar, see below).
- **Discovery starts with everything hidden** and is revealed by exploration and scanning.
- **Players can place teleport pads** in a base or anywhere on a surface.
- **Ships are built only at shipyards.** **Salvaging any wreck is always allowed.** **Blueprints can be shared.**
- **Ship combat is in 1.0**, with both flight assist and Newtonian flight.
- **Players can rename bodies.**
- **Base building is in 1.0**, with a Base editor.
- **Ships exist in both world modes.**
- **Crafting, resource gathering and base raids are in 1.0**, each with its own editor (Crafting, Resources, and raid settings in the Base editor).
- **No offline raids**, on dedicated servers too.
- **Every body has its own calendar**, with its own day length, year, months and **seasons**. A standard calendar covers things not tied to a place.
- **Weather is tied to the calendar:** each season has weather tables per biome.
- **Survival effects are in 1.0:** cold, heat and wetness affect characters, and clothing, shelter and heat sources counter them (§6).
- **Hunger and thirst are in 1.0.** Food and drink also ease status effects, and every survival effect's strength varies with the world's difficulty.
- **Hostile atmospheres are in 1.0** (thin, toxic, vacuum), with suits, air supply, life support and spacewalks.
- **Food does not spoil.**
- **Game scripts are pure Python.**
- **Linux games ship as `.tar.gz` only.**
- **Windows builds are unsigned in 1.0.** A code-signing certificate is planned after 1.0.
- **The AI assistant uses a per-user API key.** Each user provides their own.
- **No AI-generated NPC dialogue in games.**
- **macOS is out of scope** for the editor and games.
- **One person per project** in 1.0. Multi-user projects come in a later version.
- **Midgard is open source under Apache-2.0.** The Licensing Scheme artifact doesn't apply.
- **Craft classes:** besides spacecraft, 1.0 has **aircraft, airships, sailing ships and small craft**, sharing one hull-and-module system, with shipyards of the matching kind (§7, *Ships and other craft*). Land vehicles were added as a sixth class.
- **Land vehicles and mounts are in 1.0.** Land vehicles are a sixth craft class, and mounts belong to the character.
- **Bases can have a boatyard** for small craft.
- **Boarding is in 1.0**, against NPC craft only, with captured craft as prizes.
- **Flying mounts and base garages are in 1.0.** Submarines come after 1.0.
- **Up to 21 mounts per character, with breeding.** Only **one mount** can be brought into a universe someone else hosts.
- **Players fly spacecraft** within a system, through atmospheres, and into habitats and hollow worlds.
- **Ships can be bought, earned from quests, or built from modules** at shipyards (§7, *Getting a ship*).
- **Ship building is slot-based:** modules snap into a hull frame's slots. Free-form building is out of scope.
- **Destroyed ships become wrecks** that can be repaired where they lie, or towed to a shipyard for a fee (§7, *Wrecks and repair*).
- **Crew always escape** when their craft is destroyed: escape pods, parachutes or lifeboats, depending on the class. **Wrecks stay until they are repaired**; they never decay.
- **Characters move between worlds.** They are stored in the player's profile and checked by the host on join (§7).
- **The dedicated server is an export target** for Linux x86_64, **Linux arm64** (Raspberry Pi 4/5 with a 64-bit OS, ARM cloud VMs) and Windows (§2, *Dedicated server export*).

### Decided (15 October 2026): Phase 5
- **Phase 5 follows `Phase5_Roadmap.md`**: foundation first, with tests at every milestone.
- Phase 5 items that need Phase 6 systems **move to Phase 6**; Phase 5 builds their foundations.
- **Rendering by the end of Phase 5:** glTF PBR materials with cascaded shadows, fog, sky and water, plus a phone quality tier.
- **Device testing:** the developer's Android phone, through self-test APKs. There's no Raspberry Pi yet: CI's arm64 Linux runner stands in until Phase 7.
- **CC0 assets** may be committed for tests and samples, each recorded with its source and licence.
- **Terrain sculpting and painting** are stored as **terrain stamps** in the project, placed by world types or used directly by bounded-area maps, and reused for player terrain edits.
- **Phase 5 sample worlds** use the smallest allowed sizes; larger sizes are tested for precision.

### Decided (16 October 2026): while building Phase 5
- **Groups** make complex objects from primitives: a group has its own transform (its centre, scaled evenly), its parts keep their scripts and components, and a group with a moving rigid body is **one compound physics body** (its parts can't use mesh colliders). Moving a part into or out of a group keeps it where it is in the world.
- **Objects are templates.** An object is a row in the `object` table (migration 6) with a list of parts; an entity with an `object` component is an instance, and the engine fills in the parts as its children, so a changed template changes every instance. An instance acts as a group.
- **Imported models become a part per material**, with a box collider round the whole model by default (convex, exact mesh or none on request) and optional levels of detail drawn by distance.
- **Material changes to an imported model** are kept in the object's settings, in place of the model's own materials, so the model file is never changed and can be re-imported.
- **Image textures are content ids** `tex:<path>` (relative to the project), with `?linear` for slots that aren't colours; chosen images are copied into `textures/`.
- **Cooking is content-hashed:** imported models and encoded images are kept in `build/cache/` under a hash of what they're made from.
- **The editor watches `objects/` and `textures/`** while a project is open, re-importing what changed, and the running game follows (the `object.put` message).
- **Triggers are sensor bodies of their own**, so an entity can be both solid and a trigger.
- **The world's surface** (`builtin:surface`) is made by the engine from the body and casts no shadow.
- **Several renderers share the editor's process** (the viewport and the previews), so the engine never binds Vulkan's functions to one device.
- **The editor opens the last project on start**, unless turned off in Preferences.

### Still open

- **Telling Discord what's being worked on** (Rich Presence from the editor, or posting updates through a channel webhook): notes in `discord.md`, to come back to.

### Plan corrections
- ~~"Vulcan" → **Vulkan**~~ (applied to `midgard_plan.md`).
- ~~Overview: *"The editor runs on Linux and Windows. Games are exported to Linux, Windows and Android from either editor host."*~~ (applied to `midgard_plan.md`).
- ~~Deployment: *"Runtime templates are built on each OS in Midgard's CI. Game exports need no compiler."*~~ (applied to `midgard_plan.md`).

---

## Appendix A: Platform handoff description

The plan asks for a description of the application to pass to Windows or Android to create platform-specific distributions. Keep this as `platforms/<os>/HANDOFF.md`, generated from this template and updated each release:

```markdown
# Midgard <VERSION> — <Windows | Linux | Android> handoff

## What Midgard is
A game editor (Python 3.12+, PySide6 ≥ 6.8, PySide6-QtAds) and a C++20 engine
(SDL3, Vulkan, Jolt, EnTT, KTX/Basis, embedded CPython 3.14, SQLite). The editor cooks projects
into Midgard packages (manifest.json + {project}.db + game.pak) that the
engine runtime loads. Package format: <format_version>. Engine ABI: <n>.

## Runtime template (built in Midgard CI on this OS)
- Toolchain: <MSVC 2022 / GCC 13+ in manylinux / Android NDK r27+, JDK 17, SDK 35>
- Engine:   cmake --preset <windows-release | linux-release | android-arm64>
- Template: <runtime zip | runtime tarball | template APK/AAB + Gradle project>
- Server template: <midgard_server.exe zip | midgard_server tarball + Dockerfile (x86_64 and arm64) | n/a on Android>
- Editor:   python tools/package.py            (Windows and Linux only; there is no Android editor)

## Exporting games to this target
- From Linux host:   <supported / notes>
- From Windows host: <supported / notes>
- Extra tools needed: <none | JDK + Android build-tools>  (osslsigncode once Windows signing arrives after 1.0)

## Outputs
- <file names, sizes, signing status>

## Platform specifics
- Settings:  <%APPDATA%\Midgard\midgard.json | ~/.local/share/Midgard/midgard.json | n/a>
- Saves:     <path>
- Signing:   <none in 1.0 (Authenticode after 1.0) | none | keystore alias>
- Known issues: <…>

## Verify
1. Open About Midgard: version, build and date match.
2. Export samples/hello_cube to this target from both a Linux and a Windows editor; run both builds here.
3. Run samples/vertical_slice; watch a full day/night cycle; host a listen server; join from a second machine.
```
