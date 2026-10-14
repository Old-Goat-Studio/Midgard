# Midgard 0.1.0 — Windows handoff

## What Midgard is
A game editor (Python 3.12+, PySide6 ≥ 6.8, PySide6-QtAds) and a C++20 engine. The editor cooks
projects into Midgard packages (manifest.json + {project}.db + game.pak) that the engine runtime
loads. Package format: 1.0. Engine ABI: 0.

## Runtime template (built in Midgard CI on Windows)
- Toolchain: Visual Studio 2022 (MSVC), CMake ≥ 3.25, Ninja
- Engine: `python tools/setup_vcpkg.py`, then `cmake --preset windows-release` from a developer prompt
- Template: runtime zip (from Phase 2)
- Editor: `python tools/package.py` (from Phase 1)

## Exporting games to this target
- From a Linux host: supported (no compiler needed)
- From a Windows host: supported
- Extra tools needed: none. Builds are unsigned in 1.0 (signing after 1.0).

## Outputs
- Phase 0: `midgard_runtime.exe`, `midgard_server.exe`, `midgard_tests.exe`

## Platform specifics
- Settings: `%APPDATA%\Midgard\midgard.json`
- Saves: the game's user data folder (from Phase 6)
- Signing: none in 1.0 (Authenticode after 1.0)
- Known issues: SmartScreen warns about unsigned games.

## Verify
1. `midgard_runtime.exe --version` prints version, ABI, format and schema range.
2. `ctest --preset windows-release` passes.
