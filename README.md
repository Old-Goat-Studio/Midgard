<p align="center"><img src="midgard512.png" alt="Midgard — Yggdrasil" width="200"></p>

# Midgard

**A 3D game engine with an action-focused editor, for RPG-style action games.**

Midgard has two halves:

- **The editor** (Python + PySide6) runs on Windows and Linux. Designers build worlds, quests,
  characters, craft and everything else in docking windows that can spread across several
  monitors.
- **The engine** (C++20) is the runtime inside every game and dedicated server. Games are
  exported to Windows, Linux and Android, and dedicated servers to Windows, Linux x86_64 and
  Linux arm64.

The editor **cooks** a project into a package (`manifest.json` + a SQLite database + assets),
and the engine runs packages. The contract between them lives in [`schema/`](schema/).

> **Status: Phase 0 (foundations).** The repository layout, contract v0, the build system and CI
> are in place. The editor window arrives in Phase 1 and the first rendered scene in Phase 2.
> See [`midgard_roadmap.md`](midgard_roadmap.md) for the full design and plan, and
> [`midgard_plan.md`](midgard_plan.md) for the original brief.

## Repository layout

| Folder | Contents |
|---|---|
| `editor/` | The Python editor (`midgard_editor` package) and its tests |
| `engine/` | The C++ engine: `core/` library, `runtime/` (game executable), `server/` (dedicated server), `tests/` |
| `schema/` | The editor ↔ engine contract: manifest schema, database migrations, live-link messages |
| `platforms/` | Per-platform packaging notes and handoff documents |
| `Themes/` | Editor colour themes (`.qss`), from Phase 1 |
| `tools/` | Build and maintenance scripts: vcpkg setup, licence checks |
| `VERSION` | The single version number for the editor and the engine |

## Building

**Editor** (Python 3.12+):

```sh
python -m venv .venv
.venv/bin/pip install -e "editor[dev]"      # Windows: .venv\Scripts\pip
.venv/bin/python -m midgard_editor --version
.venv/bin/pytest editor tools
```

**Engine** (CMake 3.25+, Ninja, and GCC 13+ or Visual Studio 2022):

```sh
python tools/setup_vcpkg.py                 # fetches vcpkg at the pinned version into .tools/
cmake --preset linux-release                # Windows: windows-release, from a VS developer prompt
cmake --build --preset linux-release
ctest --preset linux-release
build/linux-release/engine/midgard_runtime --version
```

Other presets: `linux-arm64-release` (on an ARM host) and `android-arm64` (needs
`ANDROID_NDK_HOME`).

## Licence

Midgard is open source under the [Apache License 2.0](LICENSE). Games made with Midgard can use
any licence, including commercial ones. Third-party components are listed in
[`tools/third_party.json`](tools/third_party.json). See [CONTRIBUTING.md](CONTRIBUTING.md) to
get involved.
