// langton_loops.hpp — Langton's self-reproducing loops.
//
// Langton, C. G. "Self-reproduction in cellular automata", Physica D 10:135-144
// (1984). 8 states, von Neumann neighbourhood, rotate-4 symmetric, 219
// transitions. Transition table as transcribed by Eli Bachmutsky from Hiroki
// Sayama's loops.java; the canonical 86-cell seed is the one in every reference
// implementation.
// Table checked against Golly's Langtons-Loops.rule (GPL-2.0-or-later).
// Retained credits and license: THIRD_PARTY_NOTICES.md and licenses/Golly.txt.
//
// This is the machine that made the trade explicit: Langton threw away von
// Neumann's requirement that a self-replicator ALSO be a universal constructor,
// and bought a factor of two thousand in size. 86 cells instead of ~200,000.
// The loop replicates and can do nothing else whatsoever.
//
// The table is written CNESWC' — centre, north, east, south, west, then the new
// centre. rotate4 means each entry also stands for its three rotations of the
// four neighbours. Any neighbourhood with no matching entry leaves the cell
// unchanged.

#pragma once
#include "../sim.hpp"
#include "../parallel.hpp"
#include <algorithm>
#include <array>
#include <string_view>

namespace bench {

class LangtonLoops final : public Sim {
public:
    explicit LangtonLoops(int size = 200) : cur_(size, size), nxt_(size, size) {
        // The size ladder starts at whatever this sim was CONSTRUCTED with and
        // doubles from there, so index 0 is always the world actually in use.
        // Hardcoding 200 as the first entry would mislabel the 140-wide one the
        // self-test builds.
        for (int i = 0, s = size; i < 5; ++i, s *= 2) sizes_.push_back(s);
        about_ = Provenance{
            "Langton's Loops", "1984", "Christopher G. Langton",
            "Langton, C. G. \"Self-reproduction in cellular automata\", Physica D 10:135-144 "
            "(1984). Transition table via Bachmutsky's transcription of Sayama's loops.java.",
            Replication::Yes,
            "Yes, and that is the entire extent of its abilities. Langton dropped von Neumann's "
            "condition that a replicator must also be able to construct anything else, and the "
            "machine collapsed from ~200,000 cells to 86. It copies itself and it is otherwise "
            "completely useless - which was his point about what replication alone is worth.",
            "A loop of sheathed data path with a genome circulating inside it: six extend signals "
            "and two turn-left. At the T-junction each pulse forks - one copy stays in the loop, "
            "one drives the construction arm. Four repetitions closes a daughter, which inherits "
            "the genome and detaches. About 151 ticks per copy. Watch the colony fill: interior "
            "loops get boxed in by their own children, stop breeding, and die into static coral. "
            "Only the rim reproduces."
        };
        pal_ = {
            {{  8, 11, 14}, "empty"},
            {{ 26, 54, 64}, "core (the track)"},
            {{ 90,209,196}, "sheath (structure)"},
            {{120,140,150}, "transient"},
            {{224,164, 88}, "signal: turn left"},
            {{224,120, 88}, "signal"},
            {{240,200, 90}, "signal"},
            {{255,245,180}, "signal: extend"},
        };
        build_table();
        knobs_ = {
                  // Room to run is the whole point of this rule.
                  //
                  // The colony strangles on its own children, and on 200 squared
                  // it also runs out of floor: the field stops changing at
                  // generation 3474, and from then on you are looking at a
                  // frozen picture of a wall rather than at the effect the rule
                  // is famous for. Bigger worlds separate the two, because the
                  // rim keeps breeding for as long as there is space in front of
                  // it. At 3200 squared the front has 256 times the area to
                  // cross, and the sweep is threaded above a million cells, so a
                  // step there costs about 5.5 ms on a full field rather than 26
                  // and 3.6 ms rather than 12 while the colony is still small.
                  {"size", "world size", 0.f, float(sizes_.size() - 1), 0.f, 1.f,
                   size_choices(), true,
                   "Cells across, squared. The first entry is the size this sim was built with "
                   "and the one every measurement quoted here was made at - the generation the "
                   "field freezes, the signal-count peak, the 151 ticks a copy. Bigger worlds do "
                   "not change the rule, they postpone the wall: the colony has further to grow "
                   "before its own children box it in. The top entry is ten million cells, so "
                   "expect to watch it for a long while before the front reaches the edge."},
                  {"colonies", "starting colonies", 1.f, 4.f, 1.f, 1.f, {}, true,
                   "How many loops to plant at reset. More than one is the interesting case: "
                   "colonies grow into each other, and the collision front is where the rule "
                   "stops being a tidy demonstration and starts being a population."}};
        reset();
    }

    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_;   }
    const Field&               field()   const override { return cur_;   }
    std::uint64_t              generation() const override { return gen_; }

    std::vector<Knob>& knobs() override { return knobs_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        if (k == "size") {
            // Rebuild both buffers HERE. reset() calls cur_.fill(0), which
            // refills the cells it already has and reallocates nothing, so a
            // size knob that stops at setting kn.value would change the label
            // and leave the world exactly as it was — a no-op that looks like
            // it worked. Re-plant as well, so the field is never left blank.
            const int i = std::clamp(int(v + 0.5f), 0, int(sizes_.size()) - 1);
            const int nsz = sizes_[std::size_t(i)];
            if (nsz != cur_.w) {
                cur_ = Field(nsz, nsz);
                nxt_ = Field(nsz, nsz);
                reset();
            }
        }
    }

    void reset() override {
        cur_.fill(0); gen_ = 0;
        // More than one starting loop is the interesting case: colonies grow
        // into each other and the collision front is where the rule stops being
        // a tidy demonstration and starts being a population.
        const int n = std::max(1, int(knob("colonies") + 0.5f));
        const int cx = cur_.w / 2 - 24, cy = cur_.h / 2 - 24;
        const int spread = std::min(cur_.w, cur_.h) / 4;
        for (int i = 0; i < n; ++i) {
            const int ox = (i % 2) ? spread : -spread;
            const int oy = (i / 2) ? spread : -spread;
            plant(cx + (n > 1 ? ox : 0), cy + (n > 1 ? oy : 0));
        }
    }

    void step() override {
        const int w = cur_.w, h = cur_.h;
        if (w < 3 || h < 3) { ++gen_; return; }   // no interior to sweep

        // The whole-buffer copy stays, and it is worth saying why, because
        // removing it looks like an obvious win and measured as a loss.
        //
        // It carries over the border, which the sweep never writes, and the
        // vacuum cells, which the sweep skips. Replacing it with a copy of just
        // the border plus an `out[x] = 0` in the skip branch does strictly less
        // work on paper — at 3200 squared it drops ten megabytes of copying a
        // step — and ran 12 to 21% SLOWER at every size and every occupancy,
        // measured with the variants interleaved so drift could not account for
        // it. One vectorised memcpy moves the buffer faster than scalar stores
        // sprinkled through a branchy loop can, and the stores also displace the
        // sweep's own writes. The copy is not the bottleneck; it is the cheap part.
        nxt_.cells = cur_.cells;

        // Row-parallel above a measured size. Every row reads cur_ and the
        // transition table, both immutable for the duration of the sweep, and
        // writes only its own slice of nxt_ — this rule has no random element
        // anywhere in it, so there is no shared mutable state for a thread
        // split to expose.
        //
        // The three buffers are taken as raw pointers into LOCALS before the
        // sweep, and the lambda captures those locals by value. This is not
        // stylistic: it is worth a factor of two, and the reason is aliasing.
        //
        // Cells are std::uint8_t — unsigned char — which is allowed to alias any
        // object at all. Written as cur_.at(x, y) and nxt_.set(x, y, v), every
        // store through nxt_ could in principle land on cur_'s own std::vector
        // members, so the compiler must reload cur_.cells' base pointer and
        // width after each one, in the innermost loop. Copying the pointers into
        // locals whose address is never taken removes the possibility: a store
        // through dst cannot change a value living in a register.
        //
        // Measured at the published 200 squared, both variants restored to the
        // same snapshot before every timed block so a decaying field could not
        // confound it: through cur_.at/nxt_.set the step ran at 0.53x the
        // pre-edit speed — the size knob would have cost the default size half
        // its performance. Through these pointers it is level with it.
        const std::uint8_t* const src = cur_.cells.data();
        std::uint8_t*       const dst = nxt_.cells.data();
        const std::uint8_t* const tbl = table_.data();
        const std::size_t cells = std::size_t(w) * std::size_t(h);
        parallel_for(std::size_t(h - 2), [src, dst, tbl, w](std::size_t row) {
            const int y = int(row) + 1;
            const std::size_t base = std::size_t(y) * std::size_t(w);
            const std::uint8_t* up = src + base - std::size_t(w);
            const std::uint8_t* mi = src + base;
            const std::uint8_t* dn = src + base + std::size_t(w);
            std::uint8_t*      out = dst + base;
            for (int x = 1; x < w - 1; ++x) {
                const std::uint8_t C = mi[x],     N = up[x], E = mi[x + 1],
                                   S = dn[x],     W = mi[x - 1];
                if (!(C | N | E | S | W)) continue;          // vacuum stays vacuum
                const std::uint8_t r = tbl[key(C, N, E, S, W)];
                out[x] = (r == kNone) ? C : r;               // unmatched: unchanged
            }
        }, (cells >= kParallelCells) ? 0u : 1u);

        cur_.cells.swap(nxt_.cells);
        ++gen_;
    }

    Field* editable() override { return &cur_; }
    [[nodiscard]] std::uint8_t paint_value() const override { return 2; }  // sheath

    // Click to drop another loop into clear space.
    bool poke(float nx, float ny) override {
        return plant(int(nx * cur_.w) - 7, int(ny * cur_.h) - 5);
    }

    // How much structure exists — the measure that shows the colony growing and
    // then strangling on its own children.
    [[nodiscard]] std::size_t sheath() const { return cur_.count(2); }

    // Structure keeps climbing while the signal count plateaus: the colony
    // strangles on its own children and only the rim still breeds. That gap IS
    // the result.
    //
    // The signal series is Neither, and is called "signal cells" rather than
    // "signals in flight", because it counts every cell in a signal state
    // wherever it sits — including the ones stranded in coral that will never
    // move again. Measured on the default 200x200 field: the count peaks at 703
    // around generation 2239, the whole field stops changing at generation 3474,
    // and 584 signal-state cells are still counted, frozen, at generation 8000.
    // Neither peak nor plateau is a score, so the panel should not offer a best.
    // Sheath keeps its direction: more structure is precisely the thing this
    // rule is claimed to produce, and the self-test uses its growth as the proof
    // that it replicates.
    std::vector<Metric> metrics() const override {
        return { {"sheath cells (structure)", double(sheath()),  0.0},
                 {"signal cells",             double(signals()), 0.0, Metric::Neither} };
    }

    // Every cell in a signal state (4-7), whether it is travelling down a data
    // path or stranded in coral for good.
    [[nodiscard]] std::size_t signals() const {
        std::size_t n = 0;
        for (auto c : cur_.cells) if (c >= 4) ++n;
        return n;
    }

private:
    static constexpr std::uint8_t kNone = 255;

    // Below this many cells the thread launch costs more than the sweep saves.
    //
    // Measured along a REAL run rather than at either extreme, because what a
    // step costs here depends on how much of the field is occupied — the sweep
    // skips any cell whose whole neighbourhood is vacuum — and the two extremes
    // disagree about where the crossover is. An empty field says about a
    // million cells; a saturated one says about four hundred thousand. Neither
    // is what you watch.
    //
    // Timed on a colony grown to several occupancies, serial and threaded
    // alternately inside each try, minimum of six. Speedups from threading:
    //
    //     400 squared    160,000 cells   1.01x  1.01x  1.02x        a wash
    //     560 squared    313,600         0.78x  1.12x  0.60x        a loss
    //     800 squared    640,000         0.80x  0.88x  1.01x  1.16x marginal
    //    1131 squared  1,279,161         1.42x  1.93x               a win
    //    1600 squared  2,560,000         2.23x  1.91x               a win
    //
    // (each column a point along the run, from one loop on the field to 60%
    // full). The crossover sits just under a million cells, so that is the
    // number — four times the 250,000 GridSim and the life-like engine use,
    // because one table lookup per cell is far cheaper than a life-like
    // neighbour count and it therefore takes far more cells to pay for the same
    // thread launch. Below it the sweep is under a millisecond and the launch
    // is around 1.2 ms, which is the whole story.
    static constexpr std::size_t kParallelCells = 1000000;

    [[nodiscard]] std::vector<std::string> size_choices() const {
        std::vector<std::string> out;
        out.reserve(sizes_.size());
        for (const int s : sizes_) out.push_back(std::to_string(s));
        return out;
    }

    static constexpr std::size_t key(std::uint8_t C, std::uint8_t N, std::uint8_t E,
                                     std::uint8_t S, std::uint8_t W) {
        return (std::size_t(C) << 12) | (std::size_t(N) << 9) | (std::size_t(E) << 6)
             | (std::size_t(S) << 3)  |  std::size_t(W);
    }

    void build_table() {
        table_.assign(1u << 15, kNone);
        std::string_view t = kTable;
        std::size_t i = 0;
        while (i < t.size()) {
            while (i < t.size() && (t[i] == ' ' || t[i] == '\n')) ++i;
            if (i + 6 > t.size()) break;
            std::uint8_t d[6];
            bool ok = true;
            for (int k = 0; k < 6; ++k) {
                const char c = t[i + k];
                if (c < '0' || c > '7') { ok = false; break; }
                d[k] = std::uint8_t(c - '0');
            }
            i += 6;
            if (!ok) continue;
            std::uint8_t C = d[0], N = d[1], E = d[2], S = d[3], W = d[4];
            const std::uint8_t out = d[5];
            for (int r = 0; r < 4; ++r) {                    // rotate4 symmetry
                const std::size_t k = key(C, N, E, S, W);
                if (table_[k] == kNone) table_[k] = out;
                const std::uint8_t nN = W, nE = N, nS = E, nW = S;
                N = nN; E = nE; S = nS; W = nW;
            }
        }
    }

    bool plant(int x0, int y0) {
        constexpr int SH = 10, SW = 15;
        if (x0 < 2 || y0 < 2 || x0 + SW + 2 >= cur_.w || y0 + SH + 2 >= cur_.h) return false;
        for (int y = -2; y < SH + 2; ++y)
            for (int x = -2; x < SW + 2; ++x)
                if (cur_.at(x0 + x, y0 + y) != 0) return false;   // needs clear space
        for (int y = 0; y < SH; ++y)
            for (int x = 0; x < SW; ++x)
                cur_.set(x0 + x, y0 + y, kSeed[y][x]);
        return true;
    }

    // The canonical 86-cell loop. Its genome is the signal cells sitting in the
    // data path: six extend signals (state 7) and two turn-left (state 4), the
    // 70 70 70 70 70 70 40 40 of the reference implementations. Counted off a
    // histogram of field() at generation 0, which gives 17 core, 61 sheath, 6
    // extend and 2 turn-left — 86 cells.
    static constexpr std::uint8_t kSeed[10][15] = {
        {0,2,2,2,2,2,2,2,2,0,0,0,0,0,0},
        {2,1,7,0,1,4,0,1,4,2,0,0,0,0,0},
        {2,0,2,2,2,2,2,2,0,2,0,0,0,0,0},
        {2,7,2,0,0,0,0,2,1,2,0,0,0,0,0},
        {2,1,2,0,0,0,0,2,1,2,0,0,0,0,0},
        {2,0,2,0,0,0,0,2,1,2,0,0,0,0,0},
        {2,7,2,0,0,0,0,2,1,2,0,0,0,0,0},
        {2,1,2,2,2,2,2,2,1,2,2,2,2,2,0},
        {2,0,7,1,0,7,1,0,7,1,1,1,1,1,2},
        {0,2,2,2,2,2,2,2,2,2,2,2,2,2,0},
    };

    static constexpr std::string_view kTable =
        "000000 000012 000020 000030 000050 000063 000071 000112 000122 000132 "
        "000212 000220 000230 000262 000272 000320 000525 000622 000722 001022 "
        "001120 002020 002030 002050 002125 002220 002322 005222 012321 012421 "
        "012525 012621 012721 012751 014221 014321 014421 014721 016251 017221 "
        "017255 017521 017621 017721 025271 100011 100061 100077 100111 100121 "
        "100211 100244 100277 100511 101011 101111 101244 101277 102026 102121 "
        "102211 102244 102263 102277 102327 102424 102626 102644 102677 102710 "
        "102727 105427 111121 111221 111244 111251 111261 111277 111522 112121 "
        "112221 112244 112251 112277 112321 112424 112621 112727 113221 122244 "
        "122277 122434 122547 123244 123277 124255 124267 125275 200012 200022 "
        "200042 200071 200122 200152 200212 200222 200232 200242 200250 200262 "
        "200272 200326 200423 200517 200522 200575 200722 201022 201122 201222 "
        "201422 201722 202022 202032 202052 202073 202122 202152 202212 202222 "
        "202272 202321 202422 202452 202520 202552 202622 202722 203122 203216 "
        "203226 203422 204222 205122 205212 205222 205521 205725 206222 206722 "
        "207122 207222 207422 207722 211222 211261 212222 212242 212262 212272 "
        "214222 215222 216222 217222 222272 222442 222462 222762 222772 300013 "
        "300022 300041 300076 300123 300421 300622 301021 301220 302511 401120 "
        "401220 401250 402120 402221 402326 402520 403221 500022 500215 500225 "
        "500232 500272 500520 502022 502122 502152 502220 502244 502722 512122 "
        "512220 512422 512722 600011 600021 602120 612125 612131 612225 700077 "
        "701120 701220 701250 702120 702221 702251 702321 702525 702720";
    std::vector<Knob>   knobs_;
    [[nodiscard]] float knob(const char* key) const {
        for (auto& kn : knobs_) if (kn.key == key) return kn.value;
        return 0.f;
    }

    Provenance                about_;
    std::vector<Swatch>       pal_;
    Field                     cur_, nxt_;
    std::vector<int>          sizes_;     // the size ladder the knob indexes
    std::vector<std::uint8_t> table_;
    std::uint64_t             gen_ = 0;
};

inline SimPtr make_langton_loops(int size = 200) {
    return std::make_unique<LangtonLoops>(size);
}

} // namespace bench
