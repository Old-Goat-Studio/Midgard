// SPDX-License-Identifier: Apache-2.0
// midgard_runtime: the game executable. Phase 0 checks and describes a package; Phase 2 adds the
// window, renderer and scene.
#include <cstdio>
#include <exception>
#include <string_view>

#include "midgard/package.h"
#include "midgard/version.h"

namespace {

int usage() {
    std::puts("usage: midgard_runtime [--version] [--package <dir>]");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    const char* package_dir = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--version") {
            std::printf("Midgard runtime %s (engine ABI %d, package format %d.x, schema %d-%d)\n",
                        midgard::kVersion, midgard::kEngineAbi, midgard::kFormatMajor,
                        midgard::kSchemaMin, midgard::kSchemaMax);
            return 0;
        }
        if (arg == "--package" && i + 1 < argc) {
            package_dir = argv[++i];
        } else {
            return usage();
        }
    }
    if (!package_dir) return usage();

    try {
        const midgard::PackageInfo info = midgard::open_package(package_dir);
        const midgard::Manifest& m = info.manifest;
        std::printf("%s %s: scene '%s', schema %d, %zu content file(s), built by editor %s\n",
                    m.game_name.c_str(), m.game_version.c_str(), m.entry_scene.c_str(),
                    info.database_schema_version, m.content.size(), m.editor_build.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
