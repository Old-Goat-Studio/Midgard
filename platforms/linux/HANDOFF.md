# Midgard 0.1.0 — Linux handoff

## What Midgard is
A game editor (Python 3.12+, PySide6 ≥ 6.8, PySide6-QtAds) and a C++20 engine. The editor cooks
projects into Midgard packages that the engine runtime loads. Package format: 1.0. Engine ABI: 0.

## Runtime template (built in Midgard CI on Linux)
- Toolchain: GCC 13+ (later built in a manylinux container for wide glibc compatibility), CMake ≥ 3.25, Ninja
- Engine: `python tools/setup_vcpkg.py`, then `cmake --preset linux-release` (x86_64) or `linux-arm64-release` (arm64 host)
- Template: runtime `.tar.gz` (from Phase 2); dedicated server for x86_64 and arm64
- Editor: `python tools/package.py` (from Phase 1)

## Exporting games to this target
- From a Linux host: supported
- From a Windows host: supported (`.tar.gz` only; AppImage and Flatpak are not planned)
- Extra tools needed: none

## Outputs
- Phase 0: `midgard_runtime`, `midgard_server`, `midgard_tests`

## Platform specifics
- Settings: `~/.local/share/Midgard/midgard.json`
- Saves: the game's user data folder (from Phase 6)
- Signing: none
- Known issues: none yet

## Verify
1. `midgard_runtime --version` prints version, ABI, format and schema range.
2. `ctest --preset linux-release` passes (and `linux-arm64-release` on arm64).
