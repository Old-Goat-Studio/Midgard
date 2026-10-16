# Midgard 0.1.0 — Android handoff

## What Midgard is
A C++20 engine whose games are exported to Android (arm64-v8a). There is no Android editor, and
Android games join co-op sessions but never host. Package format: 1.0. Engine ABI: 0.

## Runtime template (built in Midgard CI on Linux)
- Toolchain: Android NDK (the CI runner's latest), CMake ≥ 3.25, Ninja, JDK 17, and the Android
  SDK's newest build-tools and platform (`ANDROID_HOME`)
- Engine: `python tools/setup_vcpkg.py`, then `cmake --preset android-arm64` (needs
  `ANDROID_NDK_HOME`). The runtime is built as `libmain.so`, with SDL3 linked in statically.
- Template: `python tools/make_template.py --preset android-arm64` writes
  `midgard-runtime-<version>-android-arm64.zip`. Inside is `base.apk`, an unsigned APK holding:
  - `AndroidManifest.xml` compiled by `aapt2` from `app/AndroidManifest.xml`, with placeholder
    package name, title and version;
  - the launcher icon (`app/res/`);
  - `classes.dex`: SDL's Java activity (from the SDL source release matching the vcpkg SDL3) and
    `MidgardActivity` (`app/java/`), which loads only `libmain.so`;
  - `lib/arm64-v8a/libmain.so`.
- Minimum Android version: 10 (API 29), for Vulkan 1.1. Target API 35. Landscape only.

## Exporting games to this target
- From a Linux or Windows host, with nothing extra installed. The editor
  (`editor/midgard_editor/android_apk.py`) patches the binary manifest, puts the cooked package in
  `assets/data/`, keeps `resources.arsc` stored and 4-byte aligned, and signs the APK with
  APK Signature Scheme v2.
- Package name: Build ▸ Export ▸ Android package, saved in the project config as
  `android_package` (default `com.example.<game>`). Version code: `major·1000000 + minor·1000 +
  patch` of the game version.
- Signing: the editor's development key, created on first export in
  `<settings>/android/`. Back it up: an installed game accepts updates only when they are signed
  with the same key. Choosing your own keystore comes later.
- On first start, the runtime copies `assets/data/` to internal storage (SQLite needs real
  files; the exporter lists the files in `assets/data/files.txt`), and copies it again whenever
  the package's `manifest.json` changes.
- Game scripts run on Python 3.14 (python.org's official Android build, fetched by
  `tools/setup_python.py --target android-arm64`): `lib/arm64-v8a/libpython3.14.so` beside
  `libmain.so`, and `assets/python.zip` (the standard library and its compiled modules),
  unpacked into internal storage on first start and again when it changes. About 10 MB.

## Outputs
- `runtime-template-android-arm64`: the template zip, from every CI run
- `HelloCube-android-arm64`: a sample game exported by the editor in CI, checked with the
  SDK's `zipalign -c`, `apksigner verify` and `aapt2 dump badging`

## Verify
1. The `android-arm64` preset configures and builds in CI, and the sample APK passes the checks.
2. Install the sample APK, or your own export, on a phone with Android 10 or later. Copy it to
   the phone and open it (allow installing from that source when asked), or run
   `adb install -r HelloCube-0.1.0-android-arm64.apk`. The scene appears in landscape; the
   Back button quits. Switching away and back keeps the game running.
3. If it fails, `adb logcat -s SDL SDL/APP AndroidRuntime DEBUG` shows the reason.
