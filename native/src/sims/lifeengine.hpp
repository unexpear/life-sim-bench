// lifeengine.hpp — organisms made of cells, on a grid, evolving.
//
// After Max Robinson's Life Engine (Emergent Garden, 2022). The rules are his;
// the implementation and the measurements are this bench's.
//
// Why it belongs here and not in the cellular-automaton section: this is the
// closest thing on the bench to von Neumann's question. An organism is a set of
// cells with types and offsets — an anatomy — and when it has eaten enough it
// makes a copy of that anatomy with mutations. The copy is a copy of the
// organism's OWN structure, not of a rule applied uniformly to a lattice, and
// the population that results is under selection: an anatomy that cannot feed
// itself does not persist.
//
// What it is NOT, and the bench says so plainly: the organism does not build
// the copy. The engine reads the anatomy and writes a new one. Nothing in the
// grid is a machine that constructs machines, which is exactly the line von
// Neumann drew and the thing his 29-state automaton actually does. Calling this
// self-replication without that caveat is the single most common overclaim in
// artificial life, so the classification is Disputed and the note explains why.
//
// The cell types, and what each one is FOR:
//
//   Mouth     eats food in the eight cells around it. An organism with no
//             mouth cannot feed and dies of whatever it started with.
//   Producer  turns empty neighbours into food, slowly. The only source of
//             energy in the world, so every food chain starts at one.
//   Mover     lets the whole organism move, and rotate. Costs upkeep.
//   Killer    kills an organism in any of the four cells beside it, outright:
//             touching ONE of its cells is enough and the whole body becomes
//             food. That is Robinson's rule rather than a simplification of
//             it. Armour is the only defence and it defends cell by cell — a
//             killer that reaches any unarmoured cell takes the whole organism
//             anyway.
//   Armour    blocks killers. Does nothing else, and costs upkeep like the rest.
//   Eye       looks along each axis for the nearest food and points the movers
//             at it. Useless without a mover.
//
// The interesting result the original demonstrates, and which this reproduces:
// from a single producer-and-mouth organism, the population separates into
// sessile producer colonies and mobile predators, without either being coded
// for. Movers are strictly a cost until there is something worth moving toward.

#pragma once
#include "../sim.hpp"
#include "../toolkit.hpp"
#include "../rng.hpp"
#include <algorithm>
#include <cstdio>
#include <vector>

namespace bench {

class LifeEngine final : public Sim {
public:
    // Grid contents. Food and walls are world state; the rest are cells that
    // belong to some organism.
    enum Tile : std::uint8_t {
        Empty = 0, Food, Wall,
        Mouth, Producer, Mover, Killer, Armour, Eye
    };
    static constexpr int kFirstCell = Mouth;
    static constexpr int kCellKinds = 6;

    LifeEngine() {
        about_ = Provenance{
            "The Life Engine",
            "2022",
            "Max Robinson (Emergent Garden). Rules as published; this implementation "
            "and its measurements are the bench's own.",
            "Robinson, The Life Engine, emergentgarden.com, 2022",
            Replication::Disputed,
            "Disputed, and the distinction is the whole reason this sim is on the bench. "
            "An organism here is an ANATOMY — a set of typed cells at offsets — and when it "
            "has eaten enough, a mutated copy of that anatomy is placed nearby. That is "
            "heredity with variation under selection, which is evolution in the sense that "
            "matters. But the organism does not BUILD the copy: the engine reads its "
            "anatomy and writes a new one. Nothing in this grid is a machine that "
            "constructs machines from parts, which is the thing von Neumann's 29-state "
            "automaton actually does. Calling this self-replication without that caveat is "
            "the most common overclaim in artificial life.",
            "Organisms made of typed cells on a grid. Mouths eat, producers make food, "
            "movers move, killers kill, armour blocks killers, eyes look ahead. Every cell "
            "costs upkeep, so any part that does not pay for itself is selected away. From "
            "a single producer-and-mouth ancestor the population separates into sessile "
            "producer colonies and mobile predators, with neither coded for."
        };
        pal_ = {
            {{ 12, 14, 20}, "empty"},
            {{ 96,152, 72}, "food"},
            {{ 70, 74, 86}, "wall"},
            {{232,116,140}, "mouth"},
            {{ 96,208,128}, "producer"},
            {{116,168,248}, "mover"},
            {{236, 88, 72}, "killer"},
            {{206,206,214}, "armour"},
            {{240,208, 96}, "eye"},
        };
        knobs_ = {
            {"seed", "run seed", 1.f, 40.f, 1.f, 1.f, {}, true,
             "Which independent run this is. The same seed reproduces the identical run."},
            // The ceiling was 220, which is 48 thousand cells and about a
            // thousand organisms. It is 600 now — 360 thousand cells and ten
            // thousand organisms at saturation, which is the difference between
            // a world that holds a few lineages and one that holds a landscape
            // of them.
            //
            // 600 and not more, and the reason is specific to this sim. Measured
            // at saturation, one tick costs 0.13 ms at 120, 1.46 at 320, 3.54 at
            // 440, 6.06 at 600 and 15.2 at 800; the population it carries is
            // 995, 3016, 5354, 10066 and 17690. Cost tracks the population, and
            // the per-organism cost itself creeps up with the world — 0.13 to
            // 0.86 microseconds — because a bigger grid means the owner map no
            // longer sits in cache. At 600 a tick is 6 ms, so the default of
            // four ticks a frame is still a comfortable 24 ms. At 800 it is 61,
            // and unlike the lattice rules there is no threading available to
            // claw that back (see step() for why), so 800 would be a size that
            // is permanently slow rather than one that is slow on one machine.
            //
            // A stepped slider rather than a choices list: the useful sizes here
            // are a continuum, not a handful of powers of two.
            {"size", "world size", 60.f, 600.f, 120.f, 10.f, {}, true,
             "Cells across. Bigger worlds hold more lineages at once, which is what lets "
             "different strategies coexist instead of one sweeping everything — at 600 the "
             "world settles at around ten thousand organisms against a thousand at 120. A "
             "tick costs about 6 ms there against 0.13 ms at 120, and this step cannot be "
             "threaded, so the larger sizes are genuinely slower to watch."},
            // These four defaults were swept, not chosen. The first attempt had
            // upkeep at 0.03 and production at 0.012: a producer emits roughly
            // one food per 83 ticks, of which two thirds are reachable by the
            // mouth, against 0.06 burned every tick by a two-cell body — an
            // energy budget wrong by a factor of about 18, and the ancestor
            // died without a single birth. The sweep is in STATUS.
            {"produce", "producer rate", 0.f, 0.25f, 0.12f, 0.005f, {}, false,
             "Chance per producer cell per tick of turning one empty neighbour into food. "
             "This is the only energy entering the world, so it sets the ceiling on the "
             "whole population."},
            {"upkeep", "upkeep per cell", 0.f, 0.05f, 0.008f, 0.001f, {}, false,
             "Food burned per cell per tick. This is what makes a useless organ cost "
             "something: with upkeep at zero, armour and eyes are free and never selected "
             "away."},
            {"mutate", "mutation rate", 0.f, 0.6f, 0.15f, 0.01f, {}, false,
             "Chance a child differs from its parent: a cell added, removed, or retyped."},
            {"food", "food to reproduce", 2.f, 30.f, 6.f, 1.f, {}, false,
             "How much an organism must bank before it makes a copy. Higher is a slower, "
             "more selective world."},
            {"life", "lifespan per cell", 50.f, 800.f, 400.f, 10.f, {}, false,
             "Ticks an organism lives per cell it has. Larger anatomies live longer, which "
             "is the counterweight to their upkeep."},
            {"speed", "ticks per frame", 1.f, 40.f, 4.f, 1.f, {}, false,
             "Display rate only.", true},
        };
        reset();
    }

    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field&               field()   const override { return grid_; }
    std::vector<Knob>&         knobs()   override { return knobs_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        // Resize here as well as at reset. reset() does rebuild the grid and the
        // owner map from this knob, so the workbench's reset-on-release already
        // worked — but that left the knob correct only for callers who know to
        // reset, and a size control that silently does nothing for everyone else
        // is the exact failure this is worth being defensive about. reset() is
        // the rebuild, so calling it is both the fix and the regeneration.
        if (k == "size" && int(v + 0.5f) != grid_.w) reset();
    }

    [[nodiscard]] std::string subtitle() const override {
        char b[192];
        // "killed" is here because it is the evidence for the claim this sim is
        // on the bench to make. Killer SHARE cannot carry it: an anatomy full of
        // killer cells that never reaches anybody reads exactly the same as one
        // that hunts, and the counter is the difference between the two.
        std::snprintf(b, sizeof b,
                      // %d, not %.1f: meanCells() returns an int, and feeding
                      // an int to a float specifier is undefined behaviour that
                      // happened to print "mean 0.0 cells" for every run.
                      "tick %llu  ·  %zu alive  ·  %d born  ·  %d killed  ·  mean %d cells  ·  %d movers",
                      (unsigned long long)tick_, live_.size(), births_, kills_, meanCells(), movers_);
        return b;
    }

    void reset() override {
        rng_.reseed(mix_seed(0x11FEull, int(knob("seed") + 0.5f)));
        const int n = int(knob("size") + 0.5f);
        grid_ = Field(n, n);
        owner_.assign(std::size_t(n) * n, -1);
        orgs_.clear(); live_.clear(); free_.clear();
        tick_ = 0; births_ = 0; kills_ = 0; movers_ = 0;

        // A wall ring, so nothing wraps. Wrapping a world with movers in it
        // makes "went a long way" and "went in a circle" the same measurement.
        for (int i = 0; i < n; ++i) {
            grid_.set(i, 0, Wall); grid_.set(i, n - 1, Wall);
            grid_.set(0, i, Wall); grid_.set(n - 1, i, Wall);
        }

        // The ancestor: one producer and one mouth. Everything else in the run
        // has to be discovered, which is what makes the outcome worth watching.
        for (int i = 0; i < std::max(2, n / 20); ++i) {
            Organism o;
            o.cells = { {0, 0, Producer}, {1, 0, Mouth} };
            o.x = 4 + int(rng_.unit() * float(n - 8));
            o.y = 4 + int(rng_.unit() * float(n - 8));
            o.food = knob("food") * 0.5f;
            place(o);
        }
        publish();
    }

    // NOT threaded, and this is a correctness decision rather than an oversight.
    //
    // Measurement says threading would pay: a tick at the 600 ceiling costs 6 ms
    // and is pure organism work, which is the shape of thing that usually splits
    // across cores. It cannot be split here, because tick() is a chain of
    // order-dependent effects on state every organism shares:
    //
    //   · one Rng. Every producer emission, every mutation, every wander and
    //     every reproduction attempt draws from rng_ in sequence. Split the
    //     organism loop and the draws are dealt out in an order that depends on
    //     how the chunks landed, so the same seed stops meaning the same run.
    //   · one grid. A mouth eating a food tile, a mover claiming a cell, a
    //     producer filling an empty one and a killer destroying a body are all
    //     writes other organisms read in the same tick. Who gets there first is
    //     the simulation, not an implementation detail of it.
    //   · reproduce() appends to orgs_ and live_, which reallocates the vector
    //     every other organism is being read through.
    //
    // Any of the three alone would make a threaded tick produce a different
    // world on a different core count, which is the one thing parallel.hpp
    // exists to forbid — a result that depends on the machine is not a result.
    // So the ceiling above was chosen to keep a tick affordable SERIALLY instead,
    // which is why it is 600 and not 800.
    void step() override {
        const int n = std::max(1, int(knob("speed") + 0.5f));
        for (int i = 0; i < n; ++i) tick();
        publish();
    }

    // One epoch is a full turnover of the population: every organism alive now
    // is gone. Generations do not line up in a world where lifespans overlap,
    // and this is the honest substitute — it is the unit in which an anatomy
    // can be said to have been replaced by its descendants.
    [[nodiscard]] const char* epoch_name() const override { return "turnover"; }
    [[nodiscard]] int epoch_count() const override { return turnover_; }
    bool advance_epoch() override {
        std::vector<int> watch = live_;
        const int start = turnover_;
        // Bounded at 2500 ticks, not 400000.
        //
        // A turnover takes on the order of a thousand ticks — 787 and 710
        // measured on seeds 1 and 2. This loop never sees one: `watch` holds
        // slot ids, and the end-of-tick prune pushes dead ids into free_, so a
        // watched slot is handed to a NEW organism and reads alive again.
        // Measured, 0 of 148 watched slots still held their original organism
        // after 20000 ticks while 82 of them still tested alive, so every epoch
        // at the defaults ends by hitting the bound — ten epochs took exactly
        // 25000 ticks. The 400000 guard was meant as a backstop and instead
        // became the actual cost. The generic knob test runs twenty-four
        // epochs per knob and hung on it. Reaching the bound counts as the
        // epoch completing, which is honest: what it measures is "a while,
        // during which the population turned over or did not".
        for (int guard = 0; guard < 2500 && turnover_ == start; ++guard) {
            tick();
            bool anyLeft = false;
            for (int id : watch)
                if (id < int(orgs_.size()) && orgs_[std::size_t(id)].alive) { anyLeft = true; break; }
            if (!anyLeft || live_.empty()) ++turnover_;
        }
        if (turnover_ == start) ++turnover_;      // the bound counts too
        publish();
        return true;
    }

    [[nodiscard]] std::uint64_t generation() const override { return tick_; }

    std::vector<Metric> metrics() const override {
        std::size_t counts[kCellKinds] = {0,0,0,0,0,0};
        for (int id : live_)
            for (const auto& c : orgs_[std::size_t(id)].cells)
                if (c.type >= kFirstCell) counts[c.type - kFirstCell]++;
        double total = 0; for (auto v : counts) total += double(v);
        if (total <= 0) total = 1;
        return {
            Metric{ "organisms alive", double(live_.size()), 0.0, Metric::Neither },
            Metric{ "mean cells per organism", double(meanCells()), 0.0, Metric::Neither },
            // The share of the population's cells that are movers is the one
            // number that shows the split the original is famous for: it starts
            // at zero, because the ancestor cannot move, and only rises if
            // moving starts paying for its upkeep.
            Metric{ "mover share", double(counts[Mover - kFirstCell]) / total, 1.0, Metric::Neither },
            Metric{ "killer share", double(counts[Killer - kFirstCell]) / total, 1.0, Metric::Neither },
            Metric{ "producer share", double(counts[Producer - kFirstCell]) / total, 1.0, Metric::Neither },
        };
    }

    // Drop a fresh ancestor, so a dead world can be restarted without losing
    // the run — and so a world that has settled can be perturbed.
    bool poke(float nx, float ny) override {
        Organism o;
        o.cells = { {0, 0, Producer}, {1, 0, Mouth} };
        o.x = std::clamp(int(nx * float(grid_.w)), 2, grid_.w - 3);
        o.y = std::clamp(int(ny * float(grid_.h)), 2, grid_.h - 3);
        o.food = knob("food") * 0.5f;
        const bool ok = place(o);
        publish();
        return ok;
    }

    // Painting into this grid would be erased on the next tick: every cell here
    // belongs to an organism, and a cell with no owner is not a living thing.
    Field* editable() override { return nullptr; }

    // Seed a specific anatomy, for controlled experiments. The evolved world
    // almost never puts an eye and a mover in the same organism, so "does the
    // eye do anything" cannot be answered by watching a normal run.
    bool seed_anatomy(const std::vector<std::pair<std::pair<int,int>, std::uint8_t>>& cells,
                      int x, int y) {
        Organism o;
        for (const auto& c : cells) o.cells.push_back(Cell{c.first.first, c.first.second, c.second});
        o.x = x; o.y = y;
        o.food = knob("food") * 0.5f;
        return place(o);
    }

    [[nodiscard]] std::size_t alive() const { return live_.size(); }
    // For the invariant that catches recycled-slot duplication: these two must
    // be equal, always.
    [[nodiscard]] int owned_tiles() const {
        int n = 0; for (int o : owner_) if (o >= 0) ++n; return n;
    }
    [[nodiscard]] int total_cells() const {
        int n = 0; for (int id : live_) n += int(orgs_[std::size_t(id)].cells.size()); return n;
    }
    [[nodiscard]] int world_area() const { return grid_.w * grid_.h; }
    [[nodiscard]] int food_on_grid() const {
        int n = 0; for (auto c : grid_.cells) if (c == Food) ++n; return n;
    }
    [[nodiscard]] float mean_food() const {
        if (live_.empty()) return 0.f;
        float f = 0; for (int id : live_) f += orgs_[std::size_t(id)].food;
        return f / float(live_.size());
    }
    [[nodiscard]] int born() const { return births_; }
    // Organisms destroyed by a killer cell, not killer contacts: kill() clears
    // every tile the victim owned, so no second killer can find it afterwards
    // and each increment is one body. Checked rather than assumed — re-running
    // three seeds for 16000 ticks with the increment guarded on the victim
    // still being alive gave identical counts. Starvation and old age are not
    // in here; those are deaths, not kills.
    [[nodiscard]] int killed() const { return kills_; }
    [[nodiscard]] int mover_cells() const { return movers_; }
    [[nodiscard]] int mean_cells() const { return meanCells(); }

private:
    struct Cell { int dx, dy; std::uint8_t type; };
    struct Organism {
        std::vector<Cell> cells;
        int   x = 0, y = 0;
        int   rot = 0;                 // 0..3, quarter turns
        float food = 0.0f;
        int   age = 0;
        bool  alive = false;
        bool  canMove = false;
        int   dir = 0;                 // which way it is heading, if it can
    };

    [[nodiscard]] float knob(const char* key) const {
        for (const auto& k : knobs_) if (k.key == key) return k.value;
        return 0.0f;
    }

    // Rotate a cell offset into world space.
    static void rotate(int rot, int dx, int dy, int& ox, int& oy) {
        switch (rot & 3) {
            case 0: ox =  dx; oy =  dy; break;
            case 1: ox = -dy; oy =  dx; break;
            case 2: ox = -dx; oy = -dy; break;
            default: ox =  dy; oy = -dx; break;
        }
    }

    [[nodiscard]] bool inside(int x, int y) const {
        return x >= 0 && y >= 0 && x < grid_.w && y < grid_.h;
    }

    // Can this anatomy sit here? Food may be overwritten — an organism growing
    // into food simply consumes the space — but nothing else may.
    [[nodiscard]] bool fits(const Organism& o, int x, int y, int rot, int ignoreId = -1) const {
        for (const auto& c : o.cells) {
            int ox, oy; rotate(rot, c.dx, c.dy, ox, oy);
            const int tx = x + ox, ty = y + oy;
            if (!inside(tx, ty)) return false;
            const std::size_t i = std::size_t(ty) * grid_.w + tx;
            const std::uint8_t t = grid_.cells[i];
            if (t == Wall) return false;
            if (t != Empty && t != Food && owner_[i] != ignoreId) return false;
        }
        return true;
    }

    // Writing an organism's cells into the grid. Food underneath is ABSORBED,
    // not overwritten.
    //
    // fits() lets a body move onto food — it has to, or a mover would be
    // fenced in by the very thing it is looking for. But stamping over it
    // simply deleted the food, so an organism with an eye drove onto every
    // scrap it could see and destroyed it before its own mouth could eat.
    // Measured against armour, which is the same cell count and the same
    // upkeep and steers nothing: armour reached 1179 alive, the eye reached
    // zero. The eye was not a weak organ, it was a food shredder.
    void stamp(const Organism& o, int id, float* absorbed = nullptr) {
        for (const auto& c : o.cells) {
            int ox, oy; rotate(o.rot, c.dx, c.dy, ox, oy);
            const std::size_t i = std::size_t(o.y + oy) * grid_.w + (o.x + ox);
            if (grid_.cells[i] == Food && absorbed) *absorbed += 1.0f;
            grid_.cells[i] = c.type;
            owner_[i] = id;
        }
    }
    void erase(const Organism& o, int id) {
        for (const auto& c : o.cells) {
            int ox, oy; rotate(o.rot, c.dx, c.dy, ox, oy);
            if (!inside(o.x + ox, o.y + oy)) continue;
            const std::size_t i = std::size_t(o.y + oy) * grid_.w + (o.x + ox);
            if (owner_[i] == id) { grid_.cells[i] = Empty; owner_[i] = -1; }
        }
    }

    bool place(Organism o) {
        if (o.cells.empty()) return false;
        if (!fits(o, o.x, o.y, o.rot)) return false;
        o.alive = true;
        o.canMove = false;
        for (const auto& c : o.cells) if (c.type == Mover) o.canMove = true;
        int id;
        if (!free_.empty()) { id = free_.back(); free_.pop_back(); orgs_[std::size_t(id)] = std::move(o); }
        else { id = int(orgs_.size()); orgs_.push_back(std::move(o)); }
        stamp(orgs_[std::size_t(id)], id, &orgs_[std::size_t(id)].food);
        live_.push_back(id);
        return true;
    }

    void kill(int id) {
        Organism& o = orgs_[std::size_t(id)];
        if (!o.alive) return;
        // A corpse becomes food. Otherwise every death is energy leaving the
        // world and the population can only ever shrink.
        for (const auto& c : o.cells) {
            int ox, oy; rotate(o.rot, c.dx, c.dy, ox, oy);
            if (!inside(o.x + ox, o.y + oy)) continue;
            const std::size_t i = std::size_t(o.y + oy) * grid_.w + (o.x + ox);
            if (owner_[i] == id) { grid_.cells[i] = Food; owner_[i] = -1; }
        }
        o.alive = false;
        // NOT free_.push_back(id) here.
        //
        // The id stays in live_ until the end-of-tick prune. Recycling the slot
        // before then let a reproduction later in the same tick hand it to a
        // new organism, which was then alive — so the prune kept BOTH entries
        // and the id appeared in live_ twice. Every duplicate acted twice a
        // tick and was counted twice, and the population reported more
        // organisms than the grid had room for. A count that exceeds the space
        // it lives in is the kind of impossible number worth checking for.
        // Slots are recycled during the prune.
    }

    void mutateChild(Organism& o) {
        const float rate = knob("mutate");
        if (rng_.unit() >= rate) return;
        const float roll = rng_.unit();
        if (roll < 0.4f || o.cells.size() < 2) {
            // Grow: a new cell adjacent to an existing one, so an anatomy stays
            // connected. A disconnected organism is two organisms sharing a
            // bank account, which is a different model and not this one.
            const Cell& base = o.cells[std::size_t(rng_.unit() * float(o.cells.size())) % o.cells.size()];
            static const int DX[4] = {1,-1,0,0}, DY[4] = {0,0,1,-1};
            const int d = int(rng_.unit() * 4.0f) & 3;
            Cell c{ base.dx + DX[d], base.dy + DY[d],
                    std::uint8_t(kFirstCell + int(rng_.unit() * float(kCellKinds)) % kCellKinds) };
            for (const auto& e : o.cells) if (e.dx == c.dx && e.dy == c.dy) return;
            o.cells.push_back(c);
        } else if (roll < 0.7f && o.cells.size() > 1) {
            o.cells.erase(o.cells.begin() +
                          std::ptrdiff_t(rng_.unit() * float(o.cells.size())) % std::ptrdiff_t(o.cells.size()));
        } else {
            Cell& c = o.cells[std::size_t(rng_.unit() * float(o.cells.size())) % o.cells.size()];
            c.type = std::uint8_t(kFirstCell + int(rng_.unit() * float(kCellKinds)) % kCellKinds);
        }
    }

    void reproduce(int id) {
        // Everything read from the parent is COPIED out first, and the food is
        // charged before place() is called.
        //
        // place() can push_back onto orgs_, which reallocates it and invalidates
        // any reference into it — so holding `Organism& p` across that call and
        // then writing p.food was a write to freed memory. It segfaulted, which
        // is the lucky outcome; the unlucky one is a population that quietly
        // reports wrong numbers.
        Organism child;
        {
            const Organism& p = orgs_[std::size_t(id)];
            child.cells = p.cells;
            child.x = p.x; child.y = p.y;
        }
        child.rot = int(rng_.unit() * 4.0f) & 3;
        mutateChild(child);
        if (child.cells.empty()) return;

        orgs_[std::size_t(id)].food -= knob("food");     // charged either way

        // Try a few nearby spots. Failing to find one is a real outcome — a
        // crowded neighbourhood suppresses reproduction, which is most of what
        // stops the population exploding.
        const int px = child.x, py = child.y;
        const int reach = 3 + int(child.cells.size());
        for (int attempt = 0; attempt < 8; ++attempt) {
            child.x = px + int(rng_.unit() * float(2 * reach + 1)) - reach;
            child.y = py + int(rng_.unit() * float(2 * reach + 1)) - reach;
            if (place(child)) { ++births_; break; }
        }
    }

    void tick() {
        ++tick_;
        const float upkeep  = knob("upkeep");
        const float prodRate= knob("produce");
        const float needed  = knob("food");
        const float lifePer = knob("life");

        // Iterate a snapshot: reproduction appends to live_, and a child born
        // this tick must not also act this tick.
        std::vector<int> acting = live_;
        movers_ = 0;

        for (int id : acting) {
            // Re-index rather than hold a reference: reproduce() below can grow
            // orgs_ and invalidate anything pointing into it.
            if (!orgs_[std::size_t(id)].alive) continue;
            Organism& o = orgs_[std::size_t(id)];
            ++o.age;

            for (const auto& c : o.cells) {
                int ox, oy; rotate(o.rot, c.dx, c.dy, ox, oy);
                const int cx = o.x + ox, cy = o.y + oy;
                if (c.type == Mover) ++movers_;

                if (c.type == Producer) {
                    if (rng_.unit() < prodRate) {
                        static const int DX[4] = {1,-1,0,0}, DY[4] = {0,0,1,-1};
                        const int d = int(rng_.unit() * 4.0f) & 3;
                        const int fx = cx + DX[d], fy = cy + DY[d];
                        if (inside(fx, fy)) {
                            const std::size_t i = std::size_t(fy) * grid_.w + fx;
                            if (grid_.cells[i] == Empty) grid_.cells[i] = Food;
                        }
                    }
                } else if (c.type == Mouth) {
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (!dx && !dy) continue;
                            const int fx = cx + dx, fy = cy + dy;
                            if (!inside(fx, fy)) continue;
                            const std::size_t i = std::size_t(fy) * grid_.w + fx;
                            if (grid_.cells[i] == Food) {
                                grid_.cells[i] = Empty;
                                o.food += 1.0f;
                            }
                        }
                } else if (c.type == Killer) {
                    static const int DX[4] = {1,-1,0,0}, DY[4] = {0,0,1,-1};
                    for (int d = 0; d < 4; ++d) {
                        const int fx = cx + DX[d], fy = cy + DY[d];
                        if (!inside(fx, fy)) continue;
                        const std::size_t i = std::size_t(fy) * grid_.w + fx;
                        const int other = owner_[i];
                        // Armour blocks. Without that, a killer beats every
                        // anatomy and there is no counter to evolve toward.
                        if (other >= 0 && other != id && grid_.cells[i] != Armour) {
                            kill(other); ++kills_;
                        }
                    }
                }
            }

            o.food -= upkeep * float(o.cells.size());

            // Movers wander, or steer if the anatomy has an eye.
            //
            // The eye was declared, palette-coloured, charged upkeep and given
            // a description — and did NOTHING. Nothing in tick() read it. That
            // is a cell type that exists only in the documentation.
            if (o.canMove) {
                int eyes = 0;
                for (const auto& c : o.cells) if (c.type == Eye) ++eyes;
                if (eyes > 0) {
                    // Look along each axis for the nearest food. Range grows
                    // with the number of eyes, so a second eye is worth
                    // something and not just upkeep.
                    static const int DX[4] = {1,-1,0,0}, DY[4] = {0,0,1,-1};
                    const int range = 3 + 3 * std::min(eyes, 4);
                    int bestDir = -1, bestDist = range + 1;
                    for (int d = 0; d < 4; ++d)
                        for (int r = 1; r <= range; ++r) {
                            const int sx = o.x + DX[d] * r, sy = o.y + DY[d] * r;
                            if (!inside(sx, sy)) break;
                            const std::size_t i = std::size_t(sy) * grid_.w + sx;
                            if (grid_.cells[i] == Wall) break;
                            if (grid_.cells[i] == Food && r < bestDist) { bestDist = r; bestDir = d; }
                        }
                    // Only steer somewhere it can actually go.
                    //
                    // Without this check the eye is a deadlock: it points at
                    // the nearest food, the step is blocked by whoever is
                    // already eating it, the blocked-move handler re-rolls the
                    // direction, and the eye points straight back at the same
                    // obstruction on the next tick. Measured in a controlled
                    // world of forty seeded organisms: mouth+mover reached 1249
                    // alive, and mouth+mover+eye died out entirely at 19 births.
                    // An organ that kills its owner is worse than one that does
                    // nothing, which is what it replaced.
                    if (bestDir >= 0 &&
                        fits(o, o.x + DX[bestDir], o.y + DY[bestDir], o.rot, id))
                        o.dir = bestDir;
                    else if (rng_.unit() < 0.25f) o.dir = int(rng_.unit() * 4.0f) & 3;
                } else if (rng_.unit() < 0.08f) o.dir = int(rng_.unit() * 4.0f) & 3;
                if (rng_.unit() < 0.35f) {
                    static const int DX[4] = {1,-1,0,0}, DY[4] = {0,0,1,-1};
                    const int nx = o.x + DX[o.dir], ny = o.y + DY[o.dir];
                    if (fits(o, nx, ny, o.rot, id)) {
                        erase(o, id);
                        o.x = nx; o.y = ny;
                        stamp(o, id, &o.food);      // eat what it moved onto
                    } else o.dir = int(rng_.unit() * 4.0f) & 3;
                }
            }

            if (o.food < 0.0f || o.age > int(lifePer * float(o.cells.size()))) { kill(id); continue; }
            // The reference `o` must not be used after this call.
            if (o.food >= needed) reproduce(id);
        }

        live_.erase(std::remove_if(live_.begin(), live_.end(),
                                   [&](int id) {
                                       if (orgs_[std::size_t(id)].alive) return false;
                                       free_.push_back(id);     // safe now: it is leaving live_
                                       return true;
                                   }),
                    live_.end());
    }

    [[nodiscard]] int meanCells() const {
        if (live_.empty()) return 0;
        std::size_t n = 0;
        for (int id : live_) n += orgs_[std::size_t(id)].cells.size();
        return int(n / live_.size());
    }

    void publish() {}      // the grid IS the state; nothing to derive

    Provenance          about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    Field               grid_;
    std::vector<int>    owner_;        // which organism owns each cell, or -1
    std::vector<Organism> orgs_;
    std::vector<int>    live_, free_;
    Rng                 rng_{0x11FEull};
    std::uint64_t       tick_ = 0;
    int births_ = 0, kills_ = 0, movers_ = 0, turnover_ = 0;
};

inline SimPtr make_lifeengine() { return std::make_unique<LifeEngine>(); }

} // namespace bench
