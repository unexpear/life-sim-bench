// dqn.hpp — one deep Q-learner, owned by one agent.
//
//   Mnih, V. et al. "Human-level control through deep reinforcement learning",
//   Nature 518 (2015) 529-533 — experience replay and a target network.
//   van Hasselt, H., Guez, A., Silver, D. "Deep Reinforcement Learning with
//   Double Q-learning", AAAI 2016 — decouple action SELECTION from EVALUATION.
//
// This is the learner that already lived inside neuralq.hpp, lifted out so more
// than one thing can own one. That sim keeps a single agent and can afford to
// hold its network, replay buffer and target copy as members; a town full of
// agents each learning its own trade cannot, and the moment there are twenty of
// them the buffer sizing and the sync counter stop being incidental details and
// become the thing you tune.
//
// WHAT IS DELIBERATELY NOT HERE. No optimiser beyond the SGD-with-momentum that
// MLP already does, no prioritised replay, no duelling head, no n-step returns.
// Each of those is a real improvement in the literature and each would be
// another claim to measure; the point of this file is that the three devices
// above can be switched off independently, so an ablation is a knob rather than
// a rewrite. Anything added here has to arrive with the measurement that shows
// it earning its place.
//
// DETERMINISM. Every draw comes from the Rng passed in, never from a global, so
// a run reproduces exactly and two agents seeded differently genuinely explore
// differently. This matters more than it looks: a town of agents that share an
// RNG stream is a town of agents whose "independent" exploration is correlated,
// and the population result would be measuring the sampler.

#pragma once
#include "mlp.hpp"
#include "../rng.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bench {

// One transition. Stored by value: the states are small here (a local patch and
// a handful of scalars), and a buffer of pointers into agent state would dangle
// the moment an agent dies, which in a survival sim is the normal case.
struct Transition {
    std::vector<float> s, s2;
    int   a = 0;
    float r = 0.0f;
    bool  done = false;
    // How many ticks the action occupied. An option that mines for 300 ticks
    // and one that steps once are not the same decision, and bootstrapping both
    // with a single gamma puts them on incomparable value scales — at gamma
    // 0.99 and k=300 the correct factor is 0.049, not 0.99. SMDP Q-learning
    // (Sutton, Precup & Singh, Artificial Intelligence 112 (1999) 181-211)
    // requires Q(s,o) <- Q + a[r + gamma^k max Q(s',o')], with r accumulated
    // DISCOUNTED over the option and gamma^k on the bootstrap.
    int   k = 1;
};

struct DqnConfig {
    std::vector<int> layers;        // {inputs, hidden..., actions}
    float  lr        = 0.02f;
    float  gamma     = 0.95f;
    float  epsilon   = 0.30f;
    float  epsMin    = 0.05f;
    float  epsDecay  = 0.0f;        // subtracted per episode; 0 keeps it fixed
    int    batch     = 8;           // replayed transitions per learning step
    int    syncEvery = 200;         // target network refresh, in learning steps
    std::size_t capacity = 4000;
    bool   replay    = true;
    bool   target    = true;
    bool   doubleQ   = true;
};

class Dqn {
public:
    Dqn() = default;
    Dqn(DqnConfig cfg, std::uint64_t seed)
        : cfg_(std::move(cfg)), rng_(seed) {
        // Linear output. A sigmoid head cannot represent a Q-value outside
        // (0,1), and every reward scale in this project puts them outside it.
        net_    = MLP(cfg_.layers, seed, MLP::Out::Linear);
        target_ = net_;
        eps_    = cfg_.epsilon;
        actions_ = cfg_.layers.empty() ? 0 : cfg_.layers.back();
    }

    [[nodiscard]] int actions() const { return actions_; }
    [[nodiscard]] float epsilon() const { return eps_; }
    void set_epsilon(float e) { eps_ = e; }
    [[nodiscard]] std::size_t remembered() const { return replay_.size(); }
    [[nodiscard]] std::uint64_t learn_steps() const { return learned_; }

    // Greedy action, with no exploration and no side effects. Separate from
    // act() because measuring a policy means measuring what it would DO, not
    // what it does while still rolling dice — the distinction that made the
    // gridworld's exploration result legible.
    [[nodiscard]] int greedy(const std::vector<float>& s) {
        const auto& q = net_.forward(s);
        int best = 0;
        for (std::size_t a = 1; a < q.size(); ++a)
            if (q[a] > q[std::size_t(best)]) best = int(a);
        return best;
    }

    // Epsilon-greedy, using this learner's own stream.
    int act(const std::vector<float>& s) {
        if (actions_ <= 0) return 0;
        if (rng_.unit() < eps_) return int(rng_.unit() * float(actions_)) % actions_;
        return greedy(s);
    }

    // Value of the state under the current network — what the agent thinks it
    // is worth standing here. Useful as a metric in its own right: a town where
    // the estimates rise is a town that is learning something.
    [[nodiscard]] float value(const std::vector<float>& s) {
        const auto& q = net_.forward(s);
        return *std::max_element(q.begin(), q.end());
    }

    void observe(Transition t) {
        if (!cfg_.replay) { learnFrom(t); afterLearn(); return; }
        if (replay_.size() < cfg_.capacity) replay_.push_back(std::move(t));
        else { replay_[at_] = std::move(t); at_ = (at_ + 1) % cfg_.capacity; }
        // A batch of remembered transitions, drawn at random. Training on only
        // the newest step is training on a sequence of near-identical states,
        // which is the failure replay exists to prevent.
        for (int k = 0; k < cfg_.batch && !replay_.empty(); ++k)
            learnFrom(replay_[std::size_t(rng_.unit() * float(replay_.size())) % replay_.size()]);
        afterLearn();
    }

    // Called at the end of an episode, so decay is per episode rather than per
    // step — a step-wise decay makes the schedule depend on how long episodes
    // happen to run, which is exactly the thing being learned.
    void end_episode() {
        if (cfg_.epsDecay > 0.0f) eps_ = std::max(cfg_.epsMin, eps_ - cfg_.epsDecay);
    }

    void reset_weights(std::uint64_t seed) {
        net_ = MLP(cfg_.layers, seed, MLP::Out::Linear);
        target_ = net_;
        replay_.clear(); at_ = 0; learned_ = 0; sinceSync_ = 0;
        eps_ = cfg_.epsilon;
        rng_.reseed(seed);
    }

    [[nodiscard]] const DqnConfig& config() const { return cfg_; }
    DqnConfig& config() { return cfg_; }

private:
    void learnFrom(const Transition& t) {
        if (t.s.empty()) return;
        const auto& q = net_.forward(t.s);
        std::vector<float> want(q.begin(), q.end());   // only the taken action moves
        // gamma^k, not gamma. See Transition::k.
        const float gk = (t.k <= 1) ? cfg_.gamma
                                    : std::pow(cfg_.gamma, float(t.k));
        want[std::size_t(t.a)] = t.done ? t.r : t.r + gk * bootstrap(t.s2);
        net_.forward(t.s);          // restore the activations the backward pass needs
        net_.zero_grad();
        net_.backward(want);
        net_.apply(cfg_.lr, 1);
        ++learned_;
    }

    // The bootstrap value of the next state.
    //
    // Double DQN: the ONLINE network picks the action, the TARGET network says
    // what it is worth. Plain DQN takes the max of the target network, which
    // takes a max over noisy estimates and so is biased upward — the two lines
    // below are the whole difference, and keeping both switchable is what makes
    // it an ablation rather than a claim.
    [[nodiscard]] float bootstrap(const std::vector<float>& s2) {
        if (s2.empty()) return 0.0f;
        MLP& evalNet = cfg_.target ? target_ : net_;
        if (!cfg_.doubleQ) {
            const auto& q = evalNet.forward(s2);
            return *std::max_element(q.begin(), q.end());
        }
        const auto& online = net_.forward(s2);
        int pick = 0;
        for (std::size_t a = 1; a < online.size(); ++a)
            if (online[a] > online[std::size_t(pick)]) pick = int(a);
        const auto& q = evalNet.forward(s2);
        return q[std::size_t(pick)];
    }

    void afterLearn() {
        if (!cfg_.target) return;
        if (++sinceSync_ >= cfg_.syncEvery) { target_ = net_; sinceSync_ = 0; }
    }

    DqnConfig cfg_{};
    MLP  net_, target_;
    Rng  rng_{0x5EEDull};
    std::vector<Transition> replay_;
    std::size_t at_ = 0;
    std::uint64_t learned_ = 0;
    int  sinceSync_ = 0;
    int  actions_ = 0;
    float eps_ = 0.3f;
};

} // namespace bench
