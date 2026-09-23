// my_sim.hpp — COPY THIS FILE to start a new simulation.
//
// This is a complete, working, registered sim in about forty lines of actual
// content. Everything you are not writing here — double buffering, toroidal
// wrapping, neighbour counting, the palette contract, the generation counter,
// knobs, click-to-poke — comes from GridSim in toolkit.hpp.
//
// The rule below is "Anneal" (also called Twisted Majority): a real, documented
// life-like rule, chosen because it does something visibly different from Life
// and takes one line to express. Replace it with yours.
//
// ── HOW TO ADD YOUR OWN ─────────────────────────────────────────────────────
//   1. cp my_sim.hpp sims/your_rule.hpp
//   2. rename the class and the make_ function
//   3. write rule()
//   4. add one line to registry.hpp
//   5. cmake --build build && ctest      (the bench picks it up automatically)
//
// ── IF THE SCAFFOLDING IS IN YOUR WAY ───────────────────────────────────────
// Don't use it. Inherit from `Sim` directly and implement the six methods
// yourself — that is exactly what von Neumann's 29-state rule does, because a
// rule with directional inputs and a construction tree does not fit a
// neighbour count. The scaffolding is a convenience, never a requirement.

#pragma once
#include "../toolkit.hpp"

namespace bench {

class MySim final : public GridSim {
public:
    explicit MySim(int size = 256)
        : GridSim(
            Provenance{
                "Anneal", "—",
                "Gerard Vichniac; discussed by Toffoli & Margolus, Cellular Automata Machines, 1987",
                "Outer-totalistic rule B4678/S35678, sometimes called Twisted Majority",
                Replication::No,
                "No. It is a smoothing rule: regions coarsen and boundaries straighten. Nothing "
                "in it copies anything - it is here as an example of how little code a rule needs.",
                "A cell takes the majority of its 9-cell neighbourhood, with the vote at 4 and 5 "
                "deliberately swapped. That single inversion is what stops it settling instantly "
                "and makes the domain walls creep and anneal."
            },
            std::vector<Swatch>{{{8,11,14},"off"},{{155,122,230},"on"}},
            size, size)
    {
        // Knobs appear as sliders in the bench automatically.
        //
        // The trailing `true` is on_reset, and it matters: this value is only
        // read by reset(), so without it the slider moves and nothing happens
        // until you reset by hand. The self-test drives every knob across its
        // range and fails any that changes nothing — which is exactly how this
        // one was caught. If your knob is read by rule(), leave it false.
        add_knob({"density", "reset density", 0.05f, 0.95f, 0.50f, 0.01f, {}, true});
        // A size knob costs four lines and you get the threading for free:
        // GridSim::step() splits the sweep by row once the world is past
        // 250,000 cells, so 512 and up run on every core. The one thing you
        // must not do is draw from rng() inside rule() — that is the single
        // thing that would make the answer depend on how the rows were split.
        // Derive the default from the size actually built, never type it in.
        // A literal here read "512" over a 256-wide field until it was
        // measured — the knob was right about what it would DO and wrong about
        // where it started, which is the hardest kind of wrong to notice.
        int idx = 0;
        for (int i = 1; i < 4; ++i)
            if (std::abs((256 << i) - size) < std::abs((256 << idx) - size)) idx = i;
        add_knob({"size", "world size", 0.f, 3.f, float(idx), 1.f,
                  {"256", "512", "1024", "2048"}, true,
                  "Cells across, squared. Copy this knob into your own sim."});
        reset();
    }

    // Note where this lives. reset() only refills the cells the field already
    // has, so resizing has to happen when the knob moves, not when the sim
    // resets — put it in reset() and the slider does nothing at all, which
    // looks exactly like a working knob until you check the cell count.
    void on_knob(const std::string& k, float v) override {
        GridSim::on_knob(k, v);
        if (k == "size") {
            int i = int(v + 0.5f);
            i = i < 0 ? 0 : (i > 3 ? 3 : i);
            const int n = 256 << i;
            if (n != cur_.w) { cur_ = Field(n, n); nxt_ = Field(n, n); }
        }
    }

    // ── the rule, and nothing else ──────────────────────────────────────────
    std::uint8_t rule(int x, int y) override {
        const int n = moore8(read(), x, y, 1) + (read().at(x, y) ? 1 : 0);   // 0..9
        // majority, with 4 and 5 swapped — that swap is the whole rule
        return std::uint8_t((n == 4 || n >= 6) ? 1 : 0);
    }

    // Overriding reset() takes over EVERYTHING GridSim::reset() did, and it did
    // two things: clear the field and zero the generation counter. Drop the
    // second and the panel goes on printing the old "gen N" over a brand-new
    // soup, and the RLE export stamps that same wrong number into the filename
    // and the header. The reseed is the same kind of promise: the density
    // slider re-seeds on release, so without it the same density gives a
    // different soup every time and nothing here is reproducible. 0x5EED is
    // GridSim's own starting seed, so the first field is the one this template
    // has always shown.
    void reset() override {
        rng().reseed(0x5EEDull);
        gen_ = 0;
        cur_.fill(0);
        const float d = knob("density");
        for (auto& c : cur_.cells) c = (rng().unit() < d) ? 1 : 0;
    }
};

inline SimPtr make_my_sim(int size = 256) { return std::make_unique<MySim>(size); }

} // namespace bench
