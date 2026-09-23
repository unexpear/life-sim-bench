// inert.cpp — controls that lie: ones that do nothing, ones that do nothing
// YET, ones that only LOOK like they do something, and help text whose numbers
// no longer match the code.
//
// ── why this file was not enough ───────────────────────────────────────────
//
// The first version asked one question — "does changing this knob change the
// run?" — and answered it with a single FNV hash over field, surface,
// generation and metrics. It reported 122 knobs working, 3 conditional, 0 dead,
// and on the same codebase six knobs were found that moved and changed nothing
// real. A hash cannot be wrong about equality, so the fault was in the
// question.
//
// One boolean over four merged channels loses everything that matters:
//
//   · WHICH channel moved. A knob that repaints the surface and touches
//     neither the field nor a single metric is a cosmetic control wearing a
//     simulation control's clothes, and the hash calls it "works".
//
//   · HOW MUCH moved. This is the one that did the damage. The seed knob
//     changed agent spawn positions while leaving the WORLD identical — the
//     terrain it was supposed to regenerate came out byte-for-byte the same.
//     The hash saw a difference, the suite assertion passed, and the bug lived.
//     A count says it plainly: the response was a handful of cells out of
//     thousands. An effect that small is a reshuffle of the random stream, not
//     an effect, and it is the exact shape of a knob whose real job is being
//     done by a side effect of the RNG advancing.
//
//   · WHETHER THE SIM ITSELF CAN SEE IT. If none of the sim's own metrics move,
//     then by the sim's own accounting nothing happened, and the only thing
//     that can assert on the knob is a hash — which is how these got through.
//
//   · WHEN it moved. A knob that changes the starting grid and is then washed
//     out by twenty steps is a real distinction from one that changes the
//     dynamics, and both from one that only bites late.
//
// So this measures four separate channels, at two times, and reports
// magnitudes rather than a bit. Switches are audited too; the old version
// tested knobs only and never touched a single rule bit.
//
// ── and the other half: numbers in text nobody measures ────────────────────
//
// `inert --claims` walks the same registry and extracts every number written in
// user-visible text — knob help, choice names, provenance, blurbs, switch help,
// metric names — then checks each against the running program. Numbers in help
// text drifted away from the code four times today and nothing looks at them.
//
// Exactly one class is checked, and the restraint is the point. A dimension
// pair with the digits touching the x — "320x200", "56x32" — can mean nothing
// else, and the fact base it is checked against is MEASURED: every on_reset
// knob is set to every position it quantises to and the resulting grid recorded,
// so a size the program can be built at is not mistaken for a lie. Drafts that
// also checked "N wide" and "N states" were removed after firing on "a 3-wide
// bus", "the neighbourhood is 26 wide" and "29-state automaton" — a tool that
// cannot tell a grid from a bus is not a check, and a defect tool that cries
// wolf is worse than none.
//
// Everything else is reported as UNMEASURED, grouped by the unit it carries,
// because that is the honest verdict and the list is the deliverable: 269
// numbers with units that nothing in the program or the suite re-derives.
//
//   inert                    audit every sim's controls
//   inert <simId>            one of them
//   inert --claims [simId]   audit the numbers in its text
//   inert --claims -v ...    list every unmeasured number, not just the first
//                            few of each unit
//
// It is a separate tool and not part of the suite because the revival search is
// quadratic in the knob count and runs the sim a few hundred times per pair.

#include "registry.hpp"
#include "tool_freshness.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ── what a run looks like from outside ─────────────────────────────────────
//
// Kept as separate channels rather than folded into one hash. Merging them is
// what made the old tool blind: any one channel moving hid the other three
// standing still.
struct Shot {
    int                       fw = 0, fh = 0;
    std::vector<std::uint8_t> cells;
    int                       sw = 0, sh = 0;
    std::vector<std::uint8_t> surf;
    std::uint64_t             gen = 0;
    std::vector<std::string>  mname;
    std::vector<double>       mval;
};

Shot capture(bench::Sim& s) {
    Shot k;
    const bench::Field& f = s.field();
    k.fw = f.w; k.fh = f.h; k.cells = f.cells;
    if (const bench::Surface* sf = s.surface()) { k.sw = sf->w; k.sh = sf->h; k.surf = sf->rgba; }
    k.gen = s.generation();
    for (const auto& m : s.metrics()) { k.mname.push_back(m.name); k.mval.push_back(m.value); }
    return k;
}

// Fraction of two byte vectors that differ. A size change is total disagreement
// — the grid was rebuilt, which is the largest response there is.
double disagree(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    if (a.empty() && b.empty()) return 0.0;
    if (a.size() != b.size()) return 1.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) ++n;
    return double(n) / double(a.size());
}

// NaN-aware, because a metric that goes NaN under one setting and not another
// is a difference, and `NaN != NaN` would call two NaNs different forever.
bool moved(double a, double b) {
    const bool na = std::isnan(a), nb = std::isnan(b);
    if (na || nb) return na != nb;
    return a != b;
}

struct Resp {
    double f0 = 0.0;                 // fraction of field cells differing at reset
    double f1 = 0.0;                 // ...and after the run
    double sr = 0.0;                 // fraction of surface bytes differing at the end
    bool   gen = false;
    std::vector<int> metrics;        // indices of the sim's own metrics that moved
    std::size_t cells0 = 0, cells1 = 0, total = 0;   // raw counts, so the reader judges

    [[nodiscard]] bool any() const {
        return f0 > 0 || f1 > 0 || sr > 0 || gen || !metrics.empty();
    }
};

void merge(Resp& into, const Resp& r) {
    into.f0 = std::max(into.f0, r.f0);
    into.f1 = std::max(into.f1, r.f1);
    into.sr = std::max(into.sr, r.sr);
    into.gen = into.gen || r.gen;
    into.cells0 = std::max(into.cells0, r.cells0);
    into.cells1 = std::max(into.cells1, r.cells1);
    into.total = std::max(into.total, r.total);
    for (int i : r.metrics)
        if (std::find(into.metrics.begin(), into.metrics.end(), i) == into.metrics.end())
            into.metrics.push_back(i);
}

Resp compare(const Shot& a0, const Shot& a1, const Shot& b0, const Shot& b1) {
    Resp r;
    r.f0 = disagree(a0.cells, b0.cells);
    r.f1 = disagree(a1.cells, b1.cells);
    r.sr = disagree(a1.surf,  b1.surf);
    r.gen = (a1.gen != b1.gen);
    r.total = std::max(a1.cells.size(), b1.cells.size());
    r.cells0 = std::size_t(r.f0 * double(std::max(a0.cells.size(), b0.cells.size())) + 0.5);
    r.cells1 = std::size_t(r.f1 * double(r.total) + 0.5);
    const std::size_t n = std::min(a1.mval.size(), b1.mval.size());
    for (std::size_t i = 0; i < n; ++i)
        if (moved(a1.mval[i], b1.mval[i])) r.metrics.push_back(int(i));
    // A metric appearing or disappearing is also a response, attributed to the
    // first index that only one side has.
    if (a1.mval.size() != b1.mval.size()) r.metrics.push_back(int(n));
    return r;
}

// ── running one configuration ──────────────────────────────────────────────

struct Cfg {
    std::string knobKey;  float knobVal = 0.0f;   // empty key = don't set
    std::string swKey;    bool  swVal   = false;
    std::string holdKey;  float holdVal = 0.0f;
};

// Returns the shot at reset and the shot after the run.
void execute(const bench::Entry& e, const Cfg& c, int epochs, int steps,
             Shot& at0, Shot& at1) {
    auto s = e.make();
    bool needReset = false;
    auto put = [&](const std::string& k, float v) {
        if (k.empty()) return;
        for (auto& kn : s->knobs())
            if (kn.key == k) { kn.value = v; if (kn.on_reset) needReset = true; }
        s->on_knob(k, v);
    };
    put(c.holdKey, c.holdVal);
    put(c.knobKey, c.knobVal);
    if (!c.swKey.empty()) {
        for (auto& sw : s->switches()) if (sw.key == c.swKey) sw.value = c.swVal;
        s->on_switch(c.swKey, c.swVal);
    }
    if (needReset) s->reset();

    at0 = capture(*s);
    if (s->epoch_name() && epochs > 0) for (int i = 0; i < epochs; ++i) s->advance_epoch();
    else                               for (int i = 0; i < steps;  ++i) s->step();
    at1 = capture(*s);
}

// Every quantised position of a small discrete knob; four points across a
// continuous one.
//
// Four points was a real hole. voxelcraft's supersampling knob has three
// positions and von Neumann's `code` knob has six; sampling four evenly-spaced
// values across a six-choice range visits four of them and calls the knob
// judged. If the two it skipped were the only ones that behaved differently,
// the knob reads dead. A discrete knob is cheap to enumerate, so enumerate it.
std::vector<float> samples(const bench::Knob& k) {
    std::vector<float> out;
    if (k.step > 0.0f) {
        const double n = double(k.max - k.min) / double(k.step);
        if (n >= 0.0 && n <= 12.0) {
            for (int i = 0; i <= int(n + 0.5); ++i)
                out.push_back(k.quantised(k.min + k.step * float(i)));
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        }
    }
    for (int t = 0; t <= 3; ++t)
        out.push_back(k.quantised(k.min + (k.max - k.min) * float(t) / 3.0f));
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// The response of every channel to moving one control, maximised over samples.
//
// `rateCompensate` is for a display_only knob on a sim with no epoch — "ticks
// per frame" and the like. Comparing those at equal FRAME counts is guaranteed
// to show a difference and says nothing: four ticks a frame really does advance
// the world four times as far, and that is the knob working, not failing. The
// suite declines to assert anything at all here ("without an epoch there is no
// frame-independent outcome to compare against"), and that left kinesis's and
// cutemold's speed knobs entirely unchecked.
//
// But there is a frame-independent outcome, and it is easy: give every setting
// the same number of TICKS by running fewer frames at a higher rate. A knob
// that is honestly a rate control must then produce identical state — that is
// what "changes how fast you watch, not what happens" MEANS. A knob that does
// not is quietly simulating something different at a different frame rate,
// which is the bug the display_only contract exists to forbid.
Resp probe_knob(const bench::Entry& e, const bench::Knob& k,
                const std::string& hold, float hv, int epochs, int steps,
                bool rateCompensate = false, bool* didCompensate = nullptr) {
    if (didCompensate) *didCompensate = false;
    Resp acc;
    const auto vs = samples(k);
    if (vs.size() < 2) return acc;

    float vmax = vs[0];
    for (float v : vs) vmax = std::max(vmax, v);
    auto framesFor = [&](float v) {
        if (!rateCompensate || v <= 0.0f || vmax <= 0.0f) return steps;
        return std::max(1, int(double(steps) * double(vmax) / double(v) + 0.5));
    };
    // Compensating by rounding to whole frames does not compensate.
    //
    // The evenly-spaced samples of a 1..40 knob are 1, 14, 27 and 40; at 24
    // frames of the fastest, 27 needs 35.6 frames and gets 36, which is twelve
    // extra ticks. Twelve ticks of a chaotic aggregation sim moves 7% of the
    // grid, and the tool duly reported kinesis's honest rate knob as a bug —
    // the tool's own rounding error, dressed as a finding. So in this mode the
    // samples are chosen to DIVIDE the maximum, and every run then executes an
    // identical number of ticks with nothing left over.
    if (rateCompensate) {
        const int hi = int(vmax + 0.5f), lo = std::max(1, int(k.min + 0.5f));
        std::vector<float> exact;
        for (int d = hi; d >= lo && exact.size() < 4; --d)
            if (hi > 0 && hi % d == 0) exact.push_back(float(d));
        if (exact.size() >= 2) {
            Resp acc2;
            Shot b0, b1;
            execute(e, Cfg{k.key, exact[0], "", false, hold, hv}, epochs, framesFor(exact[0]), b0, b1);
            for (std::size_t i = 1; i < exact.size(); ++i) {
                Shot a0, a1;
                execute(e, Cfg{k.key, exact[i], "", false, hold, hv}, epochs,
                        framesFor(exact[i]), a0, a1);
                merge(acc2, compare(b0, b1, a0, a1));
            }
            if (didCompensate) *didCompensate = true;
            return acc2;
        }
        // Fewer than two exact divisors — a knob whose range cannot be split
        // evenly. Rather than compensate approximately, which is the mistake
        // above, fall back to the plain equal-frames reading and let the caller
        // judge on that alone. An approximate answer here is not a weaker
        // finding, it is a false one.
        rateCompensate = false;
    }

    Shot base0, base1;
    execute(e, Cfg{k.key, vs[0], "", false, hold, hv}, epochs, framesFor(vs[0]), base0, base1);
    for (std::size_t i = 1; i < vs.size(); ++i) {
        Shot a0, a1;
        execute(e, Cfg{k.key, vs[i], "", false, hold, hv}, epochs, framesFor(vs[i]), a0, a1);
        merge(acc, compare(base0, base1, a0, a1));
    }
    return acc;
}

Resp probe_switch(const bench::Entry& e, const bench::Switch& sw, int epochs, int steps) {
    Shot a0, a1, b0, b1;
    execute(e, Cfg{"", 0, sw.key, false, "", 0}, epochs, steps, a0, a1);
    execute(e, Cfg{"", 0, sw.key, true,  "", 0}, epochs, steps, b0, b1);
    return compare(a0, a1, b0, b1);
}

// ── the verdict ────────────────────────────────────────────────────────────
//
// One stated threshold, not a tuned one. A response confined to under 1% of the
// field cannot be seen in the picture, cannot move a population share past its
// own display rounding, and is the size of what merely advancing the random
// stream produces. Below it, "this control works" is a claim the measurement
// does not support — so the count is printed alongside and the reader decides.
constexpr double kTiny = 0.01;

struct Verdict { const char* tag; std::string note; bool finding; };

// A display_only knob is judged against its own contract, not the ordinary one,
// and the contract has two honest readings because there are two kinds of
// cosmetic knob here:
//
//   · a RENDER setting — life3d's supersampling — must leave the model alone at
//     the same number of FRAMES;
//   · a RATE setting — "ticks per frame" — must leave the model alone at the
//     same number of TICKS, and will of course differ at equal frames, since
//     advancing further is the entire job.
//
// Checking only one of these misjudges the other, and this file did both in
// turn: at equal frames it called kinesis's honest rate knob a bug, and after
// rate-compensating everything it called clouds3d's honest supersampling knob a
// bug — by running it for a different number of steps, which changes the
// simulation for reasons that have nothing to do with the knob. So both
// readings are measured and the knob passes if EITHER leaves the model still.
Verdict judge_display(const Resp& frames, const Resp& ticks, bool triedTicks) {
    auto still = [](const Resp& r) { return r.f0 == 0 && r.f1 == 0 && r.metrics.empty(); };
    if (still(frames))
        return {"cosmetic", "render-only: at equal frames the model is byte-identical", false};
    if (triedTicks && still(ticks))
        return {"cosmetic", "rate-only: at equal ticks the model is byte-identical", false};
    const Resp& worst = triedTicks ? ticks : frames;
    char b[192];
    std::snprintf(b, sizeof b,
                  "declared display_only, but %.1f%% of the grid and %zu metrics differ "
                  "even after %s",
                  std::max(worst.f0, worst.f1) * 100.0, worst.metrics.size(),
                  triedTicks ? "matching the tick count" : "matching the frame count");
    return {"NOT COSMETIC", b, true};
}

Verdict judge(const Resp& r, int metricCount, bool displayOnly, bool onReset, int steps) {
    if (!r.any()) return displayOnly
        ? Verdict{"cosmetic", "nothing responds at all", false}
        : Verdict{"DEAD", "no channel responds at all", true};

    const bool fieldMoved  = r.f0 > 0 || r.f1 > 0;
    const bool metricMoved = !r.metrics.empty();

    // Only the picture changed. Nothing in the model did, and the control is
    // not declared display_only, so the panel presents it as a simulation
    // parameter.
    if (!fieldMoved && !metricMoved && !r.gen && r.sr > 0 && !displayOnly) {
        char b[128];
        std::snprintf(b, sizeof b, "repaints %.1f%% of the surface, moves nothing in the model",
                      r.sr * 100.0);
        return {"RENDER ONLY", b, true};
    }
    // Only the step counter moved. The classic display knob failure.
    if (!fieldMoved && !metricMoved && r.sr == 0 && r.gen && !displayOnly)
        return {"CLOCK ONLY", "only the generation counter differs", true};

    // The seed-side-effect shape: something moved, but a sliver of it, and the
    // sim's own instruments read the same before and after.
    if (!metricMoved && metricCount > 0 && r.f1 < kTiny && r.f0 < kTiny) {
        char b[192];
        std::snprintf(b, sizeof b,
                      "%zu of %zu cells differ (%.2f%%), and not one of the %d metrics moves",
                      r.cells1, r.total, r.f1 * 100.0, metricCount);
        return {"SIDE EFFECT?", b, true};
    }
    if (!metricMoved && metricCount > 0) {
        char b[160];
        std::snprintf(b, sizeof b, "%.1f%% of cells move but no metric does — only a hash sees this",
                      std::max(r.f0, r.f1) * 100.0);
        return {"UNMEASURED", b, true};
    }
    // Changed the starting grid and then got washed out.
    if (onReset && r.f0 > 0 && r.f1 == 0 && !metricMoved) {
        char b[128];
        std::snprintf(b, sizeof b, "changes the starting grid, gone again by step %d", steps);
        return {"SETUP ONLY", b, true};
    }
    return {"works", "", false};
}

void row(const char* sim, const char* kind, const char* key, const Resp& r,
         const Verdict& v, int metricCount) {
    char m[24];
    std::snprintf(m, sizeof m, "%d/%d", int(r.metrics.size()), metricCount);
    std::printf("  %-11s %-4s %-13s %6.2f %6.2f %6.2f %6s  %-12s %s\n",
                sim, kind, key, r.f0 * 100.0, r.f1 * 100.0, r.sr * 100.0, m,
                v.tag, v.note.c_str());
}

int audit(const std::string& only) {
    std::printf("controls that do nothing, do nothing yet, or only look like they do\n\n");
    std::printf("  %-11s %-4s %-13s %6s %6s %6s %6s  %-12s %s\n",
                "sim", "kind", "control", "grid0", "grid", "pixels", "metric", "verdict", "");
    std::printf("  %-11s %-4s %-13s %6s %6s %6s %6s\n\n",
                "", "", "", "  %", "  %", "  %", "moved");

    int dead = 0, conditional = 0, fine = 0, flagged = 0, controls = 0;

    for (const auto& e : bench::registry()) {
        if (!only.empty() && e.id != only) continue;
        auto probe = e.make();
        const auto knobs = probe->knobs();
        const auto sws   = probe->switches();
        const int metricCount = int(probe->metrics().size());
        const int epochs = probe->epoch_name() ? 6 : 0;
        const int steps  = 24;

        for (const auto& k : knobs) {
            ++controls;
            const int ep = k.slow ? (epochs ? 25 : 0) : epochs;
            const int st = k.slow ? 200 : steps;
            const Resp r = probe_knob(e, k, "", 0.0f, ep, st);
            Verdict v = judge(r, metricCount, k.display_only, k.on_reset, st);
            if (k.display_only) {
                // The second reading, paid for only when the first one failed —
                // it costs a run at up to the knob's full range in frames.
                auto still = [](const Resp& x) { return x.f0 == 0 && x.f1 == 0 && x.metrics.empty(); };
                Resp ticks; bool tried = false;
                if (!still(r) && !probe->epoch_name())
                    ticks = probe_knob(e, k, "", 0.0f, ep, st, true, &tried);
                v = judge_display(r, ticks, tried);
            }

            if (std::strcmp(v.tag, "DEAD") != 0) {
                row(e.id.c_str(), "knob", k.key.c_str(), r, v, metricCount);
                if (v.finding) ++flagged; else ++fine;
                continue;
            }

            // Dead at the defaults. Is there a setting of some other knob that
            // brings it to life? Report the first one found — the search is
            // over pairs, so a knob needing two conditions at once shows up as
            // a chain when its enabler is itself conditional.
            std::string foundKey; float foundVal = 0.0f; Resp foundResp;
            for (const auto& other : knobs) {
                if (other.key == k.key || other.display_only) continue;
                for (float ov : samples(other)) {
                    const Resp rr = probe_knob(e, k, other.key, ov, ep, st);
                    if (rr.any()) { foundKey = other.key; foundVal = ov; foundResp = rr; break; }
                }
                if (!foundKey.empty()) break;
            }
            if (foundKey.empty()) {
                row(e.id.c_str(), "knob", k.key.c_str(), r, v, metricCount);
                ++dead; ++flagged;
            } else {
                const bool declared = (k.requires_key == foundKey);
                char b[160];
                std::snprintf(b, sizeof b, "inert until %s = %g  [%s]",
                              foundKey.c_str(), double(foundVal),
                              declared ? "declared" : "NOT DECLARED");
                Verdict cv{"conditional", b, !declared};
                row(e.id.c_str(), "knob", k.key.c_str(), foundResp, cv, metricCount);
                ++conditional;
                if (!declared) ++flagged;
            }
        }

        // Switches were never audited before. A rule bit is a control like any
        // other and can be just as dead.
        for (const auto& sw : sws) {
            ++controls;
            Resp r = probe_switch(e, sw, epochs, steps);
            Verdict v = judge(r, metricCount, false, true, steps);
            // "DEAD" is too strong a word for a rule bit.
            //
            // A switch has no other knob to be conditional on; its enabler is
            // the STARTING PATTERN. Wireworld's H3 says a three-wide bus
            // carries a solid head column, and the shipped circuit has no
            // three-wide bus, so the bit is real and simply never fires. That
            // is still worth reporting — a control the user can flick with no
            // possible effect is the complaint — but reporting it as a coding
            // defect would be wrong. So a dead switch is re-run for forty times
            // as long, and the verdict says which kind it is.
            if (std::strcmp(v.tag, "DEAD") == 0) {
                const int longRun = steps * 40;
                const Resp r2 = probe_switch(e, sw, epochs ? epochs * 4 : 0, longRun);
                if (r2.any()) {
                    char b[160];
                    std::snprintf(b, sizeof b,
                                  "nothing at %d steps, %.1f%% of the grid at %d — the shipped "
                                  "start reaches it late", steps, std::max(r2.f0, r2.f1) * 100.0,
                                  longRun);
                    v = Verdict{"LATE", b, true};
                    r = r2;
                } else {
                    char b[160];
                    std::snprintf(b, sizeof b,
                                  "no effect after %d steps — the shipped start never reaches "
                                  "the condition this bit tests", longRun);
                    v = Verdict{"NEVER FIRES", b, true};
                }
            }
            row(e.id.c_str(), "sw", sw.key.c_str(), r, v, metricCount);
            if (std::strcmp(v.tag, "NEVER FIRES") == 0) { ++dead; ++flagged; }
            else if (v.finding) ++flagged;
            else ++fine;
        }
    }

    std::printf("\n%d controls audited: %d clean, %d conditional, %d dead, %d flagged\n",
                controls, fine, conditional, dead, flagged);
    std::printf("\ngrid0/grid are the %% of field cells that differ at reset and after the run;\n"
                "pixels is the %% of the rendered surface; metric is how many of the sim's own\n"
                "measurements moved. A control that changes under 1%% of the grid and moves no\n"
                "metric is flagged SIDE EFFECT?: that is the size of merely advancing the RNG,\n"
                "and it is what a knob looks like when something else is doing its job.\n");
    return (dead || flagged) ? 1 : 0;
}

// ── numeric claims in user-visible text ────────────────────────────────────

struct Num {
    double      value = 0;
    std::size_t at = 0, len = 0;
};

// Pull every number out of a string, commas and decimal points included.
// "16,054" and "0.98" are single numbers; "B3/S23" yields 3 and 23.
std::vector<Num> numbers(const std::string& s) {
    std::vector<Num> out;
    for (std::size_t i = 0; i < s.size();) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) { ++i; continue; }
        const std::size_t start = i;
        std::string digits;
        while (i < s.size()) {
            const char c = s[i];
            if (std::isdigit(static_cast<unsigned char>(c))) { digits += c; ++i; }
            // A comma or dot only continues the number if a digit follows it.
            else if ((c == ',' || c == '.') && i + 1 < s.size() &&
                     std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
                if (c == '.') digits += '.';
                ++i;
            } else break;
        }
        out.push_back(Num{std::atof(digits.c_str()), start, i - start});
    }
    return out;
}

// The words right after a number, lowercased — that is where the unit lives.
std::string after(const std::string& s, const Num& n, std::size_t chars = 22) {
    std::string t = s.substr(n.at + n.len, chars);
    for (auto& c : t) c = char(std::tolower(static_cast<unsigned char>(c)));
    return t;
}

// A sentence-sized window around the number, for the report.
std::string context(const std::string& s, const Num& n) {
    const std::size_t a = n.at > 34 ? n.at - 34 : 0;
    std::string t = s.substr(a, std::min<std::size_t>(s.size() - a, 74));
    for (auto& c : t) if (c == '\n') c = ' ';
    return t;
}

struct Claim {
    std::string sim, where, text, against;
    double      value = 0;
    int         kind = 0;                // index into kUnits, -1 = no unit
    const char* status = "unmeasured";   // "ok" | "MISMATCH" | "unmeasured"
};

// ── what the program can actually be ───────────────────────────────────────
//
// The first version of this check compared every "1280x800" in the help text
// against the CURRENT field size and called all of them contradictions. That is
// the tool making exactly the mistake it hunts: boids really can be 1280x800,
// the size knob puts it there, and a fact base that only knows the default
// state cannot tell a lie from a setting. So the reachable sizes are measured —
// every on_reset knob set to every position it quantises to, reset, and the
// resulting dimensions recorded — rather than assumed.
struct Dims {
    std::vector<std::pair<int,int>> pairs;
    std::vector<int>                extents;

    void add(int w, int h) {
        if (w <= 0 || h <= 0) return;
        for (const auto& d : pairs) if (d.first == w && d.second == h) return;
        pairs.push_back({w, h});
        for (int e : {w, h}) {
            bool have = false;
            for (int x : extents) if (x == e) have = true;
            if (!have) extents.push_back(e);
        }
    }
    [[nodiscard]] bool hasPair(double a, double b) const {
        for (const auto& d : pairs) if (double(d.first) == a && double(d.second) == b) return true;
        return false;
    }
    [[nodiscard]] bool hasExtent(double v) const {
        for (int x : extents) if (double(x) == v) return true;
        return false;
    }
};

Dims reachable(const bench::Entry& e) {
    Dims d;
    auto base = e.make();
    d.add(base->field().w, base->field().h);
    if (const bench::Surface* sf = base->surface()) d.add(sf->w, sf->h);

    for (const auto& k : base->knobs()) {
        // A size that only takes effect on reset is the only kind that can
        // rebuild the grid, so those are the only ones worth constructing.
        if (!k.on_reset) continue;
        // Sizes a choice knob names outright, without building them.
        for (const auto& ch : k.choices) {
            const auto ns = numbers(ch);
            if (ns.size() == 2) d.add(int(ns[0].value), int(ns[1].value));
        }
        for (float v : samples(k)) {
            auto s = e.make();
            for (auto& kn : s->knobs()) if (kn.key == k.key) kn.value = v;
            s->on_knob(k.key, v);
            s->reset();
            d.add(s->field().w, s->field().h);
            if (const bench::Surface* sf = s->surface()) d.add(sf->w, sf->h);
        }
    }
    return d;
}

// ── units ──────────────────────────────────────────────────────────────────
//
// Grouping the unmeasured numbers by what they claim is most of what makes the
// list usable. "82.6 ms a step" and "51 of them land" fail in different ways
// and are re-derived by different work.
struct Unit { const char* name; const char* marks[6]; };
const Unit kUnits[] = {
    {"timing",      {" ms", "ms ", " us", " second", " minute", nullptr}},
    {"percentage",  {"%", " per cent", " percent", nullptr, nullptr, nullptr}},
    {"steps",       {" step", "-step", " generation", " tick", " gen ", " frame"}},
    {"cells",       {" cell", "-cell", " bit", "-bit", " column", " row"}},
    {"size",        {" wide", "-wide", " tall", " high", " deep", " across"}},
    {"agents",      {" agent", " bird", " particle", " genome", " mob", " thread"}},
};
constexpr int kUnitCount = int(sizeof kUnits / sizeof kUnits[0]);

// The NEAREST unit marker wins, not the first table entry that happens to
// appear anywhere in the window. Scanning in table order put "160^3 is about 34
// ms a step" under timing because "ms" occurs within the window at all — the
// unit of a number is the word that comes right after it, so distance is the
// tie-break and not declaration order.
int unit_of(const std::string& tail) {
    int best = -1; std::size_t bestAt = std::string::npos;
    for (int u = 0; u < kUnitCount; ++u)
        for (const char* m : kUnits[u].marks) {
            if (!m) continue;
            const std::size_t at = tail.find(m);
            if (at != std::string::npos && at < bestAt) { bestAt = at; best = u; }
        }
    return best;
}

int claims(const std::string& only, bool verbose) {
    std::printf("numbers written in user-visible text, checked against the running program\n\n");

    std::vector<Claim> all;
    int mismatches = 0, checked = 0, unmeasured = 0;
    int perUnit[kUnitCount + 1] = {};

    for (const auto& e : bench::registry()) {
        if (!only.empty() && e.id != only) continue;
        auto s = e.make();
        const Dims dims = reachable(e);

        // Everything a user can read.
        std::vector<std::pair<std::string,std::string>> texts;
        const auto& p = s->about();
        texts.push_back({"title", p.title});
        texts.push_back({"who", p.who});
        texts.push_back({"citation", p.citation});
        texts.push_back({"replication", p.replication_note});
        texts.push_back({"blurb", p.blurb});
        if (!s->subtitle().empty()) texts.push_back({"subtitle", s->subtitle()});
        for (const auto& k : s->knobs()) {
            texts.push_back({"knob " + k.key + " label", k.label});
            texts.push_back({"knob " + k.key + " help", k.help});
            for (const auto& ch : k.choices) texts.push_back({"knob " + k.key + " choice", ch});
        }
        for (const auto& w : s->switches()) texts.push_back({"switch " + w.key, w.help});
        for (const auto& m : s->metrics()) texts.push_back({"metric", m.name});

        for (const auto& [where, text] : texts) {
            if (text.empty()) continue;
            const bool citationish = (where == "citation" || where == "who" || where == "title");
            const auto ns = numbers(text);
            for (std::size_t i = 0; i < ns.size(); ++i) {
                const Num& n = ns[i];
                const std::string tail = after(text, n);
                Claim c{e.id, where, context(text, n), "", n.value, unit_of(tail), "unmeasured"};

                // ── the one hard check ────────────────────────────────────
                //
                // Only patterns with unambiguous semantics are checked. A
                // dimension pair written with the digits touching the x —
                // "320x200", "56x32" — can mean nothing else, and the fact base
                // above knows every size the program can be built at. Earlier
                // drafts also checked "N wide" and "N states"; both were
                // dropped after they fired on "a 3-wide bus", "the
                // neighbourhood is 26 wide" and "29-state automaton" (whose
                // palette is 8 render categories over 29 real states, by
                // design). A check that cannot tell a grid from a bus is not a
                // check, and a defect tool that cries wolf is worse than none.
                bool isDim = false;
                if (i + 1 < ns.size() && tail.size() >= 2 &&
                    (tail[0] == 'x' || (unsigned char)tail[0] == 0xC3) &&
                    ns[i+1].at == n.at + n.len + (tail[0] == 'x' ? 1u : 2u)) {
                    isDim = true;
                    const double a = n.value, b = ns[i + 1].value;
                    const bool hit = dims.hasPair(a, b);
                    c.status = hit ? "ok" : "MISMATCH";
                    char b2[96];
                    std::snprintf(b2, sizeof b2, "no reachable size is %gx%g (%zu measured)",
                                  a, b, dims.pairs.size());
                    c.against = b2;
                }

                // A year in a citation is a fact about a book, not about this
                // build, and listing it as unmeasured is noise.
                if (!isDim && citationish && n.value > 1500 && n.value < 2100) continue;
                // The second half of a dimension pair is already accounted for.
                if (i > 0 && isDim == false) {
                    const Num& prev = ns[i - 1];
                    const std::string pt = after(text, prev, 2);
                    if (!pt.empty() && (pt[0] == 'x' || (unsigned char)pt[0] == 0xC3) &&
                        n.at <= prev.at + prev.len + 2)
                        continue;
                }

                if      (std::strcmp(c.status, "MISMATCH") == 0) ++mismatches;
                else if (std::strcmp(c.status, "ok") == 0)       ++checked;
                else { ++unmeasured; ++perUnit[c.kind < 0 ? kUnitCount : c.kind]; }
                all.push_back(c);
            }
        }
    }

    for (const auto& c : all)
        if (std::strcmp(c.status, "MISMATCH") == 0)
            std::printf("  MISMATCH  %-11s %-22s %g — %s\n            \"%s\"\n",
                        c.sim.c_str(), c.where.c_str(), c.value, c.against.c_str(), c.text.c_str());
    if (!mismatches) std::printf("  no dimension in any help string contradicts a size the program can be built at.\n");

    // The unmeasured ones, grouped by what they claim. This is the list the
    // brief asked for and the reason the tool exists: every one of these is a
    // number a human typed into shipped text that nothing re-derives.
    for (int u = 0; u < kUnitCount; ++u) {
        if (!perUnit[u]) continue;
        std::printf("\n  ── %s claims nothing measures (%d) ──\n", kUnits[u].name, perUnit[u]);
        int shown = 0;
        for (const auto& c : all) {
            if (c.kind != u || std::strcmp(c.status, "unmeasured") != 0) continue;
            if (++shown > (verbose ? 10000 : 8)) break;
            std::printf("  %-11s %-22s %-9g \"%s\"\n",
                        c.sim.c_str(), c.where.c_str(), c.value, c.text.c_str());
        }
        if (shown > 8 && !verbose) std::printf("  ... and %d more (-v)\n", perUnit[u] - 8);
    }

    std::printf("\n%d numeric claims in user-visible text.\n", checked + mismatches + unmeasured);
    std::printf("  %4d dimension pairs, checked against %s — %d contradicted.\n",
                checked + mismatches, "every size the sims can actually be built at", mismatches);
    std::printf("  %4d carry a unit and nothing re-derives them:", unmeasured - perUnit[kUnitCount]);
    for (int u = 0; u < kUnitCount; ++u) if (perUnit[u]) std::printf("  %s %d", kUnits[u].name, perUnit[u]);
    std::printf("\n  %4d bare numbers with no unit.\n", perUnit[kUnitCount]);
    std::printf("\nThe dimension pairs are checkable because \"320x200\" can only mean one\n"
                "thing and the program can be asked what sizes it has. Everything else —\n"
                "\"1.05 ms at 1x\", \"51 of them land\", \"about nine steps a column\" — was\n"
                "measured once by a person on one machine and written down. Nothing here or\n"
                "in the suite re-derives any of it, so nothing will notice when it drifts.\n"
                "That is the whole finding; the list above is where to start.\n");
    return mismatches ? 1 : 0;
}
} // namespace

int main(int argc, char** argv) {
    bench::gate("inert");
    std::vector<std::string> args(argv + 1, argv + argc);
    bool wantClaims = false, verbose = false;
    std::string only;
    for (const auto& a : args) {
        if      (a == "--claims") wantClaims = true;
        else if (a == "-v")       verbose = true;
        else                      only = a;
    }
    // A typo'd sim id audited nothing and exited 0 — "0 controls audited: 0
    // clean, 0 dead" reads as a pass. sweep already gets this right; this did
    // not, and a tool that reports green for a name it never found is worse
    // than one that crashes.
    if (!only.empty()) {
        bool known = false;
        for (const auto& e : bench::registry()) if (e.id == only) known = true;
        if (!known) {
            std::printf("inert: no sim called \"%s\". Available:\n", only.c_str());
            for (const auto& e : bench::registry())
                std::printf("  %s\n", e.id.c_str());
            return 1;
        }
    }
    return wantClaims ? claims(only, verbose) : audit(only);
}
