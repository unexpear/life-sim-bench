// stress_test.cpp — the max-speed path, exactly as the workbench drives it.
#include "registry.hpp"
#include "history.hpp"
#include "hardware.hpp"
#include <chrono>
#include <cstdio>
int main(){
    int fail=0; auto ck=[&](bool o,const std::string&m){ std::printf("    %s  %s\n",o?"ok  ":"FAIL",m.c_str()); if(!o)++fail; };
    auto hw = bench::Hardware::probe();
    bench::RateGuard guard; guard.configure(hw.stepBudgetMs, hw.maxStepsPerFrame);

    std::printf("\n  Max-speed loop (600 steps/s requested, 3 seconds of frames)\n");
    for (const char* id : {"life","loops","nowakmay","pps"}) {
        const bench::Entry* e=nullptr;
        for (auto& x : bench::registry()) if (x.id==id) e=&x;
        auto sim = e->make();
        bench::History h; h.configure(hw.historyBudget, 4);

        double acc=0, msPerStep=0; const double budget = 1.0/600.0;
        std::size_t steps=0; int frames=0, throttledFrames=0;
        const auto t_start = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now()-t_start).count() < 3000) {
            acc += 1.0/60.0;                       // a 60fps frame
            const double want = acc/budget;
            const int allowed = guard.allow(want, msPerStep);
            if (guard.throttled(want, allowed)) ++throttledFrames;
            const auto t0 = std::chrono::steady_clock::now();
            for (int i=0;i<allowed;++i){ sim->step(); acc-=budget; ++steps; }
            const auto t1 = std::chrono::steady_clock::now();
            if (allowed) {
                const double ms = std::chrono::duration<double,std::milli>(t1-t0).count();
                msPerStep = msPerStep*0.7 + (ms/allowed)*0.3;
            }
            h.observe(*sim);                        // ONCE PER FRAME
            if (acc > 1.0) acc = 1.0;
            ++frames;
        }
        const double snapsPerSec = double(h.depth())/3.0;
        std::printf("  %-9s %6zu steps  %4d frames  %.3f ms/step  %3zu snaps (%.0f/s)  %3zu MB%s\n",
            id, steps, frames, msPerStep, h.depth(), snapsPerSec,
            h.bytes()/(1024*1024), throttledFrames? "  [throttled]":"");
        ck(h.bytes() <= hw.historyBudget, std::string(id)+": stayed inside the memory budget");
        ck(snapsPerSec <= 45.0, std::string(id)+": snapshot rate is bounded by wall clock, not step rate");
        ck(frames > 20, std::string(id)+": the frame loop kept running (never wedged)");
    }
    std::printf("\n  %d failed\n", fail);
    return fail?1:0;
}
