// SPDX-License-Identifier: Apache-2.0
#include "midgard/jobs.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "midgard/profile.h"

namespace midgard {

JobSystem::JobSystem(unsigned workers) {
    if (workers == 0) {
        const unsigned cores = std::thread::hardware_concurrency();
        workers = cores > 1 ? cores - 1 : 1;
    }
    workers_.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) workers_.emplace_back([this] { worker_loop(); });
}

JobSystem::~JobSystem() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        for (auto& queue : queues_) {
            for (const JobHandle& job : queue) job->state_.store(Job::State::Cancelled);
            queue.clear();
        }
    }
    work_ready_.notify_all();
    job_finished_.notify_all();
    for (std::thread& t : workers_) t.join();
}

JobHandle JobSystem::submit(std::function<void(const JobContext&)> work, JobPriority priority) {
    auto job = std::make_shared<Job>();
    job->work_ = std::move(work);
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            job->state_.store(Job::State::Cancelled);
            return job;
        }
        queues_[static_cast<int>(priority)].push_back(job);
    }
    work_ready_.notify_one();
    return job;
}

std::size_t JobSystem::queued() const {
    std::lock_guard lock(mutex_);
    return queues_[0].size() + queues_[1].size() + queues_[2].size();
}

bool JobSystem::run_one(std::unique_lock<std::mutex>& lock) {
    JobHandle job;
    for (auto& queue : queues_) {
        if (!queue.empty()) {
            job = std::move(queue.front());
            queue.pop_front();
            break;
        }
    }
    if (!job) return false;
    std::exception_ptr error;
    if (job->cancelled_.load(std::memory_order_relaxed)) {
        job->state_.store(Job::State::Cancelled, std::memory_order_release);
    } else {
        job->state_.store(Job::State::Running, std::memory_order_release);
        lock.unlock();
        try {
            MIDGARD_PROFILE_SCOPE("job");
            job->work_(JobContext(&job->cancelled_));
        } catch (...) {
            error = std::current_exception();
        }
        job->work_ = nullptr;  // release what the job captured
        lock.lock();
        job->error_ = error;
        job->state_.store(Job::State::Done, std::memory_order_release);
    }
    job_finished_.notify_all();
    return true;
}

void JobSystem::worker_loop() {
    std::unique_lock lock(mutex_);
    while (true) {
        if (run_one(lock)) continue;
        if (stopping_) return;
        work_ready_.wait(lock);
    }
}

void JobSystem::wait(const JobHandle& job) {
    if (!job) return;
    std::unique_lock lock(mutex_);
    while (!job->done()) {
        // Help instead of blocking, so a job that waits for its own sub-jobs can't deadlock.
        if (!run_one(lock)) job_finished_.wait(lock, [&] { return job->done() || stopping_; });
        if (stopping_ && !job->done()) break;
    }
    if (std::exception_ptr error = std::exchange(job->error_, nullptr)) {
        lock.unlock();
        std::rethrow_exception(error);
    }
}

void JobSystem::wait_all(const std::vector<JobHandle>& jobs) {
    std::exception_ptr first;
    for (const JobHandle& job : jobs) {
        try {
            wait(job);
        } catch (...) {
            if (!first) first = std::current_exception();
        }
    }
    if (first) std::rethrow_exception(first);
}

void JobSystem::parallel_for(std::size_t count, const std::function<void(std::size_t)>& body,
                             JobPriority priority) {
    if (count == 0) return;
    // A few batches per worker keeps them busy without a job per item.
    const std::size_t batches = std::min<std::size_t>(count, (worker_count() + 1) * 4);
    const std::size_t per = (count + batches - 1) / batches;
    std::vector<JobHandle> jobs;
    for (std::size_t begin = 0; begin < count; begin += per) {
        const std::size_t end = std::min(count, begin + per);
        jobs.push_back(submit(
            [&body, begin, end](const JobContext&) {
                for (std::size_t i = begin; i < end; ++i) body(i);
            },
            priority));
    }
    wait_all(jobs);
}

}  // namespace midgard
