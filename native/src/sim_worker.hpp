// sim_worker.hpp — one unit of simulation work, off the window's thread.
//
// The window steps a simulation on its own thread, and cuts each batch off at
// eight milliseconds so that cheap steps never hurt it. What it cannot cut off
// is a SINGLE step, or a single training epoch: one that takes seconds leaves
// the window unable to paint, to move, or to register the very Pause that
// would end it.
//
// This runs one job at a time on a thread of its own. The rule that makes it
// safe without a lock inside any simulation is the caller's, and it is simple:
// while a job is in flight, the window does not touch the simulation at all —
// it re-shows its last frame and keeps a Pause for when the job lands. Exactly
// one thread touches a sim at any moment. The sims need no changes and know
// nothing about threads; worker_test.cpp checks that every one of them steps
// to the same world whichever thread steps it.

#pragma once
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace bench {

class SimWorker {
public:
    SimWorker() : thread_([this] { loop(); }) {}
    ~SimWorker() {
        {
            std::lock_guard<std::mutex> l(m_);
            quit_ = true;
        }
        cv_.notify_all();
        thread_.join();          // a job in flight finishes first: nothing is abandoned mid-step
    }
    SimWorker(const SimWorker&) = delete;
    SimWorker& operator=(const SimWorker&) = delete;

    // Hand over a job. Refused while another is in flight or waiting to be
    // collected: one at a time is the whole contract.
    bool start(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> l(m_);
            if (busy_) return false;
            job_ = std::move(job);
            error_ = nullptr;
            busy_ = true;
            done_ = false;
        }
        cv_.notify_all();
        return true;
    }

    // Started and not yet collected. While this is true the caller must not
    // touch whatever the job touches.
    [[nodiscard]] bool busy() const { std::lock_guard<std::mutex> l(m_); return busy_; }

    // Finished and waiting to be collected.
    [[nodiscard]] bool ready() const { std::lock_guard<std::mutex> l(m_); return busy_ && done_; }

    // Collect a finished job. What it threw is rethrown HERE, on the caller's
    // thread, so the caller's existing error handling applies unchanged — out of
    // memory still arrives as std::bad_alloc. False if nothing had finished.
    bool collect() {
        std::exception_ptr e;
        {
            std::lock_guard<std::mutex> l(m_);
            if (!busy_ || !done_) return false;
            busy_ = false;
            done_ = false;
            e = error_;
            error_ = nullptr;
        }
        if (e) std::rethrow_exception(e);
        return true;
    }

    // Block until the job in flight has finished. For shutting down: a sim must
    // never be destroyed under a step that is still running.
    void wait() {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [this] { return !busy_ || done_; });
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return quit_ || bool(job_); });
                if (!job_) return;                 // asked to quit, and nothing left to run
                job = std::move(job_);
                job_ = nullptr;
            }
            std::exception_ptr e;
            try { job(); } catch (...) { e = std::current_exception(); }
            {
                std::lock_guard<std::mutex> l(m_);
                error_ = e;
                done_ = true;
            }
            cv_.notify_all();
        }
    }

    mutable std::mutex      m_;
    std::condition_variable cv_;
    std::function<void()>   job_;
    std::exception_ptr      error_;
    bool busy_ = false, done_ = false, quit_ = false;
    std::thread thread_;        // last: it starts running only once everything above exists
};

} // namespace bench
