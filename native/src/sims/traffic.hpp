// traffic.hpp — the Nagel-Schreckenberg freeway automaton.
//
//   Nagel, K. & Schreckenberg, M. "A cellular automaton model for freeway
//   traffic", Journal de Physique I 2 (1992) 2221-2229.
//
// A single-lane ring road of L cells. A cell is either empty or holds one car,
// and a car carries an integer velocity 0..vmax. Every timestep the same four
// rules are applied IN PARALLEL to every car — every velocity is decided from
// the old configuration, and only then does anything move:
//
//   1. acceleration     v -> min(v + 1, vmax)
//   2. braking          v -> min(v, gap)
//   3. randomisation    with probability p,  v -> max(v - 1, 0)
//   4. motion           x -> x + v
//
// with gap = (position of the next car ahead) - (this car's position) - 1, i.e.
// the number of EMPTY cells in front. Rules and the gap definition as stated in
// the original paper and restated identically in the later literature — see
// Aleksandrowicz, Hartmann & Schreckenberg-lineage restatement in "Rare-Event
// Properties of the Nagel-Schreckenberg Model", arXiv:1908.04681, which gives
// them as v_n -> min(v_n + 1, v_max), v_n -> min(v_n, d_n), v_n -> max(v_n - 1, 0)
// with probability p, x_n(t+1) = x_n(t) + v_n, and d_n = x_{n+1} - x_n - 1.
//
// ── WHY STEP 3 IS THE WHOLE POINT ───────────────────────────────────────────
//
// Delete the randomisation and the model has no jams at all. Rules 1, 2 and 4
// alone are a deterministic, reversible-in-practice queueing rule: from a
// configuration where every gap is at least vmax, every car sits at vmax and
// the whole pattern translates rigidly forever. Nothing can ever start a
// slowdown, because nothing ever does anything a driver did not plan. The
// deterministic model therefore has a fundamental diagram that is two straight
// lines meeting at a corner, and a road below the corner density NEVER jams.
//
// Put step 3 back and a single car, for no reason, drops one cell per second.
// The car behind it has to brake. The car behind that one has to brake harder.
// Uniform traffic, given nothing but that, spontaneously grows a dense cluster
// of stopped cars — and the cluster is not carried along with the flow. Cars
// enter it at the back and leave it at the front, so the STRUCTURE moves
// BACKWARDS while every car in it moves forwards. That backward-travelling jam,
// out of nothing, is what the 1992 paper is famous for and the only reason this
// file exists. The `rule` knob turns step 3 off so the ablation is a control you
// can run, not an argument you have to take on trust: at p = 0 and a density of
// exactly 1/6 the road runs at vmax forever with zero stopped cars, and at
// p = 0.3 from the same start it jams within a few hundred steps. Both halves
// are asserted, with numbers, in test_traffic.cpp.
//
// ── SCALE ───────────────────────────────────────────────────────────────────
// The paper's calibration: one cell = 7.5 m (the space a car occupies in a
// dense jam, bumper to bumper), one timestep = 1 s. So one unit of velocity is
// 7.5 m/s = 27 km/h and vmax = 5 is 37.5 m/s = 135 km/h. Flow q is measured in
// cars per cell per timestep, which with a 1 s step is cars per second past a
// fixed point, so 3600*q is vehicles per hour per lane.
//
// The 7.5 m / 1 s calibration is the one universally quoted for this model and
// follows from it directly: a cell holds one car in dense jammed traffic, and
// 7.5 m is a car plus its bumper-to-bumper spacing. An earlier version of this
// comment attributed it to arXiv:1908.04681; a check of that paper did not find
// it there, so the attribution is withdrawn rather than replaced with another
// one I have not read.
//
// ── WHY p DEFAULTS TO 0.3 AND NOT THE PAPER'S 0.5 ───────────────────────────
// The original paper uses p = 0.5. Helbing's review (Rev. Mod. Phys. 73, 1067,
// 2001) records the practice as "for freeway traffic, Nagel and Schreckenberg
// have often set p = 0.5, which leads to a relatively noisy dynamics", with
// p = 0.2 and vmax = 2 for city traffic; later work commonly runs lower, e.g.
// p = 0.2 in arXiv:1908.04681, and p = 0.3 in the standard textbook restatement
// of the model. This sim defaults to p = 0.3, for a reason it MEASURES rather
// than asserts. test_traffic.cpp sweeps the fundamental diagram at both values
// on a 1000-cell ring and converts the peak with the paper's own 7.5 m / 1 s
// scale:
//
//     p = 0.3   peak q ~ 0.47 near density 0.11   ~  1700 veh/h/lane
//     p = 0.5   peak q ~ 0.34 near density 0.08   ~  1250 veh/h/lane
//
// Two significant figures on purpose. These were once written as 0.4672 at
// 0.110, which is one run of a stochastic model quoted as though it were a
// constant: the peak wanders with the seed, and the flow sits on a plateau
// across densities 0.11-0.15 rather than at a point. The test measures the
// value each time and asserts the SHAPE — rising, a maximum inside the
// published 0.08-0.15 window, then falling — because that is what is actually
// reproducible.
//
// against an observed freeway capacity of 2000 veh/h/lane in the 1985 Highway
// Capacity Manual, raised to 2400 in the 2000 edition (Texas Transportation
// Institute report 1196-2S, "Freeway Capacity in Texas"). NEITHER value reaches
// observed capacity, which is a limitation of the model and not something to
// hide behind a parameter choice — but 0.3 gets two thirds of the way there
// where 0.5 gets half. The paper's value is one notch of the p knob away and
// nothing here says it is wrong; 0.3 is a choice about which regime the default
// view shows, made on a measured number rather than a taste.

#pragma once
#include "../sim.hpp"
#include "../rng.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace bench {

// ── the model, with no display in it ────────────────────────────────────────
//
// Deliberately separate from the Sim: the fundamental diagram has to be
// measurable at arbitrary L, density, vmax and p, over tens of thousands of
// steps, without a 256-row picture being scrolled underneath it. The test
// measures THIS, and the Sim below is a view of it.
class NaSch {
public:
    // Cars are indexed in road order and that order never changes: a single
    // lane has no overtaking, so car i is behind car i+1 forever. Keeping the
    // list in fixed cyclic order means the gap to the car ahead is one
    // subtraction and there is never a re-sort — and it is also the reason the
    // model cannot produce a collision, which the test checks anyway.
    [[nodiscard]] int  length() const { return len_; }
    [[nodiscard]] int  cars()   const { return int(pos_.size()); }
    [[nodiscard]] int  vmax()   const { return vmax_; }
    [[nodiscard]] float p()     const { return p_; }
    [[nodiscard]] bool deterministic() const { return det_; }
    [[nodiscard]] std::uint64_t elapsed() const { return t_; }

    void set_vmax(int v)           { vmax_ = v < 1 ? 1 : v; }
    void set_p(float p)            { p_ = p < 0.f ? 0.f : (p > 1.f ? 1.f : p); }
    void set_deterministic(bool d) { det_ = d; }

    // `n` cars at random distinct cells, each starting at velocity v0.
    //
    // Exact count, not a per-cell coin flip: the fundamental diagram is a plot
    // AGAINST density, and a binomial density with its own +-sqrt(n) spread
    // smears the x axis of the very thing being measured. Partial Fisher-Yates
    // over the cell list gives exactly n distinct cells in O(L).
    //
    // v0 exists because the initial velocity is not a free choice for the SIM
    // even though it is irrelevant to the steady state. Seeded at rest, every
    // car on a freshly reset road is already stopped, so the click-to-brake
    // stamp has nothing to change and does nothing at all — which is exactly
    // what the bench's brush test found. Seeding the sim at vmax (a road that
    // is already running, which is also the more natural thing to arrive at)
    // makes the stamp a real disturbance from the first frame. The test asserts
    // separately that the fundamental diagram is the same either way, so this
    // is a fix to the interaction and not a thumb on the physics.
    void seed_random(int L, int n, std::uint64_t seed, int v0 = 0) {
        len_ = L < 2 ? 2 : L;
        n    = n < 0 ? 0 : (n > len_ ? len_ : n);
        rng_.reseed(seed);
        // Not `vector<int> cells(size_t(len_))` — that parses as a function
        // declaration (-Wvexing-parse) and the build refuses it.
        std::vector<int> cells;
        cells.resize(std::size_t(len_));
        for (int i = 0; i < len_; ++i) cells[std::size_t(i)] = i;
        for (int i = 0; i < n; ++i) {
            const int j = i + int(rng_.below(std::uint32_t(len_ - i)));
            std::swap(cells[std::size_t(i)], cells[std::size_t(j)]);
        }
        pos_.assign(cells.begin(), cells.begin() + n);
        std::sort(pos_.begin(), pos_.end());
        // Clamped, and safe even where it exceeds the gap: rule 2 brakes before
        // rule 4 moves anything, so a car seeded at vmax behind a neighbour one
        // cell away is braked to that gap on its first step rather than driving
        // through it.
        vel_.assign(std::size_t(n), v0 < 0 ? 0 : (v0 > vmax_ ? vmax_ : v0));
        nvel_.assign(std::size_t(n), 0);
        t_ = 0; crossings_ = 0;
    }

    // `n` cars equally spaced, every one at velocity v0.
    //
    // This is the initial condition the ablation needs. "Jams appear out of
    // uniform traffic" is only a claim about uniform traffic if the traffic
    // actually started uniform — from a random soup, a cluster is as likely to
    // be left over from the seeding as to have grown. Spacing is L/n by integer
    // division, so it is exactly equal only when n divides L; the test uses
    // pairs that divide.
    void seed_uniform(int L, int n, int v0, std::uint64_t seed) {
        len_ = L < 2 ? 2 : L;
        n    = n < 0 ? 0 : (n > len_ ? len_ : n);
        rng_.reseed(seed);
        pos_.resize(std::size_t(n));
        for (int i = 0; i < n; ++i) pos_[std::size_t(i)] = int(std::int64_t(i) * len_ / (n ? n : 1));
        vel_.assign(std::size_t(n), v0 < 0 ? 0 : (v0 > vmax_ ? vmax_ : v0));
        nvel_.assign(std::size_t(n), 0);
        t_ = 0; crossings_ = 0;
    }

    // `n` cars bumper to bumper from cell `at`, all at rest, rest of the ring
    // empty. One jam and nothing else.
    //
    // This is the initial condition that makes the backward jam speed a
    // controlled measurement instead of an estimate. Cars can only leave a
    // compact jam from its downstream end, one per timestep, each succeeding
    // with probability 1-p (accelerate to 1, gap is at least 1 so no braking,
    // then rule 3 either fires or does not). Every departure moves the front of
    // the remaining block back by exactly one cell, so the front must retreat
    // at -(1-p) cells per step. That number is DERIVED from the four rules
    // above, not remembered, and the test measures it.
    void seed_compact(int L, int n, int at, std::uint64_t seed) {
        len_ = L < 2 ? 2 : L;
        n    = n < 0 ? 0 : (n > len_ ? len_ : n);
        rng_.reseed(seed);
        pos_.resize(std::size_t(n));
        for (int i = 0; i < n; ++i) pos_[std::size_t(i)] = ((at + i) % len_ + len_) % len_;
        std::sort(pos_.begin(), pos_.end());
        vel_.assign(std::size_t(n), 0);
        nvel_.assign(std::size_t(n), 0);
        t_ = 0; crossings_ = 0;
    }

    // One timestep: the four rules, in parallel over every car.
    void step() {
        const std::size_t n = pos_.size();
        if (n == 0) { ++t_; crossings_ = 0; return; }

        // Rules 1-3. Every gap is read from the CURRENT configuration and every
        // new velocity is written to a second array — this is what "parallel"
        // means and it is the one thing that must not be shortcut. Updating in
        // place would let a car see the car ahead already moved this step,
        // which is a different (and much less jammy) model.
        for (std::size_t i = 0; i < n; ++i) {
            const int ahead = pos_[(i + 1) % n];
            int gap = ahead - pos_[i] - 1;
            if (gap < 0) gap += len_;   // the one pair that straddles the ring seam,
                                        // and also the lone-car case: gap = L-1
            // Clamp to the CURRENT vmax before applying the rules. Rule 1 only
            // ever increments under a guard and rule 2 only ever clamps down to
            // the gap, so lowering vmax while the road is running left every car
            // already above the new limit sitting there above it indefinitely —
            // a speed limit that binds new arrivals and not the traffic already
            // on the road. Nothing in the paper covers this because vmax is a
            // constant there; it is a live knob here, and a knob whose value the
            // simulation can be observed to violate is worse than no knob.
            int v = std::min(vel_[i], vmax_);
            if (v < vmax_) ++v;                  // 1. acceleration
            if (v > gap)   v = gap;              // 2. braking: never past the car ahead
            // 3. randomisation. Drawn for every car, not only the moving ones:
            // max(v-1,0) leaves a stopped car stopped, so the two readings of
            // the rule agree on the outcome and this one keeps the random
            // stream a fixed one-draw-per-car-per-step, which is what makes a
            // run reproducible from its seed.
            if (!det_ && rng_.unit() < p_) v = v > 0 ? v - 1 : 0;
            nvel_[i] = v;
        }

        // Rule 4. Motion, only after every velocity is decided.
        crossings_ = 0;
        for (std::size_t i = 0; i < n; ++i) {
            vel_[i] = nvel_[i];
            int np = pos_[i] + vel_[i];
            // A detector at one fixed point of the road: a car crosses the seam
            // between cell L-1 and cell 0 iff its move takes it past L. This is
            // flow measured the way a loop detector measures it, and it is a
            // genuinely independent check on the space-averaged sum(v)/L below.
            if (np >= len_) { np -= len_; ++crossings_; }
            pos_[i] = np;
        }
        ++t_;
    }

    // ── measurements ────────────────────────────────────────────────────────
    [[nodiscard]] long long sum_velocity() const {
        long long s = 0;
        for (int v : vel_) s += v;
        return s;
    }
    // Flow in cars per cell per timestep. Summing velocity over the road counts
    // every cell boundary crossed this step; dividing by L averages over all L
    // possible detector positions, so this is the same quantity the single
    // detector above measures, with less noise.
    [[nodiscard]] double flow() const {
        return len_ ? double(sum_velocity()) / double(len_) : 0.0;
    }
    [[nodiscard]] double mean_speed() const {
        return pos_.empty() ? 0.0 : double(sum_velocity()) / double(pos_.size());
    }
    [[nodiscard]] int stopped() const {
        int s = 0;
        for (int v : vel_) if (v == 0) ++s;
        return s;
    }
    [[nodiscard]] int crossings() const { return crossings_; }
    [[nodiscard]] double density() const {
        return len_ ? double(pos_.size()) / double(len_) : 0.0;
    }

    [[nodiscard]] const std::vector<int>& positions()  const { return pos_; }
    [[nodiscard]] const std::vector<int>& velocities() const { return vel_; }

    // Stop the car nearest to `cell`. The hand-made jam: it is exactly what
    // rule 3 does by itself, aimed.
    bool brake_near(int cell) {
        if (pos_.empty()) return false;
        std::size_t best = 0;
        int bestd = len_;
        for (std::size_t i = 0; i < pos_.size(); ++i) {
            int d = pos_[i] - cell;
            if (d < 0) d = -d;
            if (d > len_ / 2) d = len_ - d;
            if (d < bestd) { bestd = d; best = i; }
        }
        vel_[best] = 0;
        return true;
    }

private:
    std::vector<int> pos_, vel_, nvel_;
    Rng   rng_{0x7A11Cull};
    int   len_  = 512;
    int   vmax_ = 5;
    float p_    = 0.30f;
    bool  det_  = false;
    int   crossings_ = 0;
    std::uint64_t t_ = 0;
};

// ── the sim: a space-time diagram of the road ───────────────────────────────
//
// Position on x, time down. Animating dots on a line shows you cars; it does
// NOT show you that the jam is moving the other way, because your eye tracks
// the cars. On a space-time diagram a car is a stripe whose slope is its speed,
// and a jam is a band of stopped cars whose slope has the OPPOSITE sign. The
// backward-travelling jam is not something you have to be told about here; it
// is the only thing on the picture leaning the wrong way.
class Traffic final : public Sim {
public:
    static constexpr int kHistory = 256;   // rows of time kept on screen

    explicit Traffic(int roadLength = 512)
        : about_{
            "Nagel-Schreckenberg freeway traffic", "1992",
            "Kai Nagel and Michael Schreckenberg",
            "Nagel, K. & Schreckenberg, M. \"A cellular automaton model for freeway traffic\", "
            "Journal de Physique I 2 (1992) 2221-2229",
            Replication::No,
            "No, and it is not trying to. Cars are conserved exactly: the ring ends every step "
            "with the number of cars it was seeded with, and no rule here creates one. What it "
            "does make is a structure that persists and propagates without being carried - a jam "
            "- but a jam is a pattern in a fixed population, not a lineage, and it dissolves "
            "rather than reproducing.",
            "Four rules per car per second: speed up, brake to the gap ahead, dawdle at random, "
            "move. The third rule is the model. Without it uniform traffic stays uniform forever; "
            "with it a jam grows out of nothing and travels backwards against the flow, which is "
            "the thing real freeways do and the thing this model was written to explain."
          },
          pal_{
            {{ 14,  16,  22}, "empty road"},
            {{214,  58,  48}, "stopped (v=0)"},
            {{233, 118,  46}, "v=1  27 km/h"},
            {{240, 178,  58}, "v=2  54 km/h"},
            {{196, 214,  72}, "v=3  81 km/h"},
            {{112, 205, 128}, "v=4 108 km/h"},
            {{ 78, 200, 208}, "v=5 135 km/h"}
          }
    {
        // Nearest listed length to the one actually constructed. Derived, not
        // typed in: my_sim.hpp records that a literal default here read "512"
        // over a 256-wide field, and the same trap is here.
        int li = 0;
        for (int i = 1; i < 4; ++i)
            if (std::abs((256 << i) - roadLength) < std::abs((256 << li) - roadLength)) li = i;

        knobs_ = {
            // Density is the x axis of the fundamental diagram, so it is the
            // knob this sim is really about. on_reset because the car count is
            // fixed at seeding — a ring cannot gain cars halfway through
            // without inventing them.
            {"density", "density (cars/cell)", 0.01f, 0.95f, 0.15f, 0.01f, {}, true,
             "Cars per cell. The whole fundamental diagram is a sweep of this. Measured here at "
             "the default p = 0.3: flow peaks somewhere near density 0.11 and falls away above it, so the "
             "busiest road is not the fullest one. Apply setup to seed a new road."},
            // vmax is read by the rule every step, so it is live.
            {"vmax", "vmax (cells/step)", 1.f, 5.f, 5.f, 1.f, {}, false,
             "Top speed in cells per second. The paper's freeway value is 5, which at 7.5 m a "
             "cell and 1 s a step is 135 km/h. Helbing's review gives vmax 2 with p 0.2 for city "
             "traffic. At 1 this becomes a single-speed hopping model: measured peak flow 0.500 "
             "at density 0.500 with the randomisation off, against 0.830 at 0.170 for vmax 5."},
            // p is read by the rule every step, so it is live too — and it is
            // inert whenever the rule switch is on the deterministic setting,
            // which is stated rather than left to look broken.
            {"p", "randomisation p", 0.f, 1.f, 0.30f, 0.01f, {}, false,
             "Probability that a car drops one cell/s for no reason. This single number is the "
             "difference between a model with no jams and a model with jams: at 0 uniform "
             "traffic never jams, at 0.3 it jams out of nothing. The 1992 paper uses 0.5.",
             false, false, "rule", 0.f},
            // The ablation, as a control rather than an argument.
            {"rule", "rule", 0.f, 1.f, 0.f, 1.f,
             {"stochastic (Nagel-Schreckenberg)", "deterministic (rule 3 off)"}, false,
             "Turns the randomisation step off entirely. Exactly equivalent to p = 0 - "
             "test_traffic.cpp asserts the two give bit-identical runs over 5000 steps - and it "
             "exists so the control is one click rather than a slider you have to remember to "
             "zero. Off, this road never produces a single stopped car below density 1/6."},
            {"length", "road length L (cells)", 0.f, 3.f, float(li), 1.f,
             {"256", "512", "1024", "2048"}, true,
             "Cells around the ring. At 7.5 m a cell, 512 is 3.8 km of road. It does NOT change "
             "the mean flow, and that is correct rather than broken - flow is cars per cell per "
             "step, so a longer road carries proportionally more cars. What it changes is the "
             "noise: measured standard deviation of flow falls 0.042, 0.028, 0.021, 0.014 across "
             "256 to 2048, which is 1/sqrt(L). The picture keeps 256 timesteps whatever L is."},
        };

        rebuild();
        reset();
    }

    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_;   }
    const Field&               field()   const override { return view_;  }
    std::uint64_t              generation() const override { return road_.elapsed(); }
    std::vector<Knob>&         knobs()   override { return knobs_; }

    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = kn.quantised(v);
        // Geometry has to be rebuilt HERE and not in reset(): reset() refills
        // the road it already has, so a length change made there would be a
        // no-op that looks exactly like a working knob. my_sim.hpp records the
        // same lesson for its size knob.
        if (k == "length") { rebuild(); reset(); }
        if (k == "vmax")   road_.set_vmax(int(knob("vmax") + 0.5f));
        if (k == "p")      road_.set_p(knob("p"));
        if (k == "rule")   road_.set_deterministic(knob("rule") > 0.5f);
    }

    void reset() override {
        road_.set_vmax(int(knob("vmax") + 0.5f));
        road_.set_p(knob("p"));
        road_.set_deterministic(knob("rule") > 0.5f);
        const int L = length();
        const int n = int(double(knob("density")) * L + 0.5);
        // A fixed seed, so the same density gives the same road every time.
        // That is the whole of what "reproducible" means here. Cars start at
        // vmax — see seed_random for why the sim does not start them at rest.
        road_.seed_random(L, n, 0x7A11Cull, road_.vmax());
        view_.fill(0);
        flowRing_.assign(std::size_t(kHistory), 0.0);
        detRing_.assign(std::size_t(kHistory), 0.0);
        ringN_ = 0; ringAt_ = 0;
        draw_row();
    }

    void step() override {
        road_.step();
        // Scroll the picture up by one row and draw the new state at the
        // bottom, so time runs downwards. The whole array moves each step,
        // which at the largest length here is 2048*255 bytes.
        //
        // That is NOT free, and the first version of this comment claimed it
        // was. Measured: at L=2048 a full step costs 11.80 us, of which the
        // model is 1.71 us and this scroll is 10.09 us — the picture is six
        // times the cost of the simulation it is showing. It stays anyway,
        // because 11.8 us is 85,000 steps a second and the alternative is a
        // ring buffer with a moving seam, which is a visible tear across a
        // space-time diagram and would have to be un-rotated for field()
        // regardless. The cost is real and small; the wrong part was saying it
        // was smaller than the model's.
        std::memmove(view_.cells.data(),
                     view_.cells.data() + std::size_t(view_.w),
                     std::size_t(view_.w) * std::size_t(view_.h - 1));
        draw_row();

        flowRing_[ringAt_] = road_.flow();
        detRing_[ringAt_]  = double(road_.crossings());
        ringAt_ = (ringAt_ + 1) % std::size_t(kHistory);
        if (ringN_ < std::size_t(kHistory)) ++ringN_;
    }

    // Both flow numbers are averaged over exactly the window on screen, so what
    // the plot says and what the picture shows are the same 256 seconds.
    std::vector<Metric> metrics() const override {
        double f = 0, d = 0;
        for (std::size_t i = 0; i < ringN_; ++i) { f += flowRing_[i]; d += detRing_[i]; }
        const double n = ringN_ ? double(ringN_) : 1.0;
        const int cars = road_.cars();
        return {
            Metric{"flow q (cars/cell/step)", f / n, 1.0, Metric::Higher},
            Metric{"detector flow (cars/step)", d / n, 1.0, Metric::Higher},
            Metric{"mean speed (cells/step)", road_.mean_speed(), 5.0, Metric::Higher},
            Metric{"stopped cars", cars ? double(road_.stopped()) / cars : 0.0, 1.0, Metric::Lower},
            Metric{"density (cars/cell)", road_.density(), 1.0, Metric::Neither},
        };
    }

    [[nodiscard]] std::string subtitle() const override {
        char b[192];
        std::snprintf(b, sizeof b,
                      "vmax %d  p %.2f  rho %.3f  L %d   |   1 cell = 7.5 m, 1 step = 1 s",
                      road_.vmax(), road_.deterministic() ? 0.0 : double(road_.p()),
                      road_.density(), road_.length());
        return b;
    }

    // Click the road to stop the nearest car and watch the jam grow upstream of
    // where you clicked. x is position; the y of the click is time, which has
    // already happened and cannot be poked. Clicking a car that is already
    // stopped is a no-op by construction — there is nothing weaker than stopped
    // in this model — which is why the sim seeds its cars moving.
    bool poke(float nx, float /*ny*/) override {
        const int cell = std::clamp(int(nx * float(length())), 0, length() - 1);
        return road_.brake_near(cell);
    }

    // The picture is a derived view of the last 256 steps, not the state, so
    // there is nothing here for a brush to paint into — painting a pixel into
    // last Tuesday would be erased on the next scroll.
    Field* editable() override { return nullptr; }

    // Test hook: the model underneath, so the fundamental diagram can be
    // measured without a picture being scrolled under it.
    [[nodiscard]] const NaSch& road() const { return road_; }

private:
    [[nodiscard]] int length() const { return 256 << std::clamp(int(knob("length") + 0.5f), 0, 3); }
    [[nodiscard]] float knob(const char* key) const {
        for (const auto& kn : knobs_) if (kn.key == key) return kn.value;
        return 0.f;
    }
    void rebuild() {
        const int L = length();
        if (view_.w != L || view_.h != kHistory) view_ = Field(L, kHistory);
    }
    // The newest timestep, along the bottom edge.
    void draw_row() {
        const int y = view_.h - 1;
        for (int x = 0; x < view_.w; ++x) view_.set(x, y, 0);
        const auto& pos = road_.positions();
        const auto& vel = road_.velocities();
        for (std::size_t i = 0; i < pos.size(); ++i) {
            const int x = pos[i];
            if (x < 0 || x >= view_.w) continue;
            // Palette index 1 is v=0, so a car is 1+v and the colour IS the
            // speed. Clamped at 6 because the palette names six velocities and
            // the vmax knob stops at 5; a wider vmax would need more swatches
            // rather than a silent squash.
            view_.set(x, y, std::uint8_t(1 + std::min(vel[i], 5)));
        }
    }

    Provenance          about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    Field               view_;
    NaSch               road_;
    std::vector<double> flowRing_, detRing_;
    std::size_t         ringN_ = 0, ringAt_ = 0;
};

inline SimPtr make_traffic(int roadLength = 512) {
    return std::make_unique<Traffic>(roadLength);
}

} // namespace bench
