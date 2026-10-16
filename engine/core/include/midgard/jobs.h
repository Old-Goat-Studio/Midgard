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
//
// Each worker has its own queue (with its own lock), new jobs are spread across the queues, and
// a worker with nothing in its own queue takes work from the others. So threads rarely compete
// for a lock, however many workers there are. A worker always takes the highest-priority job it
// can find in any queue; with several workers, jobs of the same priority may start in any order.
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
    std::exception_ptr error_;  // set before the job is Done; taken by one waiter
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
    // Many jobs at once: one lock and one round of wake-ups for all of them.
    std::vector<JobHandle> submit_batch(std::vector<std::function<void(const JobContext&)>> work,
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
    struct Queue {
        std::mutex mutex;
        std::deque<JobHandle> jobs[3];           // by JobPriority
        std::atomic<int> counts[3] = {0, 0, 0};  // lets others skip it without locking
    };

    void worker_loop(unsigned index);
    void push(unsigned queue, std::vector<JobHandle>& jobs, JobPriority priority);
    void wake_workers(std::size_t jobs);
    JobHandle pop(unsigned first_queue);
    // Runs the next queued job, looking in `first_queue` first. False if there was none.
    bool run_one(unsigned first_queue);
    unsigned home_queue() const;  // a worker's own queue; any other thread takes turns

    std::vector<std::unique_ptr<Queue>> queues_;  // one per worker
    std::vector<std::thread> workers_;
    mutable std::atomic<unsigned> next_queue_{0};  // where threads that aren't workers push
    std::atomic<bool> stopping_{false};

    // Sleeping workers and waiting threads. Wake-ups are system calls, so they are made only
    // when someone is asleep. These counters and the job states are sequentially consistent
    // atomics: a thread about to sleep counts itself and then looks once more, and a thread
    // with news changes the state and then looks at the count, so one of them always sees
    // the other and no wake-up is lost.
    std::atomic<long long> queued_{0};  // jobs in all queues (briefly negative while popping)
    std::atomic<unsigned> idle_workers_{0};
    std::atomic<unsigned> waiters_{0};
    std::mutex sleep_mutex_;
    std::condition_variable work_ready_;
    std::mutex wait_mutex_;
    std::condition_variable job_finished_;
};

}  // namespace midgard
