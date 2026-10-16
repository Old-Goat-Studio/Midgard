// SPDX-License-Identifier: Apache-2.0
// The runtime's self-test: quick checks that the engine works on this device (jobs, identical
// data, world shapes, physics, rendering and its frame time, the game's package, Python), with the
// results shown on screen by the debug overlay. midgard_runtime --selftest runs it; on Android the
// self-test APK (an APK with a selftest.txt asset) does.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "midgard/render/overlay.h"
#include "midgard/render/quality.h"

namespace midgard::selftest {

enum class Status { Pass, Fail, Skip };

struct Check {
    std::string name;
    Status status = Status::Pass;
    std::string detail;
};

struct Options {
    std::optional<std::filesystem::path> package;  // the game's package, if there is one
    // Where the game's Python lives (beside the executable, or unpacked on Android).
    std::function<std::filesystem::path()> python_home;
    // A missing Vulkan driver counts as a skip on PCs (CI machines have no GPU) but as a
    // failure on phones, which always have one.
    bool vulkan_required = false;
    // The quality tier the frame time is measured at (default: this device's).
    std::optional<render::Quality> quality;
};

std::vector<Check> run(const Options& options);
bool passed(const std::vector<Check>& checks);  // no failures (skips are fine)
// The results for the debug overlay: a large PASS or FAIL banner, then one line per check.
std::vector<render::OverlayLine> overlay(const std::vector<Check>& checks, bool on_phone);
// One line per check, for the log and standard output.
std::string report(const std::vector<Check>& checks);

}  // namespace midgard::selftest
