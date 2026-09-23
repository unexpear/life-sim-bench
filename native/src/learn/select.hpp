// select.hpp — selection, fitness scaling and crossover, as published.
//
// These are the three decisions a genetic algorithm actually makes, and they
// are usually buried inside whatever GA you are reading. Pulling them out means
// they can be swapped, swept, and — the part that matters here — checked
// against the properties their papers claim for them.
//
// Sources:
//   Holland, Adaptation in Natural and Artificial Systems, 1975 — the GA, and
//     fitness-proportionate ("roulette") selection.
//   Goldberg, Genetic Algorithms in Search, Optimization and Machine Learning,
//     1989 — linear fitness scaling, sigma truncation, the standard treatment.
//   Baker, "Adaptive Selection Methods for Genetic Algorithms", ICGA 1985 —
//     rank selection and stochastic universal sampling.
//   Goldberg & Deb, "A Comparative Analysis of Selection Schemes Used in
//     Genetic Algorithms", FOGA 1991 — takeover time, and why roulette on raw
//     fitness behaves badly.
//   Syswerda, "Uniform Crossover in Genetic Algorithms", ICGA 1989.
//
// The reason all of this is worth having rather than hard-coding one scheme:
// fitness-proportionate selection on raw fitness is scale-dependent in a way
// that quietly breaks. Add 1000 to every score and nothing changes about the
// problem, but every selection probability collapses toward uniform and the
// search stops. That is not a subtle effect and it is invisible from the
// outside — the GA still runs, it just stops improving. Scaling exists to fix
// exactly that, and the self-test asserts the failure and the fix.

#pragma once
#include "../rng.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <string>
#include <vector>

namespace bench {

// ── how a parent is chosen ──────────────────────────────────────────────────
enum class Selection {
    Uniform,      // no pressure at all — the control, and a real baseline
    Roulette,     // fitness-proportionate (Holland 1975)
    Tournament,   // pick k at random, best wins — pressure set by k
    Rank,         // probability from rank, not from value (Baker 1985)
    Truncate      // top fraction breed, uniformly among themselves
};

// ── how raw scores become selection weights ─────────────────────────────────
enum class Scaling {
    Raw,          // use fitness as-is
    Rank,         // replace by rank, 1..n — scale-free by construction
    Sigma,        // f' = f - (mean - c*sd), clamped at 0 (Goldberg 1989)
    Linear,       // f' = a*f + b so mean maps to mean and max to c*mean
    Boltzmann     // exp(f / T) — pressure rises as T falls
};

// ── how two parents combine ─────────────────────────────────────────────────
enum class Crossover {
    None,         // asexual: the child is a mutated copy of one parent
    OnePoint,     // classic single cut (Holland 1975)
    TwoPoint,     // two cuts, so the ends can travel together
    Uniform,      // per-gene coin flip (Syswerda 1989)
    Arithmetic    // real-valued blend: c = t*a + (1-t)*b
};

[[nodiscard]] inline const char* name_of(Selection s) {
    switch (s) {
        case Selection::Uniform:    return "uniform";
        case Selection::Roulette:   return "roulette";
        case Selection::Tournament: return "tournament";
        case Selection::Rank:       return "rank";
        case Selection::Truncate:   return "truncate";
    }
    return "?";
}
[[nodiscard]] inline const char* name_of(Scaling s) {
    switch (s) {
        case Scaling::Raw:       return "raw";
        case Scaling::Rank:      return "rank";
        case Scaling::Sigma:     return "sigma";
        case Scaling::Linear:    return "linear";
        case Scaling::Boltzmann: return "boltzmann";
    }
    return "?";
}
[[nodiscard]] inline const char* name_of(Crossover c) {
    switch (c) {
        case Crossover::None:       return "none";
        case Crossover::OnePoint:   return "one-point";
        case Crossover::TwoPoint:   return "two-point";
        case Crossover::Uniform:    return "uniform";
        case Crossover::Arithmetic: return "arithmetic";
    }
    return "?";
}

// Scale raw fitness into non-negative selection weights.
//
// Every scheme returns weights >= 0, because everything downstream divides by
// their sum. A negative weight there does not mean "less likely to be chosen";
// it corrupts the arithmetic outright and can make a bad genome the most likely
// parent in the population.
//
// `param` is the scheme's one free constant: c for Sigma (Goldberg uses 2) and
// Linear (the multiple of the mean the best gets, usually 1.2–2.0), and the
// temperature T for Boltzmann.
inline std::vector<double> scale_fitness(const std::vector<double>& f, Scaling how,
                                         double param = 2.0) {
    const std::size_t n = f.size();
    std::vector<double> w(n, 0.0);
    if (n == 0) return w;

    switch (how) {
        case Scaling::Raw: {
            // Shift so the worst sits at zero. Raw fitness may be negative —
            // a cost, a log-likelihood — and roulette cannot use that at all.
            const double lo = *std::min_element(f.begin(), f.end());
            for (std::size_t i = 0; i < n; ++i) w[i] = f[i] - std::min(0.0, lo);
            break;
        }
        case Scaling::Rank: {
            // Worst gets 1, best gets n. Scale-free: adding a constant to every
            // fitness, or multiplying by one, changes nothing at all — which is
            // the entire point, and the property the self-test checks.
            std::vector<std::size_t> idx(n);
            std::iota(idx.begin(), idx.end(), std::size_t(0));
            std::stable_sort(idx.begin(), idx.end(),
                             [&](std::size_t a, std::size_t b) { return f[a] < f[b]; });
            for (std::size_t r = 0; r < n; ++r) w[idx[r]] = double(r + 1);
            break;
        }
        case Scaling::Sigma: {
            // f' = f - (mean - c*sd), floored at zero. Goldberg's sigma
            // truncation: it discards everything more than c standard
            // deviations below the mean, so an outlier cannot dominate early
            // and the pressure does not vanish late.
            double mean = 0.0;
            for (double v : f) mean += v;
            mean /= double(n);
            double var = 0.0;
            for (double v : f) var += (v - mean) * (v - mean);
            const double sd = std::sqrt(var / double(n));
            if (sd <= 0.0) { std::fill(w.begin(), w.end(), 1.0); break; }
            for (std::size_t i = 0; i < n; ++i)
                w[i] = std::max(0.0, f[i] - (mean - param * sd));
            break;
        }
        case Scaling::Linear: {
            // f' = a*f + b chosen so the mean is preserved and the best gets
            // `param` times the mean. If that would make the worst negative —
            // which happens once the population converges around a few
            // outliers — fall back to mapping the WORST to zero instead, which
            // is Goldberg's own remedy rather than a fudge.
            double mean = 0.0, hi = f[0], lo = f[0];
            for (double v : f) { mean += v; hi = std::max(hi, v); lo = std::min(lo, v); }
            mean /= double(n);
            if (hi - mean < 1e-12) { std::fill(w.begin(), w.end(), 1.0); break; }
            double a = (param - 1.0) * mean / (hi - mean);
            double b = mean - a * mean;
            if (a * lo + b < 0.0) {
                if (mean - lo < 1e-12) { std::fill(w.begin(), w.end(), 1.0); break; }
                a = mean / (mean - lo);
                b = -a * lo;
            }
            for (std::size_t i = 0; i < n; ++i) w[i] = std::max(0.0, a * f[i] + b);
            break;
        }
        case Scaling::Boltzmann: {
            // exp((f - max) / T). Subtracting the max first is not cosmetic:
            // exp(700) overflows a double, and a fitness of a few thousand is
            // ordinary. The shift cancels exactly in the ratio.
            const double T  = (param > 1e-9) ? param : 1e-9;
            const double hi = *std::max_element(f.begin(), f.end());
            for (std::size_t i = 0; i < n; ++i) w[i] = std::exp((f[i] - hi) / T);
            break;
        }
    }
    return w;
}

// Choose one index. `w` are non-negative weights, usually from scale_fitness.
//
// `param` is the tournament size k for Tournament, and the surviving fraction
// for Truncate (the paper's NEAT keeps the top 20%).
inline std::size_t select_one(const std::vector<double>& w, Selection how, Rng& rng,
                              double param = 3.0) {
    const std::size_t n = w.size();
    if (n == 0) return 0;
    if (n == 1) return 0;

    switch (how) {
        case Selection::Uniform:
            return std::size_t(double(rng.unit()) * double(n)) % n;

        case Selection::Roulette: {
            double total = 0.0;
            for (double v : w) total += std::max(0.0, v);
            // Every weight zero is a real state — a population where nothing
            // has scored yet — and dividing by it would pick index 0 forever
            // while looking like it was choosing.
            if (total <= 0.0) return std::size_t(double(rng.unit()) * double(n)) % n;
            double r = double(rng.unit()) * total;
            for (std::size_t i = 0; i < n; ++i) {
                r -= std::max(0.0, w[i]);
                if (r <= 0.0) return i;
            }
            return n - 1;                     // only reachable through rounding
        }

        case Selection::Tournament: {
            const int k = std::max(2, int(param + 0.5));
            std::size_t best = std::size_t(double(rng.unit()) * double(n)) % n;
            for (int i = 1; i < k; ++i) {
                const std::size_t c = std::size_t(double(rng.unit()) * double(n)) % n;
                if (w[c] > w[best]) best = c;
            }
            return best;
        }

        case Selection::Rank: {
            // Rank the weights here rather than assuming they arrived ranked,
            // so Selection::Rank is meaningful whatever scaling was applied.
            std::vector<std::size_t> idx(n);
            std::iota(idx.begin(), idx.end(), std::size_t(0));
            std::stable_sort(idx.begin(), idx.end(),
                             [&](std::size_t a, std::size_t b) { return w[a] < w[b]; });
            const double total = double(n) * double(n + 1) / 2.0;
            double r = double(rng.unit()) * total;
            for (std::size_t pos = 0; pos < n; ++pos) {
                r -= double(pos + 1);
                if (r <= 0.0) return idx[pos];
            }
            return idx[n - 1];
        }

        case Selection::Truncate: {
            const double frac = std::min(1.0, std::max(1.0 / double(n), param));
            const std::size_t keep = std::max<std::size_t>(1, std::size_t(double(n) * frac));
            std::vector<std::size_t> idx(n);
            std::iota(idx.begin(), idx.end(), std::size_t(0));
            std::partial_sort(idx.begin(), idx.begin() + std::ptrdiff_t(keep), idx.end(),
                              [&](std::size_t a, std::size_t b) { return w[a] > w[b]; });
            return idx[std::size_t(double(rng.unit()) * double(keep)) % keep];
        }
    }
    return 0;
}

// Combine two equal-length real vectors.
//
// The genome here is a vector of doubles, which covers the real-valued
// benchmark problems. NEAT keeps its own innovation-aligned crossover, because
// aligning two networks by gene position rather than by innovation number is
// the exact mistake NEAT was invented to avoid.
inline std::vector<double> crossover_vec(const std::vector<double>& a,
                                         const std::vector<double>& b,
                                         Crossover how, Rng& rng) {
    const std::size_t n = std::min(a.size(), b.size());
    std::vector<double> c = a;
    if (n == 0) return c;

    switch (how) {
        case Crossover::None:
            break;
        case Crossover::OnePoint: {
            const std::size_t cut = std::size_t(double(rng.unit()) * double(n)) % n;
            for (std::size_t i = cut; i < n; ++i) c[i] = b[i];
            break;
        }
        case Crossover::TwoPoint: {
            std::size_t p = std::size_t(double(rng.unit()) * double(n)) % n;
            std::size_t q = std::size_t(double(rng.unit()) * double(n)) % n;
            if (p > q) std::swap(p, q);
            for (std::size_t i = p; i <= q && i < n; ++i) c[i] = b[i];
            break;
        }
        case Crossover::Uniform:
            for (std::size_t i = 0; i < n; ++i) if (rng.unit() < 0.5f) c[i] = b[i];
            break;
        case Crossover::Arithmetic: {
            const double t = double(rng.unit());
            for (std::size_t i = 0; i < n; ++i) c[i] = t * a[i] + (1.0 - t) * b[i];
            break;
        }
    }
    return c;
}

// Selection pressure, measured rather than assumed.
//
// Goldberg & Deb define takeover time as the generations for the best
// individual's copies to fill the population under selection ALONE — no
// crossover, no mutation. It is the honest way to compare schemes, because
// "tournament size 5" and "Boltzmann at T = 0.1" are otherwise incomparable
// numbers.
//
// Their analysis is of the deterministic proportion recurrence, which assumes
// the best is never lost. A single realisation can lose it: with one copy in
// sixty-four, the chance a binary tournament never picks it in a whole
// generation is (63/64)^128 = 13%, and once gone it cannot come back. Measuring
// one run therefore returned "never took over" for schemes with plenty of
// pressure — binary tournament read 500 on one seed and 8 on the next, and the
// 500 was drift, not a property of the scheme. Seventh time in this project
// that the instrument was measuring something other than its name.
//
// So: several independent realisations, report the median, and report the
// losses separately instead of folding them into the number. The loss rate is
// worth having on its own — it is the entire reason elitism exists.
struct Takeover {
    int median = 0;   // generations for the best to fill the population
    int lost   = 0;   // realisations in which the best was lost to drift
    int trials = 0;
    [[nodiscard]] bool never() const { return median >= 100000; }
};

[[nodiscard]] inline Takeover takeover_time(const std::vector<double>& fitness, Selection how,
                                            Scaling scaling, Rng& rng, double selParam = 3.0,
                                            double scaleParam = 2.0, int limit = 500,
                                            int trials = 9) {
    Takeover out;
    const std::size_t n = fitness.size();
    if (n < 2) return out;
    const double bestVal = *std::max_element(fitness.begin(), fitness.end());

    std::vector<int> gens;
    for (int t = 0; t < trials; ++t) {
        std::vector<double> pop = fitness;
        bool done = false;
        for (int g = 1; g <= limit && !done; ++g) {
            const auto w = scale_fitness(pop, scaling, scaleParam);
            std::vector<double> next(n);
            for (std::size_t i = 0; i < n; ++i) next[i] = pop[select_one(w, how, rng, selParam)];
            pop.swap(next);
            std::size_t copies = 0;
            for (double v : pop) if (v >= bestVal) ++copies;
            if (copies == 0) { ++out.lost; done = true; }      // drift, not pressure
            else if (copies == n) { gens.push_back(g); done = true; }
        }
        if (!done) gens.push_back(limit + 1);                  // genuinely no takeover
    }
    out.trials = trials;
    if (gens.empty()) { out.median = 100000; return out; }     // every run lost it
    std::sort(gens.begin(), gens.end());
    out.median = gens[gens.size() / 2];
    if (out.median > limit) out.median = 100000;
    return out;
}

} // namespace bench
