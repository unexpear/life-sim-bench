// small_lattice.hpp — three short lattice rules that each make one point.
//
//   Brian's Brain  a medium where everything moves and nothing persists
//   Wireworld      a medium for building computers by hand
//   Langton's Ant  two rules, one agent, and a result nobody can derive

#pragma once
#include "../sim.hpp"
#include "../rng.hpp"
#include "../parallel.hpp"

namespace bench {

// ── world size choices ──────────────────────────────────────────────────────
//
// Doublings FROM THE SIZE THE SIM WAS BUILT WITH, so choice 0 always is the
// shipped world and the knob cannot quietly change a published default. The
// self-test builds these sims at 32, 40x20, 64 and 512; a hardcoded list would
// have offered those instances sizes they were never meant to have.
//
// A choice list rather than a slider, for the same reason life_like uses one:
// what makes the jump from a quarter-million cells to sixteen million legible
// is the naming of the steps. A continuous slider spends most of its travel in
// sizes nobody wants.
inline std::vector<std::string> size_choices(int base, int n) {
    std::vector<std::string> out;
    for (int i = 0; i < n; ++i) out.push_back(std::to_string(base << i));
    return out;
}
inline std::vector<std::string> size_choices_wh(int w, int h, int n) {
    std::vector<std::string> out;
    for (int i = 0; i < n; ++i)
        out.push_back(std::to_string(w << i) + "x" + std::to_string(h << i));
    return out;
}

// ── Brian's Brain ───────────────────────────────────────────────────────────
// 0 dead, 1 firing, 2 refractory. The refractory state is the whole trick: it
// stops a wave re-igniting the cells behind it, so everything propagates
// outward and nothing sits still.
class BriansBrain final : public Sim {
public:
    explicit BriansBrain(int size = 256) : base_(size), cur_(size, size), nxt_(size, size) {
        about_ = Provenance{
            "Brian's Brain", "1980s", "Brian Silverman",
            "Rule catalogued widely; Silverman also authored Wireworld",
            Replication::No,
            "No. It is a medium for travelling waves, not for organisms. But almost everything in "
            "it moves, which Life mostly does not.",
            "Three states. A dead cell with exactly two firing neighbours fires. A firing cell "
            "always becomes refractory. A refractory cell always dies. Nothing ever stays still."
        };
        pal_ = {{{8,11,14},"dead"},{{255,255,255},"firing"},{{43,111,119},"refractory"}};
        knobs_ = {
            // Measured before the knob existed, one thread, machine otherwise
            // quiet, and BEFORE the row-pointer rewrite that made the sweep
            // ~3.4x faster serially: 0.83 ms a step at 256 squared, 3.4 at 512, 14.4 at 1024,
            // 54 at 2048, 211 at 4096. Dead linear at about 79,000 cells per
            // millisecond — the same cost per cell as Conway's Life next door,
            // which is why the same list of sizes is the right one. The sweep
            // is threaded above a quarter-million cells and 4096 squared lands
            // near 50 ms; see step().
            {"size", "world size", 0.f, 4.f, 0.f, 1.f, size_choices(size, 5), true,
             "Cells across, squared, doubling from the size this rule shipped at. From the "
             "shipped 256 the top of the list is 4096 — sixteen million cells and about 50 ms a "
             "step with the sweep threaded, a size to look at rather than to run fast. Brian's "
             "Brain is all propagation, so a bigger field is the only place a wave has room to "
             "run without meeting its own tail through the wrap."},
            {"density", "initial firing density", 0.0f, 0.6f, 0.12f, 0.01f, {}, true,
             "Fraction of cells firing when the field is seeded. Starting condition, "
             "not rule."}};
        // "Exactly two firing neighbours" is the entire rule. Making it a set
        // of bits rather than a constant is the difference between a demo and
        // an instrument: turn 1 on as well and the waves stop being thin.
        for (int n = 0; n <= 8; ++n) {
            const std::string N = std::to_string(n);
            switches_.emplace_back("F" + N, N, "fires on", n == 2,
                "A DEAD cell starts firing when exactly " + N + " of its neighbours are firing."
                + (n == 2 ? "  Exactly two is the published rule, and it keeps waves thin by "
                            "trimming them at the FRONT: on a five-cell firing front the three "
                            "dead cells ahead of its middle see three firing neighbours and stay "
                            "dead, while the two ahead of its ends see exactly two and fire. What "
                            "stops the wave re-igniting the cells behind it is the refractory "
                            "state, not this clause — those cells are never counted at all." : ""));
        }
        fire_ = 1u << 2;
        reset();
    }
    const Provenance& about() const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field& field() const override { return cur_; }
    std::uint64_t generation() const override { return gen_; }

    std::vector<Knob>& knobs() override { return knobs_; }
    std::vector<Switch>& switches() override { return switches_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        if (k == "density") density_ = v;
        if (k == "size") {
            // Rebuild BOTH buffers here. reset() only refills the cells it
            // already has, so a size knob that leaves the buffers alone is a
            // no-op that looks like it worked — the slider moves, the label
            // changes, and field().w never budges.
            const int n = base_ << std::clamp(int(v + 0.5f), 0, 4);
            if (n != cur_.w) { cur_ = Field(n, n); nxt_ = Field(n, n); reset(); }
        }
    }
    void on_switch(const std::string& key, bool v) override {
        fire_ = 0;
        for (auto& sw : switches_) {
            if (sw.key == key) sw.value = v;
            if (sw.value) fire_ = std::uint16_t(fire_ | (1u << (sw.label[0] - '0')));
        }
    }
    void reset() override {
        rng_.reseed(0xB2A1u); gen_ = 0;
        for (auto& c : cur_.cells) c = (rng_.unit() < density_) ? 1 : 0;
    }
    // Below this many cells the sweep stays serial. The same threshold the rest
    // of the bench uses, and checked rather than inherited: running this body
    // with a forced worker count, 16 threads against 1 is 0.13x at 128 squared,
    // 0.64x at 256 squared and 0.93x at 384 squared — all losses — and turns
    // into a 2.2x win at 512 squared. The crossover really is a quarter of a
    // million cells for this rule.
    static constexpr std::size_t kParallelCells = 250000;

    void step() override {
        // Row-parallel above the threshold. Double buffered, so the split
        // cannot be observed: every read comes from cur_ and each row writes
        // only its own slice of nxt_. Nothing here touches rng_ — the seeding
        // is in reset(), which is serial — so there is no shared mutable state
        // for a thread to race on. Verified, not argued: 60 steps at 512 and
        // 1024 squared, 24 at 2048 and 6 at 4096 give byte-identical fields on
        // 1 worker and on 16.
        //
        // 16 workers against 1, re-measured on an idle machine AFTER the row
        // pointers below went in — the earlier figures here (512 squared 5.2 ms
        // -> 1.7, up to 4096 squared 441 -> 51) were taken on the slower sweep
        // while nine sibling benchmarks were running, and were wrong in both
        // terms: 512 squared 1.05 -> 0.58, 1024 squared 4.38 -> 1.38, 2048
        // squared 18.2 -> 3.39, 4096 squared 73.1 -> 10.2.
        //
        // Raw row pointers rather than at()/wrap()/set(). Same reason the
        // Wireworld sweep below uses them, and this one was left behind when
        // that was fixed: inside the lambda the compiler cannot prove that the
        // byte written through nxt_ does not alias the Field's own w/h/data
        // members, so it reloads them on every cell. Hoisting the three row
        // pointers is worth 2.8-3.5x on one thread, and the fields it produces
        // are byte-identical - which is the only reason it is allowed to be
        // here at all.
        const int w = cur_.w, h = cur_.h;
        const std::size_t cells = std::size_t(w) * std::size_t(h);
        const std::uint8_t* src = cur_.cells.data();
        std::uint8_t* dst = nxt_.cells.data();
        const std::uint16_t fire = fire_;
        parallel_for(std::size_t(h), [&](std::size_t row) {
            const int y = int(row);
            // Wrap the three rows once, not once per cell.
            const std::uint8_t* r0 = src + std::size_t((y - 1 + h) % h) * w;
            const std::uint8_t* r1 = src + std::size_t(y) * w;
            const std::uint8_t* r2 = src + std::size_t((y + 1) % h) * w;
            std::uint8_t* out = dst + std::size_t(y) * w;
            for (int x = 0; x < w; ++x) {
                const std::uint8_t s = r1[x];
                if (s == 1) { out[x] = 2; continue; }
                if (s == 2) { out[x] = 0; continue; }
                const int xm = (x - 1 + w) % w, xp = (x + 1) % w;
                const int n = (r0[xm] == 1) + (r0[x] == 1) + (r0[xp] == 1)
                            + (r1[xm] == 1)                + (r1[xp] == 1)
                            + (r2[xm] == 1) + (r2[x] == 1) + (r2[xp] == 1);
                out[x] = std::uint8_t(((fire >> n) & 1) ? 1 : 0);
            }
        }, cells >= kParallelCells ? 0u : 1u);
        cur_.cells.swap(nxt_.cells); ++gen_;
    }
    void clear() { cur_.fill(0); gen_ = 0; }
    void put(int x, int y, std::uint8_t v) { cur_.set(x, y, v); }
    Field* editable() override { return &cur_; }
    [[nodiscard]] std::uint8_t paint_value() const override { return 1; }  // firing
private:
    Provenance about_; std::vector<Swatch> pal_; std::vector<Knob> knobs_;
    std::vector<Switch> switches_;
    std::uint16_t fire_ = 1u << 2;
    float density_ = 0.12f;
    int base_ = 256;                    // the size this instance was built at
    Field cur_, nxt_; Rng rng_{0xB2A1u}; std::uint64_t gen_ = 0;
};

// ── Wireworld ───────────────────────────────────────────────────────────────
// 0 empty, 1 electron head, 2 electron tail, 3 conductor.
class Wireworld final : public Sim {
public:
    Wireworld(int w = 200, int h = 120) : baseW_(w), baseH_(h), cur_(w, h), nxt_(w, h) {
        about_ = Provenance{
            "Wireworld", "1987", "Brian Silverman",
            "A fully functional computer was constructed in it by Mark Owen and others, 2002",
            Replication::No,
            "No, and it does not try. It is a medium for building circuits by hand - which is a "
            "different ambition from growing something.",
            "Four states. Electron head becomes tail; tail becomes conductor; conductor becomes a "
            "head if exactly one or two neighbours are heads."
        };
        pal_ = {{{8,11,14},"empty"},{{90,209,196},"electron head"},
                {{43,111,119},"electron tail"},{{242,193,78},"conductor"}};
        // A ring clock's period is its cell count MINUS FOUR, not its perimeter:
        // the electron moves one cell per tick but the Moore neighbourhood lets
        // it cut each of the four corners diagonally, so a w x h ring runs at
        // 2(w+h)-8. Measured on the shipped circuit by timing successive head
        // arrivals at one ring cell: 24x14 -> 68, 25x14 -> 70, 40x14 -> 100,
        // 6x18 -> 40. The 36x18 ring gives 100 only with ring A silenced: on the
        // shipped circuit ring B runs at 100 for six cycles and then at 68 —
        // ring A's period — from t=686 onward, because ring A's pulses drive it
        // back up tap B.
        //
        // Both ring heights are fixed (14 and 18), so both periods are even and
        // the two can never be made coprime. Over 38 of clockB's 55 settings the
        // knob chooses nothing about ring B's period at all: anything whose
        // natural period would exceed ring A's 68 is captured and runs at 68.
        // The divides/coprime framing only applies to clockB 6..22.
        knobs_ = {
            // Board size, not circuit size. This is the one sim here whose
            // point is that you build in it by hand, and the shipped circuit
            // occupies x 12..190, y 16..70 of a 200x120 board — there is
            // nowhere to put a second circuit next to it. Bigger boards are
            // blank space to build in, and they cost almost nothing: the sweep
            // leaves on the first byte for an empty cell, so measured serially
            // on a quiet machine a step is 0.018 ms at 200x120, 0.77 at
            // 1600x960 and 3.5 at 3200x1920 — six million cells for a fifth of
            // what Brian's Brain pays for a quarter of a million.
            //
            // The demo circuit does NOT move or scale with the board. That is
            // deliberate: it is drawn at fixed coordinates on a field that does
            // not wrap, so every measured claim in these comments — the ring
            // periods, the capture of ring B at 68, the head counts at (180,42)
            // — is a statement about the same geometry at every board size.
            // Verified by stepping 1200 ticks at each size and comparing the
            // 200x120 corner cell for cell: identical.
            {"size", "board size", 0.f, 4.f, 0.f, 1.f, size_choices_wh(w, h, 5), true,
             "How much board there is to build on. The demo circuit is drawn at fixed "
             "coordinates and behaves identically at every size — what changes is how much "
             "empty space surrounds it. The field does not wrap, so the edge is a wall: a wire "
             "run off the board simply stops."},
            {"clockA", "clock A width (period = 2w+20)", 6.f, 40.f, 24.f, 1.f, {}, true,
             "Width of the first clock ring, which is 14 cells tall. One electron runs the ring at "
             "one cell per tick but cuts each of the four corners, so the period is the ring's cell "
             "count minus four — 2w+20 here: 68 ticks at the default width of 24, 100 at 40."},
            {"clockB", "clock B width (period = 2w+28 in isolation)", 6.f, 60.f, 36.f, 1.f, {}, true,
             "Width of the second clock ring, 18 cells tall. Its period in isolation is 2w+28, but "
             "on the shipped circuit that holds only for widths 6..22: anything whose natural "
             "period would exceed ring A's 68 is captured and runs at 68 instead, including the "
             "default 36. Both ring heights are fixed, so both periods are even and coprime is "
             "unreachable; within 6..22 what you can choose is whether one period divides the "
             "other — make one period a multiple of the other and they stay in step. What the "
             "shared output line does with the two taps is a messier question — see the note on "
             "the merge in reset()."},
        };
        // What "one or two neighbouring heads" does to WIDE conductor, measured
        // on a 3-wide bus (rows y=9..11, conductor x=20..90, a full-width head
        // column at x=50 with a tail column at 49): at t=1 only the two edge
        // rows fire, at x=51, and the middle stays conductor — but at t=2 all
        // three rows are heads at x=52, and from t=3 the pulse is in fragments
        // running both ways, with heads back at the launch column at t=3 and at
        // x=45 by t=8. It does NOT hold the signal to the bus edges; it shreds
        // it. Switch H3 on and the same bus carries one solid 3-cell head
        // column, still solid at t=16.
        //
        // No switch here is what stops a pulse running backwards on a 1-WIDE
        // wire: with a head at x=50 and a tail at 49 on a wire spanning x=20..90,
        // heads stay within 50..90 over 60 ticks, and turning every one of
        // H1..H8 on leaves that unchanged. The tail is what does it — the cell
        // the pulse just left is not conductor, so it cannot fire.
        for (int n = 1; n <= 8; ++n) {
            const std::string N = std::to_string(n);
            switches_.emplace_back("H" + N, N, "conductor fires on", n == 1 || n == 2,
                "A conductor becomes an electron head when exactly " + N + " neighbouring cells "
                "are heads." + ((n == 1 || n == 2)
                    ? "  One or two is the published rule."
                    : (n == 3 ? "  Switch this on and a 3-wide bus carries one solid head column "
                                "down its full width."
                              : "  Switch this on and conductor ignites more easily.")));
        }
        heads_ = (1u << 1) | (1u << 2);
        reset();
    }
    const Provenance& about() const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field& field() const override { return cur_; }
    std::uint64_t generation() const override { return gen_; }

    void reset() override {
        cur_.fill(0); gen_ = 0;
        // Two clock rings of different period, each tapping a line that runs to
        // a shared output. This comment used to call that merge "an OR gate you
        // get for free", and the circuit does not demonstrate one: the shared
        // line conducts both ways, so ring A's pulses run back up tap B, and at
        // the default widths ring B adds nothing to the output. Measured over
        // 1200 ticks by erasing one ring's electron at reset and counting heads
        // at (180,42): both rings 15, ring A alone 15, ring B alone 11.
        auto ring = [&](int x0, int y0, int w, int h) {
            std::vector<std::pair<int,int>> p;
            for (int x = x0; x < x0 + w; ++x)        p.push_back({x, y0});
            for (int y = y0 + 1; y < y0 + h; ++y)    p.push_back({x0 + w - 1, y});
            for (int x = x0 + w - 2; x >= x0; --x)   p.push_back({x, y0 + h - 1});
            for (int y = y0 + h - 2; y > y0; --y)    p.push_back({x0, y});
            for (auto& [x, y] : p) if (in(x, y)) cur_.set(x, y, 3);
            return p;
        };
        auto hline = [&](int a, int b, int y) { for (int x = a; x <= b; ++x) if (in(x,y)) cur_.set(x, y, 3); };
        auto vline = [&](int a, int b, int x) { for (int y = a; y <= b; ++y) if (in(x,y)) cur_.set(x, y, 3); };
        const int wA = std::max(6, int(knob("clockA") + 0.5f));
        const int wB = std::max(6, int(knob("clockB") + 0.5f));
        auto A = ring(12, 16, wA, 14);
        auto B = ring(12, 52, wB, 18);
        // Each tap starts one cell past its OWN ring's right column, so the
        // junction is the same piece of geometry at every width. It used to
        // start at a fixed x=36, which is right only at clockA 24 and 25: at 26
        // and above the ring outgrew the tap, the wire ran through the ring's
        // interior, and the junction there fans out to three heads, which the
        // 1-or-2 rule blocks — measured by counting electrons left in ring A
        // after 800 ticks, it was 0 for every clockA from 26 to 40, so half the
        // slider stopped the clock rather than retuning it.
        hline(12 + wA, 120, 23); hline(12 + wB, 120, 61);
        vline(23, 61, 120); hline(120, 190, 42);
        // Guarded: the demo circuit is laid out for a full-size field, and on a
        // small one (the self-test uses 40x20) parts of it fall outside. Writing
        // an electron through an unguarded set() there is an out-of-bounds write.
        auto launch = [&](std::vector<std::pair<int,int>>& p) {
            if (p.size() < 4) return;
            if (!in(p[3].first, p[3].second) || !in(p[2].first, p[2].second)) return;
            cur_.set(p[3].first, p[3].second, 1);   // head
            cur_.set(p[2].first, p[2].second, 2);   // tail behind it fixes direction
        };
        launch(A); launch(B);
    }
    // Three million, not the quarter-million the rest of the bench uses.
    //
    // A threshold copied from another sim is a guess. This sweep leaves on the
    // first byte for an empty cell, so it costs roughly a fifteenth of Brian's
    // Brain per cell and the thread pool takes correspondingly longer to pay
    // for itself. Measured directly, 16 workers against 1, by running this same
    // body with a forced worker count: 384k cells is a heavy loss, 1.5M is
    // 2.17 ms against 2.84 (still a loss), 3.07M is 3.65 against 2.22 (1.65x),
    // 6.1M is 7.33 against 3.91 (1.87x). The crossover sits between 1.5M and
    // 3M, so only the largest board on the list threads.
    static constexpr std::size_t kParallelCells = 3000000;

    void step() override {
        // The copy is the whole board and stays serial — it is a memcpy, and
        // splitting it buys nothing. Only the sweep is threaded, above the
        // threshold. Rows are independent by construction: every read is from
        // src, every write goes to this row's slice of dst, and no state
        // outside the row is touched, so 1 worker and 16 give the same board.
        // Checked rather than argued: 200 steps at 1600x960 and at 3200x1920
        // give byte-identical fields on 1 worker and on 16.
        nxt_.cells = cur_.cells;
        // Raw row pointers, because this body is cheap enough for the access
        // path to be most of it. Reading through cur_.at()/nxt_.set() inside
        // the sweep lambda costs 35% here — the compiler cannot prove the byte
        // written through nxt_ does not alias the field's own w/h/data
        // members, so it reloads them per cell. Measured across four board
        // sizes against the original nested loop, and against the same lambda
        // written with at()/set(): 1600x960 is 1.72 ms the old way, 2.35 with
        // at()/set() in the lambda, 1.68 like this. Same board out of all
        // three, checked cell for cell over 300 steps.
        const int w = cur_.w, h = cur_.h;
        const std::uint8_t* src = cur_.cells.data();
        std::uint8_t*       dst = nxt_.cells.data();
        const std::size_t cells = std::size_t(w) * std::size_t(h);
        parallel_for(std::size_t(h), [&](std::size_t row) {
            const int y = int(row);
            const std::size_t base = std::size_t(y) * std::size_t(w);
            for (int x = 0; x < w; ++x) {
                const std::uint8_t s = src[base + std::size_t(x)];
                if (s == 0) continue;
                if (s == 1) { dst[base + std::size_t(x)] = 2; continue; }
                if (s == 2) { dst[base + std::size_t(x)] = 3; continue; }
                // The field does not wrap here — the edge of the board is a
                // wall, not a seam — so an off-board neighbour is simply not
                // counted. Hoisting the row test out of the inner loop is the
                // same condition as the old in(xx, yy), one level up.
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    const int yy = y + dy;
                    if (yy < 0 || yy >= h) continue;
                    const std::size_t nb = std::size_t(yy) * std::size_t(w);
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy) continue;
                        const int xx = x + dx;
                        if (xx >= 0 && xx < w && src[nb + std::size_t(xx)] == 1) ++n;
                    }
                }
                dst[base + std::size_t(x)] = ((heads_ >> n) & 1) ? 1 : 3;
            }
        }, cells >= kParallelCells ? 0u : 1u);
        cur_.cells.swap(nxt_.cells); ++gen_;
    }
    std::vector<Knob>& knobs() override { return knobs_; }
    std::vector<Switch>& switches() override { return switches_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        if (k == "size") {
            // Both buffers, or the knob is a no-op: reset() redraws the circuit
            // into whatever board already exists and would happily report a
            // changed label over an unchanged 200x120 field.
            const int i = std::clamp(int(v + 0.5f), 0, 4);
            const int w = baseW_ << i, h = baseH_ << i;
            if (w != cur_.w || h != cur_.h) {
                cur_ = Field(w, h); nxt_ = Field(w, h); reset();
            }
        }
    }
    void on_switch(const std::string& key, bool v) override {
        heads_ = 0;
        for (auto& sw : switches_) {
            if (sw.key == key) sw.value = v;
            if (sw.value) heads_ = std::uint16_t(heads_ | (1u << (sw.label[0] - '0')));
        }
    }
    void clear() { cur_.fill(0); gen_ = 0; }
    void put(int x, int y, std::uint8_t v) { if (in(x,y)) cur_.set(x, y, v); }
    [[nodiscard]] std::uint8_t get(int x, int y) const { return cur_.at(x, y); }
    Field* editable() override { return &cur_; }
    // Painting wire is what you actually want here; drop an electron head with
    // the value picker if you want to fire it.
    [[nodiscard]] std::uint8_t paint_value() const override { return 3; }  // conductor
private:
    [[nodiscard]] bool in(int x, int y) const { return x >= 0 && y >= 0 && x < cur_.w && y < cur_.h; }
    [[nodiscard]] float knob(const char* key) const {
        for (auto& kn : knobs_) if (kn.key == key) return kn.value;
        return 0.f;
    }
    Provenance about_; std::vector<Swatch> pal_; std::vector<Knob> knobs_;
    std::vector<Switch> switches_;
    std::uint16_t heads_ = (1u << 1) | (1u << 2);
    int baseW_ = 200, baseH_ = 120;     // the board this instance was built at
    Field cur_, nxt_; std::uint64_t gen_ = 0;
};

// ── Langton's Ant ───────────────────────────────────────────────────────────
class LangtonAnt final : public Sim {
public:
    explicit LangtonAnt(int size = 320) : base_(size), cur_(size, size) {
        about_ = Provenance{
            "Langton's Ant", "1986", "Christopher Langton",
            "Langton, C. G. \"Studying artificial life with cellular automata\", Physica D 22, 1986",
            Replication::No,
            "No. One agent, two rules, no reproduction of any kind. It is on the bench for what it "
            "proves about prediction, not about life.",
            "On a white square: turn right, flip the square, step forward. On black: turn left, "
            "flip, step forward. For about ten thousand steps it produces apparent chaos, then "
            "with no warning builds a regular diagonal highway and runs off forever. Nobody has "
            "proved it must - the highway is observed, not derived."
        };
        // "RL" IS Langton's ant. The generalisation to longer turn strings is
        // the standard one (Turk's ants / generalised Langton ants): at a cell
        // of colour c, turn as the c'th letter says, advance the colour by one
        // modulo the string length, then step forward. Each string is a
        // different ant of that family, not a knob I made up — LRRRRRLLR is the
        // one that fills a SQUARE rather than building a highway. Measured on a
        // 1200-cell field, which is wide enough that the ant never reaches the
        // border, by recording every cell it visits: after 3,000,000 steps the
        // visited set is an 852x852 box, filled to 0.99999 of it, with left-right
        // and up-down mirror agreement 1.000. The shipped default field is 320
        // cells, on which the same run visits every cell, so the box is not
        // visible there. (An earlier comment here called that shape a
        // cardioid. It is not one: a cardioid has a single axis of symmetry, a
        // cusp, and fills well under all of its bounding box.)
        knobs_ = {
            {"turns", "turn rule", 0.f, 5.f, 0.f, 1.f,
             {"RL", "RLR", "LLRR", "RRLL", "LRRRRRLLR", "RRLRLLL"}, true,
             "The ant's whole rule. At a cell of colour c it turns as the c'th letter says, "
             "advances that cell's colour by one, then steps forward. RL is Langton's original: "
             "ten thousand steps of apparent chaos, then a highway nobody has proved must appear."},
            // NOT marked display_only, though it is a steps-per-tick knob. That
            // flag makes the workbench tell the user "the self-test asserts
            // exactly that", and the self-test only asserts it for a sim that
            // publishes epochs (self_test.cpp, the display_only branch of the
            // dead-knob test) — this one publishes none, so the flag would put a
            // claim on screen that nothing checks, and would drop the knob from
            // the dead-knob assertion it passes. Measured at the four values
            // that test samples — 1, 86, 171 and 256 — the field after 24 ticks
            // differs at all four.
            {"speed", "ant steps per tick", 1.f, 256.f, 32.f, 1.f, {}, false,
             "How many ant moves happen per simulation tick. The generation counter still advances "
             "once per move, so the number on screen stays honest."},
            // The one knob here whose step cost does not move at all: the ant
            // is a single agent, so a tick is `speed` moves whether the field
            // is 320 cells across or 2560. Measured at 0.0007 ms a tick at
            // every size on the list. What a bigger field costs is memory and
            // one fill at reset — 6.2 MB and 0.19 ms at 2560, against 0.1 MB
            // and 0.002 ms at 320.
            //
            // What it BUYS is steps before the wrap, and the numbers are not
            // what a doubling suggests, because the ant travels on a diagonal
            // highway: with RL it first touches the border at 17,375 steps on
            // 320, 25,695 on 640, 42,335 on 1280 and 75,615 on 2560. Doubling
            // the world buys well under double the run.
            //
            // It also makes a documented result visible that the shipped size
            // cannot show. LRRRRRLLR fills a SQUARE, 852x852 filled to 0.99999
            // of its box after 3,000,000 steps — reproduced here at both 1280
            // and 2560. On 320 and 640 that ant is at the border by 462,085 and
            // 1,687,893 steps, long before the box is finished, so what you see
            // there is the wrap and not the shape.
            //
            // The ceiling is 2560 because the picture, not the rule, is what
            // gets expensive: 6.5 million cells is already more than the
            // timeline can keep many snapshots of, and at 2560 the ant has not
            // reached the border after twenty million steps.
            {"size", "world size", 0.f, 3.f, 0.f, 1.f, size_choices(size, 4), true,
             "Cells across, squared. One agent makes one move a step whatever the size, so this "
             "costs nothing but memory. What it buys is room before the ant reaches the border "
             "and wraps: with the default RL rule that is 17,375 steps at 320 and 75,615 at "
             "2560, and the 852-cell square that LRRRRRLLR fills is only intact from 1280 up."},
        };
        setTurns("RL");
        reset();
    }
    const Provenance& about() const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field& field() const override { return cur_; }
    std::uint64_t generation() const override { return gen_; }

    std::vector<Knob>& knobs() override { return knobs_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        if (k == "turns") {
            // By key, not by index. The turn rule used to be knobs_[0] and this
            // read the list positionally, which is a trap the moment another
            // knob is added in front of it — it would have handed setTurns() a
            // world size.
            for (const auto& kn : knobs_)
                if (kn.key == "turns") {
                    const std::size_t i = std::size_t(v + 0.5f);
                    if (i < kn.choices.size()) { setTurns(kn.choices[i]); reset(); }
                }
        }
        if (k == "size") {
            // Rebuild, then reset — not optional here even though the workbench
            // resets on release anyway. The ant carries a POSITION, and a
            // smaller field would leave ax_/ay_ outside the buffer; the next
            // step_once() would read and write past the end of it.
            const int n = base_ << std::clamp(int(v + 0.5f), 0, 3);
            if (n != cur_.w) { cur_ = Field(n, n); reset(); }
        }
    }

    void reset() override { cur_.fill(0); gen_ = 0; ax_ = cur_.w/2; ay_ = cur_.h/2; dir_ = 0; }
    void step() override {
        // One ant is slow to watch, so a batch runs per tick. The generation
        // count still advances once per ant move, so the number on screen is
        // the real step count and not the frame count.
        //
        // NOT threaded, and not because it was too much trouble. There is
        // nothing here to split: the whole state is one agent, and move k+1
        // reads the cell move k just flipped and the direction move k just
        // turned. A lattice sweep is a million independent cells; this is a
        // chain of a few hundred dependent moves. Splitting the batch across
        // workers would not be a faster ant, it would be a different one —
        // and the field size, which is what the size knob changes, is not in
        // the cost at all: a tick is `speed` moves whatever the world is.
        const int batch = std::max(1, int(knob("speed") + 0.5f));
        for (int k = 0; k < batch; ++k) step_once();
    }
    Field* editable() override { return &cur_; }
    [[nodiscard]] int ant_x() const { return ax_; }
    [[nodiscard]] int ant_y() const { return ay_; }
    [[nodiscard]] int ant_dir() const { return dir_; }
    void step_once() {
        const std::uint8_t c = cur_.at(ax_, ay_);
        const int n = int(turns_.size());
        const int idx = (c < n) ? c : 0;
        // 'R' is +1 quarter turn, 'L' is +3 (i.e. -1). Same convention as the
        // original two-state version, so "RL" reproduces it exactly.
        dir_ = (dir_ + (turns_[std::size_t(idx)] == 'R' ? 1 : 3)) & 3;
        cur_.set(ax_, ay_, std::uint8_t((idx + 1) % n));
        ax_ = (ax_ + DX[dir_] + cur_.w) % cur_.w;
        ay_ = (ay_ + DY[dir_] + cur_.h) % cur_.h;
        ++gen_;
    }
private:
    // Each colour needs its own swatch or the legend lies about how many states
    // the ant actually has, so the palette is rebuilt with the rule.
    void setTurns(const std::string& t) {
        turns_ = t.empty() ? "RL" : t;
        pal_.clear();
        pal_.push_back({{8,11,14}, "unflipped"});
        static const Rgb ramp[] = {{90,209,196},{242,193,78},{155,122,230},{217,83,79},
                                   {123,216,143},{ 90,140,220},{230,140,190},{200,200,120},
                                   {110,200,220}};
        for (std::size_t i = 1; i < turns_.size(); ++i)
            pal_.push_back({ ramp[(i - 1) % (sizeof ramp / sizeof ramp[0])],
                             std::string("colour ") + char('0' + int(i)) + " (" + turns_[i] + ")" });
        if (turns_.size() == 2) pal_[1].label = "flipped";
    }
    [[nodiscard]] float knob(const char* key) const {
        for (auto& kn : knobs_) if (kn.key == key) return kn.value;
        return 0.f;
    }
    std::string turns_ = "RL";
    std::vector<Knob> knobs_;
    static constexpr int DX[4] = { 0, 1, 0, -1 };   // N E S W
    static constexpr int DY[4] = { -1, 0, 1, 0 };
    Provenance about_; std::vector<Swatch> pal_;
    int base_ = 320;                    // the size this instance was built at
    Field cur_; int ax_ = 0, ay_ = 0, dir_ = 0; std::uint64_t gen_ = 0;
};

inline SimPtr make_brains_brain(int n = 256) { return std::make_unique<BriansBrain>(n); }
inline SimPtr make_wireworld(int w = 200, int h = 120) { return std::make_unique<Wireworld>(w, h); }
inline SimPtr make_langton_ant(int n = 320) { return std::make_unique<LangtonAnt>(n); }

} // namespace bench
