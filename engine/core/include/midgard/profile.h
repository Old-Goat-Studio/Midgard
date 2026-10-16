// SPDX-License-Identifier: Apache-2.0
// Profiling markers: named, timed zones from any thread, written as a Chrome trace (open it at
// ui.perfetto.dev or chrome://tracing).
//
//   void build_chunk() { MIDGARD_PROFILE_SCOPE("build_chunk"); ... }
//
// Markers are in every build but record nothing until profiling is switched on
// (midgard_runtime --profile out.json), so when it's off each marker costs one atomic load.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace midgard::profile {

struct Zone {
    const char* name;  // a string literal
    std::uint32_t thread;
    std::int64_t start_ns;  // since profiling was switched on
    std::int64_t duration_ns;
};

void set_enabled(bool on);  // switching on clears what was recorded before
bool enabled();
// Recording stops after this many zones (about 32 MB), so a forgotten profile can't fill memory.
inline constexpr std::size_t max_zones = 1'000'000;

std::int64_t now_ns();
void record(const char* name, std::int64_t start_ns, std::int64_t duration_ns);
std::vector<Zone> take();  // the zones so far, oldest first (and forgets them)
// Writes zones in Chrome's trace event format. False if the file can't be written.
bool write_chrome_trace(const std::filesystem::path& file, const std::vector<Zone>& zones);

class Scope {
public:
    explicit Scope(const char* name) : name_(enabled() ? name : nullptr) {
        if (name_) start_ = now_ns();
    }
    ~Scope() {
        if (name_) record(name_, start_, now_ns() - start_);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char* name_;
    std::int64_t start_ = 0;
};

}  // namespace midgard::profile

#define MIDGARD_PROFILE_CONCAT2(a, b) a##b
#define MIDGARD_PROFILE_CONCAT(a, b) MIDGARD_PROFILE_CONCAT2(a, b)
#define MIDGARD_PROFILE_SCOPE(name) \
    ::midgard::profile::Scope MIDGARD_PROFILE_CONCAT(midgard_profile_scope_, __LINE__)(name)
