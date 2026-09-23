// hardware.hpp — read the machine, then bound what the bench is allowed to do.
//
// Two different failure modes, and they need different kinds of limit:
//
//   MEMORY. The timeline keeps whole-field snapshots. Left unbounded that is a
//   slow leak with a nice UI; bounded by a hardcoded constant it is either
//   wasteful on a big machine or fatal on a small one. So the budget is a share
//   of what is actually free, clamped at both ends.
//
//   TIME. Stepping happens on the UI thread. A slow rule at a high step rate
//   will happily consume every frame and the window stops responding — the app
//   looks hung when it is merely obedient. So stepping gets a wall-clock budget
//   per frame, and if a single step blows it, the rate is throttled and the user
//   is told rather than left guessing.
//
// Everything here is advisory and inspectable. Nothing silently changes what a
// simulation computes — only how much of it we do per frame and how much we
// remember.

#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef near
#undef far
#endif

namespace bench {

struct Hardware {
    // measured
    unsigned      logicalCores = 1;
    std::uint64_t totalRam     = 0;   // bytes
    std::uint64_t availRam     = 0;   // bytes
    bool          onBattery    = false;
    std::string   note;               // human-readable summary

    // derived limits
    std::size_t   historyBudget   = 32u * 1024u * 1024u;  // snapshot memory
    std::size_t   maxFieldCells   = 4u * 1024u * 1024u;   // refuse to render past this
    unsigned      workers         = 1;                     // available for parallel work
    double        stepBudgetMs    = 8.0;                   // per frame, leaves room to draw
    int           maxStepsPerFrame = 2000;

    static Hardware probe() {
        Hardware h;
        h.logicalCores = std::max(1u, std::thread::hardware_concurrency());

#ifdef _WIN32
        MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            h.totalRam = ms.ullTotalPhys;
            h.availRam = ms.ullAvailPhys;
        }
        SYSTEM_POWER_STATUS ps{};
        if (GetSystemPowerStatus(&ps)) h.onBattery = (ps.ACLineStatus == 0);
#endif
        if (h.totalRam == 0) { h.totalRam = 4ull << 30; h.availRam = 2ull << 30; }
        if (h.availRam == 0) h.availRam = h.totalRam / 2;
        h.derive();
        return h;
    }

    // Everything below is a function of the facts above, so it lives in one
    // place and is recomputed whenever those facts are re-read.
    void derive() {
        Hardware& h = *this;

        // Memory: 8% of what is free right now, never less than 16 MB (a
        // timeline shorter than that is not worth having) and never more than
        // 512 MB (past that it is hoarding, not helping).
        const std::uint64_t share = h.availRam / 12;
        h.historyBudget = std::size_t(std::clamp<std::uint64_t>(share,
                              16ull * 1024 * 1024, 512ull * 1024 * 1024));

        // A field bigger than this is almost certainly a mistake in a plugin,
        // and rendering it would stall the UI before anyone saw the problem.
        h.maxFieldCells = std::size_t(std::clamp<std::uint64_t>(h.availRam / 256,
                              1ull << 20, 64ull << 20));

        // Leave one core for the UI and everything else on the machine. On
        // battery, take half — a bench that flattens a laptop is a bad bench.
        h.workers = std::max(1u, h.logicalCores > 1 ? h.logicalCores - 1 : 1u);
        if (h.onBattery) h.workers = std::max(1u, h.workers / 2);

        // 8 ms of stepping per 16 ms frame keeps the window responsive even
        // when a rule is expensive.
        h.stepBudgetMs     = h.onBattery ? 5.0 : 8.0;
        h.maxStepsPerFrame = h.onBattery ? 800 : 2000;

        char buf[256];
        std::snprintf(buf, sizeof buf,
            "%u cores · %.1f GB RAM (%.1f free) · %s · timeline %zu MB · step budget %.0f ms",
            h.logicalCores, double(h.totalRam) / 1073741824.0,
            double(h.availRam) / 1073741824.0,
            h.onBattery ? "on battery" : "on mains",
            h.historyBudget / (1024 * 1024), h.stepBudgetMs);
        h.note = buf;
    }

    // Refresh only the volatile parts. Free memory moves; core count does not.
    // refresh() used to update the raw facts and recompute NONE of the limits
    // derived from them, so every budget was frozen at whatever was true at
    // launch. The limits exist precisely because the machine changes.
    void refresh() {
#ifdef _WIN32
        MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) availRam = ms.ullAvailPhys;
        SYSTEM_POWER_STATUS ps{};
        if (GetSystemPowerStatus(&ps)) onBattery = (ps.ACLineStatus == 0);
#endif
        derive();     // limits track the machine, not launch-time facts
    }

    // Are we close enough to the edge that we should stop growing the timeline?
    [[nodiscard]] bool memoryTight() const {
        return availRam < (512ull * 1024 * 1024);
    }
};

// Adaptive rate limiter. Feeds on the measured cost of a step and decides how
// many are affordable this frame, so an expensive rule degrades to "slow" and
// never to "frozen".
class RateGuard {
public:
    void configure(double budgetMs, int hardCap) {
        budgetMs_ = budgetMs; hardCap_ = hardCap;
    }
    // How many steps to run this frame, given the wanted rate and elapsed time.
    [[nodiscard]] int allow(double wantSteps, double msPerStep) const {
        // Clamp BEFORE narrowing. The caller bounds its accumulator, but a
        // double-to-int conversion whose value does not fit in an int is
        // undefined behaviour, not a wrapped number — and this input is derived
        // from wall-clock deltas, which anything that blocks the UI thread can
        // make arbitrarily large: a blocking build, a modal window drag, sleep
        // or hibernate advancing the tick count. Defending here costs one
        // compare and does not depend on every caller staying careful.
        if (!(wantSteps >= 1.0)) return 0;                      // also rejects NaN
        int n = int(std::min(wantSteps, double(hardCap_)));
        if (msPerStep > 1e-6) {
            const int affordable = int(budgetMs_ / msPerStep);
            n = std::min(n, std::max(1, affordable));   // always allow one
        }
        return std::min(n, hardCap_);
    }
    // True when we could not keep up with what was asked.
    [[nodiscard]] bool throttled(double wantSteps, int allowed) const {
        if (!(wantSteps >= 1.0)) return false;                  // same narrowing hazard
        return allowed < int(std::min(wantSteps, double(hardCap_))) - 1;
    }
private:
    double budgetMs_ = 8.0;
    int    hardCap_  = 2000;
};

} // namespace bench
