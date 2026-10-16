<p align="center"><img src="midgard512.png" alt="Midgard — Yggdrasil" width="200"></p>

# Midgard

[![CI](https://github.com/Old-Goat-Studio/Midgard/actions/workflows/ci.yml/badge.svg)](https://github.com/Old-Goat-Studio/Midgard/actions/workflows/ci.yml)

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

> **Status: Phase 5 (world building).** Phases 0–4 are done: the editor, the Vulkan runtime on
> Windows, Linux and Android, the live link and Python scripting. Phase 5 follows
> [`Phase5_Roadmap.md`](Phase5_Roadmap.md), foundation first.
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
| `tools/` | Build and maintenance scripts: vcpkg setup, licence checks, image tests, budgets, and the Python reference implementations (`tools/reference/`) |
| `assets/third_party/` | CC0 assets for tests and samples, each listed in `SOURCES.toml` |
| `VERSION` | The single version number for the editor and the engine |

## Building

**Editor** (Python 3.12+):

```sh
python -m venv .venv
.venv/bin/pip install -e "editor[dev]"      # Windows: .venv\Scripts\pip
.venv/bin/python -m midgard_editor               # open the editor
.venv/bin/pytest editor tools
python tools/package.py                          # packaged editor in build/dist/Midgard/
```

**Engine** (CMake 3.25+, Ninja, and GCC 13+ or Visual Studio 2022):

On Linux, SDL needs the X11 and Wayland development packages, or it builds without a window
system and the runtime fails with "No available video device". On Debian or Ubuntu:

```sh
sudo apt install pkg-config libx11-dev libxext-dev libwayland-dev libxkbcommon-dev libegl-dev wayland-protocols
```

If vcpkg already built SDL without them, delete `build/linux-release/vcpkg_installed` and configure
again with `VCPKG_BINARY_SOURCES=clear`, so the cached copy isn't reused.

```sh
python tools/setup_vcpkg.py                 # fetches vcpkg at the pinned version into .tools/
python tools/setup_python.py                # fetches the CPython that games embed (scripts)
cmake --preset linux-release                # Windows: windows-release, from a VS developer prompt
cmake --build --preset linux-release
ctest --preset linux-release
build/linux-release/engine/midgard_runtime --version
build/linux-release/engine/midgard_runtime --check-python
build/linux-release/engine/midgard_runtime --selftest   # add --show to see it on screen
python tools/images.py run --runtime build/linux-release/engine/midgard_runtime
build/linux-release/engine/midgard_bench --json bench.json && python tools/budgets.py bench.json
```

In a game window, F3 shows the frame rate and timings; `--profile trace.json` records a trace
for [ui.perfetto.dev](https://ui.perfetto.dev). `--quality phone` (or `desktop`) picks the
renderer's quality tier; phones use the phone tier unless told otherwise, and the self-test times
frames at the device's tier against 60 frames a second.

Other presets: `linux-arm64-release` (on an ARM host) and `android-arm64` (needs
`ANDROID_NDK_HOME`, and `python tools/setup_python.py --target android-arm64` for scripting).

The engine build also makes `_midgard`, the engine as a Python module that the editor uses
in-process (for example to check every cooked package). It is built for the project's `.venv`
when there is one, and needs pybind11 there (included in `editor[dev]`); without it the build
skips the module and the editor works without it.

## Licence

Midgard is created by Richard E Rogers and published by **Old Goat Studio**, open source under
the [Apache License 2.0](LICENSE). Games made with Midgard can use
any licence, including commercial ones. Third-party components are listed in
[`tools/third_party.json`](tools/third_party.json). See [CONTRIBUTING.md](CONTRIBUTING.md) to
get involved.
