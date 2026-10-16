# vcpkg overlay ports

Ports here replace vcpkg's own (CMakePresets.json sets `VCPKG_OVERLAY_PORTS` to this folder).
Each is a copy of the vcpkg port at the pinned baseline with a small, explained change. When
the baseline moves, copy the new port and apply the change again.

| Port | Change | Why |
|---|---|---|
| `ktx` | `-DKTX_FEATURE_ETC_UNPACK=OFF` | The software ETC decoder is under Ericsson's licence, which only covers OpenGL, OpenGL ES and WebGL products and isn't transferable, so it can't ship in Vulkan games under our permissive-only rule. Games don't need it: they transcode Basis Universal textures to formats the GPU supports. |
