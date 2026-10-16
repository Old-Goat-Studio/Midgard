// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "midgard/jobs.h"
#include "midgard/profile.h"

using midgard::JobContext;
using midgard::JobHandle;
using midgard::JobPriority;
using midgard::JobSystem;

TEST_CASE("every job runs exactly once, under stress") {
    JobSystem jobs(4);
    constexpr int count = 20000;
    std::vector<std::atomic<int>> runs(count);
    std::vector<JobHandle> handles;
    handles.reserve(count);
    for (int i = 0; i < count; ++i)
        handles.push_back(jobs.submit([&runs, i](const JobContext&) { runs[i].fetch_add(1); },
                                      static_cast<JobPriority>(i % 3)));
    jobs.wait_all(handles);
    for (int i = 0; i < count; ++i) REQUIRE(runs[i].load() == 1);
    for (const JobHandle& h : handles) CHECK(h->state() == midgard::Job::State::Done);
    CHECK(jobs.queued() == 0);
}

TEST_CASE("higher priority jobs start first") {
    JobSystem jobs(1);
    std::mutex gate;
    std::unique_lock hold(gate);  // keeps the only worker busy while the queue fills
    auto blocker = jobs.submit([&](const JobContext&) { std::lock_guard g(gate); });
    while (blocker->state() != midgard::Job::State::Running) std::this_thread::yield();

    std::vector<int> order;
    std::mutex order_mutex;
    std::vector<JobHandle> handles;
    for (int i = 0; i < 3; ++i) {
        for (JobPriority p : {JobPriority::Low, JobPriority::Normal, JobPriority::High}) {
            handles.push_back(jobs.submit(
                [&, p](const JobContext&) {
                    std::lock_guard g(order_mutex);
                    order.push_back(static_cast<int>(p));
                },
                p));
        }
    }
    hold.unlock();
    // Poll rather than wait(): a waiting thread runs jobs too, and two runners could record
    // out of order.
    for (const JobHandle& h : handles)
        while (!h->done()) std::this_thread::sleep_for(std::chrono::microseconds(100));
    CHECK(order == std::vector<int>{0, 0, 0, 1, 1, 1, 2, 2, 2});
}

TEST_CASE("cancelled jobs don't start, and running ones can stop early") {
    JobSystem jobs(1);
    std::atomic<bool> release{false};
    std::atomic<bool> started{false};
    std::atomic<int> loops{0};
    auto running = jobs.submit([&](const JobContext& ctx) {
        started = true;
        while (!ctx.cancelled()) {
            ++loops;
            std::this_thread::yield();
        }
        while (!release) std::this_thread::yield();
    });
    while (!started) std::this_thread::yield();
    std::atomic<bool> ran{false};
    auto queued = jobs.submit([&](const JobContext&) { ran = true; });
    queued->cancel();
    running->cancel();
    release = true;
    jobs.wait(running);
    jobs.wait(queued);
    CHECK_FALSE(ran.load());
    CHECK(queued->state() == midgard::Job::State::Cancelled);
    CHECK(running->state() == midgard::Job::State::Done);
}

TEST_CASE("a job can wait for jobs it submits, even with one worker") {
    JobSystem jobs(1);
    std::atomic<int> leaves{0};
    auto parent = jobs.submit([&](const JobContext&) {
        std::vector<JobHandle> children;
        for (int i = 0; i < 50; ++i)
            children.push_back(jobs.submit([&](const JobContext&) { ++leaves; }));
        jobs.wait_all(children);  // the waiting worker runs them itself
    });
    jobs.wait(parent);
    CHECK(leaves.load() == 50);
}

TEST_CASE("a job's exception reaches whoever waits for it") {
    JobSystem jobs(2);
    auto bad = jobs.submit([](const JobContext&) { throw std::runtime_error("chunk failed"); });
    CHECK_THROWS_WITH_AS(jobs.wait(bad), "chunk failed", std::runtime_error);
    jobs.wait(bad);  // only once
    CHECK(bad->done());
}

TEST_CASE("parallel_for covers every index once") {
    JobSystem jobs(3);
    for (std::size_t count :
         {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{100003}}) {
        std::vector<std::atomic<int>> hits(count);
        jobs.parallel_for(count, [&](std::size_t i) { hits[i].fetch_add(1); });
        for (std::size_t i = 0; i < count; ++i) REQUIRE(hits[i].load() == 1);
    }
}

TEST_CASE("shutting down with work queued cancels it and doesn't hang") {
    std::vector<JobHandle> handles;
    std::atomic<int> ran{0};
    {
        JobSystem jobs(2);
        for (int i = 0; i < 1000; ++i)
            handles.push_back(jobs.submit([&](const JobContext&) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                ++ran;
            }));
    }  // the destructor returns once running jobs finish
    int done = 0, cancelled = 0;
    for (const JobHandle& h : handles) {
        if (h->state() == midgard::Job::State::Done) ++done;
        if (h->state() == midgard::Job::State::Cancelled) ++cancelled;
    }
    CHECK(done + cancelled == 1000);
    CHECK(done == ran.load());
}

TEST_CASE("profiling records zones from every thread only while on") {
    namespace profile = midgard::profile;
    profile::set_enabled(false);
    { MIDGARD_PROFILE_SCOPE("ignored"); }
    profile::set_enabled(true);
    {
        JobSystem jobs(2);
        jobs.parallel_for(8, [](std::size_t) { MIDGARD_PROFILE_SCOPE("work"); });
        // Polled, not waited for, so a worker (not this thread) surely runs it.
        auto on_worker = jobs.submit([](const JobContext&) { MIDGARD_PROFILE_SCOPE("work"); });
        while (!on_worker->done()) std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    { MIDGARD_PROFILE_SCOPE("main"); }
    profile::set_enabled(false);
    const auto zones = profile::take();
    std::set<std::string> names;
    std::set<std::uint32_t> threads;
    for (const auto& z : zones) {
        names.insert(z.name);
        threads.insert(z.thread);
        CHECK(z.duration_ns >= 0);
    }
    CHECK(names.count("ignored") == 0);
    CHECK(names.count("work") == 1);
    CHECK(names.count("main") == 1);
    CHECK(names.count("job") == 1);  // the job system marks every job
    CHECK(threads.size() >= 2);

    std::random_device rd;
    const auto file = std::filesystem::temp_directory_path() /
                      ("midgard_trace_" + std::to_string(rd()) + ".json");
    REQUIRE(profile::write_chrome_trace(file, zones));
    std::ifstream in(file);
    const auto trace = nlohmann::json::parse(in);
    in.close();
    std::filesystem::remove(file);
    CHECK(trace["traceEvents"].size() == zones.size());
    CHECK(trace["traceEvents"][0]["ph"] == "X");
}
