// fragile.cpp — how close do the stochastic assertions run to their thresholds?
//
// The suite's checks come in two kinds. Most are deterministic: the blinker's
// period is 2, the glider moves one cell diagonally in four generations, and a
// seed either reproduces its run or it does not. Those either pass or they are
// a real bug.
//
// The rest are claims about a random process observed once. "Sphere is solved
// to better than 1e-3" is true of one seed; whether it is true of the algorithm
// is a different question, and a single run cannot tell them apart. This is the
// same mistake as reporting one takeover-time realisation as a property of a
// selection scheme — that read 500 on one seed and 8 on the next.
//
// So: re-run each stochastic claim across many seeds and report how often it
// holds and how much room it has. An assertion that passes 30/30 with two
// orders of magnitude of margin is a fact about the algorithm. One that passes
// 18/30, or 30/30 by a hair, is a fact about the seed that was picked, and it
// will fail on someone else's machine or after an unrelated change moves the
// random stream.
//
// This is not part of the suite — it is what you run when adding a stochastic
// assertion, to find out whether it deserves to be one.
//
// ── and the mirror problem, which bit this file itself ─────────────────────
//
// Every claim below re-runs an assertion that lives in self_test.cpp, which
// means every budget, threshold and repeat count here is a COPY of a number in
// another file. A comment said "kept equal on purpose" and that was the whole
// defence. It failed: the suite's XOR check was strengthened from four-of-five
// to eight-of-ten and this file went on printing a probability for the
// four-of-five check, an assertion the suite no longer contains. A tool whose
// job is to catch a claim that no longer matches the code was itself a claim
// that no longer matched the code.
//
// So the mirrors are now checked against the source at run time: each one names
// the exact text in self_test.cpp that proves the suite still makes that claim,
// and this refuses to print an audit when the text is gone. A comment cannot
// notice an edit in another file; a search can.

#include "learn/ga.hpp"
#include "learn/neat.hpp"
#include "sims/gridworld.hpp"
#include "sims/cartpole.hpp"
#include "sims/pokebattle.hpp"
#include "tool_freshness.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Claim {
    std::string name;
    std::string threshold;
    std::vector<double> values;      // the measured quantity, one per seed
    bool   greater;                  // true if the claim is value > limit
    double limit;
};

void report(const Claim& c) {
    int pass = 0;
    double worst = c.greater ? 1e300 : -1e300, mean = 0.0;
    for (double v : c.values) {
        const bool ok = c.greater ? (v > c.limit) : (v < c.limit);
        if (ok) ++pass;
        worst = c.greater ? std::min(worst, v) : std::max(worst, v);
        mean += v;
    }
    mean /= double(c.values.size());

    // Linear or logarithmic, chosen by how far the values spread.
    //
    // A quantity that ranges over more than a decade — "how much harder is
    // rastrigin than sphere" spans tens of thousands — has a standard deviation
    // dominated by its tail, and comparing a headroom to that says nothing.
    // Measured on the linear scale, a claim whose worst case clears the
    // threshold by a factor of 55,000 came out "FRAGILE". On a logarithmic
    // scale, which is the natural one for a ratio, the same claim is clear by
    // many multiplicative deviations. The rule is stated rather than tuned: use
    // logs when every value is positive and they span over a decade.
    double lo = c.values[0], hi = c.values[0];
    for (double v : c.values) { lo = std::min(lo, v); hi = std::max(hi, v); }
    const bool logScale = (lo > 0.0 && c.limit > 0.0 && hi / lo > 10.0);

    auto tx = [&](double v) { return logScale ? std::log(v) : v; };
    double lmean = 0.0;
    for (double v : c.values) lmean += tx(v);
    lmean /= double(c.values.size());
    double var = 0.0;
    for (double v : c.values) var += (tx(v) - lmean) * (tx(v) - lmean);
    const double sd = std::sqrt(var / double(c.values.size()));

    // Headroom against the SPREAD, not against the threshold.
    //
    // The first version of this reported headroom as a ratio to the threshold,
    // which is a different quantity from the one the assertion tests and gave
    // two useless answers: infinity when the worst value was exactly zero, and
    // "1.07x — tight" for a claim whose real margin was twice what it needed.
    // That is the same mistake this file exists to catch, made by the file
    // itself, which is worth leaving written down.
    //
    // The question an assertion's robustness actually asks is: is the distance
    // from the worst observed value to the threshold large compared with how
    // much the value moves between seeds? If it is not, one more seed flips it.
    const double headroom = std::fabs(worst - c.limit);
    const double sigmas   = (sd > 1e-15) ? std::fabs(tx(worst) - tx(c.limit)) / sd : 1e9;
    const char* verdict = (pass < int(c.values.size())) ? "FAILS SOMETIMES"
                        : (sigmas < 1.0) ? "FRAGILE — inside one seed's spread"
                        : (sigmas < 3.0) ? "thin"
                        : "safe";
    char sig[32];
    if (sigmas >= 1e8) std::snprintf(sig, sizeof sig, "  n/a");
    else               std::snprintf(sig, sizeof sig, "%5.1f", sigmas);
    std::printf("  %-46s %2d/%-2zu  worst %-11.4g headroom %-10.4g %s sd  %s\n",
                c.name.c_str(), pass, c.values.size(), worst, headroom, sig, verdict);
}

// Kept equal to the number in self_test.cpp's battle group — and now verified
// equal rather than asserted equal by a comment. See kMirrors below.
constexpr int kSuiteBattles = 20000;

// Repeats and budgets copied from the suite. Each is proved current by the
// matching needle in kMirrors; changing one here without changing the needle
// makes the check fail loudly, which is the point.
constexpr int kSuiteXorRuns     = 10;   // self_test: for (int run = 0; run < 10; ++run)
constexpr int kSuiteXorRequired = 8;    // self_test: check(solved >= 8, ...)
constexpr int kSuiteGaGens      = 200;
constexpr int kSuiteNeatPop     = 150;

// ── the mirrors ────────────────────────────────────────────────────────────
//
// A needle is a literal fragment of self_test.cpp. It is deliberately the whole
// expression rather than just the number: "0.02" appears all over the suite and
// finding it proves nothing, whereas finding
// "B.learner_rate() > B.greedy_rate() + 0.02" proves that this exact assertion,
// with this exact margin, is the one running.
struct Mirror { const char* what; const char* needle; };
const Mirror kMirrors[] = {
    {"pokebattle budget: 20000 battles",     "B.run(20000)"},
    {"pokebattle: learner beats greedy",     "B.learner_rate() > B.greedy_rate() + 0.02"},
    {"pokebattle: greedy beats random",      "B.greedy_rate() > B.random_rate() + 0.02"},
    {"GA budget: 200 generations",           "for (int g = 0; g < 200; ++g) ga.step();"},
    {"GA: sphere under 1e-3",                "ga.best_cost() < 1e-3"},
    {"gridworld: residual under 1e-4",       "G.bellman_residual(500) < 1e-4f"},
    {"NEAT: ten runs",                       "for (int run = 0; run < 10; ++run)"},
    {"NEAT: at least eight solve",           "check(solved >= 8,"},
    {"NEAT: population 150",                 "neat.init(2, 1, 150,"},
};

// Refuse to audit assertions the suite no longer makes.
bool check_mirrors() {
    const std::string path = bench::Paths::get().inSrc("self_test.cpp");
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("cannot read %s — the mirrors below are UNVERIFIED\n\n", path.c_str());
        return true;                 // say so rather than block on a path problem
    }
    std::ostringstream ss; ss << in.rdbuf();
    const std::string src = ss.str();

    int gone = 0;
    for (const auto& m : kMirrors) {
        if (src.find(m.needle) != std::string::npos) continue;
        if (gone == 0)
            std::printf("STOP — this file mirrors assertions that are no longer in the suite:\n");
        std::printf("   %-38s  not found: %s\n", m.what, m.needle);
        ++gone;
    }
    if (gone) {
        std::printf("\n%d of %zu mirrored claims have moved. Auditing the fragility of an\n"
                    "assertion the suite does not make measures nothing. Update this file to\n"
                    "match self_test.cpp, then run again.\n",
                    gone, sizeof kMirrors / sizeof kMirrors[0]);
        return false;
    }
    std::printf("all %zu mirrored suite constants verified against self_test.cpp\n",
                sizeof kMirrors / sizeof kMirrors[0]);
    return true;
}

const std::vector<std::vector<float>> XOR_X{{0,0},{0,1},{1,0},{1,1}};
const float XOR_Y[4] = {0, 1, 1, 0};

// One NEAT run on XOR; returns the generation it solved at, or -1.
int xor_run(std::uint64_t seed, int& hidden) {
    bench::Neat n;
    n.init(2, 1, kSuiteNeatPop, seed);
    for (int g = 0; g < 150; ++g) {
        for (int i = 0; i < n.size(); ++i) {
            float err = 0; int right = 0;
            for (int k = 0; k < 4; ++k) {
                const float o = n.genome(i).evaluate(XOR_X[std::size_t(k)])[0];
                err += std::fabs(o - XOR_Y[k]);
                if ((o > 0.5f) == (XOR_Y[k] > 0.5f)) ++right;
            }
            n.set_fitness(i, (4.0f - err) * (4.0f - err));
            if (right == 4) { hidden = int(n.genome(i).hidden_nodes()); return g; }
        }
        n.evolve();
    }
    hidden = int(n.best().hidden_nodes());
    return -1;
}

} // namespace

int main(int argc, char** argv) {
    bench::gate("fragile");
    if (!check_mirrors()) return 2;
    const int seeds = (argc > 1) ? std::max(4, std::atoi(argv[1])) : 30;
    std::printf("stochastic assertions across %d seeds\n", seeds);
    std::printf("  %-46s %-6s %-18s %-16s %s\n\n",
                "claim", "holds", "worst seed", "margin", "verdict");

    // ── the GA benchmarks ──────────────────────────────────────────────────
    {
        Claim sphere{"GA: sphere solves to better than 1e-3", "", {}, false, 1e-3};
        Claim gap{"GA: rastrigin is harder than sphere (ratio)", "", {}, true, 1.0};
        for (int s = 1; s <= seeds; ++s) {
            bench::GA a; bench::GA::Params pa; pa.problem = bench::Bench::Sphere;
            a.init(pa, bench::mix_seed(0x5EEDull, s));
            for (int g = 0; g < kSuiteGaGens; ++g) a.step();
            bench::GA b; bench::GA::Params pb; pb.problem = bench::Bench::Rastrigin;
            b.init(pb, bench::mix_seed(0x5EEDull, s));
            for (int g = 0; g < kSuiteGaGens; ++g) b.step();
            sphere.values.push_back(a.best_cost());
            gap.values.push_back(b.best_cost() / std::max(1e-12, a.best_cost()));
        }
        report(sphere);
        report(gap);
    }

    // ── NEAT on XOR ────────────────────────────────────────────────────────
    {
        // The suite asserts 8 of 10 runs solve it. That is itself a stochastic
        // claim about a stochastic claim, so what matters is the underlying
        // per-run solve rate and how much room 8-of-10 has above it.
        //
        // This said "4 of 5" for as long as the suite did, and went on saying it
        // after the suite was strengthened to ten runs requiring eight —
        // printing a probability for an assertion that no longer existed.
        // kMirrors now pins both numbers to text in self_test.cpp, so the next
        // such edit stops this file rather than being narrated past by it.
        int solved = 0; double gens = 0, hid = 0;
        for (int s = 1; s <= seeds; ++s) {
            int h = 0;
            const int g = xor_run(555 + std::uint64_t(s) * 104729, h);
            if (g >= 0) { ++solved; gens += g; hid += h; }
        }
        const double rate = double(solved) / double(seeds);
        std::printf("  %-46s %2d/%-2d  rate  %-12.3f mean gen %5.1f, hidden %.2f\n",
                    "NEAT: XOR solved per run", solved, seeds, rate,
                    gens / std::max(1, solved), hid / std::max(1, solved));
        // Binomial: chance that kSuiteXorRuns draws at this rate give at least
        // kSuiteXorRequired successes. If that is not near one, the suite's
        // check is a coin flip dressed as an assertion.
        //
        // Summed over the whole upper tail rather than enumerated by hand. The
        // old version wrote out the two terms of 4-of-5 as literals, which is
        // most of why retargeting it to 8-of-10 never happened: it was not a
        // number to change, it was an algebra rewrite.
        const double p = rate;
        double pass = 0.0;
        for (int k = kSuiteXorRequired; k <= kSuiteXorRuns; ++k) {
            double c = 1.0;                     // C(n,k), built multiplicatively
            for (int i = 0; i < k; ++i) c = c * double(kSuiteXorRuns - i) / double(i + 1);
            pass += c * std::pow(p, k) * std::pow(1.0 - p, kSuiteXorRuns - k);
        }
        char label[72];
        std::snprintf(label, sizeof label, "  so the suite's %d-of-%d check",
                      kSuiteXorRequired, kSuiteXorRuns);
        std::printf("  %-46s        P(>=%d of %d) = %.3f  %s\n", label,
                    kSuiteXorRequired, kSuiteXorRuns, pass,
                    (pass > 0.95) ? "safe" : "FRAGILE");
    }

    // ── the gridworld's Bellman residual ───────────────────────────────────
    {
        Claim res{"gridworld: residual over well-visited states", "", {}, false, 1e-4};
        for (int s = 1; s <= seeds; ++s) {
            auto sim = bench::make_gridworld(15, 11);
            for (auto& k : sim->knobs()) if (k.key == "seed") k.value = float(s);
            sim->on_knob("seed", float(s));
            sim->reset();
            for (int i = 0; i < 60; ++i) sim->advance_epoch();
            res.values.push_back(double(static_cast<bench::GridWorld*>(sim.get())->bellman_residual(500)));
        }
        report(res);
    }

    // ── the learned Pokemon player against its baselines ───────────────────
    {
        // The suite's claim is an absolute margin, not a ratio: the learner
        // must beat greedy by more than 0.02. Auditing a ratio instead would
        // be auditing a different assertion from the one that can fail.
        Claim beat{"pokebattle: learner - greedy > 0.02", "", {}, true, 0.02};
        for (int s = 1; s <= seeds; ++s) {
            auto sim = bench::make_pokebattle();
            for (auto& k : sim->knobs()) if (k.key == "seed") k.value = float(s);
            sim->on_knob("seed", float(s));
            sim->reset();
            // The same budget the suite uses. Auditing an assertion at a
            // different budget from the one it runs at audits a different
            // assertion — which is the whole failure this file looks for.
            auto& B = *static_cast<bench::PokeBattle*>(sim.get());
            B.run(kSuiteBattles);
            beat.values.push_back(double(B.learner_rate()) - double(B.greedy_rate()));
        }
        report(beat);
    }
    {
        Claim beat{"pokebattle: greedy - random > 0.02", "", {}, true, 0.02};
        for (int s = 1; s <= seeds; ++s) {
            auto sim = bench::make_pokebattle();
            for (auto& k : sim->knobs()) if (k.key == "seed") k.value = float(s);
            sim->on_knob("seed", float(s));
            sim->reset();
            auto& B = *static_cast<bench::PokeBattle*>(sim.get());
            B.run(kSuiteBattles);
            beat.values.push_back(double(B.greedy_rate()) - double(B.random_rate()));
        }
        report(beat);
    }

    std::printf("\nHeadroom is measured in standard deviations of the value across seeds,\n"
                "because that is the question robustness actually asks: is the distance to\n"
                "the threshold large compared with how much the value moves between runs?\n"
                "Under one, the next seed can flip it.\n");
    return 0;
}
