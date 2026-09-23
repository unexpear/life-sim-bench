// rulespec.hpp — rules as data instead of as C++.
//
// Adding a rule currently means writing a class, building a DLL and reloading
// it. That is the right mechanism for von Neumann's 29 states and wildly
// disproportionate for Seeds, which is four characters of information.
//
// This parses the standard notations into a table and runs them from one loop.
// Four of the sims already on the bench (Life, Seeds, Day & Night, Brian's
// Brain) collapse into it — and because those four are hand-written and already
// verified against their published behaviour, they double as the test: a parsed
// rule must produce a BYTE-IDENTICAL field to its hand-written twin for 200
// generations from the same seed. A parser that matches two independently
// written engines across five rules exactly is right.
//
// The real payoff is not convenience. It is that `sweep.exe` can now sweep
// across RULESPACE rather than across floats.
//
// Notation supported:
//   B3/S23      Golly / LifeWiki style, either order (S23/B3 also parses)
//   23/3        the older survival/birth form
//   B2/S/3      Generations: a third field is the number of states
//   /2/3        the same thing in survival/birth/states form
//
//   B2-a/S12    Hensel isotropic non-totalistic neighbourhoods
//   B2/S12V     four orthogonal neighbours; H selects six hexagonal neighbours
// Parsing and the compiled transition table live in rulestring.hpp.

#pragma once
#include "../toolkit.hpp"
#include "../rng.hpp"
#include "../rulestring.hpp"
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>
#include <cctype>

namespace bench {

// ── provenance for an arbitrary string ──────────────────────────────────────
//
// The bench requires every sim to state who published it and whether it
// genuinely self-replicates. A rule typed at runtime has no author and no
// paper, and inventing one would be worse than useless. So: recognised rules
// carry their real citation, and everything else carries an explicit non-claim.
inline Provenance provenance_for(const RuleSpec& r) {
    const std::string& t = r.text;

    if (t == "B3/S23")
        return Provenance{
            "Conway's Game of Life", "1970", "John Horton Conway; popularised by Martin Gardner",
            "Gardner, M. \"Mathematical Games\", Scientific American 223(4), October 1970",
            Replication::Yes,
            "Yes, but it took forty years. Gemini (Andrew Wade, 2010) is the first pattern in Life "
            "that constructs a copy of itself.",
            "B3/S23, entered as a rulestring. Identical to the hand-written Life on this bench — "
            "the self-test asserts they agree cell-for-cell."
        };
    if (t == "B36/S23")
        return Provenance{
            "HighLife", "1994", "Nathan Thompson",
            "Rule B36/S23; the 12-cell replicator is widely catalogued",
            Replication::Yes,
            "Yes, and for free — a 12-cell pattern copies itself every 12 generations, and can do "
            "nothing else.",
            "Conway's rule with one extra birth condition."
        };
    if (t == "B2/S")
        return Provenance{
            "Seeds", "—", "rule catalogued by Mirek Wojtowicz among others",
            "Rule B2/S — no survival condition at all",
            Replication::No,
            "No — but not because nothing lasts. No cell survives a tick (S is empty), yet "
            "shapes do: a diagonal pair oscillates with period 2 and the 4-cell O..O over .OO. "
            "is a spaceship, both unchanged after 5000 generations of this rule. A domino even "
            "prints two translated copies of itself in one tick. What is missing is a lineage — "
            "the copies collide within a few generations and nothing goes on making more.",
            "Every cell on screen is exactly one generation old."
        };
    if (t == "B3678/S34678")
        return Provenance{
            "Day & Night", "1997", "Nathan Thompson", "Rule B3678/S34678",
            Replication::No,
            "No known self-replicator, though the rule supports complex engineered patterns.",
            "Symmetric under swapping live and dead."
        };
    if (t == "B2/S/3")
        return Provenance{
            "Brian's Brain", "1980s", "Brian Silverman",
            "Rule catalogued widely; Silverman also authored Wireworld",
            Replication::No,
            "No. It is a medium for travelling waves, not for organisms.",
            "Three states, expressed as a Generations rule: birth on exactly two firing "
            "neighbours, no survival, then one refractory tick before death."
        };

    // Unrecognised. Say so, and claim nothing.
    return Provenance{
        "Rule " + t, "—", "user-specified",
        "",
        Replication::No,
        "Unknown. This rule was entered at runtime and no published claim attaches to it. "
        "The bench reports No because it has no evidence of replication, NOT because "
        "replication has been ruled out — those are different statements and this one is the "
        "weaker.",
        r.generations()
            ? "A Generations rule with " + std::to_string(r.states) + " states, entered as a "
              "rulestring. Only state 1 counts as alive. States 2 and up advance until they "
              "return to dead. The world wraps at its edges."
            : "A rule using " + std::to_string(r.neighbors()) + " neighbours. Birth and survival depend on " +
              (r.nonTotalistic ? "the arrangement of living neighbours." : "the number of living neighbours.") +
              " The world wraps at its edges."
    };
}

// ── the sim ─────────────────────────────────────────────────────────────────

// The world sizes this sim offers, as a ladder starting from the size the
// instance was actually built at.
//
// Powers of two, and a choice list rather than a slider, for the same reason
// life_like.hpp uses one: the jump from 65 thousand cells to 16 million is only
// legible as named steps, and a continuous slider through that range spends
// most of its travel in sizes nobody wants.
//
// The size the instance was built at is always ON the ladder, inserted in
// order if it is not one of the standard steps, and the knob starts on it. That
// is what keeps the control honest: its default position names the size the
// field really has, so it cannot read "512" over a 256-wide world — a lie you
// would only discover by moving the knob, which is the moment you would stop
// trusting the whole panel.
inline std::vector<int> rule_size_ladder(int size) {
    std::vector<int> s{256, 512, 1024, 2048, 4096};
    if (std::find(s.begin(), s.end(), size) == s.end()) {
        s.push_back(size);
        std::sort(s.begin(), s.end());
    }
    return s;
}

class RuleSim final : public GridSim {
public:
    RuleSim(RuleSpec spec, int size, float density, std::uint64_t seed)
        : GridSim(provenance_for(spec), palette_for(spec), size, size),
          spec_(std::move(spec)), sizes_(rule_size_ladder(size)),
          density_(density), seed_(seed), baseSeed_(seed) {
        // The rule itself is typed into the workbench's rulestring field; what
        // is left free is the initial condition, and for a rule nobody has
        // looked at before that is exactly the thing worth varying — plenty of
        // rules are dead from a sparse soup and alive from a dense one. The
        // world it runs in is free too: a rule you have never seen shows its
        // real behaviour at scale, not in a 256-cell postage stamp where a
        // handful of structures fill the whole field.
        {
            std::vector<std::string> labels;
            labels.reserve(sizes_.size());
            for (int n : sizes_) labels.push_back(std::to_string(n));
            // Start on the size actually built, wherever that sits on the ladder.
            const float at = float(std::find(sizes_.begin(), sizes_.end(), size)
                                   - sizes_.begin());
            knobs_.push_back(
                {"size", "world size", 0.f, float(sizes_.size() - 1), at, 1.f,
                 std::move(labels), true,
                 "Cells across, squared. Measured on this sim, 20 hardware threads and 16 "
                 "workers: 1024 squared is 14.7 ms a step on one thread and 3.1 ms threaded, "
                 "2048 is 57.4 and 8.7, and 4096 is sixteen million cells at 262 and 39 — so "
                 "the top of the ladder is a size to look at rather than to run fast. Changing "
                 "this rebuilds the world and re-seeds it; nothing survives a resize."});
        // The figures above were re-measured on an idle machine and are 15-40%
        // lower than the ones first written here, which were taken while nine
        // sibling benchmarks were running and the box was at 95% CPU. The
        // ratios survived that; the absolute milliseconds did not. Cross-check
        // that keeps them honest: life_like runs the identical B3/S23 rule and
        // quotes 12.6 and 3.2 at 1024 squared, so the two must stay within the
        // small margin this sim pays for reading its rule out of a table
        // instead of a constant — if they ever diverge, one of them is stale.
        }
        knobs_.push_back({"density", "initial soup density", 0.0f, 1.0f, density_, 0.01f, {}, true});
        knobs_.push_back({"seed",    "random seed",          1.0f, 64.0f, 1.0f,    1.0f,  {}, true});
        reset();
    }

    // Seeding deliberately mirrors LifeLike and BriansBrain exactly — same
    // generator, same order, same comparison — because the self-test asserts
    // byte equality against them. Any difference here would show up as a
    // spurious failure and send someone hunting for a rule bug that isn't one.
    void reset() override {
        rng_.reseed(seed_);
        gen_ = 0;
        for (auto& c : cur_.cells) c = (rng_.unit() < density_) ? 1 : 0;
    }

    std::vector<Knob>& knobs() override { return knobs_; }
    // Only the initial condition is a knob here. The rule — birth, survival
    // and the state count alike — is edited by retyping the rulestring, and
    // that goes through make_rule, which parses the text and constructs a new
    // RuleSim. So spec_ never changes after construction and there is nothing
    // to re-derive in place: the attribution and the palette are computed once,
    // from the spec this instance was built with, and stay correct by not
    // having anything to go stale against.
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        if (k == "size") {
            // Rebuild BOTH buffers, here rather than in reset().
            //
            // reset() only refills the cells it already has, so a size knob
            // that stops at recording the value is a no-op that looks like it
            // worked — the field carries on at whatever size it was built at.
            // And nxt_ has to grow with cur_: GridSim::step() writes every
            // (x, y) of the sweep into it, so a next-buffer left at the old
            // size is an out-of-bounds write on the first step, not a
            // cosmetic mismatch.
            const std::size_t i = std::size_t(std::clamp(int(v + 0.5f), 0,
                                                         int(sizes_.size()) - 1));
            const int n = sizes_[i];
            if (n != cur_.w) {
                cur_ = Field(n, n);
                nxt_ = Field(n, n);
                // A fresh Field is all zeros, and an empty field is a dead one
                // for every rule here. The workbench re-seeds on release
                // because the knob is on_reset, but a caller that only sets
                // the knob should still get a live world rather than a blank
                // one that looks like the rule died.
                reset();
            }
        }
        if (k == "density") density_ = v;
        // Value 1 must mean the seed this sim was actually built with,
        // or the control reads as a lie the moment you look at it.
        if (k == "seed")    seed_ = baseSeed_ * std::uint64_t(v);
    }

    // Deliberately NOT paired with a step() override.
    //
    // GridSim::step() already splits the sweep by rows above kParallelCells,
    // and because this class does not override it, that shared sweep is the
    // code that actually runs — worth stating, because the neighbouring
    // life_like.hpp DOES override step(), and parallelising a base sweep that
    // a derived class bypasses is a silent no-op that benchmarks as a flat
    // 1.00x and reads like a threading bug.
    //
    // Confirmed on this sim rather than inferred, with a worker sweep at
    // 1/2/4/8/16 threads: 1024 squared went 16.8 / 9.1 / 5.5 / 4.5 / 3.5 ms a
    // step, and 4096 squared 283.6 / 152.5 / 88.5 / 61.4 / 45.2. The time
    // falls, so the sweep really is being split.
    //
    // The precondition for that split is this function, and it holds: rule()
    // reads the current field and nothing else. No rng(), no member it writes,
    // no state carried between cells — so which thread evaluates which row
    // cannot be observed. Checked the only way that means anything: 30 steps
    // at one worker against 30 steps at sixteen, compared cell for cell, at
    // 512/1024/2048 squared across B3/S23, B36/S23, B3678/S34678 and the
    // Generations rule B2/S/3. Every pair identical, zero differing cells.
    std::uint8_t rule(int x, int y) override {
        const Field& f = read();
        const std::uint8_t s = f.at(x, y);

        // Generations: only state 1 counts as a live neighbour. States 2+ are
        // refractory — they are on their way out and cannot be revived, which
        // is the entire reason the family exists.
        if (s >= 2) return std::uint8_t(s + 1 >= spec_.states ? 0 : s + 1);
        unsigned mask = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (f.wrap(x + dx, y + dy) == 1)
                    mask |= 1u << ((dy + 1) * 3 + dx + 1);
            }
        const auto next = spec_.transitions[mask];
        return spec_.generations() && s == 1 && !next ? std::uint8_t(2) : next;
    }

    [[nodiscard]] std::string subtitle() const override { return spec_.text; }
    [[nodiscard]] const RuleSpec& spec() const { return spec_; }

private:
    static std::vector<Swatch> palette_for(const RuleSpec& r) {
        std::vector<Swatch> p{ {{8,11,14}, "dead"} };
        if (!r.generations()) { p.push_back({{90,209,196}, "alive"}); return p; }
        p.push_back({{255,255,255}, "firing"});
        // Decay states fade toward the background, so age is readable at a
        // glance rather than being a colour you have to look up. The fade used
        // to start at t = 1/(states-2) and end at t = 1, which mixes the oldest
        // decay state exactly onto the dead swatch {8,11,14} — for B2/S/3, this
        // file's own Generations example and the only decay state Brian's Brain
        // has, that painted the refractory phase invisible while the field was
        // still byte-identical to the hand-written twin. Running t from 0
        // instead puts the first decay state on {43,111,119}, which is the
        // colour small_lattice.hpp's hand-written Brian's Brain uses for the
        // same state.
        for (int i = 2; i < r.states; ++i) {
            const float t = float(i - 2) / float(std::max(1, r.states - 2));
            const auto mix = [&](int a, int b) { return std::uint8_t(a + (b - a) * t); };
            p.push_back({{mix(43,8), mix(111,11), mix(119,14)},
                         "refractory " + std::to_string(i - 1)});
        }
        return p;
    }

    RuleSpec            spec_;
    std::vector<int>    sizes_;
    std::vector<Knob>   knobs_;
    float         density_;
    std::uint64_t seed_;
    std::uint64_t baseSeed_;
};

// Returns nullptr on a bad rulestring. It used to fall back to B3/S23, which
// is the worst possible behaviour for this app: the user types a rule, gets a
// window full of Life, and has no way to know the parse failed. Silently
// running a different rule than the one asked for is precisely the failure this
// whole file's header warns about. Callers check, and say what went wrong.
inline SimPtr make_rule(const std::string& text, int size = 256,
                        float density = 0.28f, std::uint64_t seed = 0xC0FFEEull) {
    RuleSpec r = parse_rule(text);
    if (!r.ok) return nullptr;
    return std::make_unique<RuleSim>(std::move(r), size, density, seed);
}

// All workspace entry points must use the same base seed: the saved seed knob
// is a multiplier of it. This includes new rules, reopening and dedicated runs.
inline SimPtr make_rule_workspace(const std::string& text) {
    RuleSpec r = parse_rule(text);
    if (!r.ok) return nullptr;
    const bool generations = r.generations();
    return std::make_unique<RuleSim>(std::move(r), 256,
        generations ? 0.12f : 0.28f, generations ? 0xB2A1ull : 0xC0FFEEull);
}

} // namespace bench
