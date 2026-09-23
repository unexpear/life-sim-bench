// continuous.hpp — the systems with no lattice at all.
//
// Three of them, and they disagree about the thing this bench cares about most:
//
//   Boids           self-organises. Does not replicate. Pure phenotype.
//   Particle Life   self-organises into cell-like structures. Does NOT replicate,
//                   however often the internet says otherwise.
//   PPS             genuinely divides — and that is a measurement of the code
//                   in pps.hpp, not a citation. Peer review of the paper would
//                   not have been evidence about this build, and for a while
//                   the build did not deserve the Yes at all.
//
// Keeping all three next to each other is the point: "looks alive" and "makes
// another one of itself" are different claims, and only the third is the one
// von Neumann was asking about.

#pragma once
#include "../sim.hpp"
#include "../rng.hpp"
#include "../parallel.hpp"
#include "../toolkit.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace bench {

// A shared helper: rasterise continuous agents onto the index grid the renderer
// expects. The sims are continuous; only the presentation is a grid.
inline void splat(Field& f, float x, float y, std::uint8_t v) {
    const int ix = int(x), iy = int(y);
    if (ix >= 0 && iy >= 0 && ix < f.w && iy < f.h) f.set(ix, iy, v);
}

// ── Boids ───────────────────────────────────────────────────────────────────
class Boids final : public Sim {
public:
    // The size knob scales the world by a whole number and the flock with it.
    // Anything bigger than 4x is left off the list on purpose — see step().
    static constexpr int kMaxScale = 4;

    // Elementary operations below which threading is not worth the launch —
    // the same 250,000 GridSim::kParallelCells uses, because it is the same
    // measurement. Only mean_spacing() consults it; step() cannot be threaded
    // at any size.
    static constexpr std::size_t kParallelWork = 250000;

    explicit Boids(int n = 1200, int w = 320, int h = 200)
        : view_(w, h), n_(n), w_(float(w)), h_(float(h)),
          baseN_(n), baseW_(w), baseH_(h) {
        about_ = Provenance{
            "Boids", "1987", "Craig W. Reynolds",
            "Reynolds, C. W. \"Flocks, Herds, and Schools: A Distributed Behavioral Model\", "
            "SIGGRAPH '87, Computer Graphics 21(4), 25-34",
            Replication::No,
            "No. Nothing in a flock copies anything. A boid has no description of itself to pass "
            "on - it is pure phenotype. That is exactly why it is worth having on the bench beside "
            "the replicators: 'looks alive' and 'makes another one of itself' are unrelated claims.",
            "Three steering rules, applied to each bird's local neighbours only. Reynolds named "
            "them Collision Avoidance, Velocity Matching and Flock Centering in the 1987 paper; "
            "the now-universal Separation / Alignment / Cohesion are his own later names. No bird "
            "knows what the flock is doing. There is no leader and no plan."
        };
        pal_ = {{{8,11,14},"empty"},{{90,209,196},"bird"},{{242,193,78},"crowded"}};
        knobs_ = {
            {"separation","Collision Avoidance (separation)",0.f,4.f,2.2f},
            {"alignment", "Velocity Matching (alignment)",   0.f,4.f,1.0f},
            {"cohesion",  "Flock Centering (cohesion)",      0.f,4.f,1.0f},
            {"radius",    "neighbourhood radius",            4.f,40.f,22.f},
            // World size, as a whole-number scale on the world this sim was
            // built with — 320x200 with 1200 birds for make_boids(). The bird
            // count scales with the AREA, so 4x is 1280x800 with 19,200 birds.
            //
            // Density is what has to be held, not the count. Reynolds' rules are
            // local and the radius knob is in world units, so a bigger world at
            // the same density is more of the same flocking, seen from further
            // out — which is the point. Keeping 1200 birds and growing the box
            // would just thin the gas until nobody has a neighbour.
            //
            // A choice list rather than a slider: the positions are 1, 4, 9 and
            // 16 times the work, and the numbers in between are not sizes anyone
            // asks for.
            {"size", "world size", 1.f, float(kMaxScale), 1.f, 1.f,
             {"1x", "2x", "3x", "4x"}, true,
             "Whole-number scale on the world, with the flock scaled to match: 1x is 320x200 "
             "with 1200 birds, 4x is 1280x800 with 19,200. Measured here, a step costs 1.05 ms "
             "at 1x and 19.0 ms at 4x, and it is the same 19.0 ms however many cores you have — "
             "the flock is stepped in place, so it cannot be threaded without changing the "
             "answer. 4x is the ceiling for that reason: 8x measured 96 ms a step with nothing "
             "available to spend on it. Budget for the nearest-neighbour metric on top, which "
             "IS threaded and costs about 49 ms a frame at 4x, so 4x is a size to look at "
             "rather than one to run fast."},
        };
        reset();
    }
    const Provenance& about() const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field& field() const override { return view_; }
    std::uint64_t generation() const override { return gen_; }
    std::vector<Knob>& knobs() override { return knobs_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        if (k == "size") rescale(std::clamp(int(v + 0.5f), 1, kMaxScale));
    }

    // Rebuild at a new scale. This has to happen HERE and not be left to
    // reset(), for two separate reasons.
    //
    // reset() only refills the arrays it already has, so a size knob that does
    // not rebuild them is a no-op that looks like it worked — field().w would
    // never move. And the host is not obliged to re-seed on release: the knob
    // extremes test in self_test.cpp sets a knob and then steps 400 times with
    // no reset at all, so on_knob has to leave a state that is internally
    // consistent by itself. Hence the reset() call at the end: n_ and the
    // position arrays must never disagree about how many birds there are.
    void rescale(int scale) {
        if (scale == scale_) return;
        scale_ = scale;
        const int w = baseW_ * scale, h = baseH_ * scale;
        n_ = baseN_ * scale * scale;
        w_ = float(w); h_ = float(h);
        view_ = Field(w, h);
        reset();
        // reset() seeds positions but does not rasterise them — only step()
        // does. Without this the new world is blank until the next step, which
        // on a paused sim is a size knob that appears to have emptied the field.
        publish();
    }

    void reset() override {
        rng_.reseed(0x0B01D5u); gen_ = 0;
        px_.resize(n_); py_.resize(n_); vx_.resize(n_); vy_.resize(n_);
        for (int i = 0; i < n_; ++i) {
            px_[i] = rng_.unit() * w_; py_[i] = rng_.unit() * h_;
            const float a = rng_.unit() * 6.2831853f;
            vx_[i] = std::cos(a); vy_[i] = std::sin(a);
        }
    }

    void step() override {
        const float sepW = knob("separation"), aliW = knob("alignment"),
                    cohW = knob("cohesion"),  rad  = knob("radius");
        const float r2 = rad * rad, sep2 = (rad * 0.45f) * (rad * 0.45f);
        const float maxSpeed = 1.4f, maxForce = 0.06f;

        bins_.build(px_, py_, w_, h_, rad);
        // NOT threaded, and this is a decision with a measurement behind it
        // rather than an omission.
        //
        // The loop reads its neighbours' velocities — `ax += vx_[j]` — and
        // writes its own at the bottom. So a bird processed late sees the NEW
        // velocity of every neighbour already updated this step, and the result
        // depends on the order the birds are visited in. Any thread split
        // changes that order. Wrapping this loop in parallel_for and diffing the
        // published field against the serial run: 2,336 of 64,000 cells differ
        // after 200 steps at 1x, and 36,831 of 1,024,000 after 60 steps at 4x —
        // which at 1200 birds and two cells per moved bird is essentially the
        // whole flock landing somewhere else.
        //
        // Making it threadable means snapshotting velocities and updating them
        // all from the previous state, and that is a different flocking rule,
        // not a faster implementation of this one. A correct serial sim beats a
        // fast wrong one, so the size knob stops at 4x instead.
        for (int i = 0; i < n_; ++i) {
            float sx = 0, sy = 0, ax = 0, ay = 0, cx = 0, cy = 0;
            int nAli = 0, nCoh = 0, nSep = 0;
            bins_.each_near(px_[i], py_[i], [&](int j) {
                if (i == j) return;
                float dx = wrap_delta(px_[j] - px_[i], w_);
                float dy = wrap_delta(py_[j] - py_[i], h_);
                const float d2 = dx * dx + dy * dy;
                if (d2 > r2 || d2 < 1e-9f) return;
                ax += vx_[j]; ay += vy_[j]; ++nAli;
                cx += dx;     cy += dy;     ++nCoh;
                if (d2 < sep2) { const float inv = 1.0f / d2; sx -= dx * inv; sy -= dy * inv; ++nSep; }
            });
            // Reynolds' steering formulation (Steering Behaviors, 1999): each
            // behaviour produces a DESIRED velocity, the steering force is
            // (desired - current), and each force is truncated on its own before
            // being weighted and summed.
            //
            // The earlier version summed incomparable quantities — a raw position
            // offset for cohesion against a velocity difference for alignment —
            // and clamped only the total, so the two partially cancelled and the
            // flock collapsed into a few dense knots. Normalising each to the same
            // units is what makes the weights mean anything.
            auto steer = [&](float dx, float dy, float& ox, float& oy) {
                const float m = std::sqrt(dx * dx + dy * dy);
                if (m < 1e-6f) { ox = oy = 0.f; return; }
                ox = dx / m * maxSpeed - vx_[i];          // desired - current
                oy = dy / m * maxSpeed - vy_[i];
                const float sm = std::sqrt(ox * ox + oy * oy);
                if (sm > maxForce) { ox = ox / sm * maxForce; oy = oy / sm * maxForce; }
            };

            float fx = 0, fy = 0, ox, oy;
            if (nSep) { steer(sx, sy, ox, oy);                   fx += ox * sepW; fy += oy * sepW; }
            if (nAli) { steer(ax / nAli, ay / nAli, ox, oy);     fx += ox * aliW; fy += oy * aliW; }
            if (nCoh) { steer(cx / nCoh, cy / nCoh, ox, oy);     fx += ox * cohW; fy += oy * cohW; }

            const float fm = std::sqrt(fx * fx + fy * fy);
            if (fm > maxForce) { fx = fx / fm * maxForce; fy = fy / fm * maxForce; }
            vx_[i] += fx; vy_[i] += fy;
            const float sp = std::sqrt(vx_[i] * vx_[i] + vy_[i] * vy_[i]);
            if (sp > maxSpeed) { vx_[i] = vx_[i] / sp * maxSpeed; vy_[i] = vy_[i] / sp * maxSpeed; }
        }
        for (int i = 0; i < n_; ++i) {
            px_[i] += vx_[i]; py_[i] += vy_[i];
            px_[i] = wrap_pos(px_[i], w_);
            py_[i] = wrap_pos(py_[i], h_);
        }
        publish(); ++gen_;
    }

    // Mean nearest-neighbour spacing. This was justified here as "the thing
    // Reynolds' own fitness function scores", which was not true of anything
    // this file cites: neither the 1987 SIGGRAPH paper in the Provenance above
    // nor Steering Behaviors (1999), cited inside step(), contains a fitness
    // function, an optimisation, or any evolution at all. Both are descriptive
    // papers. If a Reynolds paper with a fitness measure was meant, this file
    // does not name it, so the appeal is struck rather than patched.
    //
    // What the number is actually for is the collapse test in self_test.cpp:
    // when cohesion and separation were being summed as incomparable quantities
    // the flock fell into a few dense knots, and the spacing is what catches
    // that. It is a measured property of the flock, not a score of it.
    //
    // Every seventh bird against all the others, so this is O(n^2/7) — 206,000
    // distance tests at the shipped 1200 birds and 53 MILLION at 4x. The
    // workbench calls metrics() once a frame through History::observe, so at 4x
    // this measured 643 ms a frame serially, against 19 ms for the step it was
    // attached to. It was comfortably the thing that would have made the big
    // worlds unwatchable — not the rule — so the sample loop is threaded, and
    // 4x costs 49 ms here instead.
    //
    // It is threaded in a way that cannot move the number. Each sampled bird's
    // nearest distance is written to its own slot, and the slots are then summed
    // in index order on one thread — the same sequence of double additions the
    // serial version performed, so the result is bit-identical rather than
    // merely close. Summing inside the workers would have reassociated the adds
    // and made a metric that drifts with the core count.
    [[nodiscard]] float mean_spacing() const {
        const int cnt = (n_ + 6) / 7;
        if (cnt <= 0) return 0.f;
        gap_.assign(std::size_t(cnt), 0.f);
        // Threshold in distance tests, not birds, because this is quadratic:
        // 1200 birds is 206,000 tests and 4800 birds is 3.3 MILLION. The number
        // is the same 250,000 the lattice sweeps use, which leaves the shipped
        // 1x flock exactly as serial as it was and threads only the sizes that
        // did not previously exist.
        const std::size_t work = std::size_t(cnt) * std::size_t(n_);
        parallel_for(std::size_t(cnt), [&](std::size_t s) {
            const int i = int(s) * 7;
            float best = 1e9f;
            for (int j = 0; j < n_; ++j) {
                if (i == j) continue;
                float dx = px_[j] - px_[i], dy = py_[j] - py_[i];
                dx = wrap_delta(dx, w_);
                dy = wrap_delta(dy, h_);
                const float d2 = dx * dx + dy * dy;
                if (d2 < best) best = d2;
            }
            gap_[s] = std::sqrt(best);
        }, work >= kParallelWork ? 0u : 1u);
        double acc = 0;
        for (float g : gap_) acc += g;
        return float(acc / cnt);
    }
    // Vicsek order parameter: the length of the mean unit heading. 0 when
    // headings are uniformly random, 1 when every bird points the same way.
    // This is the standard measure of flocking, and it is the right one —
    // nearest-neighbour spacing is not, because from a uniform-random start
    // the birds begin CLOSER than the flock's equilibrium spacing, so a
    // correctly-working separation rule pushes them apart.
    [[nodiscard]] float order_parameter() const {
        double sx = 0, sy = 0;
        for (int i = 0; i < n_; ++i) {
            const float s = std::sqrt(vx_[i] * vx_[i] + vy_[i] * vy_[i]);
            if (s < 1e-6f) continue;
            sx += vx_[i] / s; sy += vy_[i] / s;
        }
        return n_ ? float(std::sqrt(sx * sx + sy * sy) / n_) : 0.f;
    }
    [[nodiscard]] int count() const { return n_; }

    // Both are real properties of the flock, not proxies. Only the first is a
    // score: alignment is what flocking IS, so more of it is better.
    //
    // The gap is Neither, and it took a measurement to see why. It is not that
    // the direction is backwards: sampling mean_spacing() over 4,000 steps of
    // make_boids(), the gap rises from 3.47 at the uniform-random start to
    // about 5.7 as the flock finds its equilibrium spacing, so a working rule
    // really does push it up. It is that past equilibrium the quantity is
    // two-sided — from t=1500 on it just wanders between 5.37 and 6.22, and the
    // top of that wander is the most DISPERSED frame of the run, a flock coming
    // apart. A series whose extreme is not its best has no better direction,
    // and the panel would otherwise print that extreme as "best".
    std::vector<Metric> metrics() const override {
        return { {"order parameter (alignment)", double(order_parameter()), 1.0},
                 {"mean nearest-neighbour gap",  double(mean_spacing()),    0.0,
                  Metric::Neither} };
    }

private:
    [[nodiscard]] float knob(const char* k) const {
        for (auto& kn : knobs_) if (kn.key == k) return kn.value;
        return 0.f;
    }
    // Gather a handful of birds to the cursor, pointing outward. There is no
    // grid to paint, so "seed here" has to mean "move some agents here".
    bool poke(float nx, float ny) override {
        const float cx = nx * w_, cy = ny * h_;
        for (int k = 0; k < 24 && k < n_; ++k) {
            const int i = int(rng_.unit() * float(std::max(1, n_))) % std::max(1, n_);
            const float a = rng_.unit() * 6.2831853f, r = rng_.unit() * 6.0f;
            px_[i] = wrap_pos(cx + std::cos(a) * r, w_);
            py_[i] = wrap_pos(cy + std::sin(a) * r, h_);
            vx_[i] = std::cos(a); vy_[i] = std::sin(a);
        }
        return true;
    }

private:
    void publish() {
        view_.fill(0);
        for (int i = 0; i < n_; ++i) splat(view_, px_[i], py_[i], 1);
    }
    Provenance about_; std::vector<Swatch> pal_; std::vector<Knob> knobs_;
    Field view_; int n_; float w_, h_;
    // What "1x" means for THIS instance. The knob is a scale rather than a list
    // of absolute sizes because the sim is constructed at several different
    // worlds — 160x120 and 220x160 in the self-test — and a fixed ladder of
    // pixel sizes would silently move those off the size they were built at.
    int baseN_, baseW_, baseH_, scale_ = 1;
    std::vector<float> px_, py_, vx_, vy_;
    mutable std::vector<float> gap_;      // scratch for mean_spacing()
    Bins bins_;
    Rng rng_{0x0B01D5u}; std::uint64_t gen_ = 0;
};

// ── Particle Life ───────────────────────────────────────────────────────────
// k types, an ASYMMETRIC k*k attraction matrix, a repulsive core plus an
// attraction band, heavy friction. The asymmetry is what produces chasing and
// orbiting; a symmetric matrix gives boring clumps.
class ParticleLife final : public Sim {
public:
    // Bigger ceiling than Boids gets, and for one reason only: this force loop
    // is threadable and that one is not. See step().
    static constexpr int kMaxScale = 6;

    explicit ParticleLife(int n = 1200, int k = 4, int w = 320, int h = 200)
        : view_(w, h), n_(n), k_(k), w_(float(w)), h_(float(h)),
          baseN_(n), baseW_(w), baseH_(h) {
        about_ = Provenance{
            "Particle Life", "2010s", "Jeffrey Ventrella (\"Clusters\"); popularised by CodeParade",
            "Ventrella, J. Clusters (clusters.ventrella.com); the widely-copied variant follows "
            "CodeParade's 2018 video. Folk lineage - attribute with care.",
            Replication::No,
            "No, despite what is usually claimed for it. The structures self-ORGANISE: they form, "
            "hold together and move. Nothing in the dish copies the attraction matrix, and the "
            "matrix is what determines everything. It is copied by whoever is running the "
            "simulation. That is the whole distinction von Neumann's work is about.",
            "Every particle has a colour. For each ordered pair of colours a number says how "
            "strongly the first is attracted to the second - and it need not be mutual. Red can "
            "chase green while green flees red. Add a short-range repulsion so nothing collapses, "
            "and enough friction that momentum does not accumulate."
        };
        pal_ = {{{8,11,14},"empty"},{{90,209,196},"type 0"},{{242,193,78},"type 1"},
                {{217,83,79},"type 2"},{{155,122,230},"type 3"}};
        knobs_ = {
            {"rmax",     "interaction radius",   10.f, 60.f, 32.f},
            {"beta",     "repulsive core (beta)", 0.05f, 0.6f, 0.30f},
            {"friction", "friction",              0.0f, 0.5f, 0.15f},
            {"force",    "force scale",           0.1f, 3.0f, 1.0f},
            // Same scale-the-dish knob as Boids, and for the same reason: the
            // forces are local and rmax is in world units, so holding the
            // particle DENSITY is what makes a bigger world show more of the
            // same structures rather than a thinner soup that never clumps.
            {"size", "world size", 1.f, float(kMaxScale), 1.f, 1.f,
             {"1x", "2x", "3x", "4x", "5x", "6x"}, true,
             "Whole-number scale on the world, with the particle count scaled to match: 1x is "
             "320x200 with 1200 particles, 6x is 1920x1200 with 43,200. The force loop is "
             "threaded, so 6x measured 82.6 ms a step on one core and 16.5 ms on this machine's "
             "twenty. 1x stays deliberately serial - it is under the threshold, so the shipped "
             "dish behaves exactly as it always did. Note the dish is re-rolled: the attraction "
             "matrix is drawn after the "
             "particles are placed, so a different count gives a different matrix — the same "
             "rule, a different experiment."},
        };
        reset();
    }
    const Provenance& about() const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field& field() const override { return view_; }
    std::uint64_t generation() const override { return gen_; }
    std::vector<Knob>& knobs() override { return knobs_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        if (k == "size") rescale(std::clamp(int(v + 0.5f), 1, kMaxScale));
    }

    // Rebuild the dish at a new scale. Not left to reset(): reset() resizes to
    // whatever n_ already says, so without this the knob would move nothing and
    // still look like it had worked, and a caller that sets the knob without
    // re-seeding (self_test.cpp does exactly that) would step a sim whose
    // particle count and arrays disagreed.
    void rescale(int scale) {
        if (scale == scale_) return;
        scale_ = scale;
        const int w = baseW_ * scale, h = baseH_ * scale;
        n_ = baseN_ * scale * scale;
        w_ = float(w); h_ = float(h);
        view_ = Field(w, h);
        reset();
        publish();      // as in Boids: reset() seeds but does not rasterise
    }

    void reset() override {
        rng_.reseed(0x9A17C1u); gen_ = 0;
        px_.resize(n_); py_.resize(n_); vx_.assign(n_, 0.f); vy_.assign(n_, 0.f); ty_.resize(n_);
        for (int i = 0; i < n_; ++i) {
            px_[i] = rng_.unit() * w_; py_[i] = rng_.unit() * h_;
            ty_[i] = std::uint8_t(rng_.below(std::uint32_t(k_)));
        }
        randomise_matrix();
    }
    void randomise_matrix() {
        mat_.assign(std::size_t(k_) * k_, 0.f);
        for (auto& m : mat_) m = rng_.unit() * 2.f - 1.f;   // asymmetric by construction
    }

    void step() override {
        const float rmax = knob("rmax"), beta = knob("beta"),
                    fric = knob("friction"), fscale = knob("force") * 0.6f;
        const float r2max = rmax * rmax;
        bins_.build(px_, py_, w_, h_, rmax);
        // Threaded, and it is worth being precise about WHY this one is safe
        // when the flock next door is not.
        //
        // Everything this loop reads — positions, types, the attraction matrix,
        // the bins — is fixed for the whole sweep; positions are integrated in
        // the separate loop below, after every force is known. Each iteration
        // writes vx_[i] and vy_[i] and nothing else. So no iteration can observe
        // another, and the split cannot be seen in the result. It is not merely
        // close either: each particle's forces are accumulated in bin order,
        // which does not depend on the number of workers, so the floating-point
        // sum is associated identically however the range is divided. Verified
        // by running 400 steps at one worker and at twenty and comparing the
        // published field cell for cell.
        //
        // Contrast Boids, three hundred lines up, where the same-shaped loop
        // reads its neighbours' VELOCITIES while writing its own — so a bird
        // sees the new velocity of every neighbour already processed, the answer
        // depends on the order, and threading it changes what the sim does.
        // Measured rather than reasoned about: threading that loop and diffing
        // the field against the serial run showed 2,336 of 64,000 cells changed
        // after 200 steps at 1x. So it stays serial and this one does not.
        //
        // The plain pointers are not decoration. Moving the body into a lambda
        // cost 55% of the SERIAL speed on its own — 82.6 ms a step at 6x became
        // 128.4 — because the nested lambdas defeated the aliasing analysis that
        // had been keeping the vector data pointers in registers. Hoisting them
        // by hand puts it back, and matters most for anyone who forces one
        // worker: threading is not an excuse to make the single-threaded path
        // slower than it was.
        //
        // Threshold in candidate pair tests rather than particles, because that
        // is what the work actually is: rmax sets how many neighbours each
        // particle has to consider, and a 1200-particle dish at rmax 60 is more
        // work than a 4000-particle dish at rmax 10.
        const float perCell = float(n_) * (bins_.cell_size() * bins_.cell_size()) / (w_ * h_);
        const std::size_t work = std::size_t(float(n_) * 9.0f * std::max(1.0f, perCell));
        const float* PX = px_.data(); const float* PY = py_.data();
        const float* MAT = mat_.data(); const std::uint8_t* TY = ty_.data();
        float* VX = vx_.data(); float* VY = vy_.data();
        const Bins* bins = &bins_;
        const int k = k_;
        const float W = w_, H = h_;
        parallel_for(std::size_t(n_), [&](std::size_t idx) {
            const int i = int(idx);
            float fx = 0, fy = 0;
            bins->each_near(PX[i], PY[i], [&](int j) {
                if (i == j) return;
                float dx = wrap_delta(PX[j] - PX[i], W);
                float dy = wrap_delta(PY[j] - PY[i], H);
                const float d2 = dx * dx + dy * dy;
                if (d2 > r2max || d2 < 1e-6f) return;
                const float d = std::sqrt(d2), r = d / rmax;
                float f;
                if (r < beta) {
                    f = r / beta - 1.0f;                       // hard repulsive core
                } else {
                    const float a = MAT[std::size_t(TY[i]) * k + TY[j]];
                    f = a * (1.0f - std::fabs(2.0f * r - 1.0f - beta) / (1.0f - beta));
                }
                fx += dx / d * f; fy += dy / d * f;
            });
            VX[i] = VX[i] * (1.0f - fric) + fx * fscale;
            VY[i] = VY[i] * (1.0f - fric) + fy * fscale;
        }, work >= kParallelWork ? 0u : 1u);
        for (int i = 0; i < n_; ++i) {
            px_[i] += vx_[i]; py_[i] += vy_[i];
            px_[i] = wrap_pos(px_[i], w_);
            py_[i] = wrap_pos(py_[i], h_);
        }
        publish(); ++gen_;
    }
    [[nodiscard]] const std::vector<float>& matrix() const { return mat_; }
    [[nodiscard]] int types() const { return k_; }

    // Drop a cluster of one species at the cursor.
    bool poke(float nx, float ny) override {
        if (n_ <= 0) return false;
        const float cx = nx * w_, cy = ny * h_;
        const int   t  = int(rng_.unit() * float(k_)) % std::max(1, k_);
        for (int k = 0; k < 40; ++k) {
            const int i = int(rng_.unit() * float(n_)) % n_;
            const float a = rng_.unit() * 6.2831853f, r = rng_.unit() * 5.0f;
            px_[i] = wrap_pos(cx + std::cos(a) * r, w_);
            py_[i] = wrap_pos(cy + std::sin(a) * r, h_);
            vx_[i] = vy_[i] = 0.f;
            ty_[i] = std::uint8_t(t);
        }
        return true;
    }

private:
    [[nodiscard]] float knob(const char* k) const {
        for (auto& kn : knobs_) if (kn.key == k) return kn.value;
        return 0.f;
    }
    void publish() {
        view_.fill(0);
        for (int i = 0; i < n_; ++i) splat(view_, px_[i], py_[i], std::uint8_t(1 + ty_[i]));
    }
    // Below this many candidate pair tests a sweep is not worth threading.
    //
    // Deliberately the same number GridSim::kParallelCells uses for a lattice
    // sweep, because it is the same measurement: about a quarter of a million
    // elementary operations is where a few milliseconds of real work starts to
    // outweigh launching a thread per core. The shipped 1200-particle dish at
    // rmax 32 works out at 207,000 and so stays exactly as serial as it was —
    // the default sim spawns no threads at all, and only the sizes that did not
    // previously exist pay for them.
    static constexpr std::size_t kParallelWork = 250000;

    Provenance about_; std::vector<Swatch> pal_; std::vector<Knob> knobs_;
    Field view_; int n_, k_; float w_, h_;
    int baseN_, baseW_, baseH_, scale_ = 1;
    std::vector<float> px_, py_, vx_, vy_, mat_;
    Bins bins_;
    std::vector<std::uint8_t> ty_;
    Rng rng_{0x9A17C1u}; std::uint64_t gen_ = 0;
};

inline SimPtr make_boids(int n = 1200) { return std::make_unique<Boids>(n); }
inline SimPtr make_particle_life(int n = 1200) { return std::make_unique<ParticleLife>(n); }

} // namespace bench
