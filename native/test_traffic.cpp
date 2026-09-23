// test_traffic.cpp — asserts what the Nagel-Schreckenberg model is PUBLISHED to
// do, and prints every number it measured getting there.
//
//   Nagel, K. & Schreckenberg, M. "A cellular automaton model for freeway
//   traffic", Journal de Physique I 2 (1992) 2221-2229.
//
// The load-bearing tests, in order of how much they would hurt to lose:
//
//   · THE ABLATION. Same uniform road, same density, rule 3 on and off. With it
//     off the road runs at vmax forever and never produces a single stopped
//     car; with it on it jams within ten seconds. This is the paper's whole
//     claim and it is run as a control, not argued.
//   · THE FUNDAMENTAL DIAGRAM. Swept here, at this machine's numbers, not
//     copied from the paper. The peak is reported as measured and compared to
//     the published window out loud.
//   · THE BACKWARD JAM. Measured twice: once in a controlled single-jam
//     experiment where the answer -(1-p) follows from the four rules on paper,
//     and once by cross-correlating a real jammed run, where the answer is
//     messier and is reported messy.
//
// What this file did NOT catch, and the bench's own brush test did: the sim
// seeded its cars at rest, so on a freshly reset road every car was already
// stopped and the click-to-brake stamp had nothing to change. The last section
// here now runs that same check, because a defect found once by a general test
// should be pinned down by a specific one.
//
// build (one line; a trailing backslash here is a -Wcomment warning, and this
// project builds at zero warnings):
//   g++ -std=c++20 -O2 -Wall -Wextra -Isrc test_traffic.cpp -o test_traffic.exe -static -static-libgcc -static-libstdc++

#include "sims/traffic.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace bench;

static int g_checks = 0;
static void ok(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { std::printf("  FAIL: %s\n", what); std::fflush(stdout); std::abort(); }
}

static void head(const char* title) {
    std::printf("\n== %s ==\n", title);
}

// ── shared measurement helpers ──────────────────────────────────────────────

// Mean flow in the steady state: warm up, then average q = sum(v)/L.
struct FlowResult { double q = 0, speed = 0, stoppedFrac = 0, detector = 0; };

static FlowResult measure_flow(int L, int n, int vmax, float p, bool det,
                               int warm, int meas, std::uint64_t seed) {
    NaSch r; r.set_vmax(vmax); r.set_p(p); r.set_deterministic(det);
    r.seed_random(L, n, seed);
    for (int i = 0; i < warm; ++i) r.step();
    FlowResult out;
    long long crossings = 0;
    for (int i = 0; i < meas; ++i) {
        r.step();
        out.q += r.flow();
        out.speed += r.mean_speed();
        out.stoppedFrac += n ? double(r.stopped()) / n : 0.0;
        crossings += r.crossings();
    }
    const double m = double(meas);
    out.q /= m; out.speed /= m; out.stoppedFrac /= m;
    out.detector = double(crossings) / m;
    return out;
}

// A whole fundamental diagram: densities and the flow at each.
struct Diagram {
    std::vector<double> rho, q;
    double peakQ = 0, peakRho = 0;
};

static Diagram sweep_diagram(int L, int vmax, float p, bool det,
                             const std::vector<int>& carCounts,
                             int warm, int meas, std::uint64_t seed) {
    Diagram d;
    for (int n : carCounts) {
        const double q = measure_flow(L, n, vmax, p, det, warm, meas, seed).q;
        d.rho.push_back(double(n) / double(L));
        d.q.push_back(q);
        if (q > d.peakQ) { d.peakQ = q; d.peakRho = double(n) / double(L); }
    }
    return d;
}

// FNV-1a over the published field, so "the picture changed" is a fact and not
// an impression.
static std::uint64_t field_hash(const Sim& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (auto c : s.field().cells) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// Drive a knob the way the workbench drives one: write the value, notify the
// sim, and re-seed if the knob says it only means anything at reset.
static void set_knob(Sim& s, const char* key, float v) {
    bool needReset = false;
    for (auto& kn : s.knobs())
        if (kn.key == key) { kn.value = kn.quantised(v); needReset = kn.on_reset; }
    s.on_knob(key, v);
    if (needReset) s.reset();
}

// ── section 1: the four rules, hand-checkable ───────────────────────────────

static void test_rules() {
    head("rule 1 (acceleration): an isolated car gains exactly one cell/step");
    {
        NaSch r; r.set_vmax(5); r.set_p(0.f);
        r.seed_random(1000, 1, 1);
        const int want[8] = {1, 2, 3, 4, 5, 5, 5, 5};
        for (int i = 0; i < 8; ++i) {
            r.step();
            std::printf("  step %d  v = %d  (want %d)\n", i + 1, r.velocities()[0], want[i]);
            ok(r.velocities()[0] == want[i], "acceleration ladder to vmax then hold");
        }
    }

    head("rule 2 (braking): no car ever reaches the cell in front of it");
    {
        // Dense, noisy, long. If braking to the gap is wrong by one, two cars
        // land on the same cell and this finds it.
        const int L = 1000, n = 500;
        NaSch r; r.set_vmax(5); r.set_p(0.3f);
        r.seed_random(L, n, 0x7A11Cull);
        std::vector<int> stamp(std::size_t(L), -1);
        std::vector<int> gapBefore(std::size_t(n), 0);
        long long overGap = 0, collisions = 0;
        for (int t = 0; t < 20000; ++t) {
            // gaps as they stand BEFORE the step, so rule 2 can be checked
            // against the number it was actually given
            const auto& pos = r.positions();
            for (std::size_t i = 0; i < pos.size(); ++i) {
                int g = pos[(i + 1) % pos.size()] - pos[i] - 1;
                if (g < 0) g += L;
                gapBefore[i] = g;
            }
            r.step();
            const auto& np = r.positions();
            const auto& nv = r.velocities();
            ok(int(np.size()) == n, "cars are conserved");
            for (std::size_t i = 0; i < np.size(); ++i) {
                if (nv[i] > gapBefore[i]) ++overGap;
                if (np[i] < 0 || np[i] >= L) ++collisions;
                if (stamp[std::size_t(np[i])] == t) ++collisions;
                stamp[std::size_t(np[i])] = t;
            }
        }
        std::printf("  20000 steps, %d cars on %d cells: %lld velocities above the gap, "
                    "%lld collisions or out-of-range positions\n", n, L, overGap, collisions);
        ok(overGap == 0, "velocity never exceeds the gap it was given");
        ok(collisions == 0, "no two cars share a cell, ever");
    }

    head("rule 3 (randomisation): p = 1 is the rule firing every single step");
    {
        // From rest with p = 1: accelerate to 1, then lose it again, forever.
        NaSch a; a.set_vmax(5); a.set_p(1.0f);
        a.seed_uniform(1000, 100, 0, 1);
        for (int i = 0; i < 500; ++i) a.step();
        std::printf("  from rest: mean speed %.4f, flow %.4f (want exactly 0)\n",
                    a.mean_speed(), a.flow());
        ok(a.mean_speed() == 0.0 && a.flow() == 0.0, "p=1 from rest pins every car at zero");

        // From vmax with a free road: accelerate is already capped, so the
        // dawdle takes it to vmax-1 and it settles there. Same p, opposite
        // answer — which is what proves the draw is really per-step.
        NaSch b; b.set_vmax(5); b.set_p(1.0f);
        b.seed_uniform(1000, 100, 5, 1);
        for (int i = 0; i < 500; ++i) b.step();
        std::printf("  from vmax: mean speed %.4f (want exactly 4 = vmax-1)\n", b.mean_speed());
        ok(b.mean_speed() == 4.0, "p=1 from vmax settles at vmax-1");
    }

    head("rules 1+3 together: an isolated car's mean speed is exactly vmax - p");
    {
        // Follows from the rules with no simulation needed: with a free road
        // ahead, rule 1 restores v to vmax every step whatever rule 3 did, so
        // the velocity after the step is vmax minus a Bernoulli(p). Measured
        // over 200,000 seconds of one car.
        for (float p : {0.0f, 0.2f, 0.3f, 0.5f}) {
            NaSch r; r.set_vmax(5); r.set_p(p);
            r.seed_random(10000, 1, 0x7A11Cull);
            double s = 0;
            const int T = 200000;
            for (int i = 0; i < T; ++i) { r.step(); s += r.mean_speed(); }
            const double got = s / T, want = 5.0 - double(p);
            std::printf("  p %.2f  mean v %.4f  (rules give %.4f, error %+.4f)\n",
                        double(p), got, want, got - want);
            ok(std::fabs(got - want) < 0.01, "free-flow mean speed is vmax - p");
        }
    }

    head("the rule switch is exactly p = 0, not an approximation of it");
    {
        NaSch a; a.set_vmax(5); a.set_p(0.3f); a.set_deterministic(true);
        NaSch b; b.set_vmax(5); b.set_p(0.0f); b.set_deterministic(false);
        a.seed_random(500, 90, 99); b.seed_random(500, 90, 99);
        bool same = true;
        for (int i = 0; i < 5000 && same; ++i) {
            a.step(); b.step();
            same = (a.positions() == b.positions()) && (a.velocities() == b.velocities());
        }
        std::printf("  5000 steps, deterministic switch vs p=0 slider: %s\n",
                    same ? "bit-identical" : "DIFFERENT");
        ok(same, "the ablation switch is the p=0 model exactly");
    }

    head("determinism: same seed, same run, every time");
    {
        auto trace = [] {
            NaSch r; r.set_vmax(5); r.set_p(0.3f);
            r.seed_random(777, 140, 0xBEEFull);
            std::uint64_t h = 1469598103934665603ull;
            for (int i = 0; i < 3000; ++i) {
                r.step();
                for (int v : r.velocities()) { h ^= std::uint64_t(v + 1); h *= 1099511628211ull; }
                for (int x : r.positions())  { h ^= std::uint64_t(x + 1); h *= 1099511628211ull; }
            }
            return h;
        };
        const std::uint64_t a = trace(), b = trace();
        std::printf("  trajectory hash %016llx / %016llx\n",
                    (unsigned long long)a, (unsigned long long)b);
        ok(a == b, "identical trajectories from an identical seed");
    }
}

// ── section 2: the fundamental diagram, measured here ───────────────────────

static void test_fundamental_diagram() {
    head("deterministic (p = 0): the diagram is two straight lines");
    {
        // The published deterministic result: free flow q = vmax*rho up to
        // rho = 1/(vmax+1), then the jammed branch q = 1-rho. Wikipedia's
        // standard restatement of the model gives the corner for vmax=5 at
        // rho = 1/6 = 0.167. This is the one place a closed form exists, so it
        // is asserted to the last digit rather than to a tolerance.
        const int L = 1000;
        std::vector<int> counts;
        for (int n = 20; n <= 980; n += 20) counts.push_back(n);
        const Diagram d = sweep_diagram(L, 5, 0.f, true, counts, 10000, 2000, 0x7A11Cull);
        double worst = 0; double worstRho = 0;
        for (std::size_t i = 0; i < d.rho.size(); ++i) {
            const double pred = std::min(5.0 * d.rho[i], 1.0 - d.rho[i]);
            const double e = std::fabs(d.q[i] - pred);
            if (e > worst) { worst = e; worstRho = d.rho[i]; }
        }
        std::printf("  49 densities, 0.02 to 0.98: worst deviation from min(5*rho, 1-rho) "
                    "is %.3e at rho %.2f\n", worst, worstRho);
        ok(worst < 1e-9, "deterministic diagram is exactly min(vmax*rho, 1-rho)");

        // The corner itself, at the exact density the closed form names.
        const FlowResult at = measure_flow(1200, 200, 5, 0.f, true, 10000, 2000, 0x7A11Cull);
        std::printf("  at rho = 1/6 exactly: q = %.6f (5/6 = %.6f), mean speed %.4f, "
                    "stopped %.4f\n", at.q, 5.0 / 6.0, at.speed, at.stoppedFrac);
        ok(std::fabs(at.q - 5.0 / 6.0) < 1e-9, "deterministic peak flow is vmax/(vmax+1)");
        ok(at.stoppedFrac == 0.0, "deterministic road at the corner has no stopped car");

        // And the corner MOVES with vmax the way 1/(vmax+1) says it does.
        std::printf("  critical density against vmax, deterministic:\n");
        for (int vmax = 1; vmax <= 5; ++vmax) {
            // Measured, not assumed: find the density with the highest flow on
            // a grid fine enough to resolve 1/(vmax+1).
            double bestQ = -1, bestRho = 0;
            for (int n = 5; n <= 700; n += 5) {
                const double q = measure_flow(1000, n, vmax, 0.f, true, 4000, 1000, 0x7A11Cull).q;
                if (q > bestQ) { bestQ = q; bestRho = double(n) / 1000.0; }
            }
            const double want = 1.0 / (vmax + 1);
            std::printf("    vmax %d  peak q %.4f at rho %.3f  (1/(vmax+1) = %.3f)\n",
                        vmax, bestQ, bestRho, want);
            ok(std::fabs(bestRho - want) <= 0.005 + 1e-9, "deterministic peak sits at 1/(vmax+1)");
            ok(std::fabs(bestQ - double(vmax) / (vmax + 1)) < 0.02,
               "deterministic peak flow is vmax/(vmax+1)");
        }
    }

    head("stochastic: the fundamental diagram this machine measures");
    {
        const int L = 1000;
        std::vector<int> counts;
        for (int n = 5;  n <  200; n += 5)  counts.push_back(n);    // fine near the peak
        for (int n = 200; n <= 950; n += 25) counts.push_back(n);
        for (float p : {0.3f, 0.5f}) {
            const Diagram d = sweep_diagram(L, 5, p, false, counts, 3000, 5000, 0x7A11Cull);
            std::printf("\n  p = %.2f, vmax = 5, L = %d, 3000 warmup + 5000 measured steps\n",
                        double(p), L);
            std::printf("    rho     q       veh/h/lane\n");
            for (std::size_t i = 0; i < d.rho.size(); ++i)
                if (d.rho[i] <= 0.2 + 1e-9 || i % 4 == 0)
                    std::printf("    %.3f   %.4f    %6.0f\n", d.rho[i], d.q[i], d.q[i] * 3600.0);
            std::printf("    MEASURED PEAK: q = %.4f at rho = %.3f  ->  %.0f veh/h/lane\n",
                        d.peakQ, d.peakRho, d.peakQ * 3600.0);

            // The published shape, checked as a shape: rising, then a maximum,
            // then falling.
            ok(d.q.front() < d.peakQ, "there is a rising free-flow branch below the peak");
            ok(d.q.back() < d.peakQ * 0.5, "there is a falling congested branch above it");
            for (std::size_t i = 1; i < 8; ++i)
                ok(d.q[i] > d.q[i - 1], "flow increases with density in free flow");
            // Decline past the peak, sampled coarsely on purpose. The measured
            // curve has a noisy PLATEAU just above the peak — at p = 0.3 the
            // flow wanders between 0.458 and 0.467 across rho 0.11 to 0.15 —
            // so a point-by-point monotonicity assertion would be asserting
            // that the noise is small, not that the branch falls. Sampling four
            // widely separated densities tests the shape and nothing else.
            double prev = 1e9;
            for (int n : {300, 500, 700, 900}) {
                const double q = measure_flow(L, n, 5, p, false, 3000, 5000, 0x7A11Cull).q;
                std::printf("    congested branch: rho %.2f -> q %.4f\n", double(n) / L, q);
                ok(q < prev, "flow falls as density rises on the congested branch");
                prev = q;
            }
            ok(d.peakQ > prev, "the peak is above the congested branch");

            // The task's stated published window for the peak is rho 0.08-0.15.
            // Reported against, not assumed into.
            const bool inWindow = d.peakRho >= 0.08 - 1e-9 && d.peakRho <= 0.15 + 1e-9;
            std::printf("    published window for the critical density is 0.08-0.15: "
                        "measured %.3f is %s it\n", d.peakRho, inWindow ? "INSIDE" : "OUTSIDE");
            ok(inWindow, "measured critical density falls in the published window");
            ok(d.peakQ < 5.0 / 6.0, "noise costs capacity: peak is below the deterministic 5/6");
        }
    }

    head("the steady state does not remember how the road was seeded");
    {
        // The sim seeds its cars at vmax so the click-to-brake stamp has
        // something to change; the sweeps above seed them at rest. That is only
        // allowed if the fundamental diagram cannot tell the difference, which
        // is a claim, so it is measured.
        for (int n : {60, 150, 400}) {
            const double atRest = measure_flow(1000, n, 5, 0.3f, false, 3000, 20000, 0x7A11Cull).q;
            NaSch r; r.set_vmax(5); r.set_p(0.3f);
            r.seed_random(1000, n, 0x7A11Cull, 5);
            for (int i = 0; i < 3000; ++i) r.step();
            double moving = 0;
            for (int i = 0; i < 20000; ++i) { r.step(); moving += r.flow(); }
            moving /= 20000;
            std::printf("  rho %.2f  seeded at rest q %.4f   seeded at vmax q %.4f   "
                        "difference %+.4f\n", double(n) / 1000.0, atRest, moving, moving - atRest);
            ok(std::fabs(moving - atRest) < 0.01, "steady-state flow is initial-condition free");
        }
    }

    head("the two ways of measuring flow agree");
    {
        // sum(v)/L averages over every possible detector position; crossings
        // counts one real detector at the ring seam. They are the same
        // quantity, and if they disagree one of them is wrong.
        for (int n : {60, 150, 400}) {
            const FlowResult f = measure_flow(1000, n, 5, 0.3f, false, 3000, 40000, 0x7A11Cull);
            std::printf("  rho %.2f  space-averaged q %.4f   single detector %.4f   "
                        "difference %+.4f\n", double(n) / 1000.0, f.q, f.detector, f.detector - f.q);
            ok(std::fabs(f.detector - f.q) < 0.01, "detector flow matches the space average");
        }
    }
}

// ── section 3: the backward-travelling jam ──────────────────────────────────

static void test_backward_jam() {
    head("controlled: one compact jam, front retreating at -(1-p)");
    {
        // The derivation, from the four rules and nothing else: cars leave a
        // compact jam only from its downstream end, at most one per timestep,
        // each succeeding iff rule 3 does not fire. Every departure moves the
        // front back one cell. So the front velocity is -(1-p) cells/step.
        for (float p : {0.0f, 0.1f, 0.2f, 0.3f, 0.5f}) {
            const int L = 4000, N = 500, target = N - 20;
            NaSch r; r.set_vmax(5); r.set_p(p);
            r.seed_compact(L, N, 0, 0x7A11Cull);
            const std::vector<int> init = r.positions();
            int steps = 0, moved = 0;
            while (moved < target && steps < 200000) {
                r.step(); ++steps;
                moved = 0;
                const auto& pos = r.positions();
                for (std::size_t i = 0; i < pos.size(); ++i) if (pos[i] != init[i]) ++moved;
            }
            const double v = -double(moved) / double(steps);
            const double want = -(1.0 - double(p));
            std::printf("  p %.2f  %d cars out in %d steps  front velocity %+.4f  "
                        "(rules give %+.4f, error %+.4f)  = %+.1f km/h\n",
                        double(p), moved, steps, v, want, v - want, v * 7.5 * 3.6);
            ok(v < 0.0, "the jam front moves backwards");
            ok(std::fabs(v - want) < 0.03, "front velocity is -(1-p) cells/step");
        }
    }

    head("in a real jammed run: cars go forwards, the pattern goes backwards");
    {
        // Cross-correlate the stopped-car pattern with itself `lag` steps later
        // and find the shift that lines them up. No structure is assumed; the
        // shift is searched over.
        const int L = 500, n = 150, lag = 10, rows = 1500;
        NaSch r; r.set_vmax(5); r.set_p(0.3f);
        r.seed_random(L, n, 0x7A11Cull);
        for (int i = 0; i < 3000; ++i) r.step();
        std::vector<std::vector<unsigned char>> m(std::size_t(rows),
                                                  std::vector<unsigned char>(std::size_t(L), 0));
        double meanSpeed = 0;
        for (int t = 0; t < rows; ++t) {
            r.step();
            meanSpeed += r.mean_speed();
            const auto& pos = r.positions();
            const auto& vel = r.velocities();
            for (std::size_t i = 0; i < pos.size(); ++i)
                if (vel[i] == 0) m[std::size_t(t)][std::size_t(pos[i])] = 1;
        }
        meanSpeed /= rows;

        long long best = -1; int bestShift = 0;
        for (int s = -3 * lag; s <= 2 * lag; ++s) {
            long long c = 0;
            for (int t = 0; t + lag < rows; ++t) {
                const auto& a = m[std::size_t(t)];
                const auto& b = m[std::size_t(t + lag)];
                for (int x = 0; x < L; ++x)
                    if (a[std::size_t(x)]) {
                        int y = (x + s) % L; if (y < 0) y += L;
                        c += b[std::size_t(y)];
                    }
            }
            if (c > best) { best = c; bestShift = s; }
        }
        const double jamV = double(bestShift) / double(lag);
        std::printf("  L %d, rho %.2f, p 0.30, %d steps, lag %d\n", L, double(n) / L, rows, lag);
        std::printf("  cars:    mean speed %+.3f cells/step = %+.0f km/h\n",
                    meanSpeed, meanSpeed * 7.5 * 3.6);
        std::printf("  pattern: %+.3f cells/step = %+.0f km/h  (best shift %d cells over %d steps)\n",
                    jamV, jamV * 7.5 * 3.6, bestShift, lag);
        std::printf("  the two have OPPOSITE SIGN, which is the whole result\n");
        ok(meanSpeed > 0.5, "cars are moving forwards");
        ok(jamV < 0.0, "the jam pattern is moving backwards");
        // Shallower than the ideal -(1-p) = -0.70 because this mask counts every
        // stopped car, and at this density many are one-off dawdles rather than
        // members of a compact jam. Reported rather than tuned away.
        std::printf("  ideal single-jam front for p=0.30 is -0.700; the mixed pattern here "
                    "measures %+.3f\n", jamV);
        ok(jamV > -1.05 && jamV < -0.2, "jam pattern velocity is in the plausible band");
    }
}

// ── section 4: the ablation, as a control ───────────────────────────────────

static void test_ablation() {
    head("the ablation: identical uniform road, rule 3 off and on");
    {
        // rho = 1/6 exactly: 200 cars on 1200 cells, spacing 6, gap 5 = vmax.
        // The tightest road on which the deterministic model still runs free.
        const int L = 1200, N = 200, T = 20000;
        for (int mode = 0; mode < 2; ++mode) {
            NaSch r; r.set_vmax(5); r.set_p(0.3f); r.set_deterministic(mode == 0);
            r.seed_uniform(L, N, 5, 0x7A11Cull);
            int maxStopped = 0, firstStop = -1;
            double q = 0, minSpeed = 99;
            for (int t = 0; t < T; ++t) {
                r.step();
                q += r.flow();
                minSpeed = std::min(minSpeed, r.mean_speed());
                if (r.stopped() > maxStopped) maxStopped = r.stopped();
                if (firstStop < 0 && r.stopped() > 0) firstStop = t + 1;
            }
            q /= T;
            std::printf("  %-24s mean q %.4f (%4.0f veh/h)  worst mean speed %.3f  "
                        "most stopped %3d  first stop at step %d\n",
                        mode == 0 ? "rule 3 OFF (p = 0)" : "rule 3 ON  (p = 0.3)",
                        q, q * 3600.0, minSpeed, maxStopped, firstStop);
            if (mode == 0) {
                ok(maxStopped == 0, "with rule 3 off, not one car stops in 20000 steps");
                ok(minSpeed == 5.0, "with rule 3 off, every car sits at vmax the whole time");
                ok(std::fabs(q - 5.0 / 6.0) < 1e-12, "with rule 3 off, flow is exactly vmax/(vmax+1)");
            } else {
                ok(firstStop > 0 && firstStop < 100, "with rule 3 on, a jam nucleates within 100 s");
                ok(maxStopped >= 20, "with rule 3 on, jams grow to a real size");
                ok(q < 0.75 * (5.0 / 6.0), "with rule 3 on, the road loses a quarter of its capacity");
            }
        }
        std::printf("  the difference between those two lines is the entire content of the "
                    "1992 paper\n");
    }

    head("the same thing, drawn (L = 120, 200 cars/km, 44 seconds of road)");
    {
        // '.' empty, '0'..'5' a car at that velocity. Time runs down. A car is a
        // stripe leaning right; a jam is a band of 0s leaning LEFT.
        for (int mode = 0; mode < 2; ++mode) {
            NaSch r; r.set_vmax(5); r.set_p(0.3f); r.set_deterministic(mode == 0);
            r.seed_uniform(120, 20, 5, 0x7A11Cull);
            std::printf("\n  %s   (rho = 1/6, 1 char = 7.5 m, 1 row = 1 s)\n",
                        mode == 0 ? "rule 3 OFF - no jam is possible"
                                  : "rule 3 ON  - jams appear and lean the other way");
            for (int t = 0; t < 44; ++t) {
                std::string row(120, '.');
                const auto& pos = r.positions();
                const auto& vel = r.velocities();
                for (std::size_t i = 0; i < pos.size(); ++i)
                    row[std::size_t(pos[i])] = char('0' + vel[i]);
                std::printf("  |%s|\n", row.c_str());
                r.step();
            }
        }
    }
}

// ── section 5: every knob changes the simulation ────────────────────────────

// Run the sim a while and report something physical plus a hash of the picture.
struct KnobProbe { double q = 0, speed = 0; std::uint64_t hash = 0; int w = 0, cars = 0; };

static KnobProbe probe(Traffic& s, int steps) {
    for (int i = 0; i < steps; ++i) s.step();
    KnobProbe k;
    for (const auto& m : s.metrics()) {
        if (m.name == "flow q (cars/cell/step)")  k.q = m.value;
        if (m.name == "mean speed (cells/step)")  k.speed = m.value;
    }
    k.hash = field_hash(s);
    k.w = s.field().w;
    k.cars = s.road().cars();
    return k;
}

static void test_knobs() {
    head("every knob changes the simulation, with a measured number for each");

    struct Case { const char* key; std::vector<float> values; };
    const std::vector<Case> cases = {
        {"density", {0.05f, 0.15f, 0.45f, 0.85f}},
        {"vmax",    {1.f, 2.f, 3.f, 4.f, 5.f}},
        {"p",       {0.f, 0.25f, 0.5f, 1.0f}},
        {"rule",    {0.f, 1.f}},
        {"length",  {0.f, 1.f, 2.f, 3.f}},
    };

    for (const auto& c : cases) {
        std::printf("\n  %s\n", c.key);
        std::vector<KnobProbe> probes;
        for (float v : c.values) {
            Traffic s;
            // p only means anything while the rule switch is on stochastic, and
            // the knob declares that dependency; hold it there so the probe is
            // measuring what it thinks it is.
            set_knob(s, "rule", 0.f);
            set_knob(s, c.key, v);
            const KnobProbe k = probe(s, 600);
            std::string shown;
            for (const auto& kn : s.knobs()) if (kn.key == c.key) shown = kn.shown();
            std::printf("    %-32s -> flow %.4f  mean speed %.3f  L %4d  cars %4d  "
                        "field %016llx\n",
                        shown.c_str(), k.q, k.speed, k.w, k.cars, (unsigned long long)k.hash);
            probes.push_back(k);
        }
        for (std::size_t i = 1; i < probes.size(); ++i) {
            ok(probes[i].hash != probes[i - 1].hash, "knob changes the field");
            // A hash change on its own could be nothing but a reshuffled soup,
            // so every step of every knob also has to move something PHYSICAL:
            // the flow, the mean speed, the length of the road, or the number
            // of cars on it.
            const KnobProbe& a = probes[i - 1];
            const KnobProbe& b = probes[i];
            const bool moved = std::fabs(a.q - b.q) > 1e-6
                            || std::fabs(a.speed - b.speed) > 1e-6
                            || a.w != b.w || a.cars != b.cars;
            ok(moved, "knob changes a measured quantity, not just the picture");
        }
    }

    head("what the road length knob does, and what it deliberately does NOT do");
    {
        // Worth stating rather than hiding: L barely moves the MEAN flow, and
        // that is correct physics, not a dead knob. Flow is intensive — cars
        // per cell per step — so doubling the road doubles the cars and leaves
        // the ratio alone. What L changes is the size of the FLUCTUATIONS: a
        // short ring holds few independent jams, so its flow swings about; a
        // long one averages them out. Measured below, against 1/sqrt(L).
        double prevSd = 1e9, firstMean = 0;
        for (int L : {256, 512, 1024, 2048}) {
            const int n = int(0.15 * L + 0.5);
            NaSch r; r.set_vmax(5); r.set_p(0.3f);
            r.seed_random(L, n, 0x7A11Cull);
            for (int i = 0; i < 3000; ++i) r.step();
            double s = 0, ss = 0;
            const int T = 20000;
            for (int i = 0; i < T; ++i) {
                r.step();
                const double q = r.flow();
                s += q; ss += q * q;
            }
            const double mean = s / T;
            const double sd = std::sqrt(std::max(0.0, ss / T - mean * mean));
            if (firstMean == 0) firstMean = mean;
            std::printf("  L %4d  mean q %.4f  sd %.4f  sd*sqrt(L) %.3f\n",
                        L, mean, sd, sd * std::sqrt(double(L)));
            ok(std::fabs(mean - firstMean) < 0.02, "mean flow is length-independent, as it should be");
            ok(sd < prevSd, "a longer road has smaller flow fluctuations");
            prevSd = sd;
        }
    }

    head("the declared dependency is real: p is inert while the rule switch is off");
    {
        std::uint64_t first = 0;
        for (float v : {0.f, 0.3f, 0.9f}) {
            Traffic s;
            set_knob(s, "rule", 1.f);       // deterministic
            set_knob(s, "p", v);
            const std::uint64_t h = probe(s, 400).hash;
            if (v == 0.f) first = h;
            std::printf("  rule = deterministic, p = %.1f -> field %016llx\n",
                        double(v), (unsigned long long)h);
            ok(h == first, "p does nothing while rule 3 is switched off");
        }
        std::printf("  which is exactly what the p knob's requires_key says, so the panel can "
                    "call it inert instead of leaving it looking broken\n");
    }
}

// ── section 6: the picture is the state ─────────────────────────────────────

static void test_display() {
    head("the space-time diagram");
    {
        Traffic s;
        ok(s.field().h == Traffic::kHistory, "256 rows of history");
        ok(s.field().w == 512, "default road is 512 cells wide");

        for (int i = 0; i < 400; ++i) s.step();

        // The bottom row is exactly the road as it stands now.
        const Field& f = s.field();
        const int y = f.h - 1;
        std::vector<std::uint8_t> want(std::size_t(f.w), 0);
        const auto& pos = s.road().positions();
        const auto& vel = s.road().velocities();
        for (std::size_t i = 0; i < pos.size(); ++i)
            want[std::size_t(pos[i])] = std::uint8_t(1 + std::min(vel[i], 5));
        int wrong = 0;
        for (int x = 0; x < f.w; ++x) if (f.at(x, y) != want[std::size_t(x)]) ++wrong;
        std::printf("  bottom row vs the live road: %d cells differ\n", wrong);
        ok(wrong == 0, "the newest row of the diagram is the current road");

        // Time really scrolls: the row that was at the bottom is one up now.
        std::vector<std::uint8_t> before(f.cells.begin() + std::size_t(y) * f.w, f.cells.end());
        s.step();
        std::vector<std::uint8_t> after(f.cells.begin() + std::size_t(y - 1) * f.w,
                                        f.cells.begin() + std::size_t(y) * f.w);
        ok(before == after, "the diagram scrolls up by exactly one row per step");
        std::printf("  the previous bottom row is now row %d, unchanged\n", y - 1);

        // And it is not a blank or a solid block.
        std::size_t cars = 0, stopped = 0;
        for (auto c : f.cells) { if (c) ++cars; if (c == 1) ++stopped; }
        const double fill = double(cars) / double(f.cells.size());
        std::printf("  %zu of %zu cells hold a car (%.3f of the diagram; density knob is 0.15), "
                    "%zu of those are stopped\n", cars, f.cells.size(), fill, stopped);
        ok(std::fabs(fill - 0.15) < 0.02, "the diagram is as full as the density says");
        ok(stopped > 0, "there are stopped cars to see");

        // Palette covers every velocity the vmax knob can produce.
        ok(s.palette().size() == 7, "one swatch for empty road and one per velocity 0..5");
        std::printf("  subtitle: %s\n", s.subtitle().c_str());
    }

    head("poke: click the road, make a jam");
    {
        // Exactly the check the bench's own brush test runs, kept here because
        // that is where this was caught: with cars seeded at rest the stamp had
        // nothing to change on a fresh road and silently did nothing. There is
        // no state weaker than stopped in this model, so the fix was to seed the
        // road moving rather than to invent a stronger brake.
        Traffic s;
        const int movingBefore = s.road().cars() - s.road().stopped();
        std::printf("  on a freshly reset road, %d of %d cars are moving\n",
                    movingBefore, s.road().cars());
        ok(movingBefore == s.road().cars(), "a fresh road is running, not stalled");

        ok(s.poke(0.5f, 0.5f), "poke finds a car");
        s.step();
        Traffic fresh;
        fresh.step();
        std::printf("  after one step, poked field %016llx vs untouched %016llx\n",
                    (unsigned long long)field_hash(s), (unsigned long long)field_hash(fresh));
        ok(s.field().cells != fresh.field().cells, "the stamp changes the outcome");

        // And it does what it says: the jam it starts is still there later.
        for (int i = 0; i < 200; ++i) { s.step(); fresh.step(); }
        std::printf("  200 steps later: poked road has %d stopped cars, untouched has %d\n",
                    s.road().stopped(), fresh.road().stopped());
        ok(s.field().cells != fresh.field().cells, "and the difference persists");
    }
}

int main() {
    std::printf("test_traffic — Nagel-Schreckenberg, J. Phys. I France 2 (1992) 2221-2229\n");
    std::printf("scale: 1 cell = 7.5 m, 1 step = 1 s, vmax 5 = 135 km/h\n");

    test_rules();
    test_fundamental_diagram();
    test_backward_jam();
    test_ablation();
    test_knobs();
    test_display();

    std::printf("\n%d checks passed\n", g_checks);
    return 0;
}
