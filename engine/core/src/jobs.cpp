// SPDX-License-Identifier: Apache-2.0
#include "midgard/jobs.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "midgard/profile.h"

namespace midgard {

namespace {

// How many times an idle worker looks for work before it sleeps (each look yields the CPU,
// so this is some tens of microseconds).
constexpr int kSpins = 256;

// Which job system's worker this thread is (if any), and its queue.
thread_local const JobSystem* t_system = nullptr;
thread_local unsigned t_queue = 0;

}  // namespace

JobSystem::JobSystem(unsigned workers) {
    if (workers == 0) {
        const unsigned cores = std::thread::hardware_concurrency();
        workers = cores > 1 ? cores - 1 : 1;
    }
    for (unsigned i = 0; i < workers; ++i) queues_.push_back(std::make_unique<Queue>());
    workers_.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) workers_.emplace_back([this, i] { worker_loop(i); });
}

JobSystem::~JobSystem() {
    stopping_ = true;
    for (auto& q : queues_) {
        std::lock_guard lock(q->mutex);
        for (int p = 0; p < 3; ++p) {
            for (const JobHandle& job : q->jobs[p]) job->state_.store(Job::State::Cancelled);
            queued_ -= static_cast<long long>(q->jobs[p].size());
            q->counts[p] = 0;
            q->jobs[p].clear();
        }
    }
    { std::lock_guard lock(sleep_mutex_); }
    work_ready_.notify_all();
    { std::lock_guard lock(wait_mutex_); }
    job_finished_.notify_all();
    for (std::thread& t : workers_) t.join();
}

unsigned JobSystem::home_queue() const {
    if (t_system == this) return t_queue;
    return next_queue_.fetch_add(1) % static_cast<unsigned>(queues_.size());
}

void JobSystem::push(unsigned queue, std::vector<JobHandle>& jobs, JobPriority priority) {
    const int p = static_cast<int>(priority);
    Queue& q = *queues_[queue];
    std::lock_guard lock(q.mutex);
    q.jobs[p].insert(q.jobs[p].end(), jobs.begin(), jobs.end());
    q.counts[p] += static_cast<int>(jobs.size());
}

void JobSystem::wake_workers(std::size_t jobs) {
    queued_ += static_cast<long long>(jobs);  // before looking at who sleeps (see jobs.h)
    const unsigned idle = idle_workers_.load();
    if (idle > 0) {
        {
            std::lock_guard lock(sleep_mutex_);
        }  // a worker going to sleep is now asleep
        if (jobs >= idle)
            work_ready_.notify_all();
        else
            for (std::size_t i = 0; i < jobs; ++i) work_ready_.notify_one();
    }
    // A thread waiting for a job helps run new work too.
    if (waiters_.load() > 0) {
        {
            std::lock_guard lock(wait_mutex_);
        }
        job_finished_.notify_all();
    }
}

JobHandle JobSystem::submit(std::function<void(const JobContext&)> work, JobPriority priority) {
    auto job = std::make_shared<Job>();
    job->work_ = std::move(work);
    if (stopping_) {
        job->state_.store(Job::State::Cancelled);
        return job;
    }
    std::vector<JobHandle> one{job};
    push(home_queue(), one, priority);
    wake_workers(1);
    return job;
}

std::vector<JobHandle> JobSystem::submit_batch(
    std::vector<std::function<void(const JobContext&)>> work, JobPriority priority) {
    std::vector<JobHandle> jobs;
    jobs.reserve(work.size());
    for (auto& w : work) {
        jobs.push_back(std::make_shared<Job>());
        jobs.back()->work_ = std::move(w);
    }
    if (stopping_) {
        for (const JobHandle& job : jobs) job->state_.store(Job::State::Cancelled);
        return jobs;
    }
    if (jobs.empty()) return jobs;
    // An even share for each queue, so every worker has some to start with.
    const std::size_t n = queues_.size();
    const std::size_t per = (jobs.size() + n - 1) / n;
    const unsigned first = home_queue();
    for (std::size_t i = 0, begin = 0; begin < jobs.size(); ++i, begin += per) {
        std::vector<JobHandle> part(
            jobs.begin() + static_cast<std::ptrdiff_t>(begin),
            jobs.begin() + static_cast<std::ptrdiff_t>(std::min(jobs.size(), begin + per)));
        push(static_cast<unsigned>((first + i) % n), part, priority);
    }
    wake_workers(jobs.size());
    return jobs;
}

std::size_t JobSystem::queued() const {
    std::size_t total = 0;
    for (const auto& q : queues_) {
        std::lock_guard lock(q->mutex);
        for (const auto& jobs : q->jobs) total += jobs.size();
    }
    return total;
}

JobHandle JobSystem::pop(unsigned first_queue) {
    if (queued_.load(std::memory_order_relaxed) <= 0) return nullptr;
    const std::size_t n = queues_.size();
    for (int p = 0; p < 3; ++p) {
        for (std::size_t k = 0; k < n; ++k) {
            Queue& q = *queues_[(first_queue + k) % n];
            if (q.counts[p].load(std::memory_order_relaxed) <= 0) continue;
            std::lock_guard lock(q.mutex);
            if (q.jobs[p].empty()) continue;
            JobHandle job = std::move(q.jobs[p].front());
            q.jobs[p].pop_front();
            --q.counts[p];
            --queued_;
            return job;
        }
    }
    return nullptr;
}

bool JobSystem::run_one(unsigned first_queue) {
    JobHandle job = pop(first_queue);
    if (!job) return false;
    if (job->cancelled_.load(std::memory_order_relaxed)) {
        job->state_.store(Job::State::Cancelled);
    } else {
        job->state_.store(Job::State::Running);
        std::exception_ptr error;
        try {
            MIDGARD_PROFILE_SCOPE("job");
            job->work_(JobContext(&job->cancelled_));
        } catch (...) {
            error = std::current_exception();
        }
        job->work_ = nullptr;  // release what the job captured
        job->error_ = error;   // before Done, which publishes it to the waiter
        job->state_.store(Job::State::Done);
    }
    if (waiters_.load() > 0) {  // after the state changed (see jobs.h)
        {
            std::lock_guard lock(wait_mutex_);
        }
        job_finished_.notify_all();
    }
    return true;
}

void JobSystem::worker_loop(unsigned index) {
    t_system = this;
    t_queue = index;
    while (true) {
        if (run_one(index)) continue;
        // Look for new work a little longer before sleeping: jobs often come in bursts, and
        // waking a sleeping thread costs a system call per job.
        for (int spin = 0; spin < kSpins && queued_.load(std::memory_order_relaxed) <= 0 &&
                           !stopping_.load(std::memory_order_relaxed);
             ++spin)
            std::this_thread::yield();
        if (queued_.load(std::memory_order_relaxed) > 0) continue;
        std::unique_lock lock(sleep_mutex_);
        ++idle_workers_;  // before the last look at the queues (see jobs.h)
        work_ready_.wait(lock, [&] { return queued_.load() > 0 || stopping_.load(); });
        --idle_workers_;
        if (stopping_) return;
    }
}

void JobSystem::wait(const JobHandle& job) {
    if (!job) return;
    while (!job->done()) {
        // Help instead of blocking, so a job that waits for its own sub-jobs can't deadlock.
        if (run_one(home_queue())) continue;
        std::unique_lock lock(wait_mutex_);
        ++waiters_;  // before the last look at the job and the queues (see jobs.h)
        job_finished_.wait(lock,
                           [&] { return job->done() || queued_.load() > 0 || stopping_.load(); });
        --waiters_;
        if (stopping_ && !job->done()) break;
    }
    std::exception_ptr error;
    {
        std::lock_guard lock(wait_mutex_);
        error = std::exchange(job->error_, nullptr);
    }
    if (error) std::rethrow_exception(error);
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
    std::vector<std::function<void(const JobContext&)>> work;
    for (std::size_t begin = 0; begin < count; begin += per) {
        const std::size_t end = std::min(count, begin + per);
        work.emplace_back([&body, begin, end](const JobContext&) {
            for (std::size_t i = begin; i < end; ++i) body(i);
        });
    }
    wait_all(submit_batch(std::move(work), priority));
}

}  // namespace midgard
