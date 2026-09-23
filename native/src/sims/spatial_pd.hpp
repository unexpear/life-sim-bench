// spatial_pd.hpp — the spatial prisoner's dilemma.
//
// Nowak, M. A. & May, R. M. "Evolutionary games and spatial chaos",
// Nature 359:826-829 (1992).
//
// Every site is an unconditional cooperator or an unconditional defector. Each
// plays the prisoner's dilemma against its eight neighbours and itself, sums the
// payoff, then adopts the strategy of whichever cell in that neighbourhood
// scored highest. Deterministic and synchronous — which makes it a cellular
// automaton, and puts it on this bench rather than in a game-theory textbook.
//
// The payoffs are the "weak" PD they used: T = b, R = 1, P = S = 0. Because
// P and S are both zero, only interactions WITH cooperators pay anything, so a
// cell's score is just its cooperating-neighbour count, times b if it defects.
// That is the whole model.
//
// Why it matters here: no memory, no reciprocity, no tit-for-tat, no strategy
// of any kind. Cooperation survives on GEOMETRY alone — cooperators clump, and
// clumps feed themselves. Axelrod needed repeated play and reputation to get
// cooperation; this needs only neighbours.
//
// The four-colour view is Nowak & May's own: it shows TRANSITIONS, not states,
// which is what makes the dynamic fractals legible.

#pragma once
#include "../sim.hpp"
#include "../rng.hpp"
#include "../parallel.hpp"
#include <algorithm>

namespace bench {

class SpatialPD final : public Sim {
public:
    // Odd size by default so a single central defector sits on an exact centre
    // and the pattern that grows out of it can be checked for symmetry at all —
    // when that symmetry holds and when it does not is set out at is_symmetric().
    explicit SpatialPD(int size = 121) : n_(size | 1) {
        // The size ladder starts at whatever this sim was CONSTRUCTED with and
        // doubles from there, staying odd so the single-defector figure keeps
        // an exact centre at every setting.
        //
        // Generated rather than hardcoded because index 0 has to be the size
        // actually in use. A fixed list starting at 121 would mislabel every
        // SpatialPD built at another size — the self-test builds them at 81 —
        // and a control that reads 121 next to an 81-wide world is worse than
        // no control, because you cannot tell it from a working one by looking.
        for (int i = 0, s = n_; i < 5; ++i, s = s * 2 - 1) sizes_.push_back(s);
        allocate();

        // Neither end of the b range is unanimous, which is why the blurb below
        // does not say "everyone" or "everything". Measured on this class at the
        // default 121 with the reset() below, run out to 2000 steps: every b from
        // 1.0025 to 1.1125 freezes on step 6 at 0.9814 cooperators — 272 defectors,
        // none of them touching another — while b = 1.0 itself freezes a step
        // earlier, at 0.9643 (523, likewise all isolated); b >= 2.0125 freezes on
        // step 9 at 0.0380, on a field that is byte-identical for every b from
        // 2.0025 to 2.2; and neither end moves again. In that high-end field all
        // 557 surviving cooperators lie inside a fully cooperating 3x3 box (145
        // such boxes), gathered into 31 eight-connected clumps of 9 to 45 cells.
        about_ = Provenance{
            "Spatial prisoner's dilemma", "1992",
            "Martin A. Nowak & Robert M. May",
            "Nowak, M. A. & May, R. M. \"Evolutionary games and spatial chaos\", "
            "Nature 359:826-829 (1992)",
            Replication::No,
            "No. Strategies spread by IMITATION, not reproduction - a cell copies its best "
            "neighbour rather than making a copy of itself. Worth having on the bench anyway, "
            "because a purely game-theoretic rule with no biology in it produces gliders and "
            "dynamic fractals, and because it shows cooperation surviving on geometry alone.",
            "Every cell cooperates or defects, unconditionally. Each plays its eight neighbours "
            "and itself, sums the payoff, and copies whoever nearby did best. Turn b below about "
            "1.1 and cooperation takes the lattice over and it freezes - 96 to 98% cooperators, "
            "the rest lone defectors that hold their own square forever and can never spread. "
            "Push it past 2.0 and it freezes the other way, except for a scatter of cooperator "
            "clumps - under 4% of the sites - that no defector can break into. "
            "Everything interesting is in between."
        };
        // Four colours, showing what each cell just DID rather than what it is.
        pal_ = {
            {{ 38, 86,140}, "cooperating (was cooperating)"},
            {{122,216,138}, "just turned cooperator"},
            {{196, 64, 58}, "defecting (was defecting)"},
            {{240,206, 90}, "just defected"},
        };
        knobs_ = {
                   // The world is resizable now because the step is threaded and
                   // no longer wraps with integer division, which together took
                   // 961 squared from 77 ms a step to 4.6 and 1921 squared from
                   // 312 to 13, and made the big end of this list something you
                   // can actually watch rather than something you can start.
                   //
                   // A choice list rather than a slider: the jump from 14 thousand
                   // sites to 3.7 million is what makes the range legible, and a
                   // continuous slider through it spends most of its travel on
                   // sizes nobody wants.
                   {"size", "world size", 0.0f, float(sizes_.size() - 1), 0.0f, 1.0f,
                    size_choices(), true,
                    "Sites across, squared, always odd so a single central defector still has an "
                    "exact centre. Every measurement quoted on this sim - the freeze steps, the "
                    "b sweep, the symmetry survey - was made at the first entry, which is the "
                    "size this sim was built with; the larger ones are the same rule with more "
                    "room, not a different result. The top entry is 3.7 million sites, about "
                    "7 ms a step threaded here and 48 on one thread."},
                   {"b", "b - temptation to defect", 1.0f, 2.2f, 1.85f, 0.0f, {}, false,
                    "What a defector collects from each cooperating neighbour; a cooperator "
                    "collects 1 from each, and nobody collects anything from a defector. At 9/8 "
                    "and below the lattice freezes cooperative, and from 2.0025 up it freezes "
                    "defecting. In between neither side can finish the other off: swept from the "
                    "reset() soup at the default 121 in steps of 0.0025, no value between those "
                    "two ends had stopped moving after 400 steps, which is the regime worth "
                    "watching."} };
        reset();
    }

    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_;   }
    const Field&               field()   const override { return view_;  }
    std::uint64_t              generation() const override { return gen_; }
    std::vector<Knob>&         knobs() override { return knobs_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        if (k == "size") {
            // Rebuild the buffers HERE. reset() only refills the sites it
            // already has, so a size knob that leaves the allocation alone is a
            // no-op that looks like it worked: the label changes, the world
            // does not. Re-seed too, because five vectors that have just been
            // reallocated are five vectors full of zeros, and zero is a
            // cooperator — leaving it would hand you a silently blank world if
            // anything read the field before the caller's own reset().
            const int i = std::clamp(int(v + 0.5f), 0, int(sizes_.size()) - 1);
            if (sizes_[std::size_t(i)] != n_) {
                n_ = sizes_[std::size_t(i)];
                allocate();
                reset();
            }
        }
    }

    void reset() override {
        rng_.reseed(0x9D0AAu);
        gen_ = 0; churn_ = 0;
        for (auto& c : s_) c = (rng_.unit() < 0.10f) ? D : C;
        prev_ = s_;
        publish();
    }

    // The classic figure: one defector in a sea of cooperators. The rule is
    // deterministic and the neighbourhood is square, so the pattern that grows
    // is four-fold symmetric at almost every b - and staying symmetric is a
    // strong correctness check. Almost, not always: the exceptions and what
    // causes them are set out at is_symmetric() below.
    void seed_single_defector() {
        gen_ = 0; churn_ = 0;
        std::fill(s_.begin(), s_.end(), C);
        s_[std::size_t(n_ / 2) * n_ + n_ / 2] = D;
        prev_ = s_;
        publish();
    }

    void step() override {
        const float b = knob("b");
        const int n = n_;
        const unsigned workers = threads_for(std::size_t(n) * std::size_t(n));

        // Both sweeps are row-parallel above a measured size, and both wrap by
        // comparison rather than by modulo.
        //
        // The modulo was the expensive half. widx() did two integer divisions
        // per neighbour access and there are eighteen of those per site per
        // step, which held this sim to 14.5 million sites a second — about six
        // times worse per site than the life-like engine next door (83 million,
        // from its own note), on a rule that is not six times harder. Field
        // already says as much at wrap(): the modulo form measured three times
        // slower there. Wrapping by comparison is valid for
        // any offset within one width, which a 3x3 neighbourhood always is,
        // and gives exactly the same index: -1 maps to n-1 and n maps to 0
        // either way. poke() keeps widx(), because its offsets are not bounded
        // by the caller and it is not on the hot path.
        //
        // Rows are independent by construction: sweep 1 reads s_ and writes its
        // own slice of score_, sweep 2 reads s_ and score_ and writes its own
        // slice of next_. Nothing here draws from rng_ — the only shared
        // mutable state in the class, and it is touched by reset() alone — so
        // there is nothing for a thread split to make observable.

        // The buffers are taken as raw pointers into LOCALS, captured by value.
        // Cells are unsigned char and so may alias anything, which means a store
        // through next_ or view_ could in principle land on another vector's own
        // members and force the base pointers to be reloaded in the innermost
        // loop. Pointers held in locals whose address is never taken cannot be
        // hit that way. Measured on Langton's Loops, which has the same shape,
        // the difference between the two spellings was a factor of two.
        const std::uint8_t* const sp = s_.data();
        std::uint8_t*       const np = next_.data();
        std::uint8_t*       const vp = view_.cells.data();
        float*              const cp = score_.data();
        std::size_t*        const rc = rowChanged_.data();

        // 1 — everyone plays their neighbourhood (self included) and banks a payoff.
        //     P = S = 0, so only cooperating partners pay anything at all.
        parallel_for(std::size_t(n), [sp, cp, n, b](std::size_t row) {
            const int y = int(row);
            const std::uint8_t* up = sp + std::size_t(y == 0     ? n - 1 : y - 1) * std::size_t(n);
            const std::uint8_t* mi = sp + std::size_t(y) * std::size_t(n);
            const std::uint8_t* dn = sp + std::size_t(y == n - 1 ? 0     : y + 1) * std::size_t(n);
            float* out = cp + std::size_t(y) * std::size_t(n);
            for (int x = 0; x < n; ++x) {
                const int l = (x == 0)     ? n - 1 : x - 1;
                const int r = (x == n - 1) ? 0     : x + 1;
                const int coop = (up[l] == C) + (up[x] == C) + (up[r] == C)
                               + (mi[l] == C) + (mi[x] == C) + (mi[r] == C)
                               + (dn[l] == C) + (dn[x] == C) + (dn[r] == C);
                out[x] = (mi[x] == C) ? float(coop) : float(coop) * b;
            }
        }, workers);

        // 2 — copy the best-scoring cell in the neighbourhood, self included.
        //
        // The eight neighbours are visited in the original dy/dx scan order and
        // compared with a strict >, so the incumbent keeps a tie and otherwise
        // the first strictly-better neighbour wins. That order is not an
        // implementation detail here: it is exactly what decides the four b
        // values at which the single-defector figure loses its symmetry, as set
        // out at is_symmetric(). Reordering these would silently move them.
        parallel_for(std::size_t(n), [sp, np, vp, cp, rc, n](std::size_t row) {
            const int y = int(row);
            const std::size_t yu = std::size_t(y == 0     ? n - 1 : y - 1) * std::size_t(n);
            const std::size_t ym = std::size_t(y) * std::size_t(n);
            const std::size_t yd = std::size_t(y == n - 1 ? 0     : y + 1) * std::size_t(n);
            std::size_t changed = 0;
            for (int x = 0; x < n; ++x) {
                const std::size_t xc = std::size_t(x);
                const std::size_t l  = std::size_t((x == 0)     ? n - 1 : x - 1);
                const std::size_t r  = std::size_t((x == n - 1) ? 0     : x + 1);
                const std::uint8_t self = sp[ym + xc];
                float        best = cp[ym + xc];
                std::uint8_t pick = self;
                // Written out rather than looped over an array of the eight
                // indices. Not a measured win — it was changed at the same time
                // as the colouring below, so the two were never separated — but
                // an array of eight std::size_t built per site is a stack object
                // the compiler has to justify eliminating, and the unrolled form
                // asks it to justify nothing.
                auto take = [&](std::size_t j) {
                    if (cp[j] > best) { best = cp[j]; pick = sp[j]; }
                };
                take(yu + l); take(yu + xc); take(yu + r);
                take(ym + l);                take(ym + r);
                take(yd + l); take(yd + xc); take(yd + r);
                np[ym + xc] = pick;
                if (pick != self) ++changed;
                // The transition colour, computed here rather than in a third
                // pass over the lattice.
                //
                // publish() reads prev_ against s_ AFTER the rotation below,
                // which is the same pair this loop already has in registers:
                // `pick` is the new state and `self` the one it replaces. Doing
                // it here removes a whole sweep of the lattice and, with it, one
                // of the three thread launches a step was costing. parallel_for
                // spawns its workers on every call, and the floor it puts under
                // a threaded step here measured about 1.2 ms — some 80
                // microseconds for each of the fifteen threads.
                vp[ym + xc] = colour(pick == C, self == C);
            }
            rc[row] = changed;
        }, workers);

        // Summed from a per-row array rather than a shared counter. A counter
        // incremented from several threads is either a race or a contended
        // atomic; integer counts summed by index are neither, and give the same
        // total on any number of workers.
        std::size_t changed = 0;
        for (const std::size_t c : rowChanged_) changed += c;

        // A three-way rotation instead of `prev_ = s_`, which copied the whole
        // lattice every step for no reason: sweep 2 writes every site of next_
        // before anything reads it, so the buffer it lands in can be the stale
        // one. At the top size that is 3.7 MB of copying saved per step.
        prev_.swap(s_);        // prev_ holds the state just left behind
        s_.swap(next_);        // s_ holds the new one; next_ is now scratch
        churn_ = double(changed) / double(s_.size());
        ++gen_;
        // No publish() here: sweep 2 already wrote view_. The other three
        // callers — reset, the single-defector figure and poke — still use it,
        // because they change s_ without running a sweep at all.
    }

    // Click to drop a patch of defectors in.
    bool poke(float nx, float ny) override {
        const int cx = int(nx * n_), cy = int(ny * n_);
        for (int dy = -3; dy <= 3; ++dy)
            for (int dx = -3; dx <= 3; ++dx)
                s_[widx(cx + dx, cy + dy)] = D;
        publish();
        return true;
    }

    [[nodiscard]] double cooperators() const {
        std::size_t c = 0;
        for (auto v : s_) if (v == C) ++c;
        return double(c) / double(s_.size());
    }
    [[nodiscard]] double churn() const { return churn_; }

    // Cooperators keeps a direction — whether cooperation survives is the
    // question this rule is asked. Churn does not: it says how hard the lattice
    // is moving, not how well the run is going. Measured on the default 121: the
    // dead run at b = 2.10 peaks at churn 0.4968 on its very first step, which
    // takes cooperators from 0.9045 to 0.4078, and reads 0.0000 from step 9 to
    // the end; the live run at b = 1.85 peaks at that same 0.4968 on step 1 and
    // is still churning at 0.242 after 2000 steps. A running maximum therefore
    // cannot tell the two apart.
    std::vector<Metric> metrics() const override {
        return { {"cooperators", cooperators(), 1.0},
                 {"churn (cells that flipped)", churn_, 1.0, Metric::Neither} };
    }

    // Exact four-fold symmetry about the centre, and a strong check on the
    // payoff accounting and the toroidal indexing: get either wrong and a
    // symmetric start goes lopsided within a few steps.
    //
    // It does NOT hold at every b, because one step of the update is not
    // isotropic. Step 2 of step() keeps the incumbent on a tie and otherwise
    // takes the first strictly-better neighbour in dy/dx scan order, so when a
    // cooperator and a defector tie for the top score the winner is whichever
    // one the scan reaches first — and that depends on direction. It takes an
    // exact float tie k == m*b between a cooperator's neighbour count k and a
    // defector's m. Measured from seed_single_defector(), sweeping b from 1.0
    // to 2.2 in steps of 0.005 and checking after every step for 150 steps:
    // 237 of the 241 values stay symmetric throughout, and the four that break
    // (1.125, 1.4, 1.8, 2.0) are all values that admit such a tie; no tie-free
    // value broke. The self-test runs b = 1.85, which is tie-free.
    [[nodiscard]] bool is_symmetric() const {
        for (int y = 0; y < n_; ++y)
            for (int x = 0; x < n_; ++x) {
                const std::uint8_t v = s_[idx(x, y)];
                if (v != s_[idx(n_ - 1 - x, y)]) return false;   // mirror in x
                if (v != s_[idx(x, n_ - 1 - y)]) return false;   // mirror in y
                if (v != s_[idx(y, x)])          return false;   // transpose
            }
        return true;
    }

private:
    static constexpr std::uint8_t C = 0, D = 1;

    // Below this many sites the thread launch costs more than the sweep saves.
    //
    // Measured on this class after the wrap fix, serial and threaded timed
    // ALTERNATELY at each size and the minimum of seven taken — this is a laptop,
    // and interleaving stops a thermal drift halfway through the run from being
    // read as a result. 231,361 sites (481 squared) is 8.25 ms serial against
    // 8.61 threaded, a slight loss; 463,761 (681 squared) is 14.61 against
    // 11.41; 923,521 (961 squared) is 30.17 against 12.19. The crossover falls
    // between the first two, so the 250,000 that GridSim and the life-like
    // engine already use lands in the right place here and is taken unchanged.
    //
    // Below it the launch cost is not marginal, it is everything: parallel_for
    // spawns its workers on every call and joins them at the end, about 80
    // microseconds a thread here, and the whole step at the default 121 is
    // 0.52 ms. Threading that made it 6.02 ms — a twelvefold slowdown out of a
    // control that reads as an optimisation, which is exactly why the threshold
    // is measured rather than assumed.
    static constexpr std::size_t kParallelSites = 250000;
    [[nodiscard]] static unsigned threads_for(std::size_t sites) {
        return (sites >= kParallelSites) ? 0u : 1u;
    }

    // Allocate every buffer for the current n_. Five of them, which is the
    // reason a size knob here has to call this rather than lean on reset().
    void allocate() {
        view_ = Field(n_, n_);
        s_.assign(std::size_t(n_) * std::size_t(n_), 0);
        prev_ = s_; next_ = s_;
        score_.assign(s_.size(), 0.f);
        rowChanged_.assign(std::size_t(n_), 0);
    }

    [[nodiscard]] std::vector<std::string> size_choices() const {
        std::vector<std::string> out;
        out.reserve(sizes_.size());
        for (const int s : sizes_) out.push_back(std::to_string(s));
        return out;
    }

    [[nodiscard]] std::size_t idx(int x, int y) const { return std::size_t(y) * n_ + x; }
    [[nodiscard]] std::size_t widx(int x, int y) const {
        int xx = x % n_; if (xx < 0) xx += n_;
        int yy = y % n_; if (yy < 0) yy += n_;
        return std::size_t(yy) * n_ + xx;
    }
    [[nodiscard]] float knob(const char* k) const {
        for (auto& kn : knobs_) if (kn.key == k) return kn.value;
        return 0.f;
    }

    // state + previous state -> one of the four transition colours. The single
    // definition, so the copy inlined into sweep 2 cannot drift from this one.
    [[nodiscard]] static std::uint8_t colour(bool now, bool was) {
        return now ? (was ? 0 : 1) : (was ? 3 : 2);
    }

    // Repaint the whole view from prev_ and s_.
    //
    // step() does not use this — it colours each site inside sweep 2, where the
    // before and after states are already to hand. This is for the paths that
    // change the lattice without stepping it: reset, the single-defector
    // figure, and poke.
    void publish() {
        const int n = n_;
        parallel_for(std::size_t(n), [&](std::size_t row) {
            const std::size_t base = row * std::size_t(n);
            for (int x = 0; x < n; ++x) {
                const std::size_t i = base + std::size_t(x);
                view_.cells[i] = colour(s_[i] == C, prev_[i] == C);
            }
        }, threads_for(std::size_t(n) * std::size_t(n)));
    }

    Provenance                about_;
    std::vector<Swatch>       pal_;
    std::vector<Knob>         knobs_;
    Field                     view_;
    int                       n_;
    std::vector<int>          sizes_;        // the size ladder the knob indexes
    std::vector<std::uint8_t> s_, prev_, next_;
    std::vector<float>        score_;
    std::vector<std::size_t>  rowChanged_;   // per-row churn, summed after the sweep
    Rng                       rng_{0x9D0AAu};
    std::uint64_t             gen_   = 0;
    double                    churn_ = 0;
};

inline SimPtr make_spatial_pd(int size = 121) {
    return std::make_unique<SpatialPD>(size);
}

} // namespace bench
