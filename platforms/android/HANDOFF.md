# Midgard 0.1.0 — Android handoff

## What Midgard is
A C++20 engine whose games are exported to Android (arm64-v8a). There is no Android editor, and
Android games join co-op sessions but never host. Package format: 1.0. Engine ABI: 0.

## Runtime template (built in Midgard CI on Linux)
- Toolchain: Android NDK (the CI runner's latest), CMake ≥ 3.25, Ninja; later JDK 17 and SDK 35
- Engine: `python tools/setup_vcpkg.py`, then `cmake --preset android-arm64` (needs `ANDROID_NDK_HOME`)
- Template: template APK/AAB and Gradle project (from Phase 2, using SDL3's Android shell)
- Minimum Android version: 10 (API 29), for Vulkan 1.1

## Exporting games to this target
- From a Linux or Windows host: supported (needs a JDK and Android build-tools, which the editor can download)
- Signing: always, with the game author's keystore

## Outputs
- Phase 0: the `midgard_core` library and `midgard_tests`, cross-compiled (not run on a device yet)

## Verify
1. The `android-arm64` preset configures and builds in CI.
2. From Phase 2: the hello-cube package runs on a real device.
