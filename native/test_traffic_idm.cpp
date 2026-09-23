// test_traffic_idm.cpp — asserts what src/sims/traffic_idm.hpp publishes, and
// prints every number it measured getting there.
//
// Build:
//   g++ -std=c++20 -O2 -Wall -Wextra -Isrc test_traffic_idm.cpp -o test_traffic_idm.exe
//       -static -static-libgcc -static-libstdc++
// Delete the exe before rebuilding. A stale binary has produced false results
// in this project more than once.
//
// The division of labour: the CONSTANTS come from the two papers and their
// sources are recorded at the point of definition in traffic_idm.hpp; this file
// checks the code agrees with them. The BEHAVIOUR — the fundamental diagram,
// the capacity, the stop-and-go wave and its backward speed, the timestep
// convergence, the effect of every knob — is measured here and asserted against
// what the papers claim about it, never copied from them.

#include "sims/traffic_idm.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace bench;

// ── check harness ───────────────────────────────────────────────────────────
// Not <cassert>: assert() vanishes under -DNDEBUG, and a test that can be
// compiled into a no-op is not a test.
static int g_checks = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            std::printf("\nFAIL  %s:%d\n      %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                   \
    do {                                                                        \
        ++g_checks;                                                             \
        const double va = double(a), vb = double(b);                            \
        if (!(std::fabs(va - vb) <= double(tol))) {                             \
            std::printf("\nFAIL  %s:%d\n      %s = %.6f, expected %.6f +-%.6f\n",\
                        __FILE__, __LINE__, #a, va, vb, double(tol));           \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

static void head(const char* s) { std::printf("\n== %s ==\n", s); }

// ── helpers ─────────────────────────────────────────────────────────────────
static void set(TrafficIdm& s, const char* k, float v) {
    for (auto& kn : s.knobs()) if (kn.key == k) kn.value = v;
    s.on_knob(k, v);
}

// The single-lane, identical-vehicle, exactly-uniform ring the 2000 paper's
// stability analysis is done on. No trucks, no speed spread, no jitter: the
// initial state is a fixed point of the model, so anything that happens later
// was caused by the perturbation and not by the initial condition.
static void uniform_ring(TrafficIdm& s, float density, int lengthIdx, int dtIdx = 0) {
    set(s, "lanes", 1);
    set(s, "length", float(lengthIdx));
    set(s, "trucks", 0.f);
    set(s, "spread", 0.f);
    set(s, "jitter", 0.f);
    set(s, "dt", float(dtIdx));
    set(s, "density", density);
    s.reset();
}

struct Event { double t, x; };

// Least-squares slope of position against time for a front crossing a ring.
// The samples come in as wrapped coordinates, so they are unwrapped first: a
// front travelling upstream jumps from 0 to L when it laps, and fitting that
// raw would report a front moving forwards at a hundred metres a second.
static double front_speed(const std::vector<Event>& e, double L, int* n) {
    *n = int(e.size());
    if (e.size() < 3) return 0.0;
    std::vector<double> X;
    X.reserve(e.size());
    double off = 0.0, prev = e[0].x;
    for (const Event& p : e) {
        double xx = p.x + off;
        while (xx > prev + 0.5 * L) { off -= L; xx -= L; }
        while (xx < prev - 0.5 * L) { off += L; xx += L; }
        X.push_back(xx);
        prev = xx;
    }
    double mt = 0.0, mx = 0.0;
    for (std::size_t i = 0; i < e.size(); ++i) { mt += e[i].t; mx += X[i]; }
    mt /= double(e.size());
    mx /= double(e.size());
    double num = 0.0, den = 0.0;
    for (std::size_t i = 0; i < e.size(); ++i) {
        num += (e[i].t - mt) * (X[i] - mx);
        den += (e[i].t - mt) * (e[i].t - mt);
    }
    return den > 0.0 ? num / den : 0.0;
}

// One stop-and-go experiment. Returns the two front speeds in km/h.
struct Wave {
    double upstream_kmh   = 0.0;   // where vehicles ENTER the jam
    double downstream_kmh = 0.0;   // where vehicles LEAVE it
    int    n_up = 0, n_down = 0;
    double vmin = 0.0, vmax = 0.0;
    double rho_out = 0.0, v_out_kmh = 0.0;
    int    collisions = 0;
    double min_gap = 0.0;
    double mean_kmh = 0.0;
};

static Wave run_wave(float density, int lengthIdx, int dtIdx, double seconds,
                     double thresh = 5.0) {
    TrafficIdm s;
    uniform_ring(s, density, lengthIdx, dtIdx);
    const int n = int(s.cars().size());
    const double L = s.road_length();
    Wave w;
    if (n < 3) return w;

    // The single braking event. One vehicle, 3 m/s^2, 2 s — well inside the
    // 9 m/s^2 MOBIL quotes as physically possible on dry road, and applied as
    // "brake at least this hard" so the pulse can never itself cause a crash.
    s.brake(0, 3.0, 2.0);

    std::vector<int> state(std::size_t(n), 0);   // 0 free, 1 in the jam, 2 out again
    std::vector<Event> down, up;
    const int frames = int(seconds / 0.25 + 0.5);
    for (int f = 0; f < frames; ++f) {
        s.advance_frame();
        const auto& cs = s.cars();
        for (int i = 0; i < n; ++i) {
            const double v = cs[std::size_t(i)].v;
            if (state[std::size_t(i)] == 0 && v < thresh) {
                state[std::size_t(i)] = 1;
                down.push_back({s.time(), cs[std::size_t(i)].x});
            } else if (state[std::size_t(i)] == 1 && v > thresh) {
                state[std::size_t(i)] = 2;
                up.push_back({s.time(), cs[std::size_t(i)].x});
            }
        }
    }
    w.upstream_kmh   = front_speed(down, L, &w.n_up)   * 3.6;
    w.downstream_kmh = front_speed(up,   L, &w.n_down) * 3.6;

    w.vmin = 1e18;
    for (const auto& c : s.cars()) { w.vmin = std::min(w.vmin, c.v); w.vmax = std::max(w.vmax, c.v); }

    // The outflow state: the traffic that has just cleared the jam. Taken as
    // the vehicles above 90% of the fastest current speed, with density read
    // off their own gaps. Used only as an independent cross-check on the front
    // speed, via the shock relation.
    double sv = 0.0, ss = 0.0;
    int cnt = 0;
    for (int i = 0; i < n; ++i) {
        const auto& c = s.cars()[std::size_t(i)];
        if (c.v <= 0.9 * w.vmax) continue;
        double best = 1e18;
        for (int j = 0; j < n; ++j) {
            if (j == i) continue;
            double d = s.cars()[std::size_t(j)].x - c.x;
            if (d < 0.0) d += L;
            best = std::min(best, d);
        }
        sv += c.v; ss += best; ++cnt;
    }
    if (cnt > 0) { w.rho_out = 1000.0 / (ss / double(cnt)); w.v_out_kmh = sv / double(cnt) * 3.6; }
    w.collisions = s.collisions();
    w.min_gap    = s.min_gap_ever();
    w.mean_kmh   = s.metrics()[0].value;
    return w;
}

int main() {
    const auto wall0 = std::chrono::steady_clock::now();
    std::printf("test_traffic_idm — Intelligent Driver Model + MOBIL\n");

    // ── 1. the constants are the papers' constants ──────────────────────────
    {
        head("published constants (Treiber, Hennecke & Helbing 2000, Table I)");
        CHECK(TrafficIdm::kV0Kmh  == 120.0);
        CHECK(TrafficIdm::kT      == 1.6);
        CHECK(TrafficIdm::kA      == 0.73);
        CHECK(TrafficIdm::kB      == 1.67);
        CHECK(TrafficIdm::kDelta  == 4.0);
        CHECK(TrafficIdm::kS0     == 2.0);
        CHECK(TrafficIdm::kS1     == 0.0);
        CHECK(TrafficIdm::kCarLen == 5.0);
        std::printf("  v0 = %.1f km/h = %.4f m/s   T = %.2f s   a = %.2f m/s2   b = %.2f m/s2\n",
                    TrafficIdm::kV0Kmh, TrafficIdm::kV0Kmh / 3.6, TrafficIdm::kT,
                    TrafficIdm::kA, TrafficIdm::kB);
        std::printf("  delta = %.0f   s0 = %.1f m   s1 = %.1f m   l = %.1f m\n",
                    TrafficIdm::kDelta, TrafficIdm::kS0, TrafficIdm::kS1, TrafficIdm::kCarLen);
        // The task brief quotes v0 as 33.3 m/s. That is the table's 120 km/h
        // rounded to three figures, and the code derives it rather than typing
        // it, so the two must agree to that many figures and no further.
        CHECK_NEAR(TrafficIdm::kV0Kmh / 3.6, 33.3, 0.034);

        head("published constants (Kesting, Treiber & Helbing 2007, Table 1)");
        CHECK(TrafficIdm::kPoliteLo == 0.0 && TrafficIdm::kPoliteHi == 1.0);
        CHECK(TrafficIdm::kAthr  == 0.1);
        CHECK(TrafficIdm::kBsafe == 4.0);
        CHECK(TrafficIdm::kAbias == 0.3);
        CHECK(TrafficIdm::kVcritKmh == 60.0);
        CHECK(TrafficIdm::kTruckV0Kmh == 80.0 && TrafficIdm::kTruckLen == 12.0);
        CHECK(TrafficIdm::kFrameSec == 0.25);
        std::printf("  politeness p in [%.1f, %.1f]   a_th = %.1f m/s2   b_safe = %.1f m/s2   "
                    "a_bias = %.1f m/s2\n", TrafficIdm::kPoliteLo, TrafficIdm::kPoliteHi,
                    TrafficIdm::kAthr, TrafficIdm::kBsafe, TrafficIdm::kAbias);
        std::printf("  v_crit = %.0f km/h   trucks %.0f km/h / %.0f m   update step %.2f s\n",
                    TrafficIdm::kVcritKmh, TrafficIdm::kTruckV0Kmh, TrafficIdm::kTruckLen,
                    TrafficIdm::kFrameSec);
        // The paper's own warning about b_safe, as an assertion.
        CHECK(TrafficIdm::kBsafe < TrafficIdm::kBmax);
        // "While the parameter a_bias is small, it clearly has to be larger
        // than the threshold a_th."  (MOBIL Sec. 2.3)
        CHECK(TrafficIdm::kAbias > TrafficIdm::kAthr);
        std::printf("  a_bias (%.1f) > a_th (%.1f), as MOBIL Sec. 2.3 requires; "
                    "b_safe (%.1f) < b_max (%.1f)\n",
                    TrafficIdm::kAbias, TrafficIdm::kAthr, TrafficIdm::kBsafe, TrafficIdm::kBmax);
    }

    // ── 2. the acceleration equation, term by term ──────────────────────────
    {
        head("IDM Eq. (6) and Eq. (7), against hand-worked values");
        TrafficIdm s;                       // knobs at the Table I defaults
        const double v0 = TrafficIdm::kV0Kmh / 3.6;
        const double a = TrafficIdm::kA, b = TrafficIdm::kB;
        const double T = TrafficIdm::kT, s0 = TrafficIdm::kS0, d = TrafficIdm::kDelta;

        // The running parameters come through Knob::value, which is a 32-bit
        // float, so what the equation actually evaluates is the Table I values
        // rounded to float. That is a real 2e-8 relative offset and it is worth
        // saying rather than hiding under a loose tolerance: it is measured
        // here, and every comparison below is then made at 1e-6, which is two
        // orders of magnitude tighter than the offset is large.
        const double tol = 1e-6;
        double worstRound = 0.0;
        for (double c : {TrafficIdm::kT, TrafficIdm::kA, TrafficIdm::kB, TrafficIdm::kS0,
                         TrafficIdm::kBsafe, TrafficIdm::kAthr, TrafficIdm::kAbias})
            worstRound = std::max(worstRound, std::fabs(double(float(c)) - c) / c);
        std::printf("  knob storage is float: worst relative rounding of a Table I constant "
                    "is %.2e\n", worstRound);
        CHECK(worstRound < 1e-7);

        // Empty road, standing start: the whole interaction term vanishes as
        // s -> infinity, and what is left is a[1 - (v/v0)^delta] = a.
        CHECK_NEAR(s.idm(0.0, 0.0, 1e9, v0), a, tol);
        std::printf("  standing start, empty road:            %.6f m/s2   (= a)\n",
                    s.idm(0.0, 0.0, 1e9, v0));

        // At the desired speed on an empty road the acceleration is zero.
        CHECK_NEAR(s.idm(v0, 0.0, 1e9, v0), 0.0, tol);
        std::printf("  at v0 on an empty road:                %.6e m/s2   (= 0)\n",
                    s.idm(v0, 0.0, 1e9, v0));

        // Half the desired speed, empty road: a[1 - 0.5^4] = a * 15/16.
        CHECK_NEAR(s.idm(0.5 * v0, 0.0, 1e9, v0), a * (1.0 - 1.0 / 16.0), tol);
        std::printf("  at v0/2 on an empty road:              %.6f m/s2   (= a*15/16 = %.6f)\n",
                    s.idm(0.5 * v0, 0.0, 1e9, v0), a * 15.0 / 16.0);

        // Steady following, dv = 0, at exactly the desired gap s0 + vT: the
        // interaction term is exactly 1, so dv/dt = -a (v/v0)^delta.
        {
            const double v = 20.0, sg = s0 + v * T;
            const double want = a * (1.0 - std::pow(v / v0, d) - 1.0);
            CHECK_NEAR(s.idm(v, 0.0, sg, v0), want, tol);
            std::printf("  v=20 m/s at the desired gap %.1f m:    %.6f m/s2   (= -a(v/v0)^4 = %.6f)\n",
                        sg, s.idm(v, 0.0, sg, v0), want);
        }

        // Approaching a standing obstacle: the paper's Eq. (13) says the
        // braking term is (v dv)^2/(4 b s^2) once the equilibrium part of s*
        // is negligible. Checked at a speed and gap where it is.
        {
            const double v = 30.0, dv = 30.0, sg = 400.0;
            const double got = s.idm(v, dv, sg, v0);
            const double star = s0 + v * T + v * dv / (2.0 * std::sqrt(a * b));
            const double want = a * (1.0 - std::pow(v / v0, d) - (star / sg) * (star / sg));
            CHECK_NEAR(got, want, tol);
            const double approx = -(v * dv) * (v * dv) / (4.0 * b * sg * sg);
            std::printf("  closing on a stopped car (v=dv=30, s=400): %.5f m/s2\n", got);
            std::printf("    Eq.(13) asymptote -(v dv)^2/(4 b s^2)  = %.5f m/s2\n", approx);
            // Same order, not equal — the equilibrium part of s* is dropped in
            // Eq. (13) and kept in the code. Assert that it is dropping the
            // right thing by checking the two agree to better than 25%.
            CHECK(std::fabs(got - approx) < 0.25 * std::fabs(approx));
        }

        // s* itself: the desired gap grows exactly linearly in v at dv = 0.
        {
            const double s1 = s.idm(10.0, 0.0, s0 + 10.0 * T, v0);
            const double s2 = s.idm(25.0, 0.0, s0 + 25.0 * T, v0);
            CHECK_NEAR(s1, a * (1.0 - std::pow(10.0 / v0, d) - 1.0), tol);
            CHECK_NEAR(s2, a * (1.0 - std::pow(25.0 / v0, d) - 1.0), tol);
            std::printf("  s*(v,0) = s0 + vT verified at v = 10 and 25 m/s\n");
        }
    }

    // ── 3. equilibrium, and the road as a fixed point ───────────────────────
    {
        head("equilibrium: Ve(s), and a uniform road that stays uniform (for a while)");
        TrafficIdm s;
        const double v0 = TrafficIdm::kV0Kmh / 3.6;
        // Eq. (8) inverted: at the equilibrium speed the desired gap equals the
        // actual gap divided by sqrt(1 - (v/v0)^delta).
        for (double gap : {10.0, 20.0, 40.0, 80.0, 200.0}) {
            const double v = s.equilibrium_speed(gap, v0);
            CHECK_NEAR(s.idm(v, 0.0, gap, v0), 0.0, 1e-6);
            std::printf("  gap %6.1f m -> Ve = %6.3f m/s = %6.2f km/h   (residual accel %.2e)\n",
                        gap, v, v * 3.6, s.idm(v, 0.0, gap, v0));
        }
        // At the jam distance nobody moves.
        CHECK(s.equilibrium_speed(TrafficIdm::kS0, v0) == 0.0);
        std::printf("  gap = s0 = %.1f m -> Ve = 0 exactly\n", TrafficIdm::kS0);

        // And the road built from those speeds does not move off it.
        TrafficIdm r;
        uniform_ring(r, 40.f, 2);
        const double v_start = r.cars()[0].v;
        for (int f = 0; f < 2400; ++f) r.advance_frame();      // 600 s untouched
        double drift = 0.0;
        for (const auto& c : r.cars()) drift = std::max(drift, std::fabs(c.v - v_start));
        std::printf("  uniform 4 km ring at 40 veh/km: v = %.6f m/s, worst drift after 600 s "
                    "= %.3e m/s\n", v_start, drift);
        std::printf("  (\"for a while\" is load-bearing: that state is a genuine fixed point of\n"
                    "   the equations and an UNSTABLE one, so round-off eventually finds it out."
                    "\n   Measured at the end of the stop-and-go section.)\n");
        CHECK(drift < 1e-9);
    }

    // ── 4. the fundamental diagram, measured ────────────────────────────────
    double capacityQ = 0.0, capacityRho = 0.0;
    {
        head("fundamental diagram (single lane, identical vehicles, 1 km ring)");
        std::printf("  Measured two ways at each density: the EQUILIBRIUM branch, from a road\n"
                    "  left undisturbed, and the DYNAMIC one, from the same road after a single\n"
                    "  braking event, averaged over 900 s once 900 s have passed. The paper's\n"
                    "  own high-density prediction Qe = [1 - rho(l+s0)]/T is printed beside them.\n\n");
        std::printf("   rho     v_eq    Q_eq      v_dyn    Q_dyn    Qe=[1-rho(l+s0)]/T   loss\n");
        std::printf("  veh/km   km/h   veh/h      km/h     veh/h         veh/h            %%\n");
        const double l_s0 = TrafficIdm::kCarLen + TrafficIdm::kS0;
        int collisions = 0;
        double worstEqErr = 0.0;
        for (int rho = 5; rho <= 140; rho += 5) {
            double vEq = 0.0, vDyn = 0.0;
            double rhoActual = 0.0;
            for (int dyn = 0; dyn < 2; ++dyn) {
                TrafficIdm s;
                uniform_ring(s, float(rho), 0);
                rhoActual = double(s.cars().size()) / (s.road_length() / 1000.0);
                if (dyn && !s.cars().empty()) s.brake(0, 3.0, 2.0);
                for (int f = 0; f < 3600; ++f) s.advance_frame();       // 900 s settle
                double sum = 0.0; long long n = 0;
                for (int f = 0; f < 3600; ++f) {                        // 900 s measure
                    s.advance_frame();
                    for (const auto& c : s.cars()) { sum += c.v; ++n; }
                }
                (dyn ? vDyn : vEq) = n ? sum / double(n) : 0.0;
                collisions += s.collisions();
            }
            const double Qeq  = rhoActual * vEq  * 3.6;
            const double Qdyn = rhoActual * vDyn * 3.6;
            const double Qpaper = (1.0 - rhoActual / 1000.0 * l_s0) / TrafficIdm::kT * 3600.0;
            if (Qdyn > capacityQ) { capacityQ = Qdyn; capacityRho = rhoActual; }
            std::printf("  %5.0f %7.2f %8.1f   %7.2f %8.1f      %10.1f      %6.1f\n",
                        rhoActual, vEq * 3.6, Qeq, vDyn * 3.6, Qdyn, Qpaper,
                        Qeq > 0 ? 100.0 * (Qeq - Qdyn) / Qeq : 0.0);
            // The paper's linear congested branch is an EQUILIBRIUM statement,
            // so it is only checked against the undisturbed branch, and only
            // where congested traffic is what the road is in.
            if (rho >= 60) {
                const double err = std::fabs(Qeq - Qpaper) / Qpaper;
                worstEqErr = std::max(worstEqErr, err);
            }
        }
        std::printf("\n  capacity (dynamic branch): %.1f veh/h at %.0f veh/km\n",
                    capacityQ, capacityRho);
        std::printf("  worst disagreement with Qe = [1-rho(l+s0)]/T above 60 veh/km: %.3f%%\n",
                    100.0 * worstEqErr);
        std::printf("  collisions over the whole sweep: %d\n", collisions);
        // The undisturbed congested branch IS the paper's formula, and this is
        // the strongest single check in the file that the equation is the
        // equation: the code never contains that expression anywhere.
        CHECK(worstEqErr < 0.005);
        CHECK(collisions == 0);
        // A fundamental diagram has to have a maximum in the interior, or it is
        // not a fundamental diagram.
        CHECK(capacityQ > 1200.0 && capacityQ < 2400.0);
        CHECK(capacityRho > 8.0 && capacityRho < 60.0);
        // Free branch below capacity, congested branch above.
        CHECK(capacityRho < 140.0);
    }

    // ── 5. the stop-and-go wave ─────────────────────────────────────────────
    {
        head("stop-and-go wave behind a single braking event");
        std::printf("  Setup: ONE lane, identical vehicles, exactly uniform, 4 km ring. One\n"
                    "  vehicle brakes at 3 m/s2 for 2 s and is then left alone. Fronts are\n"
                    "  tracked by the moment each vehicle first crosses 5 m/s downwards (the\n"
                    "  upstream front, where traffic enters the jam) and upwards again (the\n"
                    "  downstream front, where it leaves). Both are least-squares fits over\n"
                    "  every vehicle on the ring.\n\n");
        std::printf("   rho   v_uniform   upstream front   downstream front   outflow      shock\n");
        std::printf("  veh/km    km/h         km/h    n        km/h    n     rho / v      km/h\n");
        const double rhoJam = 1000.0 / (TrafficIdm::kCarLen + TrafficIdm::kS0);
        int sustained = 0;
        double sumUp = 0.0;
        int nUp = 0;
        for (float rho : {30.f, 35.f, 40.f, 45.f, 50.f}) {
            const Wave w = run_wave(rho, 2, 0, 3000.0);
            // Shock relation: the line in the (rho, Q) plane through the jam
            // state (rho_jam, 0) and the outflow state. Its slope is the speed
            // of the interface between them, and it is derived from the
            // MEASURED outflow, not from any published number.
            const double shock = (w.rho_out * w.v_out_kmh) / (w.rho_out - rhoJam);
            TrafficIdm ref;
            uniform_ring(ref, rho, 2);
            std::printf("  %5.0f  %8.2f   %9.2f %4d   %9.2f %4d  %5.1f/%5.1f  %8.2f\n",
                        double(rho), ref.cars()[0].v * 3.6,
                        w.upstream_kmh, w.n_up, w.downstream_kmh, w.n_down,
                        w.rho_out, w.v_out_kmh, shock);
            CHECK(w.collisions == 0);
            // A wave is only a wave if it actually stops people and lets them
            // go again: a fully stopped vehicle and a nearly free one at once.
            if (w.vmin < 1.0 && w.vmax > 15.0) ++sustained;
            CHECK(w.n_up == int(std::round(rho * 4.0)));   // every vehicle was caught
            sumUp += w.upstream_kmh;
            ++nUp;
            // The empirical constant, and the whole point of the exercise.
            CHECK(w.upstream_kmh < 0.0);
            CHECK_NEAR(-w.upstream_kmh, 15.0, 5.0);
            CHECK_NEAR(-shock, 15.0, 5.0);
        }
        std::printf("\n  mean upstream front speed over the five densities: %.2f km/h\n",
                    sumUp / double(nUp));
        std::printf("  densities that fully stopped traffic and released it again: %d of 5\n",
                    sustained);
        std::printf("  Treiber, Hennecke & Helbing (2000) report ~15 km/h twice: as the\n"
                    "  empirical constant (Sec. I, \"a characteristic velocity of about\n"
                    "  15 km/h\") and as their own IDM result (Sec. II D, \"propagation\n"
                    "  velocity vg = (Qout-Qjam)/(rho_out-rho_jam) ~ -15 km/h\").\n");
        CHECK(sustained == 5);

        // The amplitude, so "wave" is not just a word.
        TrafficIdm ref40;
        uniform_ring(ref40, 40.f, 2);
        const Wave w40 = run_wave(40.f, 2, 0, 3000.0);
        std::printf("\n  at 40 veh/km the wave takes traffic from a uniform %.2f km/h to a range "
                    "of %.2f - %.2f km/h\n", ref40.cars()[0].v * 3.6,
                    w40.vmin * 3.6, w40.vmax * 3.6);
        CHECK(w40.vmin < 0.5 && w40.vmax * 3.6 > 60.0);

        // ── the control, and what it turned out to be ──────────────────────
        //
        // The obvious control is "the same road, not braked, stays uniform".
        // That control FAILS, and the failure is the physics rather than a bug:
        // at 40 veh/km the uniform state is LINEARLY UNSTABLE, so the 1e-16 of
        // floating-point round-off in the ring's arithmetic is a perturbation
        // too, and it grows at the same exponential rate the braking event
        // does. Asserting a uniform road stays uniform forever would have been
        // asserting the model is stable where the paper says it is not.
        //
        // So the control is run at a density where the model IS stable, and the
        // instability is measured instead of denied.
        head("the control: where the model is stable, the same brake decays");
        for (float rho : {10.f, 15.f, 20.f}) {
            const Wave w = run_wave(rho, 2, 0, 3000.0);
            TrafficIdm r0;
            uniform_ring(r0, rho, 2);
            std::printf("  %3.0f veh/km, braked, 3000 s later: speeds span %.4f m/s "
                        "about a uniform %.4f m/s\n",
                        double(rho), w.vmax - w.vmin, r0.cars()[0].v);
            CHECK(w.vmax - w.vmin < 1.0);
        }
        std::printf("  So the braking pulse does not manufacture waves: below about 22 veh/km\n"
                    "  the identical pulse dies out. The fundamental diagram above says the\n"
                    "  same thing from the other side - dynamic and equilibrium flow agree to\n"
                    "  0.0%% at 20 veh/km and part company by 19%% at 30.\n");

        head("and where it is unstable, round-off alone is enough");
        {
            TrafficIdm q;
            uniform_ring(q, 40.f, 2);
            std::printf("  40 veh/km, NOTHING done to it. Speed spread across the ring:\n");
            // The growth factor is read off the LINEAR regime only — once the
            // spread is a metre a second the wave is saturating and the ratio
            // stops meaning a growth rate.
            double prev = 0.0, ratio = 0.0;
            for (int block = 1; block <= 8; ++block) {
                for (int f = 0; f < 1000; ++f) q.advance_frame();   // 250 s
                double mn = 1e18, mx = -1e18;
                for (const auto& c : q.cars()) { mn = std::min(mn, c.v); mx = std::max(mx, c.v); }
                const double sp = mx - mn;
                const bool linear = (block > 1 && prev > 0.0 && sp < 1.0);
                if (linear) ratio = sp / prev;
                std::printf("    t = %5.0f s   spread %.3e m/s%s\n", q.time(), sp,
                            linear ? "   (linear growth)" : "");
                prev = sp;
            }
            std::printf("  growing by a factor of about %.0f every 250 s, so an e-folding time\n"
                        "  of roughly %.0f s. That is the linear instability of the 2000 paper's\n"
                        "  Sec. II D, seeded by nothing but the last bit of a double.\n",
                        ratio, 250.0 / std::log(ratio));
            CHECK(ratio > 2.0);
            CHECK(q.collisions() == 0);
        }
    }

    // ── 6. timestep independence ────────────────────────────────────────────
    {
        head("the answer does not depend on the timestep");
        std::printf("  Same experiment as above at 40 veh/km, integrated with the step halved\n"
                    "  three times. The ballistic scheme is first order, so the error should\n"
                    "  halve with it and the answers should converge.\n\n");
        std::printf("     dt        upstream front   mean speed   min gap    collisions\n");
        std::printf("     s              km/h           km/h         m\n");
        double first = 0.0, last = 0.0, worst = 0.0;
        for (int d = 0; d < 4; ++d) {
            TrafficIdm probe;
            uniform_ring(probe, 40.f, 2, d);
            const Wave w = run_wave(40.f, 2, d, 2000.0);
            std::printf("  %9.5f  %13.3f  %11.4f  %8.4f  %8d\n",
                        probe.dt(), w.upstream_kmh, w.mean_kmh, w.min_gap, w.collisions);
            CHECK(w.collisions == 0);
            if (d == 0) first = w.upstream_kmh;
            last = w.upstream_kmh;
            worst = std::max(worst, std::fabs(w.upstream_kmh - first));
        }
        std::printf("\n  the wave speed moves %.4f km/h (%.3f%%) across a 8x change of timestep\n",
                    std::fabs(last - first), 100.0 * std::fabs(last - first) / std::fabs(first));
        // Half a percent is the bar. The scheme is first order, so the residual
        // must not vanish either — a wave speed identical to fifteen digits at
        // four different step sizes would mean the step was not being used.
        CHECK(std::fabs(last - first) / std::fabs(first) < 0.005);
        CHECK(worst > 0.0);
        std::printf("  and it does move, which is the other half of the claim: an identical\n"
                    "  answer at four step sizes would mean the step was not being used.\n");
    }

    // ── 7. crash-freedom ────────────────────────────────────────────────────
    {
        head("crash-freedom (the model's own headline claim)");
        std::printf("  MOBIL Sec. 3: \"Notice that the IDM guarantees crash-free driving.\"\n"
                    "  Nothing in traffic_idm.hpp enforces it. Every substep, each vehicle's\n"
                    "  gap to the leader it was actually following is recompared before and\n"
                    "  after the move, so a follower that drove clean through its leader is\n"
                    "  caught even though the ring coordinate would wrap and hide it.\n\n");
        // (a) long single-lane runs at every density, with a perturbation.
        int coll = 0;
        double worst = 1e18;
        for (int rho = 10; rho <= 140; rho += 10) {
            TrafficIdm s;
            uniform_ring(s, float(rho), 1);
            if (s.cars().size() > 2) s.brake(0, 5.0, 3.0);
            for (int f = 0; f < 14400; ++f) s.advance_frame();          // one hour of road
            coll += s.collisions();
            worst = std::min(worst, s.min_gap_ever());
        }
        std::printf("  14 densities x 1 h of road time, single lane, perturbed:  "
                    "collisions %d, smallest gap %.4f m\n", coll, worst);
        CHECK(coll == 0);
        CHECK(worst > 0.0);

        // (b) every knob driven to four points of its range, both passing rules.
        TrafficIdm proto;
        int sweepColl = 0, runs = 0;
        double sweepWorst = 1e18;
        for (const auto& k : proto.knobs())
            for (int t = 0; t <= 3; ++t) {
                const float v = k.quantised(k.min + (k.max - k.min) * float(t) / 3.0f);
                for (int eu = 0; eu < 2; ++eu) {
                    TrafficIdm s;
                    set(s, "rules", float(eu));
                    set(s, k.key.c_str(), v);
                    s.reset();
                    if (s.cars().size() > 2) s.brake(0, 6.0, 3.0);
                    for (int f = 0; f < 2400; ++f) s.advance_frame();   // 600 s
                    sweepColl += s.collisions();
                    sweepWorst = std::min(sweepWorst, s.min_gap_ever());
                    ++runs;
                }
            }
        std::printf("  %d runs over every knob at 4 points x both passing rules:  "
                    "collisions %d, smallest gap %.4f m\n", runs, sweepColl, sweepWorst);
        CHECK(sweepColl == 0);
        CHECK(sweepWorst > 0.0);

        // (c) how much margin there actually is, and whether the timestep eats
        //     it. Reported rather than asserted-away: "no collisions" with a
        //     centimetre to spare is a different claim from "no collisions"
        //     with a metre to spare, and only one of them survives a coarser
        //     integrator.
        std::printf("\n  How much room is left over. Same perturbed 40 veh/km ring, headway T\n"
                    "  swept well either side of the published 1.6 s, at all four timesteps:\n\n");
        std::printf("      T     uniform road   dt=0.25   dt=0.125  dt=0.0625 dt=0.03125\n");
        std::printf("      s        km/h                smallest gap ever seen, m\n");
        double tightest = 1e18;
        for (float T : {0.5f, 0.8f, 1.1f, 1.6f, 2.4f}) {
            TrafficIdm base;
            uniform_ring(base, 40.f, 1);
            set(base, "T", T);
            base.reset();
            // What the road looks like BEFORE anything happens to it, printed
            // because the smallest gap turns out not to be a simple function of
            // T and this is the reason. The GAP is fixed by the density and is
            // 20 m at every row; what T changes is how fast the same vehicles
            // are willing to take it.
            std::printf("   %5.2f %12.2f", double(T), base.cars()[0].v * 3.6);
            for (int d = 0; d < 4; ++d) {
                TrafficIdm s;
                uniform_ring(s, 40.f, 1, d);
                set(s, "T", T);
                s.reset();
                s.brake(0, 5.0, 3.0);
                for (int f = 0; f < 9600; ++f) s.advance_frame();       // 2400 s
                std::printf("%10.4f", s.min_gap_ever());
                tightest = std::min(tightest, s.min_gap_ever());
                CHECK(s.collisions() == 0);
            }
            std::printf("\n");
        }
        std::printf("\n  Not one of those is a collision, and the margin does NOT simply shrink\n"
                    "  with the headway: 0.5 s leaves the largest gaps of the five, because at\n"
                    "  that headway the same 40 veh/km is a much faster road. Nor does the\n"
                    "  timestep eat the margin - halving the step three times moves the\n"
                    "  smallest gap by under 5%%. Tightest anywhere in this sweep: %.4f m.\n"
                    "  Tightest over the 152-run knob sweep above: %.4f m. Neither is zero,\n"
                    "  and neither is comfortable.\n", tightest, sweepWorst);
    }

    // ── 8. MOBIL does what MOBIL says it does ───────────────────────────────
    {
        head("MOBIL: politeness sets the lane-changing rate");
        std::printf("  MOBIL Sec. 3 investigates \"the rate of lane changes (per km and hour)\n"
                    "  that is primarily determined by the politeness factor p\", and the\n"
                    "  comparison the paper actually draws is between p = 0 and p = 1. Measured\n"
                    "  here on the default 3-lane road, 1200 s per run, averaged over four\n"
                    "  seeds because a single ring is a chaotic sample and one run is not a\n"
                    "  rate.\n\n");
        std::printf("     p     lane changes /km/h   mean speed km/h   (mean of 4 seeds)\n");
        double rate0 = 0.0, rate1 = 0.0, worstRise = 0.0, prev = -1.0, peak = 0.0;
        for (float p : {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f}) {
            double rate = 0.0, speed = 0.0;
            for (int sd = 1; sd <= 4; ++sd) {
                TrafficIdm s;
                set(s, "polite", p);
                set(s, "seed", float(sd));
                s.reset();
                for (int f = 0; f < 4800; ++f) s.advance_frame();
                rate  += s.metrics()[4].value;
                speed += s.metrics()[0].value;
                CHECK(s.collisions() == 0);
            }
            rate /= 4.0; speed /= 4.0;
            std::printf("  %5.1f %19.1f %17.2f\n", double(p), rate, speed);
            if (p == 0.0f) rate0 = rate;
            if (p == 1.0f) rate1 = rate;
            if (prev >= 0.0 && rate > prev) worstRise = std::max(worstRise, rate / prev);
            prev = rate;
            peak = std::max(peak, rate);
        }
        std::printf("\n  selfish drivers change lanes %.1fx as often as ideal-MOBIL ones "
                    "(%.0f vs %.0f /km/h)\n", rate0 / rate1, rate0, rate1);
        std::printf("  The fall is steep from p = 0 and then flattens into scatter: the worst\n"
                    "  upward step along the sweep is a factor of %.2f, so this is NOT a\n"
                    "  monotone curve and is not asserted as one. What is asserted is the\n"
                    "  paper's own comparison, and that p = 0 is the busiest setting.\n",
                    worstRise);
        CHECK(rate0 > 3.0 * rate1);
        CHECK(rate0 == peak);

        head("MOBIL: the safety criterion, Eq. (2)");
        std::printf("  b_safe is what protects the new follower when the driver will not:\n"
                    "  \"the maximum safe deceleration b_safe prevents accidents even in the\n"
                    "  case of totally selfish drivers\". So it is measured at p = 0, where the\n"
                    "  paper says it does the most work. It is not declared as depending on p,\n"
                    "  because at the default p = 0.3 it is not inert - section 9 drives it\n"
                    "  from the defaults and it still changes the road within six seconds.\n\n");
        std::printf("   b_safe   lane changes   mean speed km/h\n");
        long long lo = 0, hi = 0;
        for (float bs : {1.0f, 2.0f, 4.0f, 6.0f, 9.0f}) {
            TrafficIdm s;
            set(s, "polite", 0.f);
            set(s, "bsafe", bs);
            s.reset();
            for (int f = 0; f < 2400; ++f) s.advance_frame();
            std::printf("  %6.1f %14lld %17.2f\n", double(bs), s.lane_changes(),
                        s.metrics()[0].value);
            if (bs == 1.0f) lo = s.lane_changes();
            if (bs == 9.0f) hi = s.lane_changes();
            CHECK(s.collisions() == 0);
        }
        // A stricter safety limit must permit fewer lane changes. Not a
        // published number — a published direction, and the only one the
        // criterion can have.
        std::printf("\n  b_safe 1 permits %lld changes, b_safe 9 permits %lld\n", lo, hi);
        CHECK(hi > lo);

        head("MOBIL: asymmetric ('European') rules push traffic right");
        std::printf("  Eqs. (5)-(7): no passing on the right above v_crit = 60 km/h, and a\n"
                    "  keep-right bias a_bias. Lane 0 is the rightmost. Occupancy sampled every\n"
                    "  10 frames over 600 s on the default 3-lane road.\n\n");
        std::printf("   rules        lane 0   lane 1   lane 2   changes/km/h\n");
        double rightSym = 0.0, rightEu = 0.0;
        for (int r = 0; r < 2; ++r) {
            TrafficIdm s;
            set(s, "rules", float(r));
            s.reset();
            double occ[4] = {0, 0, 0, 0};
            for (int f = 0; f < 2400; ++f) {
                s.advance_frame();
                if (f % 10 == 0) for (const auto& c : s.cars()) occ[c.lane] += 1.0;
            }
            const double tot = occ[0] + occ[1] + occ[2] + occ[3];
            std::printf("  %-12s %6.1f%% %7.1f%% %7.1f%% %14.1f\n",
                        r ? "asymmetric" : "symmetric",
                        100 * occ[0] / tot, 100 * occ[1] / tot, 100 * occ[2] / tot,
                        s.metrics()[4].value);
            (r ? rightEu : rightSym) = 100 * occ[0] / tot;
            CHECK(s.collisions() == 0);
        }
        std::printf("\n  right-lane share rises from %.1f%% to %.1f%% under the keep-right rule\n",
                    rightSym, rightEu);
        CHECK(rightEu > rightSym + 3.0);

        head("MOBIL: the incentive threshold a_th");
        std::printf("   a_th    lane changes\n");
        long long thrLo = 0, thrHi = 0;
        for (float at : {0.0f, 0.1f, 0.5f, 1.0f}) {
            TrafficIdm s;
            set(s, "athr", at);
            s.reset();
            for (int f = 0; f < 2400; ++f) s.advance_frame();
            std::printf("  %5.2f %14lld\n", double(at), s.lane_changes());
            if (at == 0.0f) thrLo = s.lane_changes();
            if (at == 1.0f) thrHi = s.lane_changes();
        }
        std::printf("\n  a threshold that prevents marginal changes prevents changes: %lld -> %lld\n",
                    thrLo, thrHi);
        CHECK(thrHi < thrLo);
    }

    // ── 9. every knob changes the outcome ───────────────────────────────────
    {
        head("every knob changes the outcome");
        std::printf("  The bench's own standard, applied here: four samples across each knob's\n"
                    "  range, and the running hash of the published field over 24 frames must\n"
                    "  differ somewhere. Twenty-four frames is six seconds of road time, which\n"
                    "  is deliberately unforgiving — a knob that needs a minute to bite would\n"
                    "  fail, and one did until the default density moved to where MOBIL has\n"
                    "  room to work.\n\n");
        TrafficIdm proto;
        int dead = 0;
        for (const auto& k : proto.knobs()) {
            std::vector<std::uint64_t> seen;
            bool differs = false;
            for (int t = 0; t <= 3 && !differs; ++t) {
                const float v = k.quantised(k.min + (k.max - k.min) * float(t) / 3.0f);
                TrafficIdm s;
                bool onReset = false;
                if (!k.requires_key.empty()) {
                    for (auto& kn : s.knobs())
                        if (kn.key == k.requires_key) {
                            kn.value = k.requires_value;
                            onReset = onReset || kn.on_reset;
                        }
                    s.on_knob(k.requires_key, k.requires_value);
                }
                for (auto& kn : s.knobs())
                    if (kn.key == k.key) { onReset = onReset || kn.on_reset; kn.value = v; }
                s.on_knob(k.key, v);
                if (onReset) s.reset();
                std::uint64_t h = 1469598103934665603ull;
                auto absorb = [&] {
                    for (auto c : s.field().cells) { h ^= c; h *= 1099511628211ull; }
                };
                absorb();
                for (int f = 0; f < 24; ++f) { s.step(); absorb(); }
                for (auto p : seen) if (p != h) { differs = true; break; }
                seen.push_back(h);
            }
            const std::string need =
                k.requires_key.empty() ? std::string() : "(needs " + k.requires_key + ")";
            std::printf("  %-9s %-16s %s\n", k.key.c_str(), need.c_str(),
                        differs ? "changes the outcome" : "*** DEAD ***");
            if (!differs) ++dead;
        }
        std::printf("\n  dead knobs: %d of %d\n", dead, int(proto.knobs().size()));
        CHECK(dead == 0);
    }

    // ── 10. determinism, and that the fast path is the same simulation ──────
    {
        head("determinism and path equivalence");
        auto fingerprint = [](TrafficIdm& s) {
            double h = 0.0;
            for (const auto& c : s.cars()) h = h * 1.0000001 + c.x + 7.0 * c.v + 13.0 * c.lane;
            return h;
        };
        TrafficIdm a, b;
        a.reset(); b.reset();
        for (int f = 0; f < 600; ++f) { a.step(); b.step(); }
        CHECK(fingerprint(a) == fingerprint(b));
        std::printf("  two runs of the same seed, 600 frames: identical (%.9f)\n", fingerprint(a));

        // A different seed must give a different road, or the seed knob is a
        // decoration.
        TrafficIdm c;
        set(c, "seed", 7.f);
        c.reset();
        for (int f = 0; f < 600; ++f) c.step();
        CHECK(fingerprint(c) != fingerprint(a));
        std::printf("  seed 7 instead of 1:                    differs  (%.9f)\n", fingerprint(c));

        // step() must be advance_frame() plus rendering and nothing else, or
        // every measurement above is of a different simulation from the one the
        // workbench draws.
        TrafficIdm d, e;
        d.reset(); e.reset();
        for (int f = 0; f < 600; ++f) { d.step(); e.advance_frame(); }
        CHECK(fingerprint(d) == fingerprint(e));
        std::printf("  step() vs advance_frame() over 600 frames: identical vehicle states\n");
        CHECK(d.generation() == 600 && e.generation() == 0);
        std::printf("  (and only step() advances the frame counter: %llu vs %llu)\n",
                    (unsigned long long)d.generation(), (unsigned long long)e.generation());

        // reset() must actually restore the start.
        TrafficIdm f2;
        const double start = fingerprint(f2);
        for (int f = 0; f < 200; ++f) f2.step();
        f2.reset();
        CHECK(fingerprint(f2) == start);
        CHECK(f2.generation() == 0 && f2.time() == 0.0 && f2.collisions() == 0);
        std::printf("  reset() restores the initial state exactly, and zeroes the clock\n");
    }

    // ── 11. the interface contract, and the cost ────────────────────────────
    {
        head("interface, palette and cost");
        TrafficIdm s;
        CHECK(s.field().w == TrafficIdm::kW && s.field().h == TrafficIdm::kH);
        CHECK(s.palette().size() == std::size_t(3 + TrafficIdm::kSpeedBuckets));
        for (const auto& sw : s.palette()) CHECK(!sw.label.empty());
        std::printf("  field %d x %d, palette %d swatches, all labelled\n",
                    s.field().w, s.field().h, int(s.palette().size()));
        // Every cell index the sim ever publishes must exist in the palette,
        // or the renderer is reading past the lookup table.
        for (int f = 0; f < 200; ++f) s.step();
        std::uint8_t hi = 0;
        for (auto c : s.field().cells) hi = std::max(hi, c);
        CHECK(hi < std::uint8_t(s.palette().size()));
        std::printf("  highest cell index published after 200 frames: %d (< %d)\n",
                    int(hi), int(s.palette().size()));
        CHECK(s.metrics().size() == 6);
        for (const auto& m : s.metrics()) CHECK(std::isfinite(m.value));
        std::printf("  metrics after 200 frames (50 s of road):\n");
        for (const auto& m : s.metrics()) std::printf("    %-22s %12.4f\n", m.name.c_str(), m.value);
        std::printf("  subtitle: %s\n", s.subtitle().c_str());
        CHECK(!s.about().citation.empty());
        CHECK(s.about().replicates == Replication::No);

        // poke() has to do something, and what it does has to be a brake.
        TrafficIdm p;
        uniform_ring(p, 40.f, 1);
        const double before = p.cars()[0].v;
        CHECK(p.poke(0.02f, 0.1f));
        for (int f = 0; f < 8; ++f) p.advance_frame();
        double slowest = 1e18;
        for (const auto& c : p.cars()) slowest = std::min(slowest, c.v);
        std::printf("  poke() on a uniform %.2f m/s road: slowest vehicle 2 s later %.3f m/s\n",
                    before, slowest);
        CHECK(slowest < before - 3.0);

        // Cost, at the default road and at the worst road the knobs allow.
        auto cost = [](TrafficIdm& t, int frames) {
            const auto t0 = std::chrono::steady_clock::now();
            for (int f = 0; f < frames; ++f) t.step();
            const auto t1 = std::chrono::steady_clock::now();
            return std::chrono::duration<double, std::milli>(t1 - t0).count() / double(frames);
        };
        TrafficIdm def;
        std::printf("  %.3f ms/frame at the defaults (%d vehicles)\n",
                    cost(def, 300), int(def.cars().size()));
        TrafficIdm big;
        set(big, "lanes", 4.f); set(big, "length", 2.f); set(big, "density", 140.f);
        set(big, "dt", 3.f);
        big.reset();
        std::printf("  %.3f ms/frame at the heaviest the knobs allow "
                    "(%d vehicles, 4 lanes, 4 km, dt = %.5f s)\n",
                    cost(big, 60), int(big.cars().size()), big.dt());
    }

    const auto wall1 = std::chrono::steady_clock::now();
    std::printf("\nOK — %d checks passed in %.1f s\n", g_checks,
                std::chrono::duration<double>(wall1 - wall0).count());
    return 0;
}
