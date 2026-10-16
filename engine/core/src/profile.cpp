// SPDX-License-Identifier: Apache-2.0
#include "midgard/profile.h"

#include <atomic>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

namespace midgard::profile {

namespace {

std::atomic<bool> on{false};
std::mutex mutex;
std::vector<Zone> zones;
std::int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
std::atomic<std::int64_t> epoch{steady_ns()};  // read by every thread, so atomic

std::uint32_t thread_number() {
    // Small, stable numbers per thread make the trace easier to read than hashed ids.
    static std::atomic<std::uint32_t> next{1};
    thread_local const std::uint32_t mine = next.fetch_add(1);
    return mine;
}

}  // namespace

void set_enabled(bool enable) {
    std::lock_guard lock(mutex);
    if (enable && !on.load()) {
        zones.clear();
        epoch.store(steady_ns());
    }
    on.store(enable);
}

bool enabled() {
    return on.load(std::memory_order_relaxed);
}

std::int64_t now_ns() {
    return steady_ns() - epoch.load(std::memory_order_relaxed);
}

void record(const char* name, std::int64_t start_ns, std::int64_t duration_ns) {
    const std::uint32_t thread = thread_number();
    std::lock_guard lock(mutex);
    if (zones.size() < max_zones) zones.push_back({name, thread, start_ns, duration_ns});
}

std::vector<Zone> take() {
    std::lock_guard lock(mutex);
    std::vector<Zone> out;
    out.swap(zones);
    return out;
}

bool write_chrome_trace(const std::filesystem::path& file, const std::vector<Zone>& list) {
    nlohmann::json events = nlohmann::json::array();
    for (const Zone& z : list) {
        // Complete events ("X"); times in microseconds.
        events.push_back({{"name", z.name},
                          {"ph", "X"},
                          {"pid", 1},
                          {"tid", z.thread},
                          {"ts", static_cast<double>(z.start_ns) / 1000.0},
                          {"dur", static_cast<double>(z.duration_ns) / 1000.0}});
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << nlohmann::json{{"traceEvents", events}, {"displayTimeUnit", "ms"}}.dump();
    return bool(out);
}

}  // namespace midgard::profile
