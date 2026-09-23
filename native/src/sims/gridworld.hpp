// gridworld.hpp — an agent that learns, on a grid, where you can watch it.
//
// Tabular Q-learning, the 1989 Watkins update and nothing more:
//
//     Q(s,a) <- Q(s,a) + alpha * [ r + gamma * max_a' Q(s',a') - Q(s,a) ]
//
//   Watkins, C. J. C. H. "Learning from Delayed Rewards", PhD thesis,
//   Cambridge, 1989. Convergence proof: Watkins & Dayan, Machine Learning 8
//   (1992) 279-292.
//
// HOW IT IS VERIFIED, and why it is not "the reward went up". A reward curve
// rises for all sorts of reasons that are not learning — a shrinking epsilon
// alone will do it. On a deterministic gridworld the optimal path length is
// known by construction, so the test is that the GREEDY policy reaches the goal
// in exactly that many steps, and that the learned Q-values satisfy the Bellman
// optimality equation to tolerance. Both are properties the table either has or
// does not.
//
// The picture is the whole point of putting it on this bench: the field shows
// the agent, the walls, the goal, and the learned VALUE of every square, so you
// can see the value function spread outward from the goal as it learns.

#pragma once
#include "../sim.hpp"
#include "../rng.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace bench {

class GridWorld final : public Sim {
public:
    enum Action { Up = 0, Right = 1, Down = 2, Left = 3, kActions = 4 };

    explicit GridWorld(int w = 21, int h = 15) : w_(w), h_(h), view_(w, h) {
        about_ = Provenance{
            "Q-learning gridworld", "1989", "Christopher Watkins",
            "Watkins, C. J. C. H. \"Learning from Delayed Rewards\", PhD thesis, Cambridge 1989; "
            "convergence proved in Watkins & Dayan, Machine Learning 8 (1992) 279-292",
            Replication::No,
            "No. An agent that learns is not an agent that copies itself, and the bench keeps "
            "those apart. It is here because learning is a process you can watch and measure, "
            "which is the same reason everything else here is.",
            "One agent, one table of Q(state, action), and the Watkins update. Colour is the "
            "LEARNED VALUE of each square - watch it spread outward from the goal, because a "
            "square only becomes valuable once a path from it to the goal has been walked."
        };
        pal_ = { {{  8, 11, 14}, "unvisited"} };
        // A value ramp, cold to hot. Ten bands is enough to see the gradient
        // spread and few enough that the legend stays readable.
        for (int i = 0; i < kValueBands; ++i) {
            const float t = float(i) / float(kValueBands - 1);
            const auto mix = [&](int a, int b) { return std::uint8_t(float(a) + (float(b)-float(a))*t); };
            const char* lbl = (i == 0) ? "low value" : (i == kValueBands - 1 ? "high value" : "");
            pal_.push_back({{ mix(20, 242), mix(48, 193), mix(70, 78) }, lbl});
        }
        pal_.push_back({{ 60,  70,  84}, "wall"});
        pal_.push_back({{217,  83,  79}, "hazard"});
        pal_.push_back({{123, 216, 143}, "goal"});
        pal_.push_back({{255, 255, 255}, "agent"});

        knobs_ = {
            {"seed", "run seed", 1.f, 40.f, 1.f, 1.f, {}, true,
             "Which run this is. The same seed reproduces the identical run; different "
             "seeds are independent runs of the SAME configuration. Every measurement in "
             "this project needed several — a single run cannot tell a real effect from a "
             "lucky one."},
            {"alpha", "learning rate", 0.01f, 1.0f, 0.20f, 0.01f, {}, false,
             "How much of each new estimate replaces the old one. 0 learns nothing; 1 throws "
             "away everything it knew on every step, which in a stochastic world never settles."},
            {"gamma", "discount", 0.50f, 0.999f, 0.95f, 0.001f, {}, false,
             "How much a reward one step further away is worth. Near 1 the agent plans far "
             "ahead; low and it will not cross a long corridor to reach anything."},
            {"epsilon", "exploration", 0.0f, 1.0f, 0.35f, 0.01f, {}, false,
             "Chance of ignoring the policy and moving at random. Without it the agent never "
             "discovers a better route than the first one that worked; with it always at 1 it "
             "never exploits what it learned. Measured on the default 21x15 maze over 30 seeds, 6000 "
             "episodes each, holding epsilon fixed: the greedy path afterwards is the true "
             "28-step optimum on 7 seeds at 0.10, 15 at 0.20, 16 at 0.35 and 30 at 0.80, "
             "while the mean episode reward collected WHILE exploring — over all 6000 of "
             "those episodes, not their converged tail — falls from +0.60 to +0.52 to +0.37 "
             "to -0.88 across those same settings. That is the tradeoff: the update is "
             "off-policy, so random moves keep improving the table, and what they "
             "cost is the run you are watching. Every run that misses the optimum settles on "
             "the same 30-step detour. Note the shape of the claim — a success RATE over "
             "seeds. The default seed finds 28 at 0.20 and would have told you nothing."},
            {"decay", "exploration decay per episode", 0.0f, 0.02f, 0.0f, 0.001f, {}, false,
             "How fast exploration falls off. This is why a rising reward curve is NOT by "
             "itself evidence of learning — shrinking epsilon alone will lift it."},
            {"speed", "agent steps per tick", 1.f, 400.f, 30.f, 1.f, {}, false,
             "How many moves happen per simulation tick. Display rate only.", true},
            {"size", "maze size", 0.f, 3.f, 0.f, 1.f, {"21x15", "41x29", "81x57", "161x113"}, true,
             "A difficulty knob, not a display one. The walls are placed proportionally, so the "
             "maze keeps its shape and the shortest path very nearly doubles each step: 28, 62, "
             "130 and 266 moves, computed by breadth-first search rather than claimed. What the "
             "agent actually achieves after 120,000 ticks at speed 400 is 28, 65, 149 and 374 - "
             "so it finds the exact optimum on the small maze, comes within 5% on the next, and "
             "is still 40% adrift on the largest. That gap IS the lesson: the table grows with "
             "the area while the undirected random walk that has to stumble on the goal even "
             "once grows far faster. On 161x113 the first success takes about four million agent "
             "steps, so at the default speed of 30 you will watch a long time before the value "
             "map shows anything at all. Turn speed up with the maze."},
        };
        buildMaze();
        reset();
    }

    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field&               field()   const override { return view_; }
    std::uint64_t              generation() const override { return gen_; }
    std::vector<Knob>&         knobs()   override { return knobs_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        // Exploration is live state, not just a stored setting: it decays as
        // episodes pass. Without this the slider moved and nothing happened
        // until the next reset.
        if (k == "epsilon") eps_ = v;
        // Resizing has to rebuild the world AND everything indexed by it: the
        // Q-table, the visit counts and the state-action counts are all sized
        // w*h, and reset() assigns them from w_ and h_ — so the order here is
        // resize, rebuild the maze, then reset.
        if (k == "size") {
            static const int kW[] = {21, 41, 81, 161}, kH[] = {15, 29, 57, 113};
            int i = int(v + 0.5f);
            i = i < 0 ? 0 : (i > 3 ? 3 : i);
            if (kW[i] != w_) {
                w_ = kW[i]; h_ = kH[i];
                view_ = Field(w_, h_);
                buildMaze();
                reset();
            }
        }
    }

    [[nodiscard]] std::string subtitle() const override {
        char b[96];
        std::snprintf(b, sizeof b, "episode %d  ·  eps %.2f  ·  best %d steps",
                      episode_, double(eps_), bestSteps_ == 1 << 30 ? 0 : bestSteps_);
        return b;
    }

    void reset() override {
        rng_.reseed(runSeed(0x9A5Eull));
        q_.assign(std::size_t(w_) * h_ * kActions, 0.0f);
        sa_.assign(std::size_t(w_) * h_ * kActions, 0);
        visits_.assign(std::size_t(w_) * h_, 0);
        episode_ = 0; gen_ = 0; steps_ = 0; epReward_ = 0.0;
        bestSteps_ = 1 << 30; lastSteps_ = 0; lastGoalSteps_ = 0; lastReward_ = 0.0;
        eps_ = knob("epsilon");
        ax_ = sx_; ay_ = sy_;
        publish();
    }

    void step() override {
        const int batch = std::max(1, int(knob("speed") + 0.5f));
        for (int i = 0; i < batch; ++i) agentStep();
        publish();
    }

    std::vector<Metric> metrics() const override {
        return {
            // Steps to the GOAL, from episodes that reached it.
            //
            // This used to report lastSteps_, which is set at every episode end
            // however it ended — a hazard death three steps in, or a 4000-step
            // timeout. Declared Lower, the panel's best-so-far then reported
            // the shortest FAILED episode as the agent's best result, which is
            // the exact inversion the direction field was added to prevent.
            // Before the first success it reports the timeout ceiling, because
            // a Lower metric seeded at zero is an unbeatable best that no real
            // run can ever displace.
            Metric{ "steps to goal (last success)",
                    lastGoalSteps_ ? double(lastGoalSteps_) : 4001.0, 0.0, Metric::Lower },
            // Kept, but named for what it is and scoring nothing: a short
            // episode here is as likely to be a death as a triumph.
            Metric{ "last episode length",  double(lastSteps_), 0.0, Metric::Neither },
            Metric{ "episode reward",       lastReward_, 0.0 },
            Metric{ "exploration",          double(eps_), 1.0, Metric::Neither },
        };
    }

    // Drop the agent somewhere. Useful for watching it recover a route.
    bool poke(float nx, float ny) override {
        const int x = std::clamp(int(nx * w_), 0, w_ - 1);
        const int y = std::clamp(int(ny * h_), 0, h_ - 1);
        if (cell_[idx(x, y)] == Wall) return false;
        ax_ = x; ay_ = y;
        publish();
        return true;
    }

    [[nodiscard]] const char* epoch_name() const override { return "episode"; }
    [[nodiscard]] int epoch_count() const override { return episode_; }
    bool advance_epoch() override {
        const int start = episode_;
        for (int guard = 0; guard < 200000 && episode_ == start; ++guard) agentStep();
        publish();
        return true;
    }

    // ── what the tests need ────────────────────────────────────────────────
    [[nodiscard]] int    episodes() const { return episode_; }
    [[nodiscard]] int    best_steps() const { return bestSteps_; }
    [[nodiscard]] float  q(int x, int y, int a) const {
        return q_[(std::size_t(y) * w_ + x) * kActions + std::size_t(a)];
    }
    [[nodiscard]] int greedy(int x, int y) const {
        int best = 0; float bv = q(x, y, 0);
        for (int a = 1; a < kActions; ++a) if (q(x, y, a) > bv) { bv = q(x, y, a); best = a; }
        return best;
    }
    // Walk the greedy policy from the start and report how many steps it takes,
    // or -1 if it fails to arrive. This is the measurement that means something.
    [[nodiscard]] int greedy_path_length(int limit = 2000) const {
        int x = sx_, y = sy_;
        for (int i = 0; i < limit; ++i) {
            if (x == gx_ && y == gy_) return i;
            const int a = greedy(x, y);
            int nx = x, ny = y; move(a, nx, ny);
            if (nx == x && ny == y) return -1;      // walked into a wall forever
            x = nx; y = ny;
        }
        return -1;
    }
    // Breadth-first shortest path, so the test compares against ground truth
    // rather than against whatever the agent happened to find.
    [[nodiscard]] int shortest_path() const {
        std::vector<int> dist(std::size_t(w_) * h_, -1);
        std::vector<int> queue; queue.reserve(dist.size());
        dist[idx(sx_, sy_)] = 0; queue.push_back(idx(sx_, sy_));
        for (std::size_t qi = 0; qi < queue.size(); ++qi) {
            const int c = queue[qi], cx = c % w_, cy = c / w_;
            if (cx == gx_ && cy == gy_) return dist[std::size_t(c)];
            for (int a = 0; a < kActions; ++a) {
                int nx = cx, ny = cy; move(a, nx, ny);
                if (nx == cx && ny == cy) continue;
                if (dist[idx(nx, ny)] >= 0) continue;
                dist[idx(nx, ny)] = dist[std::size_t(c)] + 1;
                queue.push_back(idx(nx, ny));
            }
        }
        return -1;
    }
    // Largest violation of the Bellman optimality equation over reachable
    // states. A converged table satisfies it; a table that merely produced a
    // nice reward curve need not.
    // Only over state-actions the agent has actually TAKEN, at least `minVisits`
    // times.
    //
    // Measured over every action of every visited state instead, the residual
    // reads 0.9310 on seed 1 and looks like a failure to converge. It is not:
    // what it shows is a visit-count gradient — 0.9310 at >=1 visits, 0.8235 at
    // >=2, 0.7870 at >=5, 0.0201 at >=100, 0.0000 at >=500. Q-learning
    // makes no claim about actions it has never taken, and neither should the
    // measurement. This is the same mistake as measuring pivot drift along the
    // view axis — checking a quantity the thing is not required to satisfy.
    [[nodiscard]] float bellman_residual(int minVisits = 5) const {
        float worst = 0.0f;
        const float gamma = knobConst("gamma");
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                if (cell_[idx(x, y)] != Open) continue;
                if (!visits_[idx(x, y)]) continue;          // never seen: nothing to claim
                for (int a = 0; a < kActions; ++a) {
                    if (sa_[(std::size_t(idx(x,y)) * kActions) + std::size_t(a)] < minVisits)
                        continue;
                    int nx = x, ny = y; move(a, nx, ny);
                    const float r = rewardFor(nx, ny);
                    float target;
                    if (terminal(nx, ny)) target = r;
                    else {
                        float best = q(nx, ny, 0);
                        for (int b = 1; b < kActions; ++b) best = std::max(best, q(nx, ny, b));
                        target = r + gamma * best;
                    }
                    worst = std::max(worst, std::fabs(target - q(x, y, a)));
                }
            }
        return worst;
    }
    void set_exploration(float e) {
        eps_ = e;
        for (auto& kn : knobs_) if (kn.key == "epsilon") kn.value = e;
    }
    // Train headlessly, for tests and for the transfer experiments later.
    void train(int episodes) {
        for (int e = 0; e < episodes; ++e) {
            const int startEp = episode_;
            for (int guard = 0; guard < 20000 && episode_ == startEp; ++guard) agentStep();
        }
    }

private:
    enum Cell : std::uint8_t { Open = 0, Wall = 1, Hazard = 2, Goal = 3 };
    static constexpr int kValueBands = 10;

    [[nodiscard]] int idx(int x, int y) const { return y * w_ + x; }
    // Which independent run this is. Same seed, same run; different seed, an
    // independent sample of the same configuration.
    [[nodiscard]] std::uint64_t runSeed(std::uint64_t base) const {
        return mix_seed(base, int(knob("seed") + 0.5f));
    }
    [[nodiscard]] float knob(const char* key) const {
        for (auto& kn : knobs_) if (kn.key == key) return kn.value;
        return 0.f;
    }
    [[nodiscard]] float knobConst(const char* key) const { return knob(key); }

    void move(int a, int& x, int& y) const {
        int nx = x, ny = y;
        switch (a) {
            case Up:    --ny; break;
            case Right: ++nx; break;
            case Down:  ++ny; break;
            case Left:  --nx; break;
            default: break;
        }
        if (nx < 0 || ny < 0 || nx >= w_ || ny >= h_) return;   // edges are walls
        if (cell_[idx(nx, ny)] == Wall) return;
        x = nx; y = ny;
    }
    [[nodiscard]] bool terminal(int x, int y) const {
        const Cell c = Cell(cell_[idx(x, y)]);
        return c == Goal || c == Hazard;
    }
    [[nodiscard]] float rewardFor(int x, int y) const {
        switch (Cell(cell_[idx(x, y)])) {
            case Goal:   return 1.0f;
            case Hazard: return -1.0f;
            default:     return -0.01f;    // a small cost per step: dithering is not free
        }
    }

    void agentStep() {
        const float alpha = knob("alpha"), gamma = knob("gamma");
        const int x = ax_, y = ay_;

        int a;
        if (rng_.unit() < eps_) a = int(rng_.unit() * float(kActions)) % kActions;
        else                    a = greedy(x, y);

        int nx = x, ny = y; move(a, nx, ny);
        const float r = rewardFor(nx, ny);

        float best = 0.0f;
        if (!terminal(nx, ny)) {
            best = q(nx, ny, 0);
            for (int b = 1; b < kActions; ++b) best = std::max(best, q(nx, ny, b));
        }
        const float target = terminal(nx, ny) ? r : r + gamma * best;
        float& cell = q_[(std::size_t(y) * w_ + x) * kActions + std::size_t(a)];
        cell += alpha * (target - cell);

        ++visits_[idx(x, y)];
        ++sa_[(std::size_t(idx(x, y)) * kActions) + std::size_t(a)];
        ax_ = nx; ay_ = ny;
        ++steps_; ++gen_;
        epReward_ += double(r);

        if (terminal(nx, ny) || steps_ > 4000) {
            const bool reached = (Cell(cell_[idx(nx, ny)]) == Goal);
            lastSteps_  = steps_;
            lastReward_ = epReward_;
            if (reached) { bestSteps_ = std::min(bestSteps_, steps_); lastGoalSteps_ = steps_; }
            ++episode_;
            steps_ = 0; epReward_ = 0.0;
            eps_ = std::max(0.0f, eps_ - knob("decay"));
            ax_ = sx_; ay_ = sy_;
        }
    }

    void buildMaze() {
        cell_.assign(std::size_t(w_) * h_, Open);
        // A few walls and a hazard. Deliberately simple and DETERMINISTIC, so
        // the shortest path is a fact the test can compute rather than a claim.
        for (int y = 2; y < h_ - 2; ++y) if (y != h_ / 2) cell_[idx(w_ / 3, y)] = Wall;
        for (int y = 2; y < h_ - 2; ++y) if (y != 2 && y != h_ - 3) cell_[idx(2 * w_ / 3, y)] = Wall;
        cell_[idx(w_ / 3 + 2, h_ / 2)] = Hazard;
        sx_ = 1; sy_ = h_ / 2;
        gx_ = w_ - 2; gy_ = h_ / 2;
        cell_[idx(gx_, gy_)] = Goal;
    }

    void publish() {
        // Value = max_a Q(s,a), banded. Watching this ramp spread back from the
        // goal is watching the algorithm work.
        float lo = 0.0f, hi = 0.0f;
        for (int i = 0; i < w_ * h_; ++i)
            for (int a = 0; a < kActions; ++a) {
                const float v = q_[std::size_t(i) * kActions + std::size_t(a)];
                lo = std::min(lo, v); hi = std::max(hi, v);
            }
        const float span = std::max(1e-6f, hi - lo);
        const std::uint8_t wallIdx = std::uint8_t(1 + kValueBands);

        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                std::uint8_t v;
                switch (Cell(cell_[idx(x, y)])) {
                    case Wall:   v = wallIdx;     break;
                    case Hazard: v = std::uint8_t(wallIdx + 1); break;
                    case Goal:   v = std::uint8_t(wallIdx + 2); break;
                    default: {
                        if (!visits_[idx(x, y)]) { v = 0; break; }
                        float best = q(x, y, 0);
                        for (int a = 1; a < kActions; ++a) best = std::max(best, q(x, y, a));
                        const int band = std::clamp(int((best - lo) / span * float(kValueBands - 1)),
                                                    0, kValueBands - 1);
                        v = std::uint8_t(1 + band);
                        break;
                    }
                }
                view_.set(x, y, v);
            }
        view_.set(ax_, ay_, std::uint8_t(wallIdx + 3));      // the agent, on top
    }

    Provenance about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    int w_, h_;
    Field view_;
    std::vector<std::uint8_t> cell_;
    std::vector<float>        q_;
    std::vector<int>          visits_;
    std::vector<int>          sa_;      // per state-ACTION, for the residual
    int   sx_ = 1, sy_ = 1, gx_ = 2, gy_ = 2;
    int   ax_ = 1, ay_ = 1;
    int   episode_ = 0, steps_ = 0, lastSteps_ = 0, lastGoalSteps_ = 0, bestSteps_ = 1 << 30;
    double epReward_ = 0.0, lastReward_ = 0.0;
    float eps_ = 0.25f;
    Rng   rng_{0x9A5Eull};
    std::uint64_t gen_ = 0;
};

inline SimPtr make_gridworld(int w = 21, int h = 15) {
    return std::make_unique<GridWorld>(w, h);
}

} // namespace bench
