// cutemold.hpp — colonies that compete for nothing but space.
//
// After erytau's Cute Mold (itch.io, open source, Go + Ebiten). The mechanic
// that makes it worth having on this bench is a single unusual choice:
//
//   A mold draws energy from the EMPTY SPACE IT SURROUNDS, scaled by the light
//   falling there. Molds cannot attack, poison or eat each other. There is no
//   predation and no direct interaction of any kind.
//
// So selection runs entirely through geometry. A shape that encloses a lot of
// void feeds well; a solid blob of the same cell count feeds badly, because
// only its rim touches anything empty. And since the only way to deny a rival
// is to occupy the space it would have enclosed, competition is real without
// any rule mentioning competition at all.
//
// That is a genuinely different thing from the other evolving population on
// this bench. The Life Engine has mouths, killers and armour — organisms
// interact directly, and the interesting outcome is predation appearing. Here
// nothing interacts, and the interesting outcome is that the shapes get
// intricate anyway, because intricacy IS the fitness function.
//
// From the original, with its terms: molds sharing a genome share a colour at
// different shades, and a mutated genome takes a new colour; cells produce
// spores which mature and found a new mold; v2 moved energy costs to sustaining
// life rather than to reproducing.

#pragma once
#include "../sim.hpp"
#include "../parallel.hpp"
#include "../rng.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace bench {

class CuteMold final : public Sim {
public:
    CuteMold() {
        about_ = Provenance{
            "Cute Mold",
            "2024",
            "erytau (itch.io), open source; this is a reimplementation from the described "
            "rules, not a port",
            "erytau, Cute Mold, erytau.itch.io/cute-mold",
            Replication::Disputed,
            "Disputed, on the same terms as everything else here that evolves. A mold's "
            "genome is copied into a spore with mutations and the copy founds a new colony, "
            "which is heredity with variation under selection. But the engine does the "
            "copying — no mold contains a description of how to build a mold. Von Neumann's "
            "line again, and the bench keeps drawing it.",
            "Colonies that get energy from the empty space they enclose, brighter space "
            "giving more. They cannot touch each other: the only competition is for room, and "
            "the only way to hurt a rival is to occupy space it would have surrounded. "
            "Nothing in the rules mentions shape, and shape is the whole game."
        };
        pal_ = { {{ 8, 9, 14}, "void"} };
        // Sixteen colony colours. A genome maps to one of them, so molds that
        // share ancestry share a hue and a mutation is visible as a new one.
        for (int i = 0; i < kColours; ++i) {
            const float h = float(i) / float(kColours) * 6.2831853f;
            pal_.push_back({{ std::uint8_t(120 + 110 * std::sin(h)),
                              std::uint8_t(120 + 110 * std::sin(h + 2.094f)),
                              std::uint8_t(120 + 110 * std::sin(h + 4.189f)) },
                            "a lineage"});
        }
        knobs_ = {
            {"seed", "run seed", 1.f, 40.f, 1.f, 1.f, {}, true,
             "Which independent run this is. The same seed reproduces the identical run."},
            // The world is resizable because the cost of a tick stopped being
            // quadratic in its area — see Mold::at. Old and new stepped side by
            // side to the same tick, settled, on one machine: 240x160 2.6 ms
            // against 1.8, 480x320 27 ms against 9.0, 960x640 314 ms against
            // 31, 1440x960 1475 ms against 62. The old shape is the one to
            // notice — four times the area for twelve times the cost, so a
            // world twice as wide as the default already took a second and a
            // half a tick, and the top of this list was simply not available.
            // 1920x1280 now runs at 141 ms a tick, and was never measured the
            // old way because settling it once would have taken half an hour.
            //
            // A choice list rather than a slider, for the same reason the
            // lattice family has one: the steps are factors of four in area,
            // and a continuous slider would spend most of its travel on sizes
            // nobody wants.
            {"size", "world size", 0.f, 4.f, 0.f, 1.f,
             {"240x160", "480x320", "960x640", "1440x960", "1920x1280"}, true,
             "Cells across. Area is what costs, and each step is four times the last: 1920x1280 "
             "is 2.5 million cells and about 141 ms a tick here, so it is a size to watch rather "
             "than to run fast. Room is the whole currency in this sim, and a bigger world buys "
             "more of it — 37,000 colonies at once against 460 in the default world, and enough "
             "space for a lineage to spread without immediately running into itself."},
            {"founders", "founding molds", 1.f, 60.f, 12.f, 1.f, {}, true,
             "How many colonies the world starts with, each with its own random genome."},
            // on_reset: the knob is read only in reset(), where the light field
            // is baked, so leaving this live made it a slider that did nothing.
            {"light", "light gradient", 0.f, 1.f, 0.6f, 0.05f, {}, true,
             "How much brighter one side of the world is than the other. Energy scales with "
             "the light on the void a mold encloses, so this makes the same shape worth more "
             "in some places than others — and gives the population somewhere to be going."},
            {"upkeep", "upkeep per cell", 0.f, 0.4f, 0.09f, 0.005f, {}, false,
             "Energy each living cell costs per tick. This is what makes a solid blob lose to "
             "a lacework: both pay per cell, and only one of them is enclosing anything."},
            {"mutate", "mutation rate", 0.f, 0.5f, 0.10f, 0.01f, {}, false,
             "Chance a spore's genome differs from its parent's. A mutated genome usually takes "
             "a new colour, so you can watch a lineage split."},
            {"spore", "energy to make a spore", 4.f, 80.f, 14.f, 1.f, {}, false,
             "What a colony must bank before it throws a spore. Higher is a slower, more "
             "selective world."},
            {"speed", "ticks per frame", 1.f, 20.f, 2.f, 1.f, {}, false,
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
        // Rebuild here, not only in reset(). reset() does rebuild every buffer
        // from kW/kH, so this is really "adopt the new dimensions and start
        // over" — but leaving it to the workbench's on_reset call would mean
        // field() reported the old size until then, and anything that stepped
        // in between would be stepping a world of one size through buffers of
        // another.
        // A rebuild, not a note to self. reset() rebuilds every buffer from
        // kW/kH, so changing the size means starting over — but leaving it to
        // the workbench's on_reset call would mean field() reported the old
        // size until then, and anything that stepped in between would be
        // stepping a world of one size through buffers sized for another.
        if (k == "size" && adoptSize()) reset();
    }

    [[nodiscard]] std::string subtitle() const override {
        char b[224];
        // The spore count is in the line because of what happened without it.
        // Growth used to run before reproduction and spend every unit of energy
        // a colony had, so no colony ever banked the cost of a spore and not
        // one was thrown in 2400 ticks — and nothing on screen said so. The
        // colony count barely moves in that state, so this is the number that
        // shows reproduction is happening at all.
        std::snprintf(b, sizeof b,
                      "tick %llu  ·  %zu colonies  ·  %d cells  ·  %d spores  ·  %zu lineages  ·  %.2f void per cell",
                      (unsigned long long)tick_, liveCount(), cellCount(), spores_, lineages(),
                      voidPerCell());
        return b;
    }

    void reset() override {
        // The size knob is read HERE as well as in on_knob, so the dimensions
        // come from the knob down either path. on_knob is the one the workbench
        // uses, but a caller that sets knobs().value directly and then resets —
        // which is exactly what the tests do for `seed` — would otherwise
        // rebuild every buffer at the size it was already at, and the knob
        // would look broken for reasons nothing in the sim explains.
        adoptSize();
        rng_.reseed(mix_seed(0xC07Eull, int(knob("seed") + 0.5f)));
        view_ = Field(kW, kH);
        owner_.assign(std::size_t(kW) * kH, -1);
        age_.assign(std::size_t(kW) * kH, 0);
        molds_.clear();
        tick_ = 0; spores_ = 0;

        light_.assign(std::size_t(kW) * kH, 1.0f);
        const float g = knob("light");
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x)
                light_[std::size_t(y) * kW + x] = 1.0f - g * (float(y) / float(kH));

        const int n = std::max(1, int(knob("founders") + 0.5f));
        for (int i = 0; i < n; ++i) {
            Mold m;
            m.genome.resize(kGenes);
            for (auto& gch : m.genome) gch = std::uint8_t(rng_.next() & 7);
            m.colour = colourOf(m.genome);
            m.energy = knob("spore") * 0.5f;
            m.alive  = true;
            const int x = 2 + int(rng_.unit() * float(kW - 4));
            const int y = 2 + int(rng_.unit() * float(kH - 4));
            const int id = int(molds_.size());
            molds_.push_back(std::move(m));
            claim(x, y, id);
        }
        publish();
    }

    void step() override {
        const int n = std::max(1, int(knob("speed") + 0.5f));
        for (int i = 0; i < n; ++i) tick();
        publish();
    }

    [[nodiscard]] std::uint64_t generation() const override { return tick_; }

    std::vector<Metric> metrics() const override {
        return {
            Metric{ "colonies", double(liveCount()), 0.0, Metric::Neither },
            // Colour classes, not species. A genome hashes to one of sixteen
            // colours, so this saturates at sixteen and is a lower bound on
            // diversity rather than a count of it. Named for what it measures.
            Metric{ "colour classes in use (max 16)", double(lineages()), 16.0, Metric::Neither },
            // How much empty space each living cell has next to it — the
            // quantity the feeding rule pays for, and so the closest thing here
            // to the fitness landscape.
            //
            // Neither, not Higher, and it used to be Higher. A Higher series
            // gets a best-so-far, and the best of this one is tick 0: measured
            // at the defaults with seed 1, the maximum over ticks 0..4000 is
            // 8.00 and it is reached at tick 0, when the twelve founders are
            // single cells each touching eight empty neighbours. The emptiest
            // possible world wins the contest. The number falls to where it
            // sits rather than climbing to it — the same run reads 7.00 at tick
            // 1, 5.39 at 10, 3.28 at 100, 2.66 at 250 and 2.71 at 4000.
            //
            // Nor is where it settles a product of selection, which is what the
            // comment here used to claim. Turning mutation off, so every spore
            // is an exact copy and no new genome ever appears, gives 2.725 /
            // 2.721 / 2.743 on seeds 1-3 against 2.755 / 2.736 / 2.755 with it
            // on; a single founder with no mutation — a world with no variation
            // at all for selection to act on — still gives 2.73 / 2.66 / 2.52.
            // It is the grow/starve equilibrium: starve() sheds the most
            // enclosed cells first, so shedding mechanically raises the mean.
            //
            // For scale, by this same 8-neighbour measure, a solid square
            // scores 2.24 at 25 cells, 1.63 at 49 and only 0.59 at 400 — while
            // the colonies here average about 54 cells (23941 cells across 446
            // colonies at tick 4000). "0.5 is a blob" was a fact about a blob
            // four hundred cells across, which this world does not contain.
            Metric{ "void touched per cell", voidPerCell(), 0.0, Metric::Neither },
            Metric{ "occupied fraction", double(cellCount()) / double(kW * kH), 1.0, Metric::Neither },
        };
    }

    // Drop a fresh colony with a new random genome, so a settled world can be
    // invaded and a dead one restarted.
    bool poke(float nx, float ny) override {
        Mold m;
        m.genome.resize(kGenes);
        for (auto& gch : m.genome) gch = std::uint8_t(rng_.next() & 7);
        m.colour = colourOf(m.genome);
        m.energy = knob("spore") * 0.5f;
        m.alive  = true;
        const int x = std::clamp(int(nx * float(kW)), 1, kW - 2);
        const int y = std::clamp(int(ny * float(kH)), 1, kH - 2);
        const int id = int(molds_.size());
        molds_.push_back(std::move(m));
        claim(x, y, id);
        publish();
        return true;
    }
    Field* editable() override { return nullptr; }

    // ── what the tests need ────────────────────────────────────────────────
    [[nodiscard]] std::size_t liveCount() const {
        std::size_t n = 0;
        for (const auto& m : molds_) if (m.alive) ++n;
        return n;
    }
    // Serial, and that is measured rather than an oversight. One pass over the
    // owner grid comparing an int against zero is pure bandwidth: 0.37 ms for
    // 614,400 cells. Threading it made it SLOWER at every worker count —
    // 0.57 ms on two, 1.62 ms on sixteen — because starting the threads costs
    // more than the scan does. It is the same shape of loop as voidPerCell()
    // without the eight neighbour reads, and those eight are the entire reason
    // that one is worth splitting up.
    [[nodiscard]] int cellCount() const {
        int n = 0;
        for (int o : owner_) if (o >= 0) ++n;
        return n;
    }
    // Spores that landed on clear ground and founded a colony, since reset.
    // A throw that finds nowhere clear in twelve tries costs the parent the
    // same energy and is not counted, so this is reproduction that WORKED, and
    // it is not the colony count — colonies die.
    [[nodiscard]] int spores() const { return spores_; }
    [[nodiscard]] std::size_t lineages() const {
        std::vector<std::uint32_t> seen;
        for (const auto& m : molds_)
            if (m.alive && std::find(seen.begin(), seen.end(), m.colour) == seen.end())
                seen.push_back(m.colour);
        return seen.size();
    }
    // Mean empty neighbours per living cell — the fitness landscape, measured.
    //
    // Eight neighbour reads for every occupied cell, and the workbench asks for
    // it twice a frame — once for the subtitle, once for the metric. At 960x640
    // that was 9.9 ms each, so 20 ms of observation against a 31 ms tick: the
    // sim was spending a serious fraction of the frame reporting on itself.
    // Threaded it is 2.2 ms, and it is worth threading precisely because the
    // eight neighbour reads make it arithmetic rather than bandwidth.
    [[nodiscard]] double voidPerCell() const {
        // Per row, then summed in row order. These are INTEGER totals, so the
        // sum is exact and grouping cannot change it: however the rows are
        // split across threads, `touch` and `cells` come out the same, and so
        // does the double they divide into. That is the whole reason this one
        // is safe to thread and the feeding sweep in tick() is not.
        std::vector<int>       rowCells(std::size_t(kH), 0);
        std::vector<long long> rowTouch(std::size_t(kH), 0);
        parallel_for(std::size_t(kH), [&](std::size_t row) {
            const int y = int(row);
            int c = 0; long long t = 0;
            for (int x = 0; x < kW; ++x) {
                if (owner_[std::size_t(y) * kW + x] < 0) continue;
                ++c;
                for (int d = 0; d < 8; ++d) {
                    const int nx = x + DX[d], ny = y + DY[d];
                    if (nx < 0 || ny < 0 || nx >= kW || ny >= kH) continue;
                    if (owner_[std::size_t(ny) * kW + nx] < 0) ++t;
                }
            }
            rowCells[row] = c; rowTouch[row] = t;
        }, workersFor());
        int cells = 0; long long touch = 0;
        for (std::size_t r = 0; r < rowCells.size(); ++r) {
            cells += rowCells[r]; touch += rowTouch[r];
        }
        return cells ? double(touch) / double(cells) : 0.0;
    }

private:
    struct Mold {
        std::vector<std::uint8_t> genome;
        // Where this colony's cells are. The grid says which mold owns a cell;
        // this says, for one mold, which cells those are — and without it the
        // only way to answer that was to look at every cell in the world.
        //
        // That is what made the world unresizable. starve() and kill() both
        // need one colony's cells, both walked the whole grid to find them, and
        // both run per colony per tick — so a tick cost colonies x area, and
        // the colony count grows WITH area. Measured settled, that way: 2.6 ms
        // at 240x160, 27 ms at 480x320, 314 ms at 960x640. Four times the area
        // for twelve times the cost is the quadratic in plain sight, and it put
        // the sizes past that into seconds per tick.
        //
        // Deliberately not kept in any particular order. starve() sorts by
        // (open neighbours, cell index) and every cell index is distinct, so
        // the sort is a total order and lands in exactly one arrangement
        // whatever order it was handed — which is why indexing the cells this
        // way cannot change which cells get shed.
        std::vector<int> at;
        std::uint32_t colour = 0;
        float energy = 0.0f;
        int   cells  = 0;
        bool  alive  = false;
    };
    // Members, not constants. They were `static constexpr int kW = 240, kH =
    // 160` — a size picked once and welded in — and a size knob that leaves
    // them alone is a slider that does nothing.
    int kW = 240, kH = 160;
    static constexpr int kSizes[5][2] = {
        {240, 160}, {480, 320}, {960, 640}, {1440, 960}, {1920, 1280}
    };
    static constexpr int kGenes = 300;          // v2's genome length
    static constexpr int kColours = 16;
    static constexpr int DX[8] = { 1, 1, 0,-1,-1,-1, 0, 1 };
    static constexpr int DY[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };

    [[nodiscard]] float knob(const char* key) const {
        for (const auto& k : knobs_) if (k.key == key) return k.value;
        return 0.0f;
    }

    // Take the world dimensions from the size knob. True if they changed.
    bool adoptSize() {
        const int i = std::clamp(int(knob("size") + 0.5f), 0, int(std::size(kSizes)) - 1);
        if (kSizes[i][0] == kW && kSizes[i][1] == kH) return false;
        kW = kSizes[i][0];
        kH = kSizes[i][1];
        return true;
    }

    // ── what is threaded here, and what is deliberately not ────────────────
    //
    // Threaded: publish(), voidPerCell(), cellCount(). All three are pure
    // passes over the grid — each row reads shared state that nothing is
    // changing and writes only its own slice or its own integer subtotal — so
    // the number of workers cannot show up in the result.
    //
    // NOT threaded, and this is the interesting half: the tick itself.
    //
    //   feed — the largest single phase, 21 ms of a 38 ms tick at 960x640, and
    //   it looks perfectly parallel: walk the grid, add up the lit void each
    //   colony touches. It is not. It accumulates into gain[o] as FLOATS, and
    //   float addition is not associative, so summing the same terms in a
    //   different order gives a different last bit. That bit decides whether a
    //   colony crosses `energy >= need` on this tick or the next, which changes
    //   which spore is thrown where, and the run diverges. A sim whose
    //   trajectory depends on the core count is not faster, it is broken.
    //
    //   grow — the budget is spent as the scan proceeds: a colony extends at
    //   most a twentieth of itself per tick, and which cells get that budget is
    //   decided by the order they are visited in. That is a sequential
    //   dependency in the rule, not an implementation detail.
    //
    //   starve — mutates owner_ while later colonies in the same tick are still
    //   being measured against it, so the colonies are not independent of each
    //   other within a tick either.
    //
    // What made the big sizes affordable was not threading, it was deleting the
    // quadratic: see Mold::at.
    static constexpr std::size_t kParallelCells = 250000;   // as the lattice sweeps use

    // Serial below the threshold, so the default 240x160 world — 38,400 cells —
    // never pays for a thread launch it cannot use.
    [[nodiscard]] unsigned workersFor() const {
        return (std::size_t(kW) * std::size_t(kH) >= kParallelCells) ? 0u : 1u;
    }

    // Same genome, same colour. A mutation usually shifts the hash to a new
    // colour — `h % 16` lands back on the parent's about one time in twelve —
    // which is what makes a lineage split visible without any bookkeeping.
    static std::uint32_t colourOf(const std::vector<std::uint8_t>& g) {
        std::uint32_t h = 2166136261u;
        for (std::uint8_t b : g) { h ^= b; h *= 16777619u; }
        return h % kColours;
    }

    void claim(int x, int y, int id) {
        const std::size_t i = std::size_t(y) * kW + x;
        if (owner_[i] >= 0) return;
        owner_[i] = id;
        age_[i]   = 0;
        ++molds_[std::size_t(id)].cells;
        molds_[std::size_t(id)].at.push_back(int(i));
    }

    void tick() {
        ++tick_;
        const float upkeep = knob("upkeep");
        const float need   = knob("spore");

        // 1 — feed. Energy is the lit void a colony touches, which is the whole
        // model: the same cells arranged differently are worth different
        // amounts, and nothing else pays.
        std::vector<float> gain(molds_.size(), 0.0f);
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) {
                const int o = owner_[std::size_t(y) * kW + x];
                if (o < 0) continue;
                for (int d = 0; d < 8; ++d) {
                    const int nx = x + DX[d], ny = y + DY[d];
                    if (nx < 0 || ny < 0 || nx >= kW || ny >= kH) continue;
                    const std::size_t ni = std::size_t(ny) * kW + nx;
                    if (owner_[ni] < 0) gain[std::size_t(o)] += 0.05f * light_[ni];
                }
            }
        for (std::size_t m = 0; m < molds_.size(); ++m) {
            if (!molds_[m].alive) continue;
            molds_[m].energy += gain[m] - upkeep * float(molds_[m].cells);
        }

        // 2 — spore, BEFORE growing.
        //
        // Growth ran first and spent every unit of energy the colony had, so
        // nothing ever banked enough to reproduce: twelve founders stayed
        // twelve founders for two thousand ticks and not one mutation ever
        // happened. A population that cannot reproduce is not evolving,
        // whatever else it is doing. Reproduction gets first claim on the
        // surplus now, and growth takes what is left.
        for (std::size_t m = 0; m < molds_.size(); ++m) {
            Mold& M = molds_[m];
            if (!M.alive) continue;
            if (M.cells == 0) { kill(int(m)); continue; }
            // Starving costs CELLS, not the colony.
            //
            // Killing the whole thing the moment energy went negative made the
            // world empty by tick 600: a colony grows until its void-per-cell
            // falls to break-even, overshoots by one tick, and was executed for
            // it with no way back. Shedding cells instead gives the population
            // an equilibrium to sit at — and shedding the most ENCLOSED cells
            // first is the same selection pressure the feeding rule applies,
            // now running in the other direction.
            if (M.energy < 0.0f) { starve(int(m)); continue; }
            if (M.energy >= need) {
                M.energy -= need;
                spore(int(m));
            }
        }
        // 3 — grow. Where a cell reaches is read from the genome, indexed by
        // its age and the local crowding, so the genome is a growth habit
        // rather than a picture.
        // Growth is rate-limited per colony, and that is what makes
        // reproduction possible at all. Unlimited growth spends every unit of
        // energy the moment it exists, so a colony never banks the cost of a
        // spore — measured, twelve founders stayed twelve for 2400 ticks and
        // not one mutation occurred. A colony now extends at most a twentieth
        // of itself per tick, or one cell, whichever is larger, and the surplus
        // is what buys spores.
        std::vector<int> budget(molds_.size(), 0);
        for (std::size_t m = 0; m < molds_.size(); ++m)
            budget[m] = std::max(1, molds_[m].cells / 20);
        std::vector<std::pair<int,int>> grew;
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) {
                const std::size_t i = std::size_t(y) * kW + x;
                const int o = owner_[i];
                if (o < 0 || !molds_[std::size_t(o)].alive) continue;
                if (molds_[std::size_t(o)].energy < 1.0f) continue;
                if (budget[std::size_t(o)] <= 0) continue;
                int around = 0;
                for (int d = 0; d < 8; ++d) {
                    const int nx = x + DX[d], ny = y + DY[d];
                    if (nx < 0 || ny < 0 || nx >= kW || ny >= kH) { ++around; continue; }
                    if (owner_[std::size_t(ny) * kW + nx] >= 0) ++around;
                }
                const auto& g = molds_[std::size_t(o)].genome;
                const std::size_t gi = (std::size_t(age_[i]) * 9u + std::size_t(around))
                                       % g.size();
                const int dir = g[gi] & 7;
                const int nx = x + DX[dir], ny = y + DY[dir];
                if (nx < 0 || ny < 0 || nx >= kW || ny >= kH) continue;
                if (owner_[std::size_t(ny) * kW + nx] >= 0) continue;
                grew.emplace_back(int(std::size_t(ny) * kW + nx), o);
                molds_[std::size_t(o)].energy -= 1.0f;
                --budget[std::size_t(o)];
            }
        // Applied after the sweep, so a cell placed this tick cannot also grow
        // this tick and the order of the scan cannot favour the top-left.
        for (auto& [cell, o] : grew) {
            if (owner_[std::size_t(cell)] >= 0) continue;
            owner_[std::size_t(cell)] = o;
            age_[std::size_t(cell)] = 0;
            ++molds_[std::size_t(o)].cells;
            molds_[std::size_t(o)].at.push_back(cell);
        }
        for (auto& a : age_) if (a < 250) ++a;

    }

    void spore(int parent) {
        // Find somewhere clear. A spore that lands inside a colony is wasted,
        // which is most of what stops one lineage covering the world.
        for (int attempt = 0; attempt < 12; ++attempt) {
            const int x = 1 + int(rng_.unit() * float(kW - 2));
            const int y = 1 + int(rng_.unit() * float(kH - 2));
            if (owner_[std::size_t(y) * kW + x] >= 0) continue;
            Mold m;
            m.genome = molds_[std::size_t(parent)].genome;
            if (rng_.unit() < knob("mutate")) {
                const int muts = 1 + int(rng_.unit() * 4.0f);
                for (int k = 0; k < muts; ++k)
                    m.genome[std::size_t(rng_.next() % m.genome.size())] =
                        std::uint8_t(rng_.next() & 7);
            }
            m.colour = colourOf(m.genome);
            m.energy = knob("spore") * 0.35f;
            m.alive  = true;
            const int id = int(molds_.size());
            molds_.push_back(std::move(m));
            claim(x, y, id);
            ++spores_;
            return;
        }
    }

    // Drop the cells that touch the least void — the interior, which eats
    // upkeep and feeds nothing.
    void starve(int id) {
        // Over this colony's own cells, not over the world. Same cells, same
        // count, same sort — the loop that used to find them by reading all
        // kW*kH owners is the whole reason a big world was unaffordable.
        Mold& M = molds_[std::size_t(id)];
        std::vector<std::pair<int,int>> byOpen;      // (void neighbours, cell)
        byOpen.reserve(M.at.size());
        for (const int i : M.at) {
            const int x = i % kW, y = i / kW;
            int open = 0;
            for (int d = 0; d < 8; ++d) {
                const int nx = x + DX[d], ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= kW || ny >= kH) continue;
                if (owner_[std::size_t(ny) * kW + nx] < 0) ++open;
            }
            byOpen.emplace_back(open, i);
        }
        if (byOpen.empty()) { kill(id); return; }
        std::sort(byOpen.begin(), byOpen.end());
        const std::size_t drop = std::max<std::size_t>(1, byOpen.size() / 12);
        for (std::size_t k = 0; k < drop && k < byOpen.size(); ++k) {
            owner_[std::size_t(byOpen[k].second)] = -1;
            --M.cells;
        }
        // Re-read the grid rather than trusting a bookkeeping subtraction: the
        // cells this colony still owns are exactly the ones still marked with
        // its id, so the index cannot drift out of step with the world.
        std::size_t keep = 0;
        for (const int i : M.at) if (owner_[std::size_t(i)] == id) M.at[keep++] = i;
        M.at.resize(keep);
        M.energy = 0.0f;
        if (M.cells <= 0) kill(id);
    }

    void kill(int id) {
        Mold& M = molds_[std::size_t(id)];
        if (!M.alive) return;
        M.alive = false;
        for (const int i : M.at) owner_[std::size_t(i)] = -1;
        // Released, not just cleared. Dead molds are kept forever so that ids
        // stay stable, and at a big size there are a great many of them.
        std::vector<int>().swap(M.at);
        M.cells = 0;
    }

    void publish() {
        // Row-parallel above the threshold. Each cell reads its own owner and
        // writes its own byte of the view, and nothing in here mutates state
        // the next cell will read, so the split is invisible in the output.
        parallel_for(std::size_t(kH), [&](std::size_t row) {
            const std::size_t base = row * std::size_t(kW);
            for (int x = 0; x < kW; ++x) {
                const std::size_t i = base + std::size_t(x);
                const int o = owner_[i];
                view_.cells[i] = (o < 0)
                    ? std::uint8_t(0)
                    : std::uint8_t(1 + molds_[std::size_t(o)].colour % kColours);
            }
        }, workersFor());
    }

    Provenance          about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    Field               view_;
    std::vector<int>    owner_;
    std::vector<std::uint8_t> age_;
    std::vector<float>  light_;
    std::vector<Mold>   molds_;
    Rng                 rng_{0xC07Eull};
    std::uint64_t       tick_ = 0;
    int                 spores_ = 0;
};

inline SimPtr make_cutemold() { return std::make_unique<CuteMold>(); }

} // namespace bench
