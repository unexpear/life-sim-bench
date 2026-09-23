// traffic_idm.hpp — the Intelligent Driver Model on a multi-lane ring road,
// with MOBIL lane changing.
//
// Everything else on this bench updates a lattice. This does not: it is a
// system of coupled ordinary differential equations, one per vehicle, in
// continuous space and continuous time. The grid you see is a rendering of it
// and nothing more.
//
// ── the two papers ──────────────────────────────────────────────────────────
//
//   Treiber, M., Hennecke, A. & Helbing, D. "Congested traffic states in
//   empirical observations and microscopic simulations", Physical Review E 62
//   (2000) 1805-1824.  arXiv:cond-mat/0002177
//
//     Eq. (6):  dv_a/dt = a [ 1 - (v_a/v0)^delta - (s*(v_a, dv_a) / s_a)^2 ]
//     Eq. (7):  s*(v, dv) = s0 + s1 sqrt(v/v0) + T v + v dv / (2 sqrt(a b))
//
//   The paper sets s1 = 0 for everything it reports ("our emphasis is on basic
//   investigations with models as simple as possible, and therefore we will set
//   s1 = 0"), which reduces Eq. (7) to exactly the form this file implements.
//
//   Kesting, A., Treiber, M. & Helbing, D. "General Lane-Changing Model MOBIL
//   for Car-Following Models", Transportation Research Record 1999 (2007)
//   86-94.  Preprint: mtreiber.de/publications/MOBIL_TRB.pdf
//
//     Eq. (2), safety:            a~_n >= -b_safe
//     Eq. (3), incentive (US):    a~_c - a_c + p [ (a~_n - a_n) + (a~_o - a_o) ] > a_th
//     Eq. (5), passing rule:      a_c^eur = min(a_c, a~_c) if v_c > v~_lead > v_crit
//     Eq. (6), incentive L->R:    a~_c^eur - a_c + p (a~_o - a_o) > a_th - a_bias
//     Eq. (7), incentive R->L:    a~_c - a_c^eur + p (a~_n - a_n) > a_th + a_bias
//
//   Tildes are the quantities AFTER a prospective change; c is the changing
//   vehicle, n the prospective new follower on the target lane, o the follower
//   left behind on the old lane.
//
// ── what is cited and what is chosen ────────────────────────────────────────
//
// CITED — every one of these is a row of Table I of Treiber, Hennecke & Helbing
// (2000), read from the paper, not from memory:
//
//     Desired velocity v0       120 km/h      (33.33 m/s; the task brief's
//                                              "33.3 m/s" is that rounded)
//     Safe time headway T       1.6 s
//     Maximum acceleration a    0.73 m/s^2
//     Desired deceleration b    1.67 m/s^2
//     Acceleration exponent d   4
//     Jam distance s0           2 m
//     Jam distance s1           0 m
//     Vehicle length l          5 m
//
// CITED — MOBIL's Table 1, read from the preprint:
//
//     Politeness factor p       0 ... 1       (published RANGE, not a value)
//     Changing threshold a_th   0.1 m/s^2
//     Max safe decel. b_safe    4 m/s^2
//     Right-lane bias a_bias    0.3 m/s^2
//
//   and from its Sec. 2.3, v_crit = 60 km/h; from its Sec. 3, trucks at
//   v0 = 80 km/h and 12 m long, and a numerical update step of 0.25 s.
//
// CHOSEN by me, because no paper fixes them:
//
//     Politeness default 0.3.  The paper's table gives the range 0..1 and says
//       p = 1 with a_th = 0 is the parameter-free "ideal MOBIL"; Treiber's own
//       applet notes (mtreiber.de/MicroApplet/MOBIL.html) call 0 < p <= 0.5
//       the realistic band. 0.3 sits inside both. The knob spans the published
//       range so you can drive it to either endpoint.
//     Ring topology and road length (1/2/4 km). The papers simulate open roads
//       with an on-ramp; a closed ring is the standard way to hold density
//       exactly fixed while a wave is measured, and it is the only way to watch
//       one wave for twenty minutes.
//     Car length 5 m from IDM Table I rather than MOBIL's 4 m, because the IDM
//       paper is the one whose fundamental diagram this file reproduces, and
//       l enters it directly. Trucks keep MOBIL's 12 m.
//     Default 3 lanes, 22 veh/km/lane, 20% trucks, +-20% desired-speed spread.
//       The 20/20 pair are MOBIL Sec. 3's own simulation values. 22 veh/km/lane
//       is chosen from a measurement rather than taste: it sits just under the
//       capacity this build measures on a single lane (1689 veh/h at 25
//       veh/km), which is where lane changing is busiest. It is also the
//       lowest-effort place a knob can hide - at 30 veh/km/lane the traffic is
//       tight enough that MOBIL makes no changes at all for the first minute,
//       and five of the lane-changing knobs looked dead in consequence.
//     Integrator and timestep: see below.
//
// ── integrator ──────────────────────────────────────────────────────────────
//
// BALLISTIC update, Eq. (14) of Treiber, M. & Kanagaraj, V. "Comparing
// numerical integration schemes for time-continuous car-following models",
// Physica A 419 (2015) 183-195 (arXiv:1403.4881):
//
//     x(t+h) = x + h v + (1/2) h^2 a(x,v)
//     v(t+h) = v + h a(x,v)
//
// "an Euler update for the speeds, and a trapezoidal update for the positions
// [...] The ballistic update always prevails Euler's method although both are
// of first order." Their Eq. (15) special case for a vehicle that stops inside
// a step is implemented too: when v + h a < 0 the vehicle is placed at
// x - v^2/(2a) and its speed reset to zero, which is where it actually comes
// to rest. Without it a stopping car takes a negative-speed step backwards.
//
// The base timestep is 0.25 s, which is the update step MOBIL Sec. 3 used
// ("For the following simulations, we used an explicit numerical update of
// dt = 0.25 s"), and one frame of this sim is always 0.25 s of road time. The
// timestep knob subdivides that frame into 1, 2, 4 or 8 substeps, so halving
// the timestep changes the numerics and NOT the clock — which is what makes
// "the answer does not depend on the timestep" a thing you can see rather than
// only assert. test_traffic_idm.cpp measures it.
//
// ── crash-freedom ───────────────────────────────────────────────────────────
//
// The IDM paper claims it: "it behaves accident-free because of the dependence
// on the relative velocity" (Sec. I), and Sec. II D derives why — for an
// approach at ratio z = b_kinematic/b, the braking term becomes z*b_kinematic,
// so a driver in trouble overreacts rather than under-reacts. MOBIL's Sec. 3
// restates it flatly: "Notice that the IDM guarantees crash-free driving."
//
// This file does not enforce it. It MEASURES it: every substep, each vehicle's
// gap to the leader it was actually following is recomputed after the move and
// compared with the same gap before, so a follower that has driven clean
// through its leader is caught even though the ring coordinate would wrap and
// hide it. A gap that reaches zero increments collisions_, which is a metric,
// and appears in subtitle() in capitals. Nothing is clamped to keep it from
// happening. The one guard is inside the acceleration function: a gap that has
// already gone non-positive is evaluated at 1 cm instead of dividing by zero,
// so the run stays finite and keeps reporting instead of turning into NaN. The
// collision has been counted by then and stays counted.
//
// ── what it does NOT do ─────────────────────────────────────────────────────
//
// No reaction time, no estimation error, no acceleration noise: this is the
// deterministic IDM of the 2000 paper, not the stochastic extensions. No
// on-ramp; the papers' bottleneck geometry is a different experiment. Lane
// changes are instantaneous, as in MOBIL ("the transition from the present lane
// to the target lane is neglected").

#pragma once
#include "../sim.hpp"
#include "../rng.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace bench {

class TrafficIdm final : public Sim {
public:
    // ── Treiber, Hennecke & Helbing (2000), Table I ─────────────────────────
    // Stored in the units the table prints, and converted at the point of use,
    // so the number in the code is the number in the paper.
    static constexpr double kV0Kmh   = 120.0;   // desired velocity v0
    static constexpr double kT       = 1.6;     // safe time headway, s
    static constexpr double kA       = 0.73;    // maximum acceleration, m/s^2
    static constexpr double kB       = 1.67;    // desired deceleration, m/s^2
    static constexpr double kDelta   = 4.0;     // acceleration exponent
    static constexpr double kS0      = 2.0;     // jam distance, m
    static constexpr double kS1      = 0.0;     // jam distance s1, m (paper sets it to 0)
    static constexpr double kCarLen  = 5.0;     // vehicle length l = 1/rho_max, m

    // ── Kesting, Treiber & Helbing (2007), Table 1 and Secs. 2.3 / 3 ────────
    static constexpr double kPoliteLo  = 0.0;   // published range of p ...
    static constexpr double kPoliteHi  = 1.0;   // ... Table 1: "0 ... 1"
    static constexpr double kAthr      = 0.1;   // changing threshold, m/s^2
    static constexpr double kBsafe     = 4.0;   // maximum safe deceleration, m/s^2
    static constexpr double kAbias     = 0.3;   // bias for right lane, m/s^2
    static constexpr double kVcritKmh  = 60.0;  // Sec. 2.3, "e.g., v_crit = 60 km/h"
    static constexpr double kTruckV0Kmh = 80.0; // Sec. 3
    static constexpr double kTruckLen   = 12.0; // Sec. 3, m
    static constexpr double kFrameSec   = 0.25; // Sec. 3, "numerical update of dt = 0.25 s"

    // Physically possible maximum deceleration on a dry road, quoted by MOBIL
    // Sec. 2.1 ("about 9 m/s^2 on dry road surfaces") and used only to set the
    // top of the b_safe knob — the paper's point is that b_safe belongs well
    // below it, and the knob lets you go and see what happens when it does not.
    static constexpr double kBmax = 9.0;

    // The gap an already-collided pair is evaluated at, so the run stays finite
    // and keeps reporting. Never used to prevent a collision — see the header.
    static constexpr double kCrashedGapEval = 0.01;

    struct Car {
        double x   = 0.0;   // FRONT bumper along the ring, m, in [0, L)
        double xu  = 0.0;   // the same motion never wrapped: monotone, for tracking
        double v   = 0.0;   // m/s
        double acc = 0.0;   // m/s^2, the acceleration used on the last substep
        double v0f = 1.0;   // per-vehicle multiplier on the desired speed
        double len = kCarLen;
        int    lane = 0;    // 0 = rightmost
        bool   truck = false;
        double brake_left = 0.0;   // seconds of forced braking still to serve
        double brake_rate = 0.0;   // m/s^2, magnitude
    };

    // Rendering geometry. Fixed, because the space-time history is a ring
    // buffer sized once — a road that changed height would either throw the
    // history away or draw it in the wrong place.
    // Wide on purpose. The road is 2 km long and a car is 5 m, so the whole
    // question is how many metres a pixel is worth: at 1024 across a 2 km ring
    // a car is 2.6 px and reads as a speck, at 1536 it is 4 px and reads as a
    // car. The bench fits a field by min(canvasW/w, canvasH/h), so a wide
    // field is also the shape that actually fills a wide window.
    static constexpr int kW        = 1536;
    static constexpr int kH        = 448;
    static constexpr int kMaxLanes = 4;
    static constexpr int kLaneRows = 20;
    static constexpr int kRoadTop  = 3;
    static constexpr int kRoadRows = kMaxLanes * (kLaneRows + 1) + 1;
    static constexpr int kGapRows  = 8;
    static constexpr int kStTop    = kRoadTop + kRoadRows + kGapRows;
    static constexpr int kStRows   = kH - kStTop;

    // Half-width of the smoothing window for the space-time speed field, in
    // metres of road. A space-time diagram is a MACROSCOPIC picture — the speed
    // field v(x,t) — and taking each column from the nearest single vehicle
    // gave a microscopic one instead: on a four-lane road, neighbouring columns
    // snapped to vehicles in different lanes doing different speeds, and the
    // jam bands were buried under a herringbone of individual trajectories.
    // 50 m spans several vehicle spacings at any density worth looking at and
    // is far below the hundreds of metres a jam occupies, so it removes the
    // texture and keeps the structure.
    static constexpr double kSmoothMetres = 50.0;

    // Palette layout.
    static constexpr std::uint8_t kOff     = 0;
    static constexpr std::uint8_t kAsphalt = 1;
    static constexpr std::uint8_t kMark    = 2;
    static constexpr std::uint8_t kSpeed0  = 3;
    static constexpr int kSpeedBuckets = 14;   // 10 km/h each, top bucket open

    TrafficIdm() : view_(kW, kH), st_(std::size_t(kStRows) * kW, kAsphalt) {
        about_ = Provenance{
            "Intelligent Driver Model + MOBIL", "2000 / 2007",
            "Martin Treiber, Ansgar Hennecke & Dirk Helbing (IDM); "
            "Arne Kesting, Martin Treiber & Dirk Helbing (MOBIL)",
            "Treiber, M., Hennecke, A. & Helbing, D. \"Congested traffic states in empirical "
            "observations and microscopic simulations\", Physical Review E 62 (2000) 1805-1824. "
            "Kesting, A., Treiber, M. & Helbing, D. \"General Lane-Changing Model MOBIL for "
            "Car-Following Models\", Transportation Research Record 1999 (2007) 86-94.",
            Replication::No,
            "No. A car carries no description of itself and copies nothing. What this rule does "
            "make more of is JAMS: one driver brakes once, and the disturbance behind them "
            "outlives them, grows, and walks backwards through traffic that never braked at all. "
            "That is a propagating pattern in a medium, the same category as a Life glider and "
            "the same distance from replication - which is exactly why it earns a place next to "
            "the replicators rather than among them.",
            "Each driver's acceleration is one continuous function of three numbers: own speed, "
            "gap to the car in front, and closing rate. Free road, it accelerates towards its "
            "desired speed; close up, it brakes on the ratio between the gap it wants and the gap "
            "it has. Nobody is told about a jam. The jam is what the equation does when a hundred "
            "of them are put in a line. Lane changes come from MOBIL, which decides by comparing "
            "accelerations - its own, and, weighted by a politeness factor, those of the drivers "
            "it would inconvenience."
        };
        build_palette();
        build_knobs();
        reset();
    }

    // ── Sim ─────────────────────────────────────────────────────────────────
    const Provenance&          about()      const override { return about_; }
    const std::vector<Swatch>& palette()    const override { return pal_; }
    const Field&               field()      const override { return view_; }
    std::uint64_t              generation() const override { return gen_; }
    std::vector<Knob>&         knobs()            override { return knobs_; }

    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        sync();
        // The structural knobs rebuild the traffic here rather than waiting for
        // the host to call reset(). Two reasons, both learned elsewhere in this
        // project: reset() alone would leave a lane count and a car array that
        // disagree if anything stepped in between, and the workbench's knob
        // sweep is entitled to set a knob and then step without resetting.
        if (k == "lanes" || k == "length" || k == "density" || k == "trucks" ||
            k == "spread" || k == "jitter" || k == "seed")
            reset();
    }

    void reset() override {
        sync();
        gen_ = 0;
        t_   = 0.0;
        collisions_   = 0;
        laneChanges_  = 0;
        minGapEver_   = 1e18;
        rng_.reseed(mix_seed(0x7A11Cull, seed_));
        cars_.clear();
        lane_.assign(std::size_t(lanes_), {});

        // Vehicles per lane. The density knob is per lane, so the total scales
        // with the lane count on purpose: three lanes at 22 veh/km/lane is
        // three times the traffic, not the same traffic spread thinner.
        const int perLane = int(density_ * L_ / 1000.0 + 0.5);
        for (int li = 0; li < lanes_; ++li) {
            // Draw this lane's vehicle types first, because a truck is 12 m and
            // a car is 5 m and how many fit depends on which they are.
            std::vector<bool> truck;
            truck.reserve(std::size_t(perLane));
            for (int i = 0; i < perLane; ++i) truck.push_back(rng_.unit() < truckFrac_);
            // EQUAL GAPS, not equal spacing, and that distinction was a real
            // bug here rather than a nicety. Placing vehicles at L/n apart
            // overlaps a 12 m truck with whatever is behind it as soon as the
            // spacing falls below 12 m, which at 20% trucks starts at about
            // 95 veh/km — the road began the run already crashed, the collision
            // counter fired 812 times, and the model was being blamed for the
            // initial condition. Equal gaps cannot overlap while the bumpers
            // fit, and for identical vehicles it is the same placement.
            double lens = 0.0;
            while (!truck.empty()) {
                lens = 0.0;
                for (bool tk : truck) lens += (tk ? kTruckLen : kCarLen);
                if (lens + 0.5 * double(truck.size()) <= L_) break;
                truck.pop_back();
            }
            const int n = int(truck.size());
            if (n == 0) continue;
            const double g0 = (L_ - lens) / double(n);   // >= 0.5 m by construction
            double at = 0.0;
            for (int i = 0; i < n; ++i) {
                Car c;
                c.truck = truck[std::size_t(i)];
                c.len   = c.truck ? kTruckLen : kCarLen;
                c.lane  = li;
                at     += c.len;
                c.x     = at;            // front bumper
                c.xu    = c.x;
                at     += g0;
                // MOBIL Sec. 3 spreads the desired velocity "with a variation
                // of +-20% for each single vehicle in order to increase the
                // degree of heterogeneity". Same shape here, width on a knob.
                c.v0f   = 1.0 + spread_ * (2.0 * double(rng_.unit()) - 1.0);
                cars_.push_back(c);
            }
            // Equilibrium speed for the gap each vehicle actually has, so a
            // fresh road is a genuine fixed point of the model and any wave
            // that appears later was caused by something. The jitter knob is
            // the only thing that breaks it, and it defaults to zero.
            const int base = int(cars_.size()) - n;
            for (int i = 0; i < n; ++i) {
                Car& c = cars_[std::size_t(base + i)];
                const Car& ld = cars_[std::size_t(base + (i + 1) % n)];
                const double s = (n == 1) ? (L_ - ld.len) : g0;
                c.v = equilibrium_speed(s, desired(c));
                if (jitter_ > 0.0)
                    c.v = std::max(0.0, c.v + jitter_ * (2.0 * double(rng_.unit()) - 1.0));
            }
        }
        sort_lanes();
        // A blank history is a lie about a road that has not run yet, but an
        // asphalt-coloured one is not: it says "no samples", and the fill
        // counter stops the renderer drawing rows that were never measured.
        std::fill(st_.begin(), st_.end(), kAsphalt);
        stHead_ = 0;
        stFill_ = 0;
        publish();
    }

    void step() override {
        advance_frame();
        publish();
        ++gen_;
    }

    // The physics of one frame, without the rendering. step() is exactly this
    // plus publish() plus the counter, and test_traffic_idm.cpp asserts that
    // driving the sim through this path and through step() gives identical
    // vehicle states — so measurements taken the fast way are measurements of
    // the same simulation the workbench draws.
    //
    // sync() lives here rather than in step() for that reason: if it were one
    // level up, the two paths would be running different parameters the moment
    // anyone touched a knob, and the equivalence would quietly stop being true.
    void advance_frame() {
        sync();
        for (int i = 0; i < sub_; ++i) substep(kFrameSec / double(sub_));
    }

    std::string subtitle() const override {
        char b[256];
        const double perLaneKm = (lanes_ > 0 && L_ > 0.0)
            ? double(cars_.size()) / (double(lanes_) * L_ / 1000.0) : 0.0;
        std::snprintf(b, sizeof b, "%d lane%s x %.0f m ring, %d vehicles (%.1f veh/km/lane), "
                      "t = %.0f s, dt = %.4g s, %s MOBIL",
                      lanes_, lanes_ == 1 ? "" : "s", L_, int(cars_.size()), perLaneKm,
                      t_, kFrameSec / double(sub_), rules_ ? "asymmetric" : "symmetric");
        std::string out = b;
        if (collisions_ > 0) {
            char c[96];
            std::snprintf(c, sizeof c, "   ** %d COLLISIONS - gap went negative **", collisions_);
            out += c;
        }
        return out;
    }

    std::vector<Metric> metrics() const override {
        std::vector<Metric> out;
        double meanV = 0.0, var = 0.0, minGap = 1e18;
        const int n = int(cars_.size());
        if (n > 0) {
            for (const Car& c : cars_) meanV += c.v;
            meanV /= double(n);
            for (const Car& c : cars_) { const double d = c.v - meanV; var += d * d; }
            var /= double(n);
            for (int li = 0; li < lanes_; ++li) {
                const auto& L = lane_[std::size_t(li)];
                const int m = int(L.size());
                for (int k = 0; k < m; ++k) {
                    const int me = L[std::size_t(k)];
                    const int ld = L[std::size_t((k + 1) % m)];
                    minGap = std::min(minGap, gap(me, ld));
                }
            }
        }
        if (minGap > 1e17) minGap = 0.0;
        const double km = L_ / 1000.0;
        const double perLaneKm = (lanes_ > 0 && km > 0.0) ? double(n) / (double(lanes_) * km) : 0.0;
        const double hours = t_ / 3600.0;

        out.push_back(Metric{"mean speed (km/h)", meanV * 3.6, 150.0, Metric::Higher});
        // Q = rho * v, the definition every fundamental diagram is drawn from.
        out.push_back(Metric{"flow (veh/h/lane)", perLaneKm * meanV * 3.6, 2600.0, Metric::Higher});
        // The stop-and-go signature. A uniform road has zero spread; a road
        // with a wave in it has vehicles at 0 and vehicles at 100 at once.
        out.push_back(Metric{"speed spread (km/h)", std::sqrt(var) * 3.6, 60.0, Metric::Neither});
        out.push_back(Metric{"min gap (m)", minGap, 0.0, Metric::Neither});
        out.push_back(Metric{"lane changes /km/h",
                             (hours > 0.0 && km > 0.0) ? double(laneChanges_) / (hours * km) : 0.0,
                             0.0, Metric::Neither});
        // Not decoration. The model's headline claim is that this stays at
        // zero; putting it on the plot is how you would ever notice it did not.
        out.push_back(Metric{"collisions", double(collisions_), 0.0, Metric::Lower});
        return out;
    }

    // Click the road to make the nearest vehicle brake — the whole experiment
    // of the 2000 paper, on a mouse button. 3 m/s^2 for 2 s is a firm but
    // ordinary brake application, below the 9 m/s^2 MOBIL quotes as the
    // physical maximum on dry road.
    bool poke(float nx, float /*ny*/) override {
        if (cars_.empty()) return false;
        const double x = std::clamp(double(nx), 0.0, 1.0) * L_;
        int best = 0; double bestD = 1e18;
        for (int i = 0; i < int(cars_.size()); ++i) {
            double d = std::fabs(cars_[std::size_t(i)].x - x);
            d = std::min(d, L_ - d);
            if (d < bestD) { bestD = d; best = i; }
        }
        brake(best, 3.0, 2.0);
        return true;
    }

    // ── test hooks ──────────────────────────────────────────────────────────
    // Public because test_traffic_idm.cpp is the thing that makes any claim
    // here worth reading. They expose state; none of them change the rule.
    [[nodiscard]] const std::vector<Car>& cars() const { return cars_; }
    [[nodiscard]] double time()        const { return t_; }
    [[nodiscard]] double road_length() const { return L_; }
    [[nodiscard]] double dt()          const { return kFrameSec / double(sub_); }
    [[nodiscard]] int    lanes()       const { return lanes_; }
    [[nodiscard]] int    collisions()  const { return collisions_; }
    [[nodiscard]] long long lane_changes() const { return laneChanges_; }
    [[nodiscard]] double min_gap_ever() const { return minGapEver_ > 1e17 ? 0.0 : minGapEver_; }

    // Make one vehicle brake: "at least this hard", so it can never accelerate
    // into its own leader while the pulse lasts. A pure -rate override would be
    // a perturbation that can cause a crash by itself, and then the
    // crash-freedom measurement would be measuring the perturbation.
    void brake(int car, double decel, double seconds) {
        if (car < 0 || car >= int(cars_.size())) return;
        cars_[std::size_t(car)].brake_rate = decel;
        cars_[std::size_t(car)].brake_left = seconds;
    }

    // Ve(s): the speed at which a driver with this desired speed is content
    // with this gap. Solves 1 - (v/v0)^delta - ((s0 + vT)/s)^2 = 0, whose left
    // side is strictly decreasing in v, by bisection. Eq. (8) of the paper
    // gives the inverse relation in closed form; bisecting the forward one
    // avoids having to invert it for a general delta.
    [[nodiscard]] double equilibrium_speed(double s, double v0) const {
        if (v0 <= 0.0) return 0.0;
        auto f = [&](double v) {
            const double star = s0_ + v * T_;
            return 1.0 - std::pow(v / v0, delta_) - (star / s) * (star / s);
        };
        if (s <= s0_ || f(0.0) <= 0.0) return 0.0;
        if (f(v0) >= 0.0) return v0;
        double lo = 0.0, hi = v0;
        for (int i = 0; i < 80; ++i) {
            const double mid = 0.5 * (lo + hi);
            if (f(mid) > 0.0) lo = mid; else hi = mid;
        }
        return 0.5 * (lo + hi);
    }

    // The IDM acceleration itself, exposed so the test can check the equation
    // against hand-worked values rather than only against the sim's own output.
    [[nodiscard]] double idm(double v, double dv, double s, double v0) const {
        if (s <= 0.0) s = kCrashedGapEval;
        const double star = s0_ + v * T_ + v * dv / (2.0 * std::sqrt(a_ * b_));
        const double ratio = star / s;
        return a_ * (1.0 - std::pow(v / v0, delta_) - ratio * ratio);
    }
    [[nodiscard]] double desired(const Car& c) const {
        // Trucks keep MOBIL's published 80 km/h; the knob is the CARS' desired
        // speed. Two vehicle classes with two sources, kept apart on purpose.
        return (c.truck ? kTruckV0Kmh / 3.6 : v0Car_) * c.v0f;
    }

private:
    // ── knob plumbing ───────────────────────────────────────────────────────
    [[nodiscard]] float knobv(const char* key) const {
        for (const auto& kn : knobs_) if (kn.key == key) return kn.value;
        return 0.0f;
    }

    void build_knobs() {
        knobs_ = {
            // — the road —
            {"lanes", "lanes", 1.f, float(kMaxLanes), 3.f, 1.f, {}, true,
             "Lanes in one direction. MOBIL only has anything to do above one; at one lane this "
             "is the pure single-lane IDM of the 2000 paper, which is what the fundamental "
             "diagram and the stop-and-go measurement in the test are run on."},
            {"length", "road length", 0.f, 2.f, 1.f, 1.f, {"1 km", "2 km", "4 km"}, true,
             "Circumference of the ring. Longer roads hold a wave for longer before it laps "
             "itself: at the 14.6 km/h measured here a wave crosses 4 km in about 16 minutes."},
            {"density", "density (veh/km/lane)", 5.f, 140.f, 22.f, 1.f, {}, true,
             "Vehicles per kilometre PER LANE. This is the x axis of the fundamental diagram and "
             "the single control that decides whether traffic is stable, metastable or "
             "spontaneously jamming. Above about 1/(l+s0) = 143 veh/km the vehicles would not "
             "fit; the road drops the overflow and the metric reports what was actually placed. "
             "The default 22 sits just under the capacity measured on a single lane here "
             "(1689 veh/h at 25 veh/km), which is where lane changing is busiest; for the "
             "stop-and-go wave the paper is about, go to one lane at 40."},
            // — IDM, Table I of Treiber, Hennecke & Helbing (2000) —
            {"v0", "desired speed v0 (cars, km/h)", 60.f, 150.f, float(kV0Kmh), 1.f, {}, false,
             "Table I: 120 km/h. Free-road target speed for cars; trucks keep MOBIL's published "
             "80 km/h regardless. Sets the free branch of the fundamental diagram."},
            {"T", "safe time headway T (s)", 0.5f, 3.f, float(kT), 0.05f, {}, false,
             "Table I: 1.6 s. The paper reproduced every congested state it observed by varying "
             "THIS parameter alone, so it is the one to reach for first. It sets the congested "
             "branch of the fundamental diagram, Q = [1 - rho(l+s0)]/T, and lowering it "
             "destabilises traffic."},
            {"a", "maximum acceleration a (m/s2)", 0.2f, 3.f, float(kA), 0.01f, {}, false,
             "Table I: 0.73 m/s2, which the paper notes is 0 to 100 km/h in 45 s - everyday "
             "acceleration, not the car's capability. Does not enter the fundamental diagram at "
             "all, so it tunes stability independently of flow: traffic gets MORE unstable as a "
             "falls."},
            {"b", "desired deceleration b (m/s2)", 0.5f, 4.f, float(kB), 0.01f, {}, false,
             "Table I: 1.67 m/s2. The comfortable braking rate the interaction term is scaled to. "
             "Raising it means less anticipative braking and, per the paper, more instability."},
            {"s0", "jam distance s0 (m)", 0.f, 8.f, float(kS0), 0.1f, {}, false,
             "Table I: 2 m. Bumper-to-bumper distance kept at a standstill. With the 5 m vehicle "
             "length it fixes the jam density at 1/(l+s0) = 143 veh/km."},
            {"delta", "acceleration exponent", 0.f, 3.f, 2.f, 1.f, {"1", "2", "4", "8"}, false,
             "Table I: 4. Shapes the approach to the desired speed. The paper: as delta goes to "
             "infinity the fundamental diagram becomes triangular, and it smooths as delta falls."},
            // — MOBIL, Table 1 of Kesting, Treiber & Helbing (2007) —
            {"polite", "politeness p", float(kPoliteLo), float(kPoliteHi), 0.3f, 0.05f, {}, false,
             "MOBIL's own parameter. 0 is a selfish lane-hopper, 1 is the parameter-free 'ideal "
             "MOBIL' that only changes lane when the sum of everyone's accelerations goes up. The "
             "range shown is the published one. The paper's own comparison is p = 0 against "
             "p = 1, and measured here selfish drivers change lanes about 5.6 times as often; "
             "the fall is steep out of 0 and then flattens into scatter, so do not expect a "
             "tidy monotone curve - the test prints the whole of it."},
            {"bsafe", "max safe deceleration b_safe (m/s2)", 1.f, float(kBmax), float(kBsafe), 0.1f, {}, false,
             "Table 1: 4 m/s2. A change is refused if it would force the new follower to brake "
             "harder than this. The paper puts it 'considerably below the physically possible "
             "maximum deceleration of about 9 m/s2 on dry roads'; the knob goes all the way there "
             "so you can watch what it costs."},
            {"athr", "changing threshold a_th (m/s2)", 0.f, 1.f, float(kAthr), 0.05f, {}, false,
             "Table 1: 0.1 m/s2. Inertia: a change has to be worth at least this much or it is "
             "not made. Acts globally on the lane-changing rate."},
            {"rules", "passing rules", 0.f, 1.f, 0.f, 1.f, {"symmetric (US)", "asymmetric (EU)"}, false,
             "Symmetric is Eq. (3) - lanes are interchangeable. Asymmetric adds the European pair: "
             "no passing on the right above v_crit = 60 km/h (Eq. 5) and a keep-right bias "
             "(Eqs. 6-7). It visibly redistributes traffic towards the right-hand lane."},
            {"abias", "keep-right bias a_bias (m/s2)", 0.f, 1.f, float(kAbias), 0.05f, {}, false,
             "Table 1: 0.3 m/s2. How hard the keep-right directive pushes. The paper notes it "
             "must exceed a_th or the threshold would forbid returning to the right on an empty "
             "road.", false, false, "rules", 1.f},
            // — the traffic mix —
            {"trucks", "truck fraction", 0.f, 0.5f, 0.2f, 0.05f, {}, true,
             "MOBIL Sec. 3 runs 20% trucks at 80 km/h and 12 m long. Without some heterogeneity "
             "identical vehicles reach a stationary state and there is nothing to overtake - "
             "which is the paper's own reason for introducing it."},
            {"spread", "desired-speed spread", 0.f, 0.4f, 0.2f, 0.02f, {}, true,
             "Uniform +-this fraction on each vehicle's desired speed. MOBIL Sec. 3 uses +-20%."},
            {"jitter", "initial speed jitter (m/s)", 0.f, 3.f, 0.f, 0.1f, {}, true,
             "Random speed offset at reset. Zero means the road starts at an exact equilibrium "
             "fixed point, which is what the stop-and-go experiment needs: then anything that "
             "happens later was caused by the braking event and not by the initial condition."},
            {"seed", "random seed", 1.f, 64.f, 1.f, 1.f, {}, true,
             "Which draw of truck positions, speed spread and jitter. Same seed, same road, every "
             "time."},
            // — numerics —
            {"dt", "timestep", 0.f, 3.f, 0.f, 1.f,
             {"0.25 s", "0.125 s", "0.0625 s", "0.03125 s"}, false,
             "Substeps within the fixed 0.25 s frame, so this changes the INTEGRATION and not the "
             "clock. A first-order scheme's error halves with the step; the test halves it three "
             "times and reports how little the answers move."},
        };
        sync();
    }

    void sync() {
        lanes_     = std::clamp(int(knobv("lanes") + 0.5f), 1, kMaxLanes);
        const int li = std::clamp(int(knobv("length") + 0.5f), 0, 2);
        L_         = 1000.0 * double(1 << li);
        density_   = double(knobv("density"));
        v0Car_     = double(knobv("v0")) / 3.6;
        T_         = double(knobv("T"));
        a_         = double(knobv("a"));
        b_         = double(knobv("b"));
        s0_        = double(knobv("s0"));
        delta_     = double(1 << std::clamp(int(knobv("delta") + 0.5f), 0, 3));
        polite_    = double(knobv("polite"));
        bsafe_     = double(knobv("bsafe"));
        athr_      = double(knobv("athr"));
        rules_     = std::clamp(int(knobv("rules") + 0.5f), 0, 1);
        abias_     = double(knobv("abias"));
        truckFrac_ = double(knobv("trucks"));
        spread_    = double(knobv("spread"));
        jitter_    = double(knobv("jitter"));
        seed_      = int(knobv("seed") + 0.5f);
        sub_       = 1 << std::clamp(int(knobv("dt") + 0.5f), 0, 3);
        if (int(lane_.size()) != lanes_) lane_.assign(std::size_t(lanes_), {});
    }

    // ── geometry on a ring ──────────────────────────────────────────────────
    // Bumper to bumper. A vehicle alone in its lane follows itself around the
    // whole ring, which is the correct closed-road answer and avoids a special
    // case in every caller.
    [[nodiscard]] double gap(int follower, int leader) const {
        const Car& f = cars_[std::size_t(follower)];
        const Car& l = cars_[std::size_t(leader)];
        if (follower == leader) return L_ - l.len;
        double d = l.x - f.x;
        if (d < 0.0) d += L_;
        return d - l.len;
    }

    void sort_lanes() {
        for (auto& l : lane_) l.clear();
        for (int i = 0; i < int(cars_.size()); ++i) {
            int li = cars_[std::size_t(i)].lane;
            if (li < 0) li = 0;
            if (li >= lanes_) li = lanes_ - 1;
            cars_[std::size_t(i)].lane = li;
            lane_[std::size_t(li)].push_back(i);
        }
        // Ties broken by index so the order is a function of the state and not
        // of whatever std::sort felt like: two vehicles at identical x in the
        // same lane would otherwise make the run non-reproducible.
        for (auto& l : lane_)
            std::sort(l.begin(), l.end(), [&](int p, int q) {
                const double xp = cars_[std::size_t(p)].x, xq = cars_[std::size_t(q)].x;
                return xp != xq ? xp < xq : p < q;
            });
        pos_.assign(cars_.size(), 0);
        for (int li = 0; li < lanes_; ++li) reindex(li);
    }
    void reindex(int li) {
        const auto& l = lane_[std::size_t(li)];
        for (int k = 0; k < int(l.size()); ++k) pos_[std::size_t(l[std::size_t(k)])] = k;
    }

    struct Neigh { int lead = -1; int foll = -1; };

    // Neighbours of a vehicle in the lane it is already in.
    [[nodiscard]] Neigh own_neighbours(int i) const {
        const auto& l = lane_[std::size_t(cars_[std::size_t(i)].lane)];
        const int m = int(l.size());
        Neigh n;
        if (m == 0) return n;
        if (m == 1) { n.lead = i; n.foll = i; return n; }
        const int k = pos_[std::size_t(i)];
        n.lead = l[std::size_t((k + 1) % m)];
        n.foll = l[std::size_t((k - 1 + m) % m)];
        return n;
    }

    // Neighbours a vehicle WOULD have if it appeared at x in lane li. Used only
    // for MOBIL's prospective evaluation, so the vehicle itself is not in this
    // lane's list and cannot be found by the search.
    [[nodiscard]] Neigh insert_neighbours(int li, double x) const {
        const auto& l = lane_[std::size_t(li)];
        const int m = int(l.size());
        Neigh n;
        if (m == 0) return n;
        const auto it = std::lower_bound(l.begin(), l.end(), x, [&](int q, double val) {
            return cars_[std::size_t(q)].x < val;
        });
        const int idx = int(it - l.begin());
        n.lead = l[std::size_t(idx % m)];
        n.foll = l[std::size_t((idx - 1 + m) % m)];
        return n;
    }

    [[nodiscard]] double accel_of(int i, int lead) const {
        const Car& c = cars_[std::size_t(i)];
        if (lead < 0) return a_ * (1.0 - std::pow(c.v / desired(c), delta_));   // free road
        return idm(c.v, c.v - cars_[std::size_t(lead)].v, gap(i, lead), desired(c));
    }
    [[nodiscard]] double accel_free(int i) const {
        const Car& c = cars_[std::size_t(i)];
        return a_ * (1.0 - std::pow(c.v / desired(c), delta_));
    }

    // ── MOBIL ───────────────────────────────────────────────────────────────
    // Returns the MARGIN by which the incentive criterion is met, so that when
    // both neighbouring lanes qualify the paper's tie-break ("the change is
    // performed to the lane where the incentive is larger") compares like with
    // like even under the asymmetric rules, whose two thresholds differ.
    bool evaluate(int c, int t, double& margin) const {
        const Car& C = cars_[std::size_t(c)];
        const Neigh cur = own_neighbours(c);
        const Neigh tgt = insert_neighbours(t, C.x);

        // Not one of the paper's criteria: a physical impossibility. MOBIL
        // relies on the car-following model returning boundless braking for a
        // vanishing gap, which is true of the IDM but becomes a division by
        // zero for a gap of exactly zero or less. An insertion that overlaps
        // steel is refused outright.
        if (tgt.lead >= 0 && gap(c, tgt.lead) <= 0.0) return false;
        if (tgt.foll >= 0 && gap(tgt.foll, c) <= 0.0) return false;

        const double ac  = accel_of(c, cur.lead);
        const double act = (tgt.lead >= 0) ? accel_of(c, tgt.lead) : accel_free(c);

        double an = 0.0, ant = 0.0;
        if (tgt.foll >= 0) {
            // A lone vehicle in the target lane currently follows itself round
            // the ring; with two or more, its leader is the one ahead of it.
            const int nLead = (tgt.lead >= 0 && tgt.lead != tgt.foll) ? tgt.lead : tgt.foll;
            an  = accel_of(tgt.foll, nLead);
            ant = accel_of(tgt.foll, c);
            if (ant < -bsafe_) return false;                    // Eq. (2)
        }
        double ao = 0.0, aot = 0.0;
        if (cur.foll >= 0 && cur.foll != c) {
            ao = accel_of(cur.foll, c);
            // After c leaves: if the old lane held only c and this follower,
            // the follower is alone and follows itself.
            const int oLead = (cur.lead == cur.foll) ? cur.foll : cur.lead;
            aot = accel_of(cur.foll, oLead);
        }

        if (rules_ == 0) {                                       // Eq. (3)
            margin = (act - ac) + polite_ * ((ant - an) + (aot - ao)) - athr_;
            return margin > 0.0;
        }

        // Asymmetric rules. The paper writes them for two lanes; applied here
        // to the two lanes involved in the prospective change, which is the
        // generalisation it calls straightforward.
        const bool toRight = (t < C.lane);
        const double aRight = toRight ? act : ac;
        const double aLeft  = toRight ? ac  : act;
        const int leftLead  = toRight ? cur.lead : tgt.lead;
        double aRightEur = aRight;                               // Eq. (5)
        if (leftLead >= 0) {
            const double vll = cars_[std::size_t(leftLead)].v;
            if (C.v > vll && vll > kVcritKmh / 3.6) aRightEur = std::min(aRight, aLeft);
        }
        margin = toRight
            ? (aRightEur - aLeft) + polite_ * (aot - ao) - (athr_ - abias_)   // Eq. (6)
            : (aLeft - aRightEur) + polite_ * (ant - an) - (athr_ + abias_);  // Eq. (7)
        return margin > 0.0;
    }

    void do_change(int i, int t) {
        const int from = cars_[std::size_t(i)].lane;
        auto& f = lane_[std::size_t(from)];
        f.erase(std::find(f.begin(), f.end(), i));
        cars_[std::size_t(i)].lane = t;
        auto& to = lane_[std::size_t(t)];
        to.insert(std::lower_bound(to.begin(), to.end(), cars_[std::size_t(i)].x,
                                   [&](int q, double val) {
                                       return cars_[std::size_t(q)].x < val;
                                   }), i);
        reindex(from);
        reindex(t);
        ++laneChanges_;
    }

    void mobil_pass() {
        if (lanes_ < 2) return;
        // Sequential in index order and applied immediately, so a vehicle
        // evaluating a gap sees anyone who has already taken it. Two vehicles
        // cannot merge into the same hole. The order is fixed, so the run is
        // reproducible; the paper does not specify one.
        for (int i = 0; i < int(cars_.size()); ++i) {
            int best = -1;
            double bestMargin = 0.0;
            for (int d = -1; d <= 1; d += 2) {
                const int t = cars_[std::size_t(i)].lane + d;
                if (t < 0 || t >= lanes_) continue;
                double m = 0.0;
                if (evaluate(i, t, m) && (best < 0 || m > bestMargin)) { best = t; bestMargin = m; }
            }
            if (best >= 0) do_change(i, best);
        }
    }

    // ── one integration substep ─────────────────────────────────────────────
    void substep(double h) {
        sort_lanes();
        if (cars_.empty()) { t_ += h; return; }
        mobil_pass();

        const std::size_t n = cars_.size();
        leadOf_.assign(n, -1);
        gapBefore_.assign(n, 0.0);
        for (std::size_t i = 0; i < n; ++i) {
            const Neigh nb = own_neighbours(int(i));
            leadOf_[i] = nb.lead;
            gapBefore_[i] = (nb.lead >= 0) ? gap(int(i), nb.lead) : 1e18;
            cars_[i].acc = accel_of(int(i), nb.lead);
            if (cars_[i].brake_left > 0.0) {
                cars_[i].acc = std::min(cars_[i].acc, -cars_[i].brake_rate);
                cars_[i].brake_left -= h;
            }
        }

        for (Car& c : cars_) {
            double dx, nv;
            if (c.acc < 0.0 && c.v + h * c.acc < 0.0) {
                // Treiber & Kanagaraj Eq. (15): the vehicle stops inside this
                // step, so put it where it actually stops.
                dx = -c.v * c.v / (2.0 * c.acc);
                nv = 0.0;
            } else {
                dx = c.v * h + 0.5 * c.acc * h * h;   // never negative: = h(v + v')/2, v' >= 0
                nv = c.v + c.acc * h;
            }
            c.x  += dx;
            c.xu += dx;
            c.v   = nv;
            while (c.x >= L_) c.x -= L_;
        }
        t_ += h;

        // Crash check, against the leader each vehicle was ACTUALLY following.
        // A follower that has driven clean through its leader ends up ahead of
        // it, and the ring gap would then read as almost a full lap of clear
        // road — a collision reported as the emptiest road on the map. The
        // before/after comparison catches it: no pair can honestly gain half a
        // ring of gap in a quarter of a second.
        for (std::size_t i = 0; i < n; ++i) {
            const int ld = leadOf_[i];
            if (ld < 0 || ld == int(i)) continue;
            double s = gap(int(i), ld);
            if (s > gapBefore_[i] + 0.5 * L_) s -= L_;
            if (s < minGapEver_) minGapEver_ = s;
            if (s <= 0.0) ++collisions_;
        }
    }

    // ── rendering ───────────────────────────────────────────────────────────
    void build_palette() {
        pal_.clear();
        pal_.push_back({{8, 11, 14}, "off-road"});
        pal_.push_back({{28, 31, 36}, "asphalt"});
        pal_.push_back({{74, 79, 88}, "lane marking"});
        // Stopped-to-free ramp. Five stops, linearly interpolated: the reason
        // for a ramp rather than a handful of bands is the space-time view,
        // where the shape of the wave is carried entirely by the gradient.
        struct Stop { double t; double r, g, b; };
        static const Stop stops[5] = {
            {0.00, 196,  46,  52},   // stopped
            {0.25, 232, 112,  46},
            {0.50, 240, 200,  84},
            {0.75, 138, 205, 110},
            {1.00,  90, 209, 196},   // free flow
        };
        for (int i = 0; i < kSpeedBuckets; ++i) {
            const double u = double(i) / double(kSpeedBuckets - 1);
            int k = 0;
            while (k < 3 && u > stops[k + 1].t) ++k;
            const double f = (u - stops[k].t) / (stops[k + 1].t - stops[k].t);
            auto mix = [&](double p, double q) { return std::uint8_t(p + (q - p) * f + 0.5); };
            char lbl[32];
            if (i == kSpeedBuckets - 1) std::snprintf(lbl, sizeof lbl, "%d+ km/h", i * 10);
            else                        std::snprintf(lbl, sizeof lbl, "%d-%d km/h", i * 10, i * 10 + 10);
            pal_.push_back({{mix(stops[k].r, stops[k + 1].r),
                             mix(stops[k].g, stops[k + 1].g),
                             mix(stops[k].b, stops[k + 1].b)}, lbl});
        }
    }

    [[nodiscard]] static std::uint8_t speed_index(double v) {
        int bckt = int(v * 3.6 / 10.0);
        bckt = std::clamp(bckt, 0, kSpeedBuckets - 1);
        return std::uint8_t(kSpeed0 + bckt);
    }

    // One row of the space-time diagram: the macroscopic speed field along the
    // road. Vehicles are binned into columns, the bins are box-averaged over a
    // window of kSmoothMetres either side using ring prefix sums, and the rare
    // column whose whole window is empty falls back to the nearest vehicle on
    // the ring. Nothing is invented: every value is a mean of real speeds, or
    // the speed of the nearest real vehicle when there is nothing else to say.
    void sample_spacetime() {
        const int W = view_.w;
        stSum_.assign(std::size_t(W), 0.0);
        stCnt_.assign(std::size_t(W), 0);
        for (const Car& c : cars_) {
            int px = int(c.x / L_ * double(W));
            px = std::clamp(px, 0, W - 1);
            stSum_[std::size_t(px)] += c.v;
            stCnt_[std::size_t(px)] += 1;
        }
        // Nearest-vehicle fill first, as the fallback layer.
        stVal_.assign(std::size_t(W), 0.0);
        stDist_.assign(std::size_t(W), 1 << 28);
        for (int i = 0; i < W; ++i)
            if (stCnt_[std::size_t(i)] > 0) {
                stVal_[std::size_t(i)] = stSum_[std::size_t(i)] / double(stCnt_[std::size_t(i)]);
                stDist_[std::size_t(i)] = 0;
            }
        // Two sweeps each way, twice round, so the fill crosses the seam.
        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i < 2 * W; ++i) {
                const std::size_t cur = std::size_t(i % W);
                const std::size_t prv = std::size_t((i % W + W - 1) % W);
                if (stDist_[prv] + 1 < stDist_[cur]) {
                    stDist_[cur] = stDist_[prv] + 1;
                    stVal_[cur]  = stVal_[prv];
                }
            }
        for (int pass = 0; pass < 2; ++pass)
            for (int i = 2 * W - 1; i >= 0; --i) {
                const std::size_t cur = std::size_t(i % W);
                const std::size_t nxt = std::size_t((i % W + 1) % W);
                if (stDist_[nxt] + 1 < stDist_[cur]) {
                    stDist_[cur] = stDist_[nxt] + 1;
                    stVal_[cur]  = stVal_[nxt];
                }
            }
        // Box average over +-R columns, wrapped, from prefix sums over one lap.
        //
        // The upper clamp is HARDENING rather than a fix for anything observed,
        // and is stated so nobody later assumes it was load-bearing: a window
        // of 2R+1 >= W would wrap onto itself, the [lo,hi) test would then read
        // lo <= hi and return one column instead of the whole ring, and the
        // smoothing would silently invert into no smoothing at all. It cannot
        // happen at any reachable setting — R is 50 m of road in columns, so
        // even the shortest 1 km ring gives R = 77 of 1536 — but the failure
        // mode is invisible rather than loud, which is the kind worth closing.
        const int R = std::clamp(int(kSmoothMetres * double(W) / L_ + 0.5), 1, W / 2 - 1);
        stPreS_.assign(std::size_t(W) + 1, 0.0);
        stPreN_.assign(std::size_t(W) + 1, 0.0);
        for (int i = 0; i < W; ++i) {
            stPreS_[std::size_t(i) + 1] = stPreS_[std::size_t(i)] + stSum_[std::size_t(i)];
            stPreN_[std::size_t(i) + 1] = stPreN_[std::size_t(i)] + double(stCnt_[std::size_t(i)]);
        }
        const double totS = stPreS_[std::size_t(W)], totN = stPreN_[std::size_t(W)];
        auto range = [&](int lo, int hi, double& sv, double& nv) {   // [lo,hi), wrapped
            if (lo <= hi) {
                sv = stPreS_[std::size_t(hi)] - stPreS_[std::size_t(lo)];
                nv = stPreN_[std::size_t(hi)] - stPreN_[std::size_t(lo)];
            } else {                                                  // crosses the seam
                sv = totS - stPreS_[std::size_t(lo)] + stPreS_[std::size_t(hi)];
                nv = totN - stPreN_[std::size_t(lo)] + stPreN_[std::size_t(hi)];
            }
        };
        stSmooth_.assign(std::size_t(W), 0.0);
        for (int i = 0; i < W; ++i) {
            const int lo = ((i - R) % W + W) % W;
            const int hi = ((i + R + 1) % W + W) % W;
            double sv = 0.0, nv = 0.0;
            range(lo, hi, sv, nv);
            stSmooth_[std::size_t(i)] = (nv > 0.0) ? sv / nv : stVal_[std::size_t(i)];
        }
        // A second, half-width box pass. Two boxes convolve to a triangle, and
        // that is what takes the sawtooth off the band edges: with a single box
        // the window gains and loses whole vehicles as it slides, so the mean
        // steps rather than slides, and every jam front came out serrated.
        const int R2 = std::max(1, R / 2);
        for (int i = 0; i < W; ++i)
            stPreS_[std::size_t(i) + 1] = stPreS_[std::size_t(i)] + stSmooth_[std::size_t(i)];
        const double tot2 = stPreS_[std::size_t(W)];
        for (int i = 0; i < W; ++i) {
            const int lo = ((i - R2) % W + W) % W;
            const int hi = ((i + R2 + 1) % W + W) % W;
            const double sv = (lo <= hi)
                ? stPreS_[std::size_t(hi)] - stPreS_[std::size_t(lo)]
                : tot2 - stPreS_[std::size_t(lo)] + stPreS_[std::size_t(hi)];
            stVal_[std::size_t(i)] = sv / double(2 * R2 + 1);
        }
        stHead_ = (stHead_ + 1) % kStRows;
        std::uint8_t* row = &st_[std::size_t(stHead_) * std::size_t(W)];
        const bool any = !cars_.empty();
        for (int i = 0; i < W; ++i)
            row[i] = any ? speed_index(stVal_[std::size_t(i)]) : kAsphalt;
        if (stFill_ < kStRows) ++stFill_;
    }

    void publish() {
        view_.fill(kOff);
        const int roadH = lanes_ * kLaneRows + (lanes_ + 1);
        const int top   = kRoadTop + (kRoadRows - roadH) / 2;
        for (int r = 0; r < roadH; ++r) {
            const int y = top + r;
            if (y < 0 || y >= view_.h) continue;
            const bool mark = (r % (kLaneRows + 1)) == 0;
            for (int x = 0; x < view_.w; ++x) view_.set(x, y, mark ? kMark : kAsphalt);
        }
        const double mpp = L_ / double(view_.w);
        for (const Car& c : cars_) {
            // Lane 0 is the rightmost lane and is drawn at the BOTTOM band, so
            // the keep-right rules read the way a road does.
            const int band = lanes_ - 1 - c.lane;
            const int y0 = top + 1 + band * (kLaneRows + 1);
            const int y1 = y0 + kLaneRows - 1;
            const std::uint8_t col = speed_index(c.v);
            const int xf = std::clamp(int(c.x / mpp), 0, view_.w - 1);
            const int nx = std::max(2, int(c.len / mpp + 0.5));
            for (int k = 0; k < nx; ++k) {
                int x = xf - k;
                while (x < 0) x += view_.w;
                for (int y = std::max(0, y0); y <= y1 && y < view_.h; ++y) view_.set(x, y, col);
            }
        }
        sample_spacetime();
        // Newest row at the top of the strip, older below: time therefore runs
        // UPWARD, which is how every space-time plot in the traffic literature
        // is drawn, and a wave moving against the traffic leans one way while a
        // wave moving with it leans the other.
        for (int r = 0; r < kStRows && r < stFill_; ++r) {
            const int src = ((stHead_ - r) % kStRows + kStRows) % kStRows;
            const std::uint8_t* row = &st_[std::size_t(src) * std::size_t(view_.w)];
            const int y = kStTop + r;
            if (y >= view_.h) break;
            for (int x = 0; x < view_.w; ++x) view_.set(x, y, row[x]);
        }
    }

    // ── state ───────────────────────────────────────────────────────────────
    Provenance          about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    Field               view_;

    std::vector<Car>              cars_;
    std::vector<std::vector<int>> lane_;
    std::vector<int>              pos_;
    std::vector<int>              leadOf_;
    std::vector<double>           gapBefore_;

    // Space-time history, a ring buffer of rendered rows plus the scratch the
    // sampler needs. Members rather than locals so a long measurement run does
    // not allocate four vectors per frame.
    std::vector<std::uint8_t> st_;
    std::vector<double>       stSum_, stVal_, stSmooth_, stPreS_, stPreN_;
    std::vector<int>          stCnt_, stDist_;
    int  stHead_ = 0;
    int  stFill_ = 0;

    Rng           rng_{0x7A11Cull};
    std::uint64_t gen_ = 0;
    double        t_   = 0.0;
    int           collisions_  = 0;
    long long     laneChanges_ = 0;
    double        minGapEver_  = 1e18;

    // Cached knob values, refreshed by sync().
    int    lanes_ = 3, rules_ = 0, seed_ = 1, sub_ = 1;
    double L_ = 2000.0, density_ = 30.0;
    double v0Car_ = kV0Kmh / 3.6, T_ = kT, a_ = kA, b_ = kB, s0_ = kS0, delta_ = kDelta;
    double polite_ = 0.3, bsafe_ = kBsafe, athr_ = kAthr, abias_ = kAbias;
    double truckFrac_ = 0.2, spread_ = 0.2, jitter_ = 0.0;
};

inline SimPtr make_traffic_idm() { return std::make_unique<TrafficIdm>(); }

} // namespace bench
