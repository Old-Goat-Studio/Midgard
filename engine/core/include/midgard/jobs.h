// SPDX-License-Identifier: Apache-2.0
// The job system: worker threads that run small pieces of work (chunk generation, navmesh
// building, audio decoding) off the main thread, by priority.
//
//   midgard::JobSystem jobs;                          // one worker per spare core
//   auto job = jobs.submit([](const midgard::JobContext& ctx) {
//       for (...) { if (ctx.cancelled()) return; ... }
//   });
//   jobs.wait(job);                                   // or job->done() to poll
//
// A thread that waits runs queued jobs meanwhile, so a job may wait for jobs it submitted.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace midgard {

enum class JobPriority { High = 0, Normal = 1, Low = 2 };

class JobContext {
public:
    // True once the job has been cancelled: long jobs check it and return early.
    bool cancelled() const { return cancelled_->load(std::memory_order_relaxed); }

private:
    friend class JobSystem;
    explicit JobContext(const std::atomic<bool>* cancelled) : cancelled_(cancelled) {}
    const std::atomic<bool>* cancelled_;
};

class Job {
public:
    enum class State { Queued, Running, Done, Cancelled };

    State state() const { return state_.load(std::memory_order_acquire); }
    // Finished: ran to the end, or was cancelled before it started.
    bool done() const {
        const State s = state();
        return s == State::Done || s == State::Cancelled;
    }
    // Asks the job to stop. A queued job never starts; a running one sees ctx.cancelled().
    void cancel() { cancelled_.store(true, std::memory_order_relaxed); }

private:
    friend class JobSystem;
    std::function<void(const JobContext&)> work_;
    std::exception_ptr error_;  // guarded by the JobSystem's mutex
    std::atomic<State> state_{State::Queued};
    std::atomic<bool> cancelled_{false};
};

using JobHandle = std::shared_ptr<Job>;

class JobSystem {
public:
    // workers = 0: one per hardware thread, less one for the main thread (at least one).
    explicit JobSystem(unsigned workers = 0);
    ~JobSystem();  // cancels what is still queued and waits for running jobs to finish
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    JobHandle submit(std::function<void(const JobContext&)> work,
                     JobPriority priority = JobPriority::Normal);
    // Waits until the job is finished, running other queued jobs in the meantime. An exception
    // thrown by the job is rethrown here (once, to the first waiter).
    void wait(const JobHandle& job);
    void wait_all(const std::vector<JobHandle>& jobs);
    // Runs body(i) for i in [0, count), split into jobs, and waits for all of them.
    void parallel_for(std::size_t count, const std::function<void(std::size_t)>& body,
                      JobPriority priority = JobPriority::Normal);

    unsigned worker_count() const { return static_cast<unsigned>(workers_.size()); }
    std::size_t queued() const;

private:
    void worker_loop();
    // With the lock held: runs the next queued job (unlocking while it runs). False if none.
    bool run_one(std::unique_lock<std::mutex>& lock);

    mutable std::mutex mutex_;
    std::condition_variable work_ready_;
    std::condition_variable job_finished_;
    std::deque<JobHandle> queues_[3];  // by JobPriority
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

}  // namespace midgard
