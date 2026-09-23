// dynscript.hpp — dynamic scripting: learn WEIGHTS OVER RULES, not a value function.
//
//   Spronck, P., Ponsen, M., Sprinkhuizen-Kuyper, I., Postma, E.
//   "Adaptive Game AI with Dynamic Scripting", Machine Learning 63(3) (2006)
//   217-248.
//   Ponsen, M. & Spronck, P. "Improving Adaptive Game AI with Evolutionary
//   Learning", CGAIDE 2004 — a separate rulebase per tech-tree state.
//
// WHY THIS AND NOT MORE DQN. Measured in this project: a hand-written priority
// list reaches mean tech-tree rung 4.5 and the top of the tree; a deep Q-network
// on the identical action space and senses reaches ~0. That gap is not a bug —
// it is the published result. No MineRL research-track entry obtained a diamond
// in three years of competition, and the two systems that ever did from scratch
// spent 70,000 hours of video and 720 GPUs, or 30 million environment steps.
// This sim runs thirty thousand ticks. Three orders of magnitude is not a
// tuning problem.
//
// So put the learning where the evidence says it belongs: INSIDE the
// hand-authored structure rather than in place of it. Dynamic scripting keeps
// the rules a person wrote — which are what already works — and learns only
// which of them to use and in what order. It is the technique commercial games
// actually shipped for adaptive AI, it needs no network, no bootstrap, no
// discount factor and no gradient, and the whole thing is integer-friendly
// arithmetic over a few hundred weights.
//
// THE ALGORITHM, exactly as published:
//
//   1. Every rule in the rulebase carries a weight, all starting equal.
//   2. At the start of an episode, draw `scriptSize` DISTINCT rules by roulette
//      wheel on weight, and order them by their authored priority. That ordered
//      list is the script the agent runs.
//   3. Play the episode. Compute a fitness in [0,1].
//   4. Adjust the weight of every rule that was IN the script:
//
//          F >= b :  dW = +Rmax * (F - b) / (1 - b)
//          F <  b :  dW = -Pmax * (b - F) / b
//
//      where b is the break-even fitness. Clip to [Wmin, Wmax].
//   5. REDISTRIBUTE the total weight change over the rules that were not in the
//      script, so the sum of all weights is conserved. Without this the whole
//      rulebase drifts up or down together and the roulette wheel stops
//      discriminating — the step everyone forgets, and the reason a naive
//      implementation appears to learn nothing.
//
// The conservation in step 5 is what makes this a competition BETWEEN rules
// rather than an absolute score for each, and it is why the method needs no
// learning rate schedule to stay stable.

#pragma once
#include "../rng.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace bench {

struct DynScriptConfig {
    double wInit = 100.0;
    double wMin  = 0.0;
    double wMax  = 2000.0;
    double rMax  = 100.0;    // largest reward to a rule in a winning script
    double pMax  = 100.0;    // largest penalty in a losing one
    double breakEven = 0.30; // fitness above which a script counts as a success
    int    scriptSize = 8;
};

class DynamicScript {
public:
    DynamicScript() = default;
    // `priority` orders the rules WITHIN a generated script; lower runs first.
    // It is authored, not learned: dynamic scripting learns which rules are in
    // the script, not what a sensible order is once they are.
    DynamicScript(int rules, std::vector<int> priority, DynScriptConfig cfg, std::uint64_t seed)
        : cfg_(cfg), prio_(std::move(priority)), rng_(seed) {
        w_.assign(std::size_t(rules), cfg_.wInit);
        if (prio_.size() != w_.size()) prio_.assign(w_.size(), 0);
    }

    [[nodiscard]] std::size_t rules() const { return w_.size(); }
    [[nodiscard]] const std::vector<double>& weights() const { return w_; }
    [[nodiscard]] const std::vector<int>& script() const { return script_; }
    [[nodiscard]] int episodes() const { return episodes_; }
    [[nodiscard]] double total_weight() const {
        return std::accumulate(w_.begin(), w_.end(), 0.0);
    }

    // Draw a fresh script. Roulette wheel WITHOUT replacement, so a heavy rule
    // cannot fill the script with copies of itself.
    void new_script() {
        script_.clear();
        if (w_.empty()) return;
        const int want = std::min<int>(cfg_.scriptSize, int(w_.size()));
        std::vector<char> taken(w_.size(), 0);
        for (int pick = 0; pick < want; ++pick) {
            double total = 0.0;
            for (std::size_t i = 0; i < w_.size(); ++i) if (!taken[i]) total += w_[i];
            if (total <= 0.0) {
                // Every remaining weight has bottomed out. Take uniformly rather
                // than looping forever or silently returning a short script.
                std::vector<int> left;
                for (std::size_t i = 0; i < w_.size(); ++i) if (!taken[i]) left.push_back(int(i));
                if (left.empty()) break;
                const int c = left[std::size_t(rng_.unit() * float(left.size())) % left.size()];
                taken[std::size_t(c)] = 1; script_.push_back(c);
                continue;
            }
            double r = double(rng_.unit()) * total, acc = 0.0;
            int chosen = -1;
            for (std::size_t i = 0; i < w_.size(); ++i) {
                if (taken[i]) continue;
                acc += w_[i];
                if (r <= acc) { chosen = int(i); break; }
            }
            if (chosen < 0) {   // floating-point shortfall on the last bucket
                for (std::size_t i = w_.size(); i-- > 0; )
                    if (!taken[i]) { chosen = int(i); break; }
            }
            if (chosen < 0) break;
            taken[std::size_t(chosen)] = 1;
            script_.push_back(chosen);
        }
        // Authored priority decides the order the script is evaluated in.
        std::stable_sort(script_.begin(), script_.end(),
                         [&](int a, int b) { return prio_[std::size_t(a)] < prio_[std::size_t(b)]; });
    }

    // Close the episode: reward or punish the rules that were in the script,
    // then conserve total weight by pushing the opposite change onto the rest.
    void end_episode(double fitness) {
        ++episodes_;
        if (script_.empty() || w_.empty()) return;
        fitness = std::clamp(fitness, 0.0, 1.0);
        const double b = cfg_.breakEven;
        const double dW = (fitness >= b)
            ? cfg_.rMax * (fitness - b) / std::max(1e-9, 1.0 - b)
            : -cfg_.pMax * (b - fitness) / std::max(1e-9, b);

        std::vector<char> inScript(w_.size(), 0);
        for (int r : script_) inScript[std::size_t(r)] = 1;

        // Apply, clipping, and record how much the clipping actually allowed —
        // redistributing the INTENDED change rather than the granted one is how
        // the sum quietly drifts once weights start hitting their limits.
        double granted = 0.0;
        for (int r : script_) {
            double& w = w_[std::size_t(r)];
            const double before = w;
            w = std::clamp(w + dW, cfg_.wMin, cfg_.wMax);
            granted += w - before;
        }
        int others = 0;
        for (std::size_t i = 0; i < w_.size(); ++i) if (!inScript[i]) ++others;
        if (others == 0 || granted == 0.0) return;
        const double share = -granted / double(others);
        double leftover = 0.0;
        for (std::size_t i = 0; i < w_.size(); ++i) {
            if (inScript[i]) continue;
            const double before = w_[i];
            w_[i] = std::clamp(w_[i] + share, cfg_.wMin, cfg_.wMax);
            leftover += share - (w_[i] - before);
        }
        // Anything the clips refused stays refused. Recorded rather than
        // silently dropped so a test can assert how far conservation slipped.
        drift_ += leftover;
    }

    [[nodiscard]] double drift() const { return drift_; }

    void reset(std::uint64_t seed) {
        std::fill(w_.begin(), w_.end(), cfg_.wInit);
        script_.clear(); episodes_ = 0; drift_ = 0.0;
        rng_.reseed(seed);
    }

private:
    DynScriptConfig cfg_{};
    std::vector<double> w_;
    std::vector<int>    prio_;
    std::vector<int>    script_;
    Rng    rng_{0xD15Cull};
    int    episodes_ = 0;
    double drift_ = 0.0;
};

} // namespace bench
