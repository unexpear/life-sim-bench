// gasim.hpp — a genetic algorithm you can watch the population of.
//
// Every GA demonstration shows one curve: best fitness over time. That curve
// cannot distinguish the two failures that actually happen. A GA that has
// converged — every genome identical, no search left — draws exactly the same
// flat line as a GA on a problem that is merely hard. One is dead and one is
// working; the curve says the same thing about both.
//
// So this draws the POPULATION. Three views, and each answers a question the
// fitness curve cannot:
//
//   The band       every genome's fitness, one row per rank, one column per
//                  generation. Convergence is visible directly, as the rows
//                  flattening to a single colour.
//   The histogram  this generation's fitness distribution. Selection pressure
//                  is the shape of it: too much and it is a spike, too little
//                  and it never leaves its initial spread.
//   The genome     the best individual's genes against the known optimum, so
//                  "how close is it really" is a picture and not a number you
//                  have to trust.
//
// The problems are the standard real-valued benchmarks with known optima, which
// is what makes any of this checkable — see learn/ga.hpp.

#pragma once
#include "../learn/ga.hpp"
#include "../sim.hpp"
#include "../toolkit.hpp"
#include <algorithm>
#include <cstdio>
#include <deque>

namespace bench {

class GASim final : public Sim {
public:
    GASim() {
        about_ = Provenance{
            "Genetic algorithm, population visible",
            "1975",
            "Holland, Adaptation in Natural and Artificial Systems; selection schemes "
            "after Goldberg & Deb 1991; benchmark functions as published",
            "Holland 1975; Goldberg 1989; Baker 1985; Syswerda 1989; Goldberg & Deb 1991",
            Replication::Disputed,
            "Disputed, and the dispute is the interesting part. A GA population does copy "
            "itself with variation, which is reproduction in the sense that matters to "
            "evolution. But the copying is done BY the algorithm, from outside — no genome "
            "here contains instructions for building a genome. That is exactly the line von "
            "Neumann drew, and this bench keeps the two apart wherever it can.",
            "A real-valued GA on the standard benchmark functions, with every decision it "
            "makes exposed: how a parent is chosen, how raw scores become selection "
            "pressure, and how two parents combine. The picture is the POPULATION, not just "
            "its best member — because a converged GA and a hard problem draw the same "
            "fitness curve, and only the population can tell them apart."
        };
        pal_ = {
            {{ 10, 13, 18}, "unfit"},
            {{ 36, 58, 85}, "low fitness"},
            {{200,155, 60}, "middling"},
            {{111,227,192}, "the best of them"},
        };
        knobs_ = {
            {"seed", "run seed", 1.f, 40.f, 1.f, 1.f, {}, true,
             "Which independent run this is. The same seed reproduces the identical run."},
            {"problem", "problem", 0.f, 4.f, 1.f, 1.f,
             {"sphere", "rastrigin", "rosenbrock", "griewank", "schwefel"}, true,
             "Sphere is convex and separable — if a GA cannot solve it nothing else means "
             "anything. Rastrigin lays a cosine lattice over it, about 10^n local minima. "
             "Rosenbrock's optimum is in a narrow curved valley, so per-coordinate progress "
             "barely helps. Griewank COUPLES the coordinates through a product. Schwefel "
             "puts the optimum near a bound with the second-best basin at the far end, so "
             "converging early converges wrong."},
            // The population controller. Live, not on-reset: growing clones and
            // mutates existing members rather than injecting fresh randoms,
            // because a random genome dropped in at generation 200 is worthless
            // and would make every "bigger population is better" reading wrong
            // in the same direction.
            {"population", "population", 8.f, 400.f, 60.f, 4.f, {}, false,
             "Resized live. Growing clones and mutates existing members; shrinking keeps the "
             "best. A fresh random genome inserted late is worthless, so growth does not add "
             "any."},
            {"genes", "dimensions", 2.f, 32.f, 8.f, 1.f, {}, true,
             "How many variables the function takes. Difficulty rises sharply with this: "
             "Rastrigin has roughly 10^n local minima."},
            {"selection", "selection", 0.f, 4.f, 2.f, 1.f,
             {"uniform", "roulette", "tournament", "rank", "truncate"}, false,
             "How a parent is chosen. Uniform is the no-pressure control. Roulette is "
             "Holland's fitness-proportionate original and is scale-dependent in a way that "
             "quietly stops working. Tournament's pressure is set by k, rank's by "
             "construction, truncate's by the surviving fraction."},
            {"selparam", "k / keep fraction", 0.05f, 12.f, 3.f, 0.05f, {}, false,
             "Tournament size k, or the surviving fraction for truncation. Ignored by the "
             "other schemes."},
            {"scaling", "fitness scaling", 0.f, 4.f, 1.f, 1.f,
             {"raw", "rank", "sigma", "linear", "boltzmann"}, false,
             "How raw scores become selection weights. Raw is scale-dependent: add a "
             "constant to every fitness and the pressure collapses toward uniform without "
             "anything appearing to break. Rank is invariant to that by construction.  ·  "
             "Declared as needing ROULETTE, because roulette is the only scheme that reads "
             "the weights PROPORTIONALLY; the others merely compare them, so any strictly "
             "monotone rescaling leaves them alone. Sigma is not strictly monotone — it "
             "clamps the worst genomes to a common zero weight, and equal weights are then "
             "ordered by index, which changes who breeds. "
             "Measured on rastrigin at these defaults over 30 generations and 40 run seeds: "
             "sigma alters the result on 2 seeds under rank and 1 under truncation, and no "
             "other scaling alters anything under any scheme but roulette.", false, false, "selection", 1.f},
            {"scaleparam", "scaling constant", 0.02f, 4.f, 2.f, 0.02f, {}, false,
             "Goldberg's c for sigma truncation, the multiple of the mean the best gets for "
             "linear scaling, or the temperature T for Boltzmann — lower T is more pressure.  ·  "
             "Needs two things at once: roulette selection, and a scaling scheme that has a "
             "constant to read — raw and rank both ignore it entirely. Requiring sigma here "
             "gets both, because sigma in turn requires roulette.",
             false, false, "scaling", 2.f},
            {"crossover", "crossover", 0.f, 4.f, 3.f, 1.f,
             {"none", "one-point", "two-point", "uniform", "arithmetic"}, false,
             "How two parents combine. None is asexual — a real baseline, not a null option. "
             "One-point cuts once, which destroys structure that spans the cut; uniform flips "
             "per gene; arithmetic blends, which for real values can beat all of them and "
             "cannot ever leave the convex hull of the population."},
            {"crossrate", "crossover rate", 0.f, 1.f, 0.85f, 0.01f, {}, false,
             "Chance a child comes from two parents rather than one."},
            {"mutrate", "mutation rate", 0.f, 0.5f, 0.08f, 0.005f, {}, false,
             "Per gene, per child. Too low and the population converges and stops; too high "
             "and selection cannot hold on to anything it finds."},
            {"mutstep", "mutation step", 0.005f, 0.6f, 0.15f, 0.005f, {}, false,
             "Size of a mutation, as a fraction of the search range."},
            {"elites", "elites", 0.f, 10.f, 2.f, 1.f, {}, false,
             "Copied unchanged into the next generation. At zero the best is lost on all "
             "nine of seeds 1-9; at the default of 2 it never is."},
            {"speed", "generations per tick", 1.f, 60.f, 4.f, 1.f, {}, false,
             "Display rate only.", true},
        };
        surf_.resize(kW, kH);
        view_ = Field(64, 48);
        reset();
    }

    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field&               field()   const override { return view_; }
    const Surface*             surface() const override { return &surf_; }
    std::vector<Knob>&         knobs()   override { return knobs_; }

    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        if (k == "population") { ga_.resize(int(v + 0.5f)); render(); return; }
        applyLive();
    }

    [[nodiscard]] std::string subtitle() const override {
        char b[192];
        std::snprintf(b, sizeof b, "%s  ·  gen %d  ·  pop %d  ·  best cost %.5g  ·  %s / %s / %s",
                      name_of(ga_.params().problem), ga_.generation(), ga_.size(),
                      ga_.best_cost(), name_of(ga_.params().selection),
                      name_of(ga_.params().scaling), name_of(ga_.params().crossover));
        return b;
    }

    void reset() override {
        GA::Params p;
        p.population = int(knob("population") + 0.5f);
        p.genes      = int(knob("genes") + 0.5f);
        p.problem    = Bench(std::clamp(int(knob("problem") + 0.5f), 0, 4));
        readLive(p);
        ga_.init(p, mix_seed(0x6A1Eull, int(knob("seed") + 0.5f)));
        band_.clear();
        gen_ = 0; sinceBest_ = 0; bestEver_ = ga_.best_cost();
        pushBand();
        render();
    }

    void step() override {
        const int n = std::max(1, int(knob("speed") + 0.5f));
        for (int i = 0; i < n; ++i) advance();
        render();
    }

    // One epoch is one generation, which is the only boundary a GA has.
    [[nodiscard]] const char* epoch_name() const override { return "generation"; }
    [[nodiscard]] int epoch_count() const override { return ga_.generation(); }
    bool advance_epoch() override { advance(); render(); return true; }

    [[nodiscard]] std::uint64_t generation() const override { return gen_; }

    std::vector<Metric> metrics() const override {
        return {
            Metric{ "best cost", ga_.best_cost(), 0.0, Metric::Lower },
            Metric{ "mean fitness", ga_.mean_fitness(), 1.0, Metric::Higher },
            // Diversity is the number that separates the two failures a fitness
            // curve cannot: a converged population and a hard problem draw the
            // same flat line, and only this tells them apart.
            Metric{ "diversity", ga_.diversity(), 1.0, Metric::Neither },
            Metric{ "generations since gain", double(sinceBest_), 0.0, Metric::Neither },
        };
    }

    // Drop a burst of fresh random genomes over the worst of the population.
    // The standard remedy for a converged GA, and it makes the collapse in the
    // band visible as something you can undo.
    bool poke(float, float) override {
        ga_.immigrate(std::max(1, ga_.size() / 5));
        pushBand();
        render();
        return true;
    }

    [[nodiscard]] const GA& ga() const { return ga_; }

private:
    static constexpr int kW = 512, kH = 340;
    static constexpr int kBandH = 190;          // the generation x rank band
    static constexpr int kBandCols = kW;

    [[nodiscard]] float knob(const char* key) const {
        for (const auto& k : knobs_) if (k.key == key) return k.value;
        return 0.0f;
    }

    void readLive(GA::Params& p) const {
        p.selection     = Selection(std::clamp(int(knob("selection") + 0.5f), 0, 4));
        p.scaling       = Scaling  (std::clamp(int(knob("scaling")   + 0.5f), 0, 4));
        p.crossover     = Crossover(std::clamp(int(knob("crossover") + 0.5f), 0, 4));
        p.selParam      = double(knob("selparam"));
        p.scaleParam    = double(knob("scaleparam"));
        p.crossoverRate = double(knob("crossrate"));
        p.mutationRate  = double(knob("mutrate"));
        p.mutationStep  = double(knob("mutstep"));
        p.elites        = int(knob("elites") + 0.5f);
    }
    void applyLive() { readLive(ga_.params()); }

    void advance() {
        applyLive();
        ga_.step();
        ++gen_;
        if (ga_.best_cost() < bestEver_ - 1e-12) { bestEver_ = ga_.best_cost(); sinceBest_ = 0; }
        else ++sinceBest_;
        pushBand();
    }

    // One column of the band: every genome's fitness, sorted best-first.
    void pushBand() {
        std::vector<double> f = ga_.fitness();
        std::sort(f.begin(), f.end(), std::greater<double>());
        band_.push_back(std::move(f));
        if (int(band_.size()) > kBandCols) band_.pop_front();
    }

    static void put(Surface& s, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        if (x < 0 || y < 0 || x >= s.w || y >= s.h) return;
        const std::size_t i = (std::size_t(y) * std::size_t(s.w) + std::size_t(x)) * 4;
        s.rgba[i] = r; s.rgba[i+1] = g; s.rgba[i+2] = b; s.rgba[i+3] = 255;
    }
    static void box(Surface& s, int x0, int y0, int x1, int y1,
                    std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) put(s, x, y, r, g, b);
    }

    // Fitness to colour: a dark-blue to gold to pale ramp. Perceptually ordered
    // rather than a hue wheel, so "brighter is better" is true everywhere on it
    // and a rainbow's false banding cannot invent structure that is not there.
    static void ramp(double t, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
        t = std::clamp(t, 0.0, 1.0);
        const double a[3] = { 0.05, 0.07, 0.12 };
        const double m[3] = { 0.78, 0.55, 0.20 };
        const double z[3] = { 0.96, 0.98, 0.90 };
        double c[3];
        if (t < 0.5) { const double u = t * 2.0;
            for (int i = 0; i < 3; ++i) c[i] = a[i] + (m[i] - a[i]) * u; }
        else { const double u = (t - 0.5) * 2.0;
            for (int i = 0; i < 3; ++i) c[i] = m[i] + (z[i] - m[i]) * u; }
        r = std::uint8_t(c[0] * 255.0); g = std::uint8_t(c[1] * 255.0); b = std::uint8_t(c[2] * 255.0);
    }

    void render() {
        box(surf_, 0, 0, kW, kH, 10, 13, 18);

        // ── the band: x is generation, y is rank, colour is fitness ─────────
        //
        // Scaled to the whole band's own range rather than each column's, so a
        // column is comparable with the one beside it. Per-column normalisation
        // would renormalise a converged population back to full contrast and
        // draw convergence as if nothing had happened.
        double lo = 1e300, hi = -1e300;
        for (const auto& col : band_) for (double v : col) { lo = std::min(lo, v); hi = std::max(hi, v); }
        if (!(hi > lo)) { hi = lo + 1e-12; }
        const int cols = int(band_.size());
        if (cols > 0) {
            const int colW = std::max(1, kW / cols);
            for (int cx = 0; cx < cols; ++cx) {
                const auto& col = band_[std::size_t(cx)];
                const int x0 = (cols * colW >= kW) ? kW - cols + cx : cx * colW;
                const int x1 = (cols * colW >= kW) ? x0 + 1 : std::min(kW, x0 + colW);
                const int n = int(col.size());
                for (int y = 0; y < kBandH; ++y) {
                    const int rank = n > 1 ? y * (n - 1) / (kBandH - 1) : 0;
                    std::uint8_t r, g, b;
                    ramp((col[std::size_t(rank)] - lo) / (hi - lo), r, g, b);
                    for (int x = x0; x < x1; ++x) put(surf_, x, y, r, g, b);
                }
            }
        }
        // A rule at the top edge marks rank 0 — the elite row, which is the one
        // that stops moving first.
        for (int x = 0; x < kW; ++x) put(surf_, x, 0, 110, 227, 192);

        // ── histogram of this generation's fitness ─────────────────────────
        const int hy0 = kBandH + 12, hy1 = kH - 10, hx0 = 8, hx1 = kW / 2 - 12;
        box(surf_, hx0, hy0, hx1, hy1, 16, 20, 27);
        {
            const auto& f = ga_.fitness();
            const int bins = 40;
            std::vector<int> hist(std::size_t(bins), 0);
            double flo = 1e300, fhi = -1e300;
            for (double v : f) { flo = std::min(flo, v); fhi = std::max(fhi, v); }
            if (!(fhi > flo)) fhi = flo + 1e-12;
            for (double v : f) {
                int b = int((v - flo) / (fhi - flo) * double(bins - 1));
                hist[std::size_t(std::clamp(b, 0, bins - 1))]++;
            }
            int peak = 1;
            for (int c : hist) peak = std::max(peak, c);
            const int bw = std::max(1, (hx1 - hx0 - 4) / bins);
            for (int b = 0; b < bins; ++b) {
                const int h = (hy1 - hy0 - 4) * hist[std::size_t(b)] / peak;
                std::uint8_t r, g, bl;
                ramp(double(b) / double(bins - 1), r, g, bl);
                box(surf_, hx0 + 2 + b * bw, hy1 - 2 - h, hx0 + 2 + b * bw + bw - 1, hy1 - 2, r, g, bl);
            }
        }

        // ── the best genome against the known optimum ──────────────────────
        //
        // A bar per gene, centred on the optimum. Bars collapsing onto the
        // centre line is the whole run succeeding, in a form that needs no
        // number read off it.
        const int gx0 = kW / 2 + 4, gx1 = kW - 8;
        box(surf_, gx0, hy0, gx1, hy1, 16, 20, 27);
        {
            const auto& best = ga_.best();
            const double range = bench_range(ga_.params().problem);
            const double opt   = bench_optimum_at(ga_.params().problem);
            const int midY = (hy0 + hy1) / 2;
            for (int x = gx0 + 2; x < gx1 - 2; ++x) put(surf_, x, midY, 60, 72, 90);
            const int n  = int(best.size());
            const int bw = std::max(1, (gx1 - gx0 - 4) / std::max(1, n));
            for (int i = 0; i < n; ++i) {
                const double d = (best[std::size_t(i)] - opt) / range;      // -1..1
                const int h = int(std::clamp(d, -1.0, 1.0) * double((hy1 - hy0) / 2 - 3));
                const int x0 = gx0 + 2 + i * bw;
                const std::uint8_t r = std::uint8_t(std::fabs(d) > 0.25 ? 200 : 111);
                const std::uint8_t g = std::uint8_t(std::fabs(d) > 0.25 ? 155 : 227);
                const std::uint8_t b = std::uint8_t(std::fabs(d) > 0.25 ?  60 : 192);
                if (h >= 0) box(surf_, x0, midY - h, x0 + bw - 1, midY + 1, r, g, b);
                else        box(surf_, x0, midY, x0 + bw - 1, midY - h + 1, r, g, b);
            }
        }

        // The index view exists only so the timeline has something to store.
        // It is a coarse picture of the same band.
        for (int y = 0; y < view_.h; ++y)
            for (int x = 0; x < view_.w; ++x) {
                if (band_.empty()) { view_.set(x, y, 0); continue; }
                const int cx = int(band_.size()) - 1 - (view_.w - 1 - x);
                if (cx < 0) { view_.set(x, y, 0); continue; }
                const auto& col = band_[std::size_t(cx)];
                const int rank = int(col.size()) > 1
                               ? y * (int(col.size()) - 1) / (view_.h - 1) : 0;
                const double t = (col[std::size_t(rank)] - lo) / (hi - lo);
                view_.set(x, y, std::uint8_t(t > 0.75 ? 3 : t > 0.4 ? 2 : t > 0.05 ? 1 : 0));
            }
    }

    Provenance          about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    Surface             surf_;
    Field               view_;
    GA                  ga_;
    std::deque<std::vector<double>> band_;
    std::uint64_t gen_ = 0;
    int    sinceBest_ = 0;
    double bestEver_  = 1e300;
};

inline SimPtr make_gasim() { return std::make_unique<GASim>(); }

} // namespace bench
