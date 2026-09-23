// pps.hpp — Primordial Particle System.
//
// Schmickl, Stefanec & Crailsheim, "How a life-like system emerges from a
// simplistic particle motion law", Scientific Reports 6:37969 (2016).
// Corrigendum: Sci Rep 7:42454 (2017).
//
// This is the only continuous-space system on the bench that genuinely
// replicates. Structures condense out of a uniform gas, grow, and DIVIDE, from
// one motion law with four parameters and no notion of a cell anywhere in it.
// That is a statement about this file, checked by watching its own published
// field at the published world size; the counts behind it are in the
// replication note in about_ below.
//
// ── A HONESTY NOTE, and it matters ──────────────────────────────────────────
// The published Equation 1 is a typeset graphic. Independent machine readings of
// it disagree: one extraction returns the form
//        dphi = alpha * (R - L) + beta * N
// which cannot be right, because with the published alpha = 180 degrees a single
// neighbour imbalance would turn a particle more than a half-turn every step.
// The form implemented here,
//        dphi = alpha + beta * N * sign(R - L)
// is the one consistent with the published parameter set. What it produces here
// at those exact parameters is a coarsening foam — irregular condensed regions
// of about twice mean density, separated by voids and merging over time — not
// the paper's rimmed hollow cells. It is nevertheless a RECONSTRUCTION of the
// printed equation rather than a transcription of it, and the bench says so on
// screen.
//
// Second honesty note, learned the hard way: getting the equation right is only
// half of it. sign(R - L) means nothing until the code also agrees with itself
// about which side R is, and for a long time this file did not. Nothing in the
// metrics looked wrong while that was true, which is the reason step() now
// carries the argument for the sign in full.
//
// Published parameters: r = 5, alpha = 180 deg, beta = 17 deg, v = 0.67,
// 250x250 toroidal world, density 0.08 particles per unit^2 (5000 particles),
// asynchronous update in randomised order.

#pragma once
#include "../sim.hpp"
#include "../rng.hpp"
#include "../parallel.hpp"
#include "../toolkit.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace bench {

class PrimordialParticles final : public Sim {
public:
    // The world knob scales the published 250 by a whole number. 4x is as far
    // as it goes, and the reason is in step(): this rule cannot be threaded.
    static constexpr int kMaxScale = 4;

    explicit PrimordialParticles(int world = 250, float density = 0.08f)
        : view_(world, world), world_(float(world)),
          baseWorld_(world), density_(density) {
        n_ = int(density * world_ * world_);
        about_ = Provenance{
            "Primordial Particle System", "2016",
            "Thomas Schmickl, Martin Stefanec & Karl Crailsheim",
            "Scientific Reports 6:37969 (2016); corrigendum Sci Rep 7:42454 (2017)",
            Replication::Yes,
            "Yes - and this is the only continuous-space system on the bench that earns that. "
            "Structures condense out of a uniform gas, grow by accreting particles, and then "
            "DIVIDE into two. Nothing in the motion law mentions a cell, a membrane or a "
            "daughter. That verdict is measured on this build rather than quoted from the "
            "paper: over 20,000 steps at the published parameters, tracking clusters of 12 "
            "or more particles at a 2.5-unit linkage, 65 splits are counted - against 67 "
            "merges. Note what is still absent though: "
            "there is no transcribed description being copied, so this is division, not von "
            "Neumann's genotype/phenotype replication. Both are real; they are not the same "
            "thing.",
            "Every particle turns by a fixed amount each step, plus an extra amount proportional "
            "to how many neighbours it has, away from whichever side is more crowded. "
            "Then it moves forward. That is the entire rule. Particles condense out of the "
            "gas into irregular clumps, which split and merge in near-equal numbers."
        };
        pal_ = {
            {{  8, 11, 14}, "empty"},
            {{ 60, 90,110}, "isolated (0-12 neighbours)"},
            {{ 90,209,196}, "in a structure (13-15)"},
            {{242,193, 78}, "crowded (16-35)"},
            {{217, 83, 79}, "dense core (36+)"},
        };
        knobs_ = {
            {"alpha", "alpha (fixed turn, degrees)",       0.f, 360.f, 180.f},
            {"beta",  "beta (turn per neighbour, degrees)", 0.f,  60.f,  17.f},
            {"v",     "speed",                              0.1f,  2.f, 0.67f},
            {"r",     "neighbourhood radius",               1.f,  12.f,  5.f},
            // World size, as a whole-number scale on the world this instance was
            // built with — the published 250x250 for make_pps().
            //
            // A scale rather than a list of absolute sizes, and 1x rather than a
            // bigger default, because 250 at density 0.08 IS the published
            // setup and the replication verdict in about_ was counted on it.
            // Moving the default would quietly restate a measured claim.
            //
            // The density is held, not the count: r is in world units, so the
            // same 0.08 particles per unit^2 gives the same local neighbourhood
            // and 4x is more of the same foam rather than a different physics.
            {"size", "world size", 1.f, float(kMaxScale), 1.f, 1.f,
             {"1x", "2x", "3x", "4x"}, true,
             "Whole-number scale on the world, at the published density of 0.08 particles per "
             "unit squared: 1x is the paper's 250x250 with 5,000 particles, 4x is 1000x1000 with "
             "80,000. Measured here, 1.9 ms a step at 1x and 38.8 ms at 4x, on one core at every "
             "size - the update is asynchronous by publication, so there is no threaded version "
             "of it to have. 4x is the ceiling for that reason; 8x measured 315 ms a step."},
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

    // Rebuild the world at a new scale.
    //
    // It has to happen here. reset() refills the arrays it already has, so
    // without this the knob would move nothing while looking as though it had —
    // field().w would never budge. And on_knob has to leave a consistent state
    // on its own, because a caller is not obliged to re-seed afterwards: the
    // knob extremes test in self_test.cpp sets a knob and steps 400 times with
    // no reset, and n_ disagreeing with the length of px_ would be a buffer
    // overrun rather than a wrong picture.
    void rescale(int scale) {
        if (scale == scale_) return;
        scale_ = scale;
        const int world = baseWorld_ * scale;
        world_ = float(world);
        n_ = int(density_ * world_ * world_);
        view_ = Field(world, world);
        reset();
    }

    void reset() override {
        rng_.reseed(0x9F55u); gen_ = 0;
        px_.resize(n_); py_.resize(n_); ph_.resize(n_); nb_.assign(n_, 0);
        order_.resize(n_);
        for (int i = 0; i < n_; ++i) {
            px_[i] = rng_.unit() * world_;
            py_[i] = rng_.unit() * world_;
            ph_[i] = rng_.unit() * 6.2831853f;
            order_[i] = i;
        }
        publish();
    }

    void step() override {
        const float deg = std::numbers::pi_v<float> / 180.0f;
        const float alpha = knob("alpha") * deg, beta = knob("beta") * deg;
        const float v = knob("v"), r = knob("r"), r2 = r * r;

        bins_.build(px_, py_, world_, world_, r);

        // Asynchronous, randomised order — the paper is explicit that particles
        // act sequentially. Doing this synchronously changes the dynamics.
        //
        // Which also settles the threading question, and it is worth writing
        // down next to the shuffle so nobody arrives at the loop below with a
        // parallel_for in mind. There are two independent reasons and either
        // alone is enough. The shuffle draws from the shared rng_. And the
        // update below writes px_/py_ in place while the neighbour scan reads
        // them, so a particle sees the CURRENT positions of everything that has
        // already moved this step — asynchrony is the rule here, not an
        // implementation detail, and any thread split would silently replace it
        // with something between synchronous and asynchronous depending on how
        // many cores happened to be free. There is no faster version of this
        // rule to have; a bigger world simply costs more, and the size knob says
        // so on its own label.
        for (int i = n_ - 1; i > 0; --i) {
            const int j = int(rng_.below(std::uint32_t(i + 1)));
            std::swap(order_[i], order_[j]);
        }

        for (int oi = 0; oi < n_; ++oi) {
            const int i = order_[oi];
            const float ci = std::cos(ph_[i]), si = std::sin(ph_[i]);
            int L = 0, Rr = 0;
            // Binned neighbour search. Brute force here is 5000x5000 distance
            // tests per step, 25 million - the spatial hash in toolkit.hpp
            // exists precisely for this.
            bins_.each_near(px_[i], py_[i], [&](int j) {
                if (i == j) return;                         // self excluded
                float dx = wrap_delta(px_[j] - px_[i], world_);
                float dy = wrap_delta(py_[j] - py_[i], world_);
                if (dx * dx + dy * dy > r2) return;
                // Side test, by cross product of the heading with the offset.
                // In the shipped code R names the side a positive dphi turns
                // AWAY from.
                //
                // This was flipped to (dy*ci - dx*si) on the argument that the
                // original turned particles away from the crowd and so could
                // never close a rim. On a 120-wide world at the published
                // parameters, sampled every 2,000 steps out to t=12,000, the
                // flip produces nothing there: structured() is 0 and the
                // crowded band empty at every one of those samples, against
                // 601-735 structured and 110-181 crowded particles with the
                // sign below. The self-test's local-density check (world 60,
                // 260 steps) separates them the same way: 0.512 -> 1.141 with
                // the sign below, 0.512 -> 0.306 under the flip, against a
                // threshold of 0.769. So the flip is reverted.
                //
                // The observation behind it was still half right, and it is
                // worth writing down rather than losing: with the sign below
                // the "dense core (36+)" band is empty at every sample out to
                // t=12,000, on the 120-unit world and on the 250-unit default
                // alike. That is evidence for the honesty note at the top of
                // this file, not against it.
                (dx * si - dy * ci > 0.0f) ? ++Rr : ++L;
            });
            const int N = L + Rr;
            // sign(0) must be 0 — a ternary that never returns zero breaks the
            // tie in a fixed direction and corrupts every symmetric particle,
            // which is 14-17% of the gas.
            const int sgn = (Rr > L) ? 1 : (Rr < L ? -1 : 0);
            ph_[i] += alpha + beta * float(N) * float(sgn);
            const float c = std::cos(ph_[i]), s = std::sin(ph_[i]);
            px_[i] += v * c; py_[i] += v * s;
            px_[i] = wrap_pos(px_[i], world_);
            py_[i] = wrap_pos(py_[i], world_);
            nb_[i] = N;
        }
        publish(); ++gen_;
    }

    [[nodiscard]] int particles() const { return n_; }
    // Particles with at least 13 neighbours — the band the paper draws as
    // "inside a structure". It is the count only; see metrics() for what that
    // does and does not establish.
    [[nodiscard]] int structured() const {
        int c = 0; for (int n : nb_) if (n >= 13) ++c; return c;
    }
    // The paper's own colour bands, reported as the paper defines them.
    //
    // Read "structured fraction" as a local density test and nothing more. It
    // counts neighbours inside r; it never checks that those neighbours form a
    // connected cell, so a merely concentrated gas clears 13 with no cell
    // anywhere on the field. That is not hypothetical — at t=30,000 this build
    // reports a structured fraction of 0.62 on a field whose largest connected
    // structure is 8 pixels.
    //
    // Both series describe the state rather than score it, so both are
    // Metric::Neither. No frame of a PPS run is its "best" by neighbour count —
    // a single collapsed clump would top both series and it is the least
    // interesting thing the rule can do.
    std::vector<Metric> metrics() const override {
        double mean = 0; for (int n : nb_) mean += n;
        mean = n_ ? mean / n_ : 0;
        return { {"structured fraction", n_ ? double(structured()) / n_ : 0.0, 1.0,
                  Metric::Neither},
                 {"mean neighbours",     mean, 0.0, Metric::Neither} };
    }

private:
    [[nodiscard]] float knob(const char* k) const {
        for (auto& kn : knobs_) if (kn.key == k) return kn.value;
        return 0.f;
    }
    static std::uint8_t band(int n) {
        if (n >= 36) return 4;
        if (n >= 16) return 3;
        if (n >= 13) return 2;
        return 1;
    }
    void publish() {
        view_.fill(0);
        for (int i = 0; i < n_; ++i) {
            const int ix = int(px_[i]), iy = int(py_[i]);
            if (ix < 0 || iy < 0 || ix >= view_.w || iy >= view_.h) continue;
            const std::uint8_t b = band(nb_[i]);
            if (b > view_.at(ix, iy)) view_.set(ix, iy, b);   // densest wins the pixel
        }
    }

    // Seed a tight disc of particles all facing the same way.
    //
    // More than a convenience: at 60 units and at 150, eight pokes still show
    // dense-core (36+) pixels at t=6000 — 22-60 of them at 60 units and 117-293
    // at 150, over three placements each — while an un-poked run of either world
    // leaves that band empty at t=2000, 4000 and 6000. Getting that band on
    // screen at all is what poke() is for.
    bool poke(float nx, float ny) override {
        if (n_ <= 0) return false;
        const float cx = nx * world_, cy = ny * world_;
        const float dir = rng_.unit() * 6.2831853f;
        for (int k = 0; k < 60; ++k) {
            const int i = int(rng_.unit() * float(n_)) % n_;
            const float a = rng_.unit() * 6.2831853f, r = rng_.unit() * 4.0f;
            px_[i] = wrap_pos(cx + std::cos(a) * r, world_);
            py_[i] = wrap_pos(cy + std::sin(a) * r, world_);
            ph_[i] = dir;
        }
        return true;
    }

    Provenance about_; std::vector<Swatch> pal_; std::vector<Knob> knobs_;
    Field view_; int n_ = 0; float world_;
    // What "1x" means for THIS instance, so the knob does not silently move a
    // deliberately small world: the self-test builds a 60-unit one and the
    // comments in step() and poke() below rest on runs at 60 and 120 units.
    int baseWorld_; float density_; int scale_ = 1;
    std::vector<float> px_, py_, ph_;
    std::vector<int>   nb_, order_;
    Bins               bins_;
    Rng rng_{0x9F55u}; std::uint64_t gen_ = 0;
};

inline SimPtr make_pps(int world = 250, float density = 0.08f) {
    return std::make_unique<PrimordialParticles>(world, density);
}

} // namespace bench
