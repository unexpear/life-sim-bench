// neat.hpp — NeuroEvolution of Augmenting Topologies.
//
//   Stanley, K. O. & Miikkulainen, R. "Evolving Neural Networks through
//   Augmenting Topologies", Evolutionary Computation 10(2) (2002) 99-127.
//
// This is the algorithm behind MarI/O (SethBling, 2015). It is not gradient
// descent: there is no backward pass and no derivative. A population of genomes
// is scored, the good ones breed, and mutation adds NODES AND CONNECTIONS as
// well as changing weights — so the network's shape is evolved rather than
// chosen in advance. That is the "augmenting topologies" part, and it is why
// the picture of a NEAT network changes structure while it learns instead of
// only changing edge thickness.
//
// Three things make it work, and all three are in here:
//
//   INNOVATION NUMBERS. Every structural mutation gets a global id, so two
//   genomes that independently grew the same connection can be lined up during
//   crossover. Without this you cannot tell whether two networks share a part
//   or merely have parts in the same array slot.
//
//   SPECIATION. Genomes are grouped by a compatibility distance and compete
//   mainly within their group, so a brand-new structure gets time to be
//   optimised before being judged against long-tuned rivals. New topology is
//   almost always worse at birth; without protection it never survives.
//
//   FITNESS SHARING. A genome's score is divided by its species size, so a
//   species that discovers something good cannot immediately swamp the
//   population with copies of it.
//
// VERIFIED ON XOR, which is the benchmark in Stanley's own paper — a problem
// with no linear solution, so solving it is evidence the algorithm really is
// adding hidden structure and not just tuning a perceptron.

#pragma once
#include "../rng.hpp"
#include "select.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace bench {

struct NeatConn {
    int   from = 0, to = 0;
    float weight = 0.0f;
    bool  enabled = true;
    int   innovation = 0;
};

class NeatGenome {
public:
    int inputs = 0, outputs = 0, nextNode = 0;
    std::vector<NeatConn> conns;
    // Node ids: 0..inputs-1 are inputs, then a bias, then outputs, then hidden.
    [[nodiscard]] int biasNode()   const { return inputs; }
    [[nodiscard]] int outputNode(int i) const { return inputs + 1 + i; }
    [[nodiscard]] int firstHidden() const { return inputs + 1 + outputs; }

    void init(int in, int out) {
        inputs = in; outputs = out;
        nextNode = in + 1 + out;
        conns.clear();
    }

    // Evaluate by repeated relaxation over a topologically sorted order. NEAT
    // networks have arbitrary shape, so there is no layer structure to walk.
    [[nodiscard]] std::vector<float> evaluate(const std::vector<float>& x) const {
        std::map<int, float> value;
        for (int i = 0; i < inputs; ++i) value[i] = (i < int(x.size())) ? x[std::size_t(i)] : 0.0f;
        value[biasNode()] = 1.0f;

        // Order nodes so every node comes after everything feeding it. The
        // graph is kept acyclic when connections are added, so this terminates.
        std::vector<int> order = topological();
        for (int id : order) {
            if (id < inputs || id == biasNode()) continue;
            float sum = 0.0f;
            for (const auto& c : conns)
                if (c.enabled && c.to == id) {
                    auto it = value.find(c.from);
                    if (it != value.end()) sum += it->second * c.weight;
                }
            // The paper's STEEPENED sigmoid, 1/(1+e^-4.9x), not tanh. This is
            // not a detail: the targets are 0 and 1, and tanh spans [-1,1], so
            // a tanh node has to drive its input to exactly zero to output a
            // clean 0. The steep sigmoid saturates hard near both ends, which
            // is what lets a small network commit to a binary answer.
            value[id] = 1.0f / (1.0f + std::exp(-4.9f * sum));
        }
        std::vector<float> out(std::size_t(outputs), 0.0f);
        for (int i = 0; i < outputs; ++i) {
            auto it = value.find(outputNode(i));
            if (it != value.end()) out[std::size_t(i)] = it->second;
        }
        return out;
    }

    [[nodiscard]] std::vector<int> topological() const {
        std::vector<int> nodes;
        for (int i = 0; i < nextNode; ++i) nodes.push_back(i);
        std::vector<int> depth(std::size_t(nextNode), 0);
        // Longest-path depth; safe because the graph is acyclic by construction.
        for (int pass = 0; pass < nextNode; ++pass) {
            bool changed = false;
            for (const auto& c : conns)
                if (c.enabled && c.from < nextNode && c.to < nextNode &&
                    depth[std::size_t(c.to)] < depth[std::size_t(c.from)] + 1) {
                    depth[std::size_t(c.to)] = depth[std::size_t(c.from)] + 1;
                    changed = true;
                }
            if (!changed) break;
        }
        std::sort(nodes.begin(), nodes.end(),
                  [&](int a, int b) { return depth[std::size_t(a)] < depth[std::size_t(b)]; });
        return nodes;
    }

    [[nodiscard]] bool wouldCycle(int from, int to) const {
        if (from == to) return true;
        // Can we get back from `to` to `from`? If so, adding from->to closes a loop.
        std::vector<int> stack{to};
        std::vector<char> seen(std::size_t(nextNode) + 1, 0);
        while (!stack.empty()) {
            const int n = stack.back(); stack.pop_back();
            if (n == from) return true;
            if (n < 0 || n > nextNode || seen[std::size_t(n)]) continue;
            seen[std::size_t(n)] = 1;
            for (const auto& c : conns)
                if (c.enabled && c.from == n) stack.push_back(c.to);
        }
        return false;
    }

    // Nodes ACTUALLY wired in, not the id counter. nextNode ratchets upward
    // across the population through crossover, so counting the id range
    // reported 38 hidden nodes for a genome with 10 connections.
    [[nodiscard]] std::size_t hidden_nodes() const {
        std::size_t n = 0;
        const int first = inputs + 1 + outputs;
        for (int id = first; id < nextNode; ++id)
            for (const auto& c : conns)
                if (c.enabled && (c.from == id || c.to == id)) { ++n; break; }
        return n;
    }
    [[nodiscard]] std::size_t enabled_conns() const {
        std::size_t n = 0;
        for (const auto& c : conns) if (c.enabled) ++n;
        return n;
    }
};

class Neat {
public:
    struct Params {
        float weightMutate   = 0.80f;   // chance a genome perturbs its weights
        float weightReplace  = 0.10f;   // ...and within that, chance of a fresh value
        float addConn        = 0.08f;
        float addNode        = 0.03f;
        float crossoverRate  = 0.75f;
        float c1 = 1.0f, c2 = 1.0f, c3 = 0.4f;   // compatibility coefficients
        float compatThreshold = 3.0f;
        // 0 or >= population means "leave the threshold alone" — a fixed
        // threshold, as in the paper. Anything smaller nudges the threshold
        // each generation toward that many species.
        //
        // The default is no adjustment, and that was measured. XOR at
        // population 150, twenty runs each, AFTER the semantics were fixed
        // (see evolve(); the earlier sweep was measuring a threshold pinned at
        // its 0.3 floor, which is not the same thing as an absent cap):
        //
        //     target    none     40     20     10      5
        //     solved   16/20  15/20  12/20  12/20   9/20
        //     mean gen  75.2   63.3   55.8   74.2   73.9
        //     hidden    4.00   2.40   3.75   3.67   3.78
        //     species   51.1   40.3   18.7   10.8    5.2
        //
        // But the threshold does NOT port between tasks, and that is the more
        // useful finding. At a fixed 3.0 the same code gives 51 species on XOR
        // and exactly ONE on the platformer, for twenty generations running.
        // The reason is in compatibility(): the weight term is divided by N,
        // the gene count of the larger genome. XOR genomes have about five
        // genes and take N = 1 by the paper's small-genome rule; the
        // platformer starts fully connected at 202 links, so every distance is
        // divided by 202 and no threshold tuned on XOR can ever split it.
        //
        // So this is a per-task setting, not a constant. The platformer sets
        // its own: one species means fitness sharing does nothing at all, and
        // over six seeds it cost real distance (162.70 against 182.87).
        int   targetSpecies   = 100000;
        // Generations a species may go without improving before it stops being
        // allowed to breed.
        //
        // This was declared here and used NOWHERE — species stagnation is one
        // of NEAT's three mechanisms, alongside innovation numbers and fitness
        // sharing, and the parameter was a claim the code did not back up. A
        // species that has not improved in fifteen generations is occupying
        // population budget that could be exploring; the paper culls it, and
        // this now does too. See evolve().
        //
        // Default OFF, and that is measured rather than chosen. On XOR at
        // population 150 the per-run solve rate is 1.000 across thirty seeds
        // with stagnation off, and 0.875 with it on at fifteen generations —
        // which also makes the suite's "4 of 5 runs" check fail about one time
        // in eight. The mechanism is real and tested; it is simply not an
        // improvement on this problem, and shipping a default measured worse
        // than the alternative would be choosing the paper over the evidence.
        // Set it per task, the way the species target is set.
        int   staleLimit = 0;

        // ── complexity cap ──────────────────────────────────────────────────
        //
        // NEAT complexifies monotonically: addNode and addConn only ever add,
        // and nothing removes. On a task where a bigger network is not better
        // that is pure cost — every genome carries more genes to evaluate, more
        // weights to tune, and compatibility distance is divided by the gene
        // count, so unchecked growth also quietly dissolves speciation.
        //
        // A cap refuses the structural mutation rather than deleting structure
        // afterwards. Deleting would break the innovation-number alignment that
        // makes NEAT's crossover work at all, which is the one thing in the
        // algorithm that must not be touched.
        //
        // 0 means uncapped. Named that way and checked as `> 0` so it cannot
        // repeat the targetSpecies mistake, where a sentinel of 100000 was
        // silently a different behaviour from no cap.
        int   maxHiddenNodes  = 0;
        int   maxConnections  = 0;

        // How a parent is chosen within a species.
        //
        // The paper kills the worst fraction of each species before breeding.
        // This picked a parent UNIFORMLY from every member, which makes the
        // worst genome in a species exactly as likely to breed as the best —
        // so within a species there was no selection pressure at all, only
        // between them. Measured below.
        // The paper's survival threshold is 0.2 — the top fifth of a species
        // breeds. Measured on XOR at population 150, sixteen runs each:
        //
        //     parent      uniform  trunc50  trunc20  tourn k=3  roulette
        //     solved        13/16    12/16    15/16      13/16     12/16
        //     mean gen       80.2     76.9     74.1       62.5      62.8
        //
        // The spread across sixteen runs is wide enough that only the ordering
        // is worth anything, so the default is the paper's number rather than
        // the winner of this table.
        Selection parentSelection = Selection::Truncate;
        double    parentParam     = 0.2;   // truncation fraction, or tournament k
    };

    void init(int inputs, int outputs, int population, std::uint64_t seed = 0x1EA7ull) {
        rng_.reseed(seed);
        in_ = inputs; out_ = outputs; pop_ = population;
        innovation_ = 0; generation_ = 0;
        staleSpecies_ = 0;
        speciesHistory_.clear();
        history_.clear();
        genomes_.clear();
        for (int i = 0; i < pop_; ++i) {
            NeatGenome g; g.init(in_, out_);
            // Start minimal and fully connected input->output, which is what the
            // paper prescribes: complexity is something to be earned, not
            // assumed. Starting from a big random net is the thing NEAT exists
            // to avoid.
            for (int i2 = 0; i2 <= in_; ++i2)
                for (int o = 0; o < out_; ++o)
                    g.conns.push_back(NeatConn{ i2, g.outputNode(o), rand11() * 2.0f, true,
                                                innov(i2, g.outputNode(o)) });
            genomes_.push_back(std::move(g));
        }
        fitness_.assign(std::size_t(pop_), 0.0f);
    }

    [[nodiscard]] int  size() const { return pop_; }

    // Change the population size without restarting the run.
    //
    // Growing clones and mutates existing genomes rather than generating fresh
    // minimal ones. A fresh zero-hidden genome inserted at generation 200 is
    // not a fair sample of the population — it is a handicap, and it would make
    // every "larger population searches better" reading wrong in the same
    // direction. Shrinking keeps the fittest, because dropping at random would
    // discard the champion a fifth of the time.
    //
    // Returns false if a resize was refused, which happens mid-generation: the
    // caller is part-way through scoring genome k of n and moving the ground
    // under it would score one genome and attribute it to another.
    bool resize(int n) {
        n = std::max(2, n);
        if (n == pop_) return true;
        if (int(fitness_.size()) != pop_) return false;
        if (n < pop_) {
            std::vector<int> idx(static_cast<std::size_t>(pop_), 0);   // braces would be an init-list
            for (int i = 0; i < pop_; ++i) idx[std::size_t(i)] = i;
            std::partial_sort(idx.begin(), idx.begin() + n, idx.end(),
                              [&](int a, int b) { return fitness_[std::size_t(a)] > fitness_[std::size_t(b)]; });
            std::vector<NeatGenome> keep;
            keep.reserve(std::size_t(n));
            for (int i = 0; i < n; ++i) keep.push_back(genomes_[std::size_t(idx[std::size_t(i)])]);
            genomes_.swap(keep);
        } else {
            const std::size_t was = genomes_.size();
            while (int(genomes_.size()) < n) {
                NeatGenome c = genomes_[std::size_t(rng_.unit() * float(was)) % was];
                mutate(c);
                if (!fits_cap(c)) c = genomes_[std::size_t(rng_.unit() * float(was)) % was];
                genomes_.push_back(std::move(c));
            }
        }
        pop_ = n;
        fitness_.assign(std::size_t(pop_), 0.0f);
        bestIndex_ = 0;
        return true;
    }
    [[nodiscard]] int  generation() const { return generation_; }
    [[nodiscard]] const NeatGenome& genome(int i) const { return genomes_[std::size_t(i)]; }
    [[nodiscard]] const NeatGenome& best() const { return genomes_[std::size_t(bestIndex_)]; }
    [[nodiscard]] float best_fitness() const { return bestFitness_; }
    // The population's mean, alongside its champion. Best alone cannot tell a
    // population that is learning from one that got a lucky mutation and is
    // otherwise flat: the champion is a running maximum by construction, so it
    // never goes down. Mean does, and that is the signal.
    [[nodiscard]] float mean_fitness() const { return meanFitness_; }
    [[nodiscard]] std::size_t species_count() const { return speciesCount_; }
    // How many species were frozen out for stagnation this generation. Worth a
    // metric of its own: a run where this climbs is one where most of the
    // population has stopped contributing and the search has narrowed.
    [[nodiscard]] int stale_species() const { return staleSpecies_; }
    // Clamped at zero because the breeding quotas are proportional to summed
    // species fitness, and a negative score there does not mean "less likely to
    // breed" — it corrupts the share arithmetic outright.
    void set_fitness(int i, float f) { fitness_[std::size_t(i)] = std::max(0.0f, f); }

    // One generation: speciate, share fitness, breed.
    void evolve() {
        bestIndex_ = 0;
        for (int i = 1; i < pop_; ++i)
            if (fitness_[std::size_t(i)] > fitness_[std::size_t(bestIndex_)]) bestIndex_ = i;
        bestFitness_ = fitness_[std::size_t(bestIndex_)];
        double sum = 0.0;
        for (int i = 0; i < pop_; ++i) sum += double(fitness_[std::size_t(i)]);
        meanFitness_ = float(sum / double(pop_ > 0 ? pop_ : 1));
        const NeatGenome champion = genomes_[std::size_t(bestIndex_)];

        // ── speciate ───────────────────────────────────────────────────────
        std::vector<std::vector<int>> species;
        std::vector<NeatGenome>       reps;
        for (int i = 0; i < pop_; ++i) {
            bool placed = false;
            for (std::size_t s = 0; s < reps.size(); ++s)
                if (compatibility(genomes_[std::size_t(i)], reps[s]) < p_.compatThreshold) {
                    species[s].push_back(i); placed = true; break;
                }
            if (!placed) { reps.push_back(genomes_[std::size_t(i)]); species.push_back({i}); }
        }
        speciesCount_ = species.size();

        // ── stagnation ─────────────────────────────────────────────────────
        //
        // Species are re-formed from scratch every generation, so "the same
        // species as last time" has to be decided by something that survives
        // the rebuild: each one is matched to the previous generation's record
        // by its representative genome, using the same compatibility measure
        // that formed it. A species with no match is new and starts fresh.
        std::vector<SpeciesRecord> now(species.size());
        for (std::size_t sp = 0; sp < species.size(); ++sp) {
            float best = 0.0f;
            for (int i : species[sp]) best = std::max(best, fitness_[std::size_t(i)]);
            now[sp].rep  = reps[sp];
            now[sp].best = best;
            now[sp].stale = 0;
            now[sp].scratch = -1;
        }
        // Match one-to-one, nearest first.
        //
        // Taking the first record within the threshold let MANY of this
        // generation's species all match the SAME previous record and each
        // inherit its staleness — so a single stale lineage could freeze a
        // dozen healthy new ones. On XOR that took the solve rate from 5 of 5
        // to 3 of 5. Each record may now be claimed once, by the species
        // closest to it.
        std::vector<char> claimed(speciesHistory_.size(), 0);
        for (std::size_t sp = 0; sp < species.size(); ++sp) {
            std::size_t bestPrev = speciesHistory_.size();
            float bestDist = p_.compatThreshold;
            for (std::size_t q = 0; q < speciesHistory_.size(); ++q) {
                if (claimed[q]) continue;
                const float d = compatibility(reps[sp], speciesHistory_[q].rep);
                if (d < bestDist) { bestDist = d; bestPrev = q; }
            }
            if (bestPrev < speciesHistory_.size()) {
                claimed[bestPrev] = 1;
                const auto& prev = speciesHistory_[bestPrev];
                now[sp].stale = (now[sp].best > prev.best + 1e-6f) ? 0 : prev.stale + 1;
                now[sp].best  = std::max(now[sp].best, prev.best);
            }
        }
        // Never cull everything. If every species is stale the run is over
        // anyway, and emptying the population turns a plateau into a crash.
        // The species holding the overall champion is also spared, because
        // losing the best genome to a bookkeeping rule is not stagnation
        // control, it is just losing the best genome.
        std::vector<char> frozen(species.size(), 0);
        if (p_.staleLimit > 0) {
            std::size_t alive = 0;
            for (std::size_t sp = 0; sp < species.size(); ++sp) {
                bool holdsChampion = false;
                for (int i : species[sp]) if (i == bestIndex_) holdsChampion = true;
                const bool cull = now[sp].stale > p_.staleLimit && !holdsChampion;
                frozen[sp] = cull ? 1 : 0;
                if (!cull) ++alive;
            }
            if (alive == 0) std::fill(frozen.begin(), frozen.end(), 0);
        }
        staleSpecies_ = 0;
        for (char c : frozen) if (c) ++staleSpecies_;
        speciesHistory_.swap(now);

        // Nudge the threshold toward a target species count. A fixed threshold
        // cannot work across a run: early genomes are nearly identical and late
        // ones are not, so the same number that gives one species at the start
        // gives seventy at the end — which is just as broken, because a species
        // of one shares fitness with nobody and the protection does nothing.
        //
        // Only when a REAL target is set. targetSpecies = 100000 was meant to
        // say "do not cap the species count"; what it actually did was
        // decrement the threshold every single generation — the count is
        // always below 100000 — until it hit the 0.3 floor. Traced on the
        // platformer: threshold 3.0 -> 1.8 -> 0.6 -> 0.3 while the population
        // was still clonal and reported ONE species, then 1 -> 45 species in a
        // single generation the moment structural mutations accumulated. The
        // XOR sweep that chose "uncapped" was therefore measuring a threshold
        // pinned at its minimum, not an absent cap. Fifth time in this project
        // that the instrument turned out to be measuring something other than
        // what its name said.
        if (p_.targetSpecies > 0 && p_.targetSpecies < pop_) {
            if (int(species.size()) > p_.targetSpecies)      p_.compatThreshold += 0.3f;
            else if (int(species.size()) < p_.targetSpecies) p_.compatThreshold -= 0.3f;
            p_.compatThreshold = std::max(0.3f, p_.compatThreshold);
        }

        // ── fitness sharing ────────────────────────────────────────────────
        // Divide each genome's score by the size of its species, so a good idea
        // cannot immediately fill the population with copies of itself.
        std::vector<float> shared(std::size_t(pop_), 0.0f);
        std::vector<float> speciesScore(species.size(), 0.0f);
        for (std::size_t s = 0; s < species.size(); ++s) {
            if (frozen[s]) continue;                 // stagnant: no share, no quota
            for (int i : species[s]) {
                shared[std::size_t(i)] = fitness_[std::size_t(i)] / float(species[s].size());
                speciesScore[s] += shared[std::size_t(i)];
            }
        }
        float totalScore = 0.0f;
        for (float v : speciesScore) totalScore += v;
        if (totalScore <= 0.0f) totalScore = 1.0f;

        // ── breed ──────────────────────────────────────────────────────────
        std::vector<NeatGenome> next;
        next.push_back(champion);                        // elitism: never lose the best
        for (std::size_t s = 0; s < species.size() && int(next.size()) < pop_; ++s) {
            if (frozen[s]) continue;
            int quota = std::max(1, int(float(pop_) * speciesScore[s] / totalScore));
            std::vector<int> members = species[s];
            std::sort(members.begin(), members.end(),
                      [&](int a, int b) { return fitness_[std::size_t(a)] > fitness_[std::size_t(b)]; });
            // Weights for choosing a parent WITHIN this species, in the
            // species' own fitness order. Selection used to be uniform over
            // every member, which meant the worst genome in a species bred as
            // often as the best and the only pressure in the whole algorithm
            // was between species, not inside them.
            std::vector<double> w(members.size());
            for (std::size_t m = 0; m < members.size(); ++m)
                w[m] = double(fitness_[std::size_t(members[m])]);
            for (int k = 0; k < quota && int(next.size()) < pop_; ++k) {
                const int a = members[select_one(w, p_.parentSelection, rng_, p_.parentParam)];
                NeatGenome child;
                if (members.size() > 1 && rng_.unit() < p_.crossoverRate) {
                    const int b = members[select_one(w, p_.parentSelection, rng_, p_.parentParam)];
                    child = crossover(genomes_[std::size_t(a)], genomes_[std::size_t(b)],
                                      fitness_[std::size_t(a)], fitness_[std::size_t(b)]);
                } else child = genomes_[std::size_t(a)];
                mutate(child);
                // Enforce the cap on the FINISHED child, not only on the
                // mutation. Refusing addNode is not enough: crossover of two
                // parents that are each at the cap but carry DIFFERENT hidden
                // nodes produces a child with both, and measured that way a cap
                // of 1 let 3 hidden nodes through. The remedy is to fall back to
                // a mutated copy of the fitter parent rather than to delete
                // genes — deleting would break the innovation alignment that
                // makes NEAT's crossover work, which is the one thing in the
                // algorithm that must not be touched.
                if (!fits_cap(child)) { child = genomes_[std::size_t(a)]; mutate(child); }
                if (!fits_cap(child)) child = genomes_[std::size_t(a)];
                next.push_back(std::move(child));
            }
        }
        while (int(next.size()) < pop_) {
            const std::size_t pick = std::size_t(rng_.unit() * float(pop_)) % std::size_t(pop_);
            NeatGenome c = genomes_[pick];
            mutate(c);
            if (!fits_cap(c)) c = genomes_[pick];
            next.push_back(std::move(c));
        }
        genomes_.swap(next);
        genomes_.resize(std::size_t(pop_));
        fitness_.assign(std::size_t(pop_), 0.0f);
        ++generation_;
    }

    Params& params() { return p_; }

private:
    float rand11() { return rng_.unit() * 2.0f - 1.0f; }

    // A global id per (from,to) structural innovation, so the same mutation
    // arising in two genomes gets the same number and crossover can align them.
    int innov(int from, int to) {
        const auto key = std::make_pair(from, to);
        auto it = history_.find(key);
        if (it != history_.end()) return it->second;
        history_[key] = innovation_;
        return innovation_++;
    }

    [[nodiscard]] float compatibility(const NeatGenome& a, const NeatGenome& b) const {
        std::size_t matching = 0, disjoint = 0;
        float weightDiff = 0.0f;
        std::map<int, const NeatConn*> ma, mb;
        for (const auto& c : a.conns) ma[c.innovation] = &c;
        for (const auto& c : b.conns) mb[c.innovation] = &c;
        for (const auto& [k, ca] : ma) {
            auto it = mb.find(k);
            if (it != mb.end()) { ++matching; weightDiff += std::fabs(ca->weight - it->second->weight); }
            else ++disjoint;
        }
        for (const auto& [k, cb] : mb) { (void)cb; if (ma.find(k) == ma.end()) ++disjoint; }
        // N normalises for genome size — but the paper is explicit that N is
        // set to 1 when both genomes are small (under 20 genes), and that is not
        // a nicety. With N left as the gene count, a 3-gene genome differing in
        // 3 genes scores 3/8 = 0.375 against a threshold of 3.0, so NOTHING ever
        // separates: the whole population stays one species, fitness sharing
        // does nothing, and every new topology is judged immediately against
        // long-tuned rivals and dies. Measured before this fix: one species
        // throughout, fitness stuck at exactly the 3-of-4 ceiling a linear
        // solution reaches, which is the precise symptom of new structure never
        // being given time.
        const std::size_t genes = std::max(a.conns.size(), b.conns.size());
        const float N = (genes < 20) ? 1.0f : float(genes);
        const float W = matching ? weightDiff / float(matching) : 0.0f;
        return p_.c1 * float(disjoint) / N + p_.c3 * W;
    }

    NeatGenome crossover(const NeatGenome& a, const NeatGenome& b, float fa, float fb) {
        const NeatGenome& fit   = (fa >= fb) ? a : b;
        const NeatGenome& other = (fa >= fb) ? b : a;
        NeatGenome child; child.init(in_, out_);
        child.nextNode = std::max(a.nextNode, b.nextNode);
        std::map<int, const NeatConn*> mo;
        for (const auto& c : other.conns) mo[c.innovation] = &c;
        // Matching genes are inherited at random from either parent; disjoint
        // and excess genes come from the FITTER parent only.
        for (const auto& c : fit.conns) {
            NeatConn g = c;
            auto it = mo.find(c.innovation);
            if (it != mo.end() && rng_.unit() < 0.5f) g.weight = it->second->weight;
            child.conns.push_back(g);
        }
        return child;
    }

    void mutate(NeatGenome& g) {
        if (rng_.unit() < p_.weightMutate)
            for (auto& c : g.conns)
                c.weight = (rng_.unit() < p_.weightReplace) ? rand11() * 2.0f
                                                            : c.weight + rand11() * 0.25f;
        // Weight mutation is never refused by the cap. Only structural growth
        // is capped, so a capped population still searches its weight space at
        // full rate — a cap that froze the whole genome would be a different
        // and much worse thing wearing the same name.
        if (rng_.unit() < p_.addConn && conn_room(g)) addConnection(g);
        if (rng_.unit() < p_.addNode && node_room(g)) addNode(g);
    }

    // Whether a finished genome is inside the caps.
    [[nodiscard]] bool fits_cap(const NeatGenome& g) const {
        if (p_.maxHiddenNodes > 0 && int(g.hidden_nodes()) > p_.maxHiddenNodes) return false;
        if (p_.maxConnections > 0 && int(g.conns.size())  > p_.maxConnections) return false;
        return true;
    }

    [[nodiscard]] bool node_room(const NeatGenome& g) const {
        return p_.maxHiddenNodes <= 0 || int(g.hidden_nodes()) < p_.maxHiddenNodes;
    }
    [[nodiscard]] bool conn_room(const NeatGenome& g) const {
        // A node split adds two connections, so a genome one under the cap
        // cannot take one — checking here keeps the cap a real bound rather
        // than one it can step over.
        return p_.maxConnections <= 0 || int(g.conns.size()) + 1 < p_.maxConnections;
    }

    void addConnection(NeatGenome& g) {
        for (int attempt = 0; attempt < 20; ++attempt) {
            const int from = int(rng_.unit() * float(g.nextNode)) % g.nextNode;
            const int to   = g.inputs + 1 +
                             int(rng_.unit() * float(g.nextNode - g.inputs - 1)) %
                             std::max(1, g.nextNode - g.inputs - 1);
            if (to <= g.inputs) continue;                 // never feed an input or the bias
            if (g.wouldCycle(from, to)) continue;          // keep it feed-forward
            bool exists = false;
            for (const auto& c : g.conns) if (c.from == from && c.to == to) { exists = true; break; }
            if (exists) continue;
            g.conns.push_back(NeatConn{ from, to, rand11() * 2.0f, true, innov(from, to) });
            return;
        }
    }

    // Split an existing connection with a new node. The old connection is
    // DISABLED rather than removed, and the two new ones are weighted 1 and
    // (old weight) so the network's behaviour is initially unchanged — a
    // structural change that broke behaviour immediately would always be
    // selected against before it could be optimised.
    void addNode(NeatGenome& g) {
        if (g.conns.empty()) return;
        std::vector<int> live;
        for (std::size_t i = 0; i < g.conns.size(); ++i) if (g.conns[i].enabled) live.push_back(int(i));
        if (live.empty()) return;
        const int pick = live[std::size_t(rng_.unit() * float(live.size())) % live.size()];
        NeatConn& old = g.conns[std::size_t(pick)];
        old.enabled = false;
        const int newNode = g.nextNode++;
        g.conns.push_back(NeatConn{ old.from, newNode, 1.0f, true, innov(old.from, newNode) });
        g.conns.push_back(NeatConn{ newNode, old.to, old.weight, true, innov(newNode, old.to) });
    }

    Params p_;
    std::vector<NeatGenome> genomes_;
    std::vector<float>      fitness_;
    std::map<std::pair<int,int>, int> history_;
    int in_ = 0, out_ = 0, pop_ = 0, innovation_ = 0, generation_ = 0, bestIndex_ = 0;
    // One species as it stood last generation, so stagnation can be measured
    // across a rebuild that does not preserve identity.
    struct SpeciesRecord { NeatGenome rep; float best = 0.0f; int stale = 0; int scratch = -1; };
    std::vector<SpeciesRecord> speciesHistory_;
    int   staleSpecies_ = 0;
    float bestFitness_ = 0.0f;
    float meanFitness_ = 0.0f;
    std::size_t speciesCount_ = 0;
    Rng rng_;
};

} // namespace bench
