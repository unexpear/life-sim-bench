// worker_test.cpp — stepping off the window's thread, checked.
//
// Two things have to be true before the window may step a sim on another
// thread. The worker must behave: one job at a time, errors delivered to the
// caller, nothing destroyed under a running job. And the SIMS must not care
// which thread steps them. The second is the one that could be silently false:
// a sim with thread-local state, or a pool bound to the thread that first used
// it, would produce a different world when stepped from somewhere else, and no
// existing test would notice. Every library sim is checked for exactly that.

#include "sim_worker.hpp"
#include "registry.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

bool same_world(bench::Sim& a, bench::Sim& b) {
    if (a.generation() != b.generation() || a.field().cells != b.field().cells) return false;
    const auto ma = a.metrics(), mb = b.metrics();
    if (ma.size() != mb.size()) return false;
    for (std::size_t i = 0; i < ma.size(); ++i) {
        if (ma[i].name != mb[i].name) return false;
        const bool bothNan = std::isnan(ma[i].value) && std::isnan(mb[i].value);
        if (!bothNan && ma[i].value != mb[i].value) return false;
    }
    const auto* sa = a.surface();
    const auto* sb = b.surface();
    if (sa && sb && sa->rgba != sb->rgba) return false;
    return true;
}

} // namespace

int main() {
    int checks = 0, failed = 0;
    auto check = [&](bool ok, const std::string& what) {
        ++checks;
        if (!ok) { ++failed; std::printf("FAIL %s\n", what.c_str()); }
    };
    using namespace std::chrono_literals;
    auto until = [](const bench::SimWorker& w) {
        const auto t0 = std::chrono::steady_clock::now();
        while (!w.ready() && std::chrono::steady_clock::now() - t0 < 10s)
            std::this_thread::sleep_for(1ms);
        return w.ready();
    };

    // ── the worker's contract ───────────────────────────────────────────────
    {
        bench::SimWorker w;
        check(!w.busy() && !w.ready() && !w.collect(), "a new worker is idle, with nothing to collect");
        std::thread::id ran;
        check(w.start([&] { ran = std::this_thread::get_id(); std::this_thread::sleep_for(50ms); }),
              "a job starts");
        check(w.busy(), "and is in flight straight away");
        check(!w.start([] {}), "a second job is refused while the first is in flight");
        check(until(w) && w.collect() && !w.busy(), "the job finishes and is collected once");
        check(ran != std::thread::id{} && ran != std::this_thread::get_id(),
              "it ran on the worker's own thread, not the caller's");
        check(!w.collect(), "collecting twice gives nothing");
    }
    {
        bench::SimWorker w;
        w.start([] { throw std::runtime_error("boom"); });
        until(w);
        bool rethrown = false;
        std::string msg;
        try { w.collect(); } catch (const std::runtime_error& e) { rethrown = true; msg = e.what(); }
        check(rethrown && msg == "boom" && !w.busy(),
              "an exception reaches the caller with its type and message, and frees the worker");
        w.start([] { throw std::bad_alloc(); });
        until(w);
        bool oom = false;
        try { w.collect(); } catch (const std::bad_alloc&) { oom = true; }
        check(oom, "out of memory arrives as out of memory, so the window's own handler still sees it");
        check(w.start([] {}) && until(w) && w.collect(), "and the worker still works after both");
    }
    {
        bench::SimWorker w;
        w.start([] { std::this_thread::sleep_for(100ms); });
        const auto t0 = std::chrono::steady_clock::now();
        w.wait();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        check(ms >= 80.0 && w.ready() && w.collect(), "wait() blocks until the job in flight has finished");
    }
    {
        std::atomic<bool> finished{false};
        {
            bench::SimWorker w;
            w.start([&] { std::this_thread::sleep_for(100ms); finished = true; });
        }
        check(finished.load(), "destroying the worker waits for the job in flight instead of abandoning it");
    }
    {
        bench::SimWorker w;
        long long total = 0;
        bool ok = true;
        // wait(), not until(): polling sleeps in whole scheduler ticks, about
        // 15.6 ms each at Windows' default timer resolution, so this loop took
        // 30 seconds or 2 depending on what else had raised the resolution.
        for (int i = 0; i < 2000 && ok; ++i) {
            ok = w.start([&total] { ++total; });
            if (ok) { w.wait(); ok = w.ready() && w.collect(); }
        }
        check(ok && total == 2000, "two thousand jobs in a row, each collected, none lost");
    }

    // ── THE property: a sim does not care which thread steps it ──────────────
    //
    // `a` is stepped alternately here and on the worker, strictly one after the
    // other — exactly the pattern the window will use. `b` never leaves this
    // thread. A sim that differs is re-run with both copies on one thread: if
    // THOSE differ too, the sim was not deterministic to begin with, which is
    // worth knowing but is not a threading fault and is reported separately.
    std::string threadDependent, nondeterministic;
    bench::SimWorker w;
    for (const auto& e : bench::registry()) {
        auto a = e.make();
        auto b = e.make();
        for (int i = 0; i < 12; ++i) {
            if (i % 2) { w.start([&] { a->step(); }); w.wait(); w.collect(); }
            else a->step();
            b->step();
        }
        if (same_world(*a, *b)) continue;
        auto c = e.make();
        auto d = e.make();
        for (int i = 0; i < 12; ++i) { c->step(); d->step(); }
        (same_world(*c, *d) ? threadDependent : nondeterministic) += " " + e.id;
    }
    check(threadDependent.empty(),
          "every library sim steps to the same world whichever thread steps it" +
          (threadDependent.empty() ? std::string(" (") + std::to_string(bench::registry().size()) + " sims)"
                                   : std::string(": these do not:") + threadDependent));
    if (!nondeterministic.empty())
        std::printf("note: not deterministic even on one thread, so not judged here:%s\n",
                    nondeterministic.c_str());

    std::printf("%d worker checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
