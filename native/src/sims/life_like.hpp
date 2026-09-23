// life_like.hpp — the outer-totalistic family: B.../S...
//
// One engine covers Conway's Life, HighLife, Seeds, Day & Night and every other
// "life-like" rule, because they differ only in which neighbour counts cause
// birth and which allow survival. Writing them as separate classes would be
// four copies of the same loop.

#pragma once
#include "../sim.hpp"
#include "../rng.hpp"
#include "rulespec.hpp"
#include <bitset>

namespace bench {

class LifeLike final : public Sim {
public:
    // birth/survive given as bitmasks over neighbour counts 0..8, so B3/S23 is
    // birth = 1<<3, survive = (1<<2)|(1<<3).
    // Which notch of the size list a constructed width corresponds to. Rounds
    // to the nearest listed size rather than asserting, because a caller is
    // free to build an odd width and the knob still has to name something.
    [[nodiscard]] static int sizeIndex(int size) {
        int best = 0;
        for (int i = 1; i < 5; ++i)
            if (std::abs((256 << i) - size) < std::abs((256 << best) - size)) best = i;
        return best;
    }

    LifeLike(Provenance about, std::uint16_t birth, std::uint16_t survive,
             int size, float density, std::vector<Swatch> pal)
        : about_(std::move(about)), birth_(birth), survive_(survive),
          density_(density), pal_(std::move(pal)),
          cur_(size, size), nxt_(size, size) {
        // The two things that are genuinely free in a life-like rule. The rule
        // itself is fixed here by definition — vary that with the rulestring
        // template instead, which is what it exists for.
        knobs_ = {
            // The world is resizable now, because the sweep is threaded and a
            // big one is affordable. Measured at 1024 squared: 12.6 ms a step
            // on one thread and 3.2 ms threaded; at 2048 squared, 50.6 and 8.9.
            // The old fixed 256 was chosen when a step cost whatever a step
            // cost and nothing could be done about it.
            //
            // Powers of two, because the choice list is what makes the jump
            // from 1 to 4 million cells legible — a continuous slider through
            // that range spends most of its travel in sizes nobody wants.
            // The default is DERIVED from the size actually constructed, not
            // typed in. It was typed in, as 1, while the registry builds these
            // at 256 — so the panel read "512" over a 256-wide field in four
            // sims at once, and would have gone on lying for any caller that
            // passed a size other than the one the literal happened to match.
            {"size", "world size", 0.f, 4.f, float(sizeIndex(size)), 1.f,
             {"256", "512", "1024", "2048", "4096"}, true,
             "Cells across. 4096 wide is sixteen million cells square and about 40 ms a step "
             "here, so it is a size to look at rather than to run fast; the timeline is "
             "budgeted in bytes, so it simply keeps fewer snapshots as the world grows."},
            // Shape, separately from size, because the window is not square
            // and the field is fitted by min(canvasW/w, canvasH/h). A square
            // world in a 1920x1080 window is limited by the HEIGHT: it gets
            // 910x910 of a 1711x910 canvas and the other 55% is black bar.
            // Measured, after being assumed the other way round — hiding both
            // side panels widens the canvas by 660 pixels and gives the
            // simulation exactly none of them.
            {"shape", "world shape", 0.f, 2.f, 0.f, 1.f,
             {"square", "wide 16:9", "ultrawide 21:9"}, true,
             "Square is the classic setup and the one every published figure uses. The wide "
             "shapes exist because a wide window cannot show you any more of a square world "
             "however much room you give it - matching the world to the screen is the only "
             "thing that actually fills it. 2048 wide at 16:9 is 2.4 million cells against "
             "4.2 million square, so wide is also CHEAPER per step than the square of the "
             "same width."},
            {"density", "initial soup density", 0.0f, 1.0f, density_, 0.01f, {}, true,
             "Fraction of cells alive when the field is seeded. Changes the STARTING "
             "condition, not the rule. Apply setup to seed a new field."},
            {"seed",    "random seed",          1.0f, 64.0f, 1.0f,     1.0f,  {}, true,
             "Which random soup to start from. Same seed, same field, every time — that is "
             "what makes a result here reproducible. 1 is the seed this sim was built with."},
        };
        // The rule itself, as eighteen live bits. This is the actual transition
        // function, editable while it runs: turn S8 on and Conway's rule stops
        // killing its own crowds; turn B6 on and you have HighLife, replicator
        // and all. Density and seed change what you start with; these change
        // what the thing DOES.
        for (int n = 0; n <= 8; ++n) {
            const std::string N = std::to_string(n);
            switches_.emplace_back("B" + N, N, "birth", ((birth_ >> n) & 1) != 0,
                "BIRTH on " + N + ".  A DEAD cell with exactly " + N + " live neighbours "
                "comes alive next generation." + (n == 0
                    ? "  Leave this off: with it on, every empty cell is born at once and the "
                      "whole field ignites."
                    : (n == 3 ? "  This is Conway's only birth condition."
                              : "  Conway's rule does not birth on " + N + ".")));
        }
        for (int n = 0; n <= 8; ++n) {
            const std::string N = std::to_string(n);
            switches_.emplace_back("S" + N, N, "survive", ((survive_ >> n) & 1) != 0,
                "SURVIVAL on " + N + ".  A LIVE cell with exactly " + N + " live neighbours "
                "stays alive; with any count not switched on here it dies." + (n == 2 || n == 3
                    ? "  Conway's rule survives on 2 and 3 — under 2 is loneliness, over 3 is "
                      "overcrowding."
                    : ""));
        }
        base_ = about_;
        baseBirth_ = birth_; baseSurvive_ = survive_;
        reset();
    }

    std::vector<Knob>& knobs() override { return knobs_; }
    std::vector<Switch>& switches() override { return switches_; }

    // Editing the rule takes effect on the next step — no reset, so you can
    // watch a running pattern react to the rule changing underneath it.
    void on_switch(const std::string& key, bool v) override {
        for (auto& sw : switches_) if (sw.key == key) sw.value = v;
        birth_ = survive_ = 0;
        for (const auto& sw : switches_) {
            if (!sw.value) continue;
            const int n = sw.label[0] - '0';
            if (sw.group == "birth") birth_   = std::uint16_t(birth_   | (1u << n));
            else                     survive_ = std::uint16_t(survive_ | (1u << n));
        }
        retitle();
    }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        if (k == "size" || k == "shape") resize();
        if (k == "density") density_ = v;
        // Value 1 must mean the seed this sim was actually built with,
        // or the control reads as a lie the moment you look at it.
        if (k == "seed")    seed_ = baseSeed_ * std::uint64_t(v);
    }

    // Size and shape together decide the field, so both go through here —
    // two knobs each rebuilding it their own way is how they end up
    // disagreeing about which one was applied last.
    //
    // Rebuilding lives here rather than in reset() because reset() only
    // refills the cells the field already has: a size change made there would
    // be a no-op that looks exactly like a working knob.
    void resize() {
        const int n = 256 << std::clamp(int(knobValue("size") + 0.5f), 0, 4);
        const int shape = std::clamp(int(knobValue("shape") + 0.5f), 0, 2);
        int h = n;
        if (shape == 1) h = n * 9 / 16;
        if (shape == 2) h = n * 9 / 21;
        // Even, so the halving in the centre-out seeding lands on a cell.
        h = std::max(16, h & ~1);
        if (n != cur_.w || h != cur_.h) { cur_ = Field(n, h); nxt_ = Field(n, h); }
    }
    [[nodiscard]] float knobValue(const char* key) const {
        for (const auto& kn : knobs_) if (kn.key == key) return kn.value;
        return 0.f;
    }

    // If you edit the rule, this must stop claiming to be the rule it was.
    // Conway's Life with B6 switched on is HighLife, and saying otherwise would
    // be exactly the kind of false attribution the bench refuses everywhere
    // else. provenance_for() already knows the named rules and issues an
    // explicit non-claim for everything else, so editing B3/S23 into B36/S23
    // correctly hands you HighLife's real citation.
    void retitle() {
        RuleSpec r; r.birth = birth_; r.survive = survive_; r.states = 2;
        r.text = rule_to_string(birth_, survive_, 2);
        r.ok = true;
        const Provenance p = provenance_for(r);
        about_ = (r.text == rule_to_string(baseBirth_, baseSurvive_, 2)) ? base_ : p;
    }

    [[nodiscard]] std::string subtitle() const override {
        return rule_to_string(birth_, survive_, 2);
    }
    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field&               field()   const override { return cur_; }
    std::uint64_t              generation() const override { return gen_; }

    void reset() override {
        rng_.reseed(seed_);
        gen_ = 0;
        for (auto& c : cur_.cells) c = (rng_.unit() < density_) ? 1 : 0;
    }

    void step() override {
        // Row-parallel above a size threshold. This overrides GridSim::step()
        // with a tight loop that inlines the neighbour count instead of going
        // through the virtual rule(), so parallelising the base class did
        // nothing for the four sims that live here — worth saying, because
        // benchmarking `life` after changing the base sweep showed a flat 1.00x
        // across every worker count and looked like a threading failure rather
        // than like code that was never called.
        //
        // Double buffered, so rows are independent by construction: everything
        // read comes from cur_ and each row writes its own slice of nxt_.
        const int w = cur_.w, h = cur_.h;
        const std::size_t cells = std::size_t(w) * std::size_t(h);
        parallel_for(std::size_t(h), [&](std::size_t row) {
            const int y = int(row);
            for (int x = 0; x < w; ++x) {
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy) continue;
                        n += cur_.wrap(x + dx, y + dy) ? 1 : 0;
                    }
                const bool alive = cur_.at(x, y) != 0;
                const bool live  = alive ? ((survive_ >> n) & 1) : ((birth_ >> n) & 1);
                nxt_.set(x, y, live ? 1 : 0);
            }
        }, cells >= GridSim::kParallelCells ? 0u : 1u);
        cur_.cells.swap(nxt_.cells);
        ++gen_;
    }

    bool poke(float nx, float ny) override {
        const int cx = int(nx * cur_.w), cy = int(ny * cur_.h);
        for (int dy = -3; dy <= 3; ++dy)
            for (int dx = -3; dx <= 3; ++dx)
                if (rng_.unit() < 0.6f) {
                    int x = (cx + dx + cur_.w) % cur_.w;
                    int y = (cy + dy + cur_.h) % cur_.h;
                    cur_.set(x, y, 1);
                }
        return true;
    }

    // Real state, so the brush can paint straight into it.
    Field* editable() override { return &cur_; }

    // Test hooks: build an exact pattern rather than soup.
    void clear() { cur_.fill(0); gen_ = 0; }
    void put(int x, int y) { cur_.set(x, y, 1); }
    [[nodiscard]] std::size_t population() const { return cur_.count(1); }

private:
    Provenance          about_;
    std::uint16_t       birth_, survive_;
    float               density_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    std::vector<Switch> switches_;
    Provenance          base_;
    Field               cur_, nxt_;
    Rng                 rng_;
    std::uint64_t       gen_  = 0;
    std::uint16_t       baseBirth_ = 0, baseSurvive_ = 0;
    std::uint64_t       seed_ = 0xC0FFEEull;
    std::uint64_t       baseSeed_ = 0xC0FFEEull;
};

// ── the named rules ────────────────────────────────────────────────────────
inline constexpr std::uint16_t bits(std::initializer_list<int> ns) {
    std::uint16_t m = 0;
    for (int n : ns) m = std::uint16_t(m | (1u << n));
    return m;
}

inline SimPtr make_life(int size = 256) {
    Provenance p{
        "Conway's Game of Life", "1970",
        "John Horton Conway; popularised by Martin Gardner",
        "Gardner, M. \"Mathematical Games\", Scientific American 223(4), October 1970",
        Replication::Yes,
        "Yes, but it took forty years. Gemini (Andrew Wade, 2010) is the first pattern in Life "
        "that constructs a copy of itself. Conway proved the rule Turing-complete long before "
        "anyone built one.",
        "B3/S23. A dead cell with exactly three live neighbours is born; a live cell with two or "
        "three survives. Conway was hunting the simplest rule that was neither boring nor "
        "explosive, and tuned it by hand on a Go board."
    };
    return std::make_unique<LifeLike>(std::move(p), bits({3}), bits({2,3}), size, 0.28f,
        std::vector<Swatch>{{{8,11,14},"dead"},{{90,209,196},"alive"}});
}

inline SimPtr make_highlife(int size = 256) {
    Provenance p{
        "HighLife", "1994", "Nathan Thompson",
        "Rule B36/S23; the 12-cell replicator is widely catalogued",
        Replication::Yes,
        "Yes, and for free — a 12-cell pattern copies itself every 12 generations. It is also "
        "completely stupid: it can copy, and it can do nothing else. Which is exactly why "
        "replication alone was never the interesting property.",
        "Conway's rule with one extra birth condition: six neighbours also give birth. One digit "
        "of difference buys a natural self-replicator."
    };
    return std::make_unique<LifeLike>(std::move(p), bits({3,6}), bits({2,3}), size, 0.28f,
        std::vector<Swatch>{{{8,11,14},"dead"},{{242,193,78},"alive"}});
}

inline SimPtr make_seeds(int size = 256) {
    Provenance p{
        "Seeds", "—", "rule catalogued by Mirek Wojtowicz among others",
        "Rule B2/S — no survival condition at all",
        Replication::No,
        "No — but not because nothing lasts. No cell survives a tick (S is empty), yet shapes "
        "do: a diagonal pair is a period-2 oscillator and the 4-cell O..O over .OO. is a "
        "spaceship, both still exactly themselves after 5000 generations of this sim. A domino "
        "even prints two translated copies of itself in a single tick. What is missing is a "
        "lineage — the copies collide within a few generations and nothing here goes on making "
        "more.",
        "B2/S. Born with exactly two neighbours; never survives. Every cell on screen is exactly "
        "one generation old. It is here as the far side of the boundary Life sits on."
    };
    return std::make_unique<LifeLike>(std::move(p), bits({2}), 0, size, 0.004f,
        std::vector<Swatch>{{{8,11,14},"dead"},{{217,83,79},"alive for one tick"}});
}

inline SimPtr make_day_night(int size = 256) {
    Provenance p{
        "Day & Night", "1997", "Nathan Thompson",
        "Rule B3678/S34678",
        Replication::No,
        "No known self-replicator, though the rule supports complex engineered patterns.",
        "B3678/S34678. Symmetric under swapping live and dead — patterns and their photographic "
        "negatives obey the same rule, so 'inkspots' of either colour behave identically."
    };
    return std::make_unique<LifeLike>(std::move(p), bits({3,6,7,8}), bits({3,4,6,7,8}), size, 0.5f,
        std::vector<Swatch>{{{8,11,14},"dead"},{{155,122,230},"alive"}});
}

} // namespace bench
