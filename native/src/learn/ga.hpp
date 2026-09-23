// ga.hpp — a real-valued genetic algorithm, and the functions to test it on.
//
// Separate from NEAT on purpose. NEAT evolves a network's weights AND its
// shape, which makes almost every question about selection or crossover
// confounded by topology. A fixed-length real vector on a published benchmark
// function has a known global optimum, so "did the GA work" has an answer
// rather than an impression.
//
// The benchmark set is the standard one, and each is chosen because it breaks a
// different assumption:
//
//   Sphere      — convex, separable, one optimum. If a GA cannot solve this,
//                 nothing else it does means anything.
//   Rastrigin   — Rastrigin 1974; a sphere with a cosine lattice laid over it,
//                 about 10^n local minima. Punishes greedy selection.
//   Rosenbrock  — Rosenbrock 1960; the banana valley. The optimum sits in a
//                 narrow curved trough, so per-coordinate progress is nearly
//                 useless and separable crossover struggles.
//   Griewank    — Griewank 1981; the product term COUPLES the coordinates, so
//                 one-point crossover cutting between them destroys structure.
//   Schwefel    — Schwefel 1981; the global optimum is near a bound and the
//                 second-best basin is at the opposite end of the space, so a
//                 population that converges early converges wrong.
//
// All are minimisation problems with a known optimum of 0 (Schwefel's constant
// is subtracted so it is 0 too), which is what makes them checkable. The GA
// maximises, so fitness is a monotone decreasing transform of the cost.

#pragma once
#include "select.hpp"
#include "../rng.hpp"
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace bench {

enum class Bench { Sphere, Rastrigin, Rosenbrock, Griewank, Schwefel };

[[nodiscard]] inline const char* name_of(Bench b) {
    switch (b) {
        case Bench::Sphere:     return "sphere";
        case Bench::Rastrigin:  return "rastrigin";
        case Bench::Rosenbrock: return "rosenbrock";
        case Bench::Griewank:   return "griewank";
        case Bench::Schwefel:   return "schwefel";
    }
    return "?";
}

// The search box each function is conventionally evaluated on.
[[nodiscard]] inline double bench_range(Bench b) {
    switch (b) {
        case Bench::Sphere:     return 5.12;
        case Bench::Rastrigin:  return 5.12;
        case Bench::Rosenbrock: return 2.048;
        case Bench::Griewank:   return 600.0;
        case Bench::Schwefel:   return 500.0;
    }
    return 5.12;
}

// Where the optimum is, per coordinate. Everything is at the origin except
// Schwefel, whose optimum sits at 420.9687 — near the edge of the box, which is
// precisely what makes it hard.
[[nodiscard]] inline double bench_optimum_at(Bench b) {
    return (b == Bench::Schwefel) ? 420.9687 : 0.0;
}

// Cost, minimised, zero at the optimum.
[[nodiscard]] inline double bench_cost(Bench b, const std::vector<double>& x) {
    const std::size_t n = x.size();
    switch (b) {
        case Bench::Sphere: {
            double s = 0.0;
            for (double v : x) s += v * v;
            return s;
        }
        case Bench::Rastrigin: {
            double s = 10.0 * double(n);
            for (double v : x) s += v * v - 10.0 * std::cos(2.0 * 3.14159265358979323846 * v);
            return s;
        }
        case Bench::Rosenbrock: {
            double s = 0.0;
            for (std::size_t i = 0; i + 1 < n; ++i) {
                const double a = x[i + 1] - x[i] * x[i];
                const double b = 1.0 - x[i];
                s += 100.0 * a * a + b * b;
            }
            return s;
        }
        case Bench::Griewank: {
            double sum = 0.0, prod = 1.0;
            for (std::size_t i = 0; i < n; ++i) {
                sum  += x[i] * x[i] / 4000.0;
                prod *= std::cos(x[i] / std::sqrt(double(i + 1)));
            }
            return sum - prod + 1.0;
        }
        case Bench::Schwefel: {
            // 418.9829*n - sum(x sin sqrt|x|). The constant is the published
            // value that puts the optimum at exactly zero.
            double s = 418.9829 * double(n);
            for (double v : x) s -= v * std::sin(std::sqrt(std::fabs(v)));
            return s;
        }
    }
    return 0.0;
}

// Cost turned into something to maximise.
//
// 1/(1+cost) rather than -cost, because several selection schemes need
// non-negative weights and a bare negation gives none. It is monotone, so it
// changes which numbers come out and not which genome is best — and it
// compresses hard at large cost, which is exactly the regime where raw
// roulette selection stops discriminating. That is a real property of this
// choice and the reason fitness scaling is not optional here.
[[nodiscard]] inline double bench_fitness(Bench b, const std::vector<double>& x) {
    return 1.0 / (1.0 + bench_cost(b, x));
}

// A textbook generational GA, with every decision exposed.
class GA {
public:
    struct Params {
        int       population   = 60;
        int       genes        = 8;
        Bench     problem      = Bench::Rastrigin;
        Selection selection    = Selection::Tournament;
        Scaling   scaling      = Scaling::Rank;
        Crossover crossover    = Crossover::Uniform;
        double    selParam     = 3.0;    // tournament k, or truncation fraction
        double    scaleParam   = 2.0;    // sigma c, linear multiple, or Boltzmann T
        double    crossoverRate= 0.85;
        double    mutationRate = 0.08;   // per gene
        double    mutationStep = 0.15;   // as a fraction of the search range
        int       elites       = 2;      // copied unchanged, the usual safeguard
    };

    void init(const Params& p, std::uint64_t seed) {
        p_ = p;
        p_.population = std::max(4, p_.population);
        p_.genes      = std::max(1, p_.genes);
        rng_.reseed(seed ? seed : 1);
        generation_ = 0;
        const double r = bench_range(p_.problem);
        pop_.assign(std::size_t(p_.population), {});
        for (auto& g : pop_) {
            g.resize(std::size_t(p_.genes));
            for (auto& v : g) v = (double(rng_.unit()) * 2.0 - 1.0) * r;
        }
        evaluate();
    }

    // Resize the population without restarting the run.
    //
    // Growing clones and mutates existing members rather than sampling fresh
    // random ones: a random genome inserted at generation 200 is worthless and
    // merely dilutes the population, which would make every "more population
    // is better" measurement wrong in the same direction.
    void resize(int n) {
        n = std::max(4, n);
        if (n == int(pop_.size())) return;
        if (n < int(pop_.size())) {
            std::vector<std::size_t> idx(pop_.size());
            for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
            std::partial_sort(idx.begin(), idx.begin() + n, idx.end(),
                              [&](std::size_t a, std::size_t b) { return fit_[a] > fit_[b]; });
            std::vector<std::vector<double>> keep;
            keep.reserve(std::size_t(n));
            for (int i = 0; i < n; ++i) keep.push_back(pop_[idx[std::size_t(i)]]);
            pop_.swap(keep);
        } else {
            const std::size_t was = pop_.size();
            while (int(pop_.size()) < n) {
                std::vector<double> child = pop_[std::size_t(double(rng_.unit()) * double(was)) % was];
                mutate(child);
                pop_.push_back(std::move(child));
            }
        }
        p_.population = n;
        evaluate();
    }

    // Replace the worst `n` with fresh random genomes.
    //
    // The standard remedy for a converged population, and the one that makes
    // the collapse visible as something reversible. Deliberately NOT automatic:
    // an immigration rate that fires on its own would mask convergence, which
    // is the thing this sim exists to show.
    void immigrate(int n) {
        n = std::min(std::max(0, n), int(pop_.size()) - 1);
        if (n <= 0) return;
        std::vector<std::size_t> idx(pop_.size());
        for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::partial_sort(idx.begin(), idx.begin() + n, idx.end(),
                          [&](std::size_t a, std::size_t b) { return fit_[a] < fit_[b]; });
        const double r = bench_range(p_.problem);
        for (int i = 0; i < n; ++i)
            for (auto& v : pop_[idx[std::size_t(i)]]) v = (double(rng_.unit()) * 2.0 - 1.0) * r;
        evaluate();
    }

    void step() {
        const auto w = scale_fitness(fit_, p_.scaling, p_.scaleParam);

        // Elites, by raw fitness rather than by scaled weight — scaling exists
        // to shape SELECTION pressure, and letting it decide who is actually
        // best would let a scaling choice change the answer.
        std::vector<std::size_t> idx(pop_.size());
        for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        const int e = std::min(std::max(0, p_.elites), int(pop_.size()) - 1);
        if (e > 0)
            std::partial_sort(idx.begin(), idx.begin() + e, idx.end(),
                              [&](std::size_t a, std::size_t b) { return fit_[a] > fit_[b]; });

        std::vector<std::vector<double>> next;
        next.reserve(pop_.size());
        for (int i = 0; i < e; ++i) next.push_back(pop_[idx[std::size_t(i)]]);

        while (next.size() < pop_.size()) {
            const std::size_t a = select_one(w, p_.selection, rng_, p_.selParam);
            std::vector<double> child;
            if (p_.crossover != Crossover::None && double(rng_.unit()) < p_.crossoverRate) {
                const std::size_t b = select_one(w, p_.selection, rng_, p_.selParam);
                child = crossover_vec(pop_[a], pop_[b], p_.crossover, rng_);
            } else child = pop_[a];
            mutate(child);
            next.push_back(std::move(child));
        }
        pop_.swap(next);
        evaluate();
        ++generation_;
    }

    [[nodiscard]] int    generation()  const { return generation_; }
    [[nodiscard]] int    size()        const { return int(pop_.size()); }
    [[nodiscard]] double best_fitness()const { return bestFit_; }
    [[nodiscard]] double best_cost()   const { return bestCost_; }
    [[nodiscard]] double mean_fitness()const { return meanFit_; }
    [[nodiscard]] const std::vector<double>& best() const { return pop_[bestIdx_]; }
    [[nodiscard]] const std::vector<double>& fitness() const { return fit_; }
    [[nodiscard]] const std::vector<double>& genome(int i) const {
        return pop_[std::size_t(i) % pop_.size()];
    }
    [[nodiscard]] const Params& params() const { return p_; }
    Params& params() { return p_; }

    // Mean pairwise distance between genomes, normalised by the search range.
    //
    // The one number that says whether a GA is still searching. A population
    // that has converged reports a great best-so-far and cannot improve again,
    // and from the fitness curve alone that is indistinguishable from a hard
    // problem. Sampled rather than exhaustive: the full n^2 is real work every
    // generation and the estimate is stable well before it matters.
    [[nodiscard]] double diversity() const {
        const std::size_t n = pop_.size();
        if (n < 2) return 0.0;
        const double r = bench_range(p_.problem);
        double sum = 0.0; int pairs = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i * 7 + 3) % n;      // fixed stride, no RNG in a const method
            if (i == j) continue;
            double d = 0.0;
            for (std::size_t k = 0; k < pop_[i].size(); ++k) {
                const double dv = pop_[i][k] - pop_[j][k];
                d += dv * dv;
            }
            sum += std::sqrt(d); ++pairs;
        }
        if (!pairs) return 0.0;
        return sum / double(pairs) / (r * std::sqrt(double(p_.genes)));
    }

private:
    void mutate(std::vector<double>& g) {
        const double r = bench_range(p_.problem);
        for (auto& v : g) {
            if (double(rng_.unit()) >= p_.mutationRate) continue;
            // Gaussian-ish step from two uniforms, scaled to the search range.
            const double u = double(rng_.unit()) + double(rng_.unit()) - 1.0;
            v += u * p_.mutationStep * r;
            v = std::max(-r, std::min(r, v));      // stay inside the published box
        }
    }

    void evaluate() {
        fit_.assign(pop_.size(), 0.0);
        bestIdx_ = 0; bestFit_ = -1e300; meanFit_ = 0.0;
        for (std::size_t i = 0; i < pop_.size(); ++i) {
            fit_[i] = bench_fitness(p_.problem, pop_[i]);
            meanFit_ += fit_[i];
            if (fit_[i] > bestFit_) { bestFit_ = fit_[i]; bestIdx_ = i; }
        }
        meanFit_ /= double(pop_.size());
        bestCost_ = bench_cost(p_.problem, pop_[bestIdx_]);
    }

    Params p_;
    Rng    rng_{1};
    std::vector<std::vector<double>> pop_;
    std::vector<double> fit_;
    std::size_t bestIdx_ = 0;
    double bestFit_ = 0.0, bestCost_ = 0.0, meanFit_ = 0.0;
    int    generation_ = 0;
};

} // namespace bench
