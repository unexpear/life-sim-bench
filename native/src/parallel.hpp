// parallel.hpp — run independent work on several cores.
//
// Deliberately small. A general work-stealing pool would be more impressive and
// nothing here needs one: the two things worth parallelising in this bench are
// both embarrassingly parallel, and both have a natural chunk.
//
//   Rendering  — every pixel of a frame is independent, so rows split cleanly.
//   Repeats    — every seed of a sweep is an independent simulation that shares
//                nothing, which is exactly why several seeds are needed at all.
//
// The rule this file exists to enforce: **parallel must not change the answer.**
// A renderer that produces a slightly different image on eight threads than on
// one, or a sweep whose result depends on how many cores the machine has, is
// not faster — it is broken in a way that only shows up on someone else's
// computer. So the split is static and index-based, every task writes only to
// its own slice, and the self-test renders and sweeps both ways and compares
// them byte for byte.
//
// What is NOT parallelised, and why: stepping a cellular automaton. Every cell
// reads its neighbours, so a row split needs the previous generation intact at
// the boundary — doable with double buffering, which these already use, but the
// grids here are small enough that thread launch costs more than it saves.
// Measured before deciding, not assumed.

#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace bench {

// How many workers to use by default. One less than the hardware reports, so a
// long render leaves the machine usable — this is an interactive workbench and
// a frozen window is worse than a slower one.
// A global override, so a caller can force single-threaded execution.
//
// This exists for the tests and the benchmarks: the only way to show that
// threading did not change an answer is to compute the answer both ways, and
// the only way to show it is faster is to time both. 0 means "ask the machine".
inline unsigned& max_workers() { static unsigned n = 0; return n; }
inline void set_max_workers(unsigned n) { max_workers() = n; }

[[nodiscard]] inline unsigned default_workers() {
    if (max_workers() != 0) return max_workers();
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw <= 2) return 1;
    return std::min(hw - 1u, 16u);
}

// Run body(i) for i in [0, n), split across `workers` threads.
//
// Static contiguous chunks rather than an atomic counter. A shared counter is
// better when tasks vary wildly in cost; here they do not, and a static split
// has no synchronisation at all in the inner loop and — the part that matters —
// gives every index the same thread every run, which keeps anything the body
// does with thread-local state reproducible.
template <typename Body>
void parallel_for(std::size_t n, Body&& body, unsigned workers = 0) {
    if (n == 0) return;
    if (workers == 0) workers = default_workers();
    // An explicit request is still capped by the override, so forcing
    // single-threaded really is single-threaded everywhere.
    if (max_workers() != 0) workers = std::min(workers, max_workers());
    workers = std::max(1u, std::min<unsigned>(workers, unsigned(n)));
    if (workers == 1) {
        for (std::size_t i = 0; i < n; ++i) body(i);
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(workers - 1);
    const std::size_t chunk = (n + workers - 1) / workers;
    for (unsigned w = 1; w < workers; ++w) {
        const std::size_t lo = std::min(n, std::size_t(w) * chunk);
        const std::size_t hi = std::min(n, lo + chunk);
        if (lo >= hi) break;
        pool.emplace_back([lo, hi, &body] { for (std::size_t i = lo; i < hi; ++i) body(i); });
    }
    // The calling thread takes the first chunk rather than idling — one fewer
    // thread to create, and on a two-core machine that is half the work.
    for (std::size_t i = 0; i < std::min(n, chunk); ++i) body(i);
    for (auto& t : pool) t.join();
}

// Map over independent items, each producing a result.
//
// Results land in the returned vector by INDEX, never by completion order. A
// parallel map that appends as tasks finish returns a different order on every
// run and on every machine, which turns a reproducible measurement into a
// stream of noise that still looks plausible.
template <typename R, typename Fn>
[[nodiscard]] std::vector<R> parallel_map(std::size_t n, Fn&& fn, unsigned workers = 0) {
    std::vector<R> out(n);
    parallel_for(n, [&](std::size_t i) { out[i] = fn(i); }, workers);
    return out;
}

} // namespace bench
