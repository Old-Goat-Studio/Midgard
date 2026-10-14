// SPDX-License-Identifier: Apache-2.0
// midgard_server: the headless dedicated server (no rendering, audio or UI). Phase 0 checks a
// server package; Phase 7 adds networking, server.json and admin commands.
#include <cstdio>
#include <exception>
#include <string_view>

#include "midgard/package.h"
#include "midgard/version.h"

int main(int argc, char** argv) {
    const char* package_dir = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--version") {
            std::printf("Midgard dedicated server %s (engine ABI %d)\n", midgard::kVersion,
                        midgard::kEngineAbi);
            return 0;
        }
        if (arg == "--package" && i + 1 < argc) {
            package_dir = argv[++i];
        } else {
            std::puts("usage: midgard_server [--version] [--package <dir>]");
            return 2;
        }
    }
    if (!package_dir) {
        std::puts("usage: midgard_server [--version] [--package <dir>]");
        return 2;
    }
    try {
        const midgard::PackageInfo info = midgard::open_package(package_dir);
        if (info.manifest.role != "server")
            std::fprintf(stderr, "warning: this is a '%s' package, not a server cook\n",
                         info.manifest.role.c_str());
        std::printf("Ready to host %s %s (one universe per server)\n",
                    info.manifest.game_name.c_str(), info.manifest.game_version.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
