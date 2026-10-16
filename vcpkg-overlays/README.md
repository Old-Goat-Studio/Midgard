# vcpkg overlay ports

Ports here replace vcpkg's own (CMakePresets.json sets `VCPKG_OVERLAY_PORTS` to this folder).
Each is a copy of the vcpkg port at the pinned baseline with a small, explained change. When
the baseline moves, copy the new port and apply the change again.

| Port | Change | Why |
|---|---|---|
| `ktx` | `-DKTX_FEATURE_ETC_UNPACK=OFF` | The software ETC decoder is under Ericsson's licence, which only covers OpenGL, OpenGL ES and WebGL products and isn't transferable, so it can't ship in Vulkan games under our permissive-only rule. Games don't need it: they transcode Basis Universal textures to formats the GPU supports. |
| `joltphysics` | `-DDOUBLE_PRECISION=ON`; `USE_AVX`, `USE_AVX2`, `USE_AVX512`, `USE_F16C`, `USE_FMADD`, `USE_LZCNT` and `USE_TZCNT` off | Worlds are thousands of kilometres across, so physics positions are doubles like the engine's (roadmap §7). The x86 build keeps to SSE4.2, which every 64-bit PC of the last fifteen years has: AVX2 or LZCNT would crash games on older CPUs, and fused multiply-adds round differently from the engine's own maths. Arm builds are unaffected (NEON). |
