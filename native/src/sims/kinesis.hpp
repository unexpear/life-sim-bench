// kinesis.hpp — aggregation without ever steering toward anything.
//
// Fraenkel & Gunn drew the line this sim exists to show: a TAXIS is directed
// movement — the animal senses which way is better and goes that way. A KINESIS
// is not. The animal has no idea where anything is. All that changes is how it
// moves, and it changes with the intensity of what it is standing in:
//
//   Orthokinesis   speed varies with the stimulus. Fast where conditions are
//                  bad, slow where they are good.
//   Klinokinesis   turning rate varies with it. Erratic where bad, straight
//                  where good.
//
// Neither carries any information about direction. Both are measured here
// against the one thing that makes the claim falsifiable: with both mechanisms
// off, agents are uniformly distributed, so the fraction of them standing in
// favourable ground is exactly the fraction of the world that IS favourable.
// Anything above that is aggregation, and it has to have come from somewhere.
//
// MEASURED, five seeds, 600 steps, favourable ground covering 10.5% of the
// world. The index is the share of agents standing on favourable ground
// divided by the share of the world that IS favourable, so 1.0 is no
// aggregation whatsoever:
//
//     neither (the control)      0.985x
//     orthokinesis only          2.159x
//     klinokinesis only          0.991x
//     both                       2.219x
//
// Those four numbers are at the default 240x160, and they are not a property
// of that size. The world is resizable up to 1920x1280, sixty-four times the
// cells, and because agent speed scales with the world the whole thing is
// self-similar — same protocol, five seeds, 600 steps, at every size:
//
//     size          neither    ortho    klino     both    favourable
//     240x160        0.985x   2.159x   0.991x   2.219x       10.5%
//     480x320        0.984x   2.156x   0.991x   2.198x       10.5%
//     960x640        0.985x   2.155x   0.985x   2.212x       10.5%
//     1440x960       0.984x   2.153x   0.998x   2.229x       10.5%
//     1920x1280      0.984x   2.156x   0.996x   2.227x       10.5%
//
// So the result is not an artefact of a 240x160 grid, which is the objection a
// fixed world size invites and could not previously answer.
//
// Orthokinesis does all of it, and that is not a bug in the klinokinesis. A
// run-and-tumble walker at CONSTANT speed has a uniform steady-state density
// however its turning rate varies — density goes as 1/speed, and the tumble
// rate does not enter. Varying the turn rate changes how fast the population
// mixes and not where it ends up. Swept both ways to be sure, since the
// obvious suspicion is a sign error: making the turning erratic in GOOD ground
// instead gives 1.000x, 1.001x, 1.000x. It is flat in both directions.
//
// The popular description of klinokinesis is careful about this and it is worth
// repeating precisely: erratic turning in bad ground is described as helping an
// animal ESCAPE danger, not as concentrating a population in comfort. Those are
// two different claims and only the second one is measured here. The first is
// NOT tested: an attempt at it measured nothing, because an agent escaping a
// hostile patch sits at zero comfort for the whole escape, so both rules give
// it exactly the same turning rate and the harness could not tell them apart.
// Left as an open measurement rather than a claim.
//
// Sources:
//   Fraenkel & Gunn, The Orientation of Animals: Kineses, Taxes and Compass
//     Reactions, Oxford 1940 — where the terms are defined.
//   Jennings, Behavior of the Lower Organisms, Columbia 1906 — the observations
//     of Paramecium the definitions were built from.

#pragma once
#include "../sim.hpp"
#include "../rng.hpp"
#include "../parallel.hpp"
#include "../toolkit.hpp"        // GridSim::kParallelCells, the shared threshold
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <vector>

namespace bench {

class Kinesis final : public Sim {
public:
    Kinesis() {
        about_ = Provenance{
            "Kinesis: aggregation without direction",
            "1940",
            "Fraenkel & Gunn, who defined orthokinesis and klinokinesis; the observations are "
            "Jennings' (1906) on Paramecium",
            "Fraenkel, G. S. & Gunn, D. L. The Orientation of Animals: Kineses, Taxes and "
            "Compass Reactions. Oxford University Press, 1940",
            Replication::No,
            "No, and nothing here pretends otherwise — these are agents on a field, not "
            "copies of anything. It earns its place by being the smallest thing on the bench "
            "that produces an ORDERED outcome from a rule containing no information about "
            "that order. The agents cannot sense direction at all.",
            "Agents that cannot tell which way is better, and end up where it is better "
            "anyway. Speed rises in bad ground and falls in good; turning gets erratic in bad "
            "ground and straight in good. Neither rule mentions a direction. Switch both off "
            "and the population spreads out to exactly the fraction of the world that is "
            "favourable, which is the number the aggregation is measured against."
        };
        pal_ = {
            {{ 10, 12, 18}, "hostile"},
            {{ 24, 40, 52}, "poor"},
            {{ 34, 70, 66}, "fair"},
            {{ 46,110, 82}, "good"},
            {{ 74,164, 98}, "favourable"},
            {{242,232,140}, "an agent"},
        };
        knobs_ = {
            {"size", "world size", 0.f, 4.f, 0.f, 1.f,
             {"240x160", "480x320", "960x640", "1440x960", "1920x1280"}, true,
             "Cells across by cells down. The default is the size every number in the "
             "comment at the top of this file was measured at.  ·  A bigger world is the "
             "SAME experiment at higher resolution rather than a slower one, because agent "
             "speed scales with it — a step covers the same fraction of the world at every "
             "size, so the index converges in the same number of steps. Measured over five "
             "seeds: the index is 2.219x at 240x160 and 2.227x at 1920x1280, and the control "
             "is 0.985x and 0.984x.  ·  Sixty-four times the cells costs about four times a "
             "step, 0.7 ms against 2.7 ms here, because only the parts that grow with the "
             "world are threaded. The agents do not grow with it — they have their own knob, "
             "and 1500 of them are sparse on 2.5 million cells."},
            {"seed", "run seed", 1.f, 40.f, 1.f, 1.f, {}, true,
             "Which independent run this is. The same seed reproduces the identical run."},
            {"agents", "agents", 100.f, 6000.f, 1500.f, 50.f, {}, true,
             "How many walkers. The aggregation index is a proportion, so this changes how "
             "noisy the measurement is and not what it settles on."},
            {"patches", "favourable patches", 1.f, 12.f, 5.f, 1.f, {}, true,
             "Blobs of good ground scattered over hostile ground. Their total area is what "
             "the aggregation index is measured against."},
            {"patchsize", "patch size", 0.04f, 0.30f, 0.12f, 0.01f, {}, true,
             "Radius of each patch as a fraction of the world. Bigger patches are easier to "
             "stumble into and easier to wander out of."},
            {"slow", "orthokinesis strength", 0.f, 1.f, 0.85f, 0.05f, {}, false,
             "How much slower an agent moves in good ground. At 0 speed is constant and "
             "orthokinesis is off; at 1 an agent in the best ground very nearly stops. A "
             "random walker spends time in a place in inverse proportion to its speed there."},
            {"turn", "klinokinesis strength", 0.f, 1.f, 0.85f, 0.05f, {}, false,
             "How much more erratically an agent turns in bad ground. At 0 the turning rate "
             "is constant and klinokinesis is off.  ·  Measured: this does NOT aggregate the "
             "population — 0.991x against the control's 0.985x, and flat in both directions. "
             "A walker at constant speed has a uniform steady state however its turning "
             "varies. Orthokinesis is what concentrates them; this changes how fast they mix."},
            {"speed", "steps per frame", 1.f, 40.f, 6.f, 1.f, {}, false,
             "Display rate only.", true},
        };
        reset();
    }

    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field&               field()   const override { return view_; }
    std::vector<Knob>&         knobs()   override { return knobs_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        // The world is built by reset(), so a size change has to reach reset()
        // or the knob is a no-op that looks like it worked. reset() reads the
        // knob itself, so this only has to notice that the dimensions moved.
        if (k == "size" && (chosen_size().w != w_ || chosen_size().h != h_)) reset();
    }

    [[nodiscard]] std::string subtitle() const override {
        char b[192];
        std::snprintf(b, sizeof b,
                      "%s  ·  %.1f%% of agents on %.1f%% of the world  ·  index %.2fx",
                      mechanism(), 100.0 * occupancy(), 100.0 * favourable_area(),
                      aggregation());
        return b;
    }

    void reset() override {
        // Size is applied HERE as well as in on_knob, so every path that
        // rebuilds the world picks it up — reset() is the only thing that
        // allocates, and a size that only landed in on_knob would be lost the
        // moment anything else reset.
        const WorldSize sz = chosen_size();
        w_ = sz.w; h_ = sz.h;
        // A step covers the same fraction of the world at every size. Without
        // this the base speed is a fixed number of CELLS, so an eight-times
        // wider world mixes sixty-four times slower and the index reads ~1.0
        // for a very long time — a size nobody can wait for is not a feature.
        // Exactly 1.0 at the default size, so nothing published moves.
        sizeScale_ = float(w_) / float(kSizes[0].w);
        rng_.reseed(mix_seed(0x4A17ull, int(knob("seed") + 0.5f)));
        view_ = Field(w_, h_);
        comfort_.assign(std::size_t(w_) * std::size_t(h_), 0.0f);

        // Favourable blobs on hostile ground. Their total area is computed by
        // counting cells, not from the radii — patches overlap and run off the
        // edge, and the null model has to be the real area or the whole
        // measurement is against the wrong number.
        const int np = std::max(1, int(knob("patches") + 0.5f));
        const float pr = knob("patchsize") * float(w_);
        std::vector<float> px(static_cast<std::size_t>(np), 0.0f);   // braces would be an init-list
        std::vector<float> py(static_cast<std::size_t>(np), 0.0f);
        for (int i = 0; i < np; ++i) {
            px[std::size_t(i)] = rng_.unit() * float(w_);
            py[std::size_t(i)] = rng_.unit() * float(h_);
        }
        // Row-parallel above the usual threshold. Every centre is drawn BEFORE
        // this loop, so the loop touches no rng at all — each row reads px/py
        // and writes only its own slice of comfort_, which is what makes the
        // threaded field bit-identical to the serial one. At the largest size
        // this is twelve patches over 2.5 million cells with a sqrt and an exp
        // apiece, and it is by a wide margin the most expensive thing here.
        parallel_for(std::size_t(h_), [&](std::size_t row) {
            const int y = int(row);
            const std::size_t i0 = row * std::size_t(w_);
            for (int x = 0; x < w_; ++x) {
                float best = 0.0f;
                for (int i = 0; i < np; ++i) {
                    const float dx = float(x) - px[std::size_t(i)];
                    const float dy = float(y) - py[std::size_t(i)];
                    const float d  = std::sqrt(dx*dx + dy*dy) / pr;
                    best = std::max(best, std::exp(-d * d * 2.0f));   // smooth blob
                }
                comfort_[i0 + std::size_t(x)] = best;
            }
        }, comfort_.size() >= GridSim::kParallelCells ? 0u : 1u);
        favArea_ = 0.0;
        for (float c : comfort_) if (c >= kFavourable) favArea_ += 1.0;
        favArea_ /= double(comfort_.size());

        const int n = std::max(1, int(knob("agents") + 0.5f));
        agents_.assign(std::size_t(n), {});
        for (auto& a : agents_) {
            a.x = rng_.unit() * float(w_);
            a.y = rng_.unit() * float(h_);
            a.h = rng_.unit() * 6.28318531f;
        }
        gen_ = 0; sampled_ = 0; occSum_ = 0.0;
        publish();
    }

    void step() override {
        const int n = std::max(1, int(knob("speed") + 0.5f));
        for (int i = 0; i < n; ++i) tick();
        publish();
    }

    [[nodiscard]] std::uint64_t generation() const override { return gen_; }

    std::vector<Metric> metrics() const override {
        return {
            // The whole claim in one number: how many times over-represented
            // the agents are in favourable ground, against a uniform spread.
            // 1.0 means no aggregation at all, and that is what both switches
            // off must produce.
            Metric{ "aggregation index", aggregation(), 0.0, Metric::Higher },
            Metric{ "agents on good ground", occupancy(), 1.0, Metric::Higher },
            Metric{ "favourable share of world", favArea_, 1.0, Metric::Neither },
            Metric{ "mean speed", meanSpeed(), 0.0, Metric::Neither },
        };
    }

    // Scatter the population uniformly again without rebuilding the world, so
    // the same terrain can be watched settling more than once.
    bool poke(float, float) override {
        for (auto& a : agents_) {
            a.x = rng_.unit() * float(w_);
            a.y = rng_.unit() * float(h_);
            a.h = rng_.unit() * 6.28318531f;
        }
        occSum_ = 0.0; sampled_ = 0;
        publish();
        return true;
    }

    // The agents are rasterised onto the field, so painting into it would be
    // erased on the next step.
    Field* editable() override { return nullptr; }

    // ── what the tests need ────────────────────────────────────────────────
    [[nodiscard]] double favourable_area() const { return favArea_; }
    [[nodiscard]] double occupancy() const {
        if (agents_.empty()) return 0.0;
        std::size_t on = 0;
        for (const auto& a : agents_) if (comfortAt(a.x, a.y) >= kFavourable) ++on;
        return double(on) / double(agents_.size());
    }
    // Averaged over every sample since the last scatter, because a single frame
    // of a random walk is noisy enough to argue with.
    [[nodiscard]] double aggregation() const {
        if (favArea_ <= 0.0) return 0.0;
        const double occ = sampled_ ? occSum_ / double(sampled_) : occupancy();
        return occ / favArea_;
    }
    [[nodiscard]] const char* mechanism() const {
        const bool o = knob("slow") > 0.001f, k = knob("turn") > 0.001f;
        return (o && k) ? "orthokinesis + klinokinesis"
             : o ? "orthokinesis only" : k ? "klinokinesis only" : "neither (the control)";
    }

private:
    struct Agent { float x, y, h; };
    struct WorldSize { int w, h; };

    // The world keeps its original 3:2 whatever size it is. Five rungs from
    // 38 thousand cells to 2.5 million: the first two steps quadruple, then
    // they tighten to 2.25x and 1.78x, because the top of the range is where a
    // step starts to cost something and where you actually want the choice.
    //
    // A list rather than a slider, for the reason life_like gives: a continuous
    // control over a 64x range spends most of its travel in sizes nobody wants.
    //
    // The first entry is the size this sim has always been, and it is the
    // default — the numbers at the top of this file were measured there.
    static constexpr WorldSize kSizes[] = {
        {240, 160}, {480, 320}, {960, 640}, {1440, 960}, {1920, 1280}
    };

    // NOT static constexpr any more. They were, and a size knob over constants
    // is a knob that cannot do anything.
    int                    w_ = kSizes[0].w, h_ = kSizes[0].h;
    float                  sizeScale_ = 1.0f;
    static constexpr float kFavourable = 0.5f;      // what counts as good ground

    // The publish pass gets its own threshold, four times GridSim's, and that
    // is a measurement rather than a preference.
    //
    // GridSim::kParallelCells = 250000 is calibrated for a lattice sweep, which
    // reads nine neighbours and branches for every cell. Publishing reads one
    // float and writes one byte, perhaps a tenth of the work per cell, so the
    // same cell count buys nowhere near enough to cover creating fifteen
    // threads — parallel_for has no pool and builds them per call.
    //
    // Measured on this machine, publish alone, minimum of seven runs: at 614400
    // cells it went 0.88x, 1.16x, 0.86x, 0.81x on repeated runs — a wash that
    // lands on the wrong side as often as the right one. At 1382400 it is 1.5x
    // to 2.2x and at 2457600 it is 2.1x to 3.5x. So the line goes above the
    // wash: the two biggest worlds thread, the rest stay serial and pay
    // nothing. The comfort build in reset() keeps the ordinary threshold,
    // because a sqrt and an exp per patch per cell is expensive enough that it
    // wins by 4.7x at that same 614400 cells.
    static constexpr std::size_t kPublishParallelCells = 1000000;

    [[nodiscard]] WorldSize chosen_size() const {
        const int i = std::clamp(int(knob("size") + 0.5f), 0, int(std::size(kSizes)) - 1);
        return kSizes[i];
    }

    [[nodiscard]] float knob(const char* key) const {
        for (const auto& k : knobs_) if (k.key == key) return k.value;
        return 0.0f;
    }

    [[nodiscard]] float comfortAt(float x, float y) const {
        const int ix = std::clamp(int(x), 0, w_ - 1), iy = std::clamp(int(y), 0, h_ - 1);
        return comfort_[std::size_t(iy) * std::size_t(w_) + std::size_t(ix)];
    }

    [[nodiscard]] double meanSpeed() const {
        if (agents_.empty()) return 0.0;
        double s = 0;
        for (const auto& a : agents_) s += double(speedAt(comfortAt(a.x, a.y)));
        return s / double(agents_.size());
    }

    // Fast in bad ground, slow in good. This is the entire orthokinesis rule
    // and it contains no direction.
    [[nodiscard]] float speedAt(float comfort) const {
        return kBaseSpeed * sizeScale_ * (1.0f - knob("slow") * comfort);
    }
    // Erratic in bad ground, straight in good. Likewise.
    [[nodiscard]] float turnAt(float comfort) const {
        return kBaseTurn * (1.0f - knob("turn") * comfort);
    }

    // NOT THREADED, and it must stay that way.
    //
    // This is the obvious loop to parallelise — thousands of agents, each one
    // reading a comfort value and updating its own three floats — and it is the
    // one loop here that cannot be. Every agent draws from rng_, a single
    // xorshift stream, in agent order. Split it across threads and the draws
    // interleave differently every run, so the same seed stops reproducing the
    // same run and the aggregation index becomes a number that depends on how
    // many cores the machine has. That is not a faster sim, it is a broken one:
    // reproducibility from a seed is the property this whole bench is for.
    //
    // Giving each agent its own stream would fix the race and change every
    // published number in the comment at the top of this file, so it is not a
    // free win either. The parts of a step that CAN be threaded — the publish
    // pass and the comfort build — are, and they are the parts whose cost
    // actually grows with the world. The agent loop's cost grows with the
    // AGENT COUNT, which has its own knob and is unchanged.
    void tick() {
        ++gen_;
        for (auto& a : agents_) {
            const float c = comfortAt(a.x, a.y);
            a.h += (rng_.unit() * 2.0f - 1.0f) * turnAt(c);
            const float v = speedAt(c);
            float nx = a.x + std::cos(a.h) * v;
            float ny = a.y + std::sin(a.h) * v;
            // Reflect at the walls. Wrapping would let an agent leave a patch
            // and re-enter it from the far side, which quietly inflates the
            // index by turning a bounded world into a torus.
            if (nx < 0.0f || nx >= float(w_)) { a.h = 3.14159265f - a.h; nx = std::clamp(nx, 0.0f, float(w_) - 0.001f); }
            if (ny < 0.0f || ny >= float(h_)) { a.h = -a.h;              ny = std::clamp(ny, 0.0f, float(h_) - 0.001f); }
            a.x = nx; a.y = ny;
        }
        occSum_ += occupancy();
        ++sampled_;
    }

    void publish() {
        // Row-parallel above kPublishParallelCells — a pure map from one array
        // to another, each row writing only its own slice, and the only part of
        // a step whose cost grows with the world. The agents are a scatter of at
        // most a few thousand writes and stay serial: two agents in the same
        // cell would race, and at these counts the launch would cost more than
        // the work anyway.
        parallel_for(std::size_t(h_), [&](std::size_t row) {
            const std::size_t i0 = row * std::size_t(w_);
            for (int x = 0; x < w_; ++x) {
                const float c = comfort_[i0 + std::size_t(x)];
                view_.cells[i0 + std::size_t(x)] =
                    std::uint8_t(c >= 0.75f ? 4 : c >= 0.5f ? 3 : c >= 0.25f ? 2 : c >= 0.08f ? 1 : 0);
            }
        }, comfort_.size() >= kPublishParallelCells ? 0u : 1u);
        for (const auto& a : agents_) {
            const int ix = std::clamp(int(a.x), 0, w_ - 1), iy = std::clamp(int(a.y), 0, h_ - 1);
            view_.cells[std::size_t(iy) * std::size_t(w_) + std::size_t(ix)] = 5;
        }
    }

    Provenance          about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    Field               view_;
    std::vector<float>  comfort_;
    std::vector<Agent>  agents_;
    Rng                 rng_{0x4A17ull};
    double              favArea_ = 0.0, occSum_ = 0.0;
    std::uint64_t       sampled_ = 0, gen_ = 0;

    static constexpr float kBaseSpeed = 0.9f;
    static constexpr float kBaseTurn  = 1.2f;
};

inline SimPtr make_kinesis() { return std::make_unique<Kinesis>(); }

} // namespace bench
