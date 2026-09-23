// neuralq.hpp — the same maze, learned by a network instead of a table, with
// the table running alongside as the control.
//
// Replacing a Q-table with a function approximator is not a free upgrade. It
// combines the three things Sutton named the DEADLY TRIAD — function
// approximation, bootstrapping, and off-policy learning — and the combination
// has no convergence guarantee at all. The tabular agent's does (Watkins &
// Dayan 1992). So the honest way to present a neural agent is next to the
// tabular one on the identical maze, and let the two curves say what happened.
//
//   Mnih, V. et al. "Human-level control through deep reinforcement learning",
//   Nature 518 (2015) 529-533 — replay and a target network, the two devices
//   that make this stable enough to work.
//   Sutton, R. S. & Barto, A. G. "Reinforcement Learning", 2nd ed., ch. 11.
//
// Both devices are switches here, because being able to turn them OFF and watch
// the thing fall apart is the clearest evidence of why they exist.
//
// The picture is the maze on the left, coloured by the NETWORK's value
// estimate, and the policy network on the right drawn from its live weights.

#pragma once
#include "../sim.hpp"
#include "../learn/mlp.hpp"
#include "../render/voxel.hpp"     // Surface
#include "../rng.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace bench {

class NeuralQ final : public Sim {
public:
    enum Action { Up = 0, Right = 1, Down = 2, Left = 3, kActions = 4 };

    NeuralQ(int w = 17, int h = 13) : w_(w), h_(h) {
        about_ = Provenance{
            "Neural Q-learning (vs a table)", "2015",
            "Volodymyr Mnih et al.; the tabular control is Watkins 1989",
            "Mnih, V. et al. \"Human-level control through deep reinforcement learning\", "
            "Nature 518 (2015) 529-533",
            Replication::No,
            "No. It learns a policy; it does not copy itself.",
            "A network estimates Q(s,a) instead of a table storing it. That combination - "
            "approximation, bootstrapping, off-policy - is the deadly triad, and it has no "
            "convergence guarantee, while the table's does. So the table runs on the same maze "
            "at the same time as a control. Replay and the target network are switches: turn "
            "them off and watch what they were for."
        };
        pal_ = { {{8,11,14}, "unvisited"} };
        for (int i = 0; i < kBands; ++i) {
            const float t = float(i) / float(kBands - 1);
            const auto mix = [&](int a, int b) { return std::uint8_t(float(a)+(float(b)-float(a))*t); };
            pal_.push_back({{ mix(20,242), mix(48,193), mix(70,78) },
                            i == 0 ? "low value" : (i == kBands-1 ? "high value" : "")});
        }
        pal_.push_back({{60,70,84},"wall"});
        pal_.push_back({{217,83,79},"hazard"});
        pal_.push_back({{123,216,143},"goal"});
        pal_.push_back({{255,255,255},"agent"});

        knobs_ = {
            {"seed", "run seed", 1.f, 40.f, 1.f, 1.f, {}, true,
             "Which run this is. The same seed reproduces the identical run; different "
             "seeds are independent runs of the SAME configuration. Every measurement in "
             "this project needed several — a single run cannot tell a real effect from a "
             "lucky one."},
            {"lr", "learning rate", 0.001f, 0.2f, 0.02f, 0.001f, {}, false,
             "Gradient step for the Q-network. Higher is less stable — with bootstrapping the "
             "target moves as you chase it."},
            {"epsilon", "exploration", 0.0f, 1.0f, 0.35f, 0.01f, {}, false,
             "Same meaning as for the table. Held constant here so the two agents are "
             "compared under identical conditions."},
            {"replay", "experience replay", 0.f, 1.f, 1.f, 1.f, {"off", "on"}, false,
             "Train on a random batch of remembered transitions instead of only the newest "
             "one. Consecutive steps in a maze are heavily correlated, and a network trained "
             "on them in order chases its own tail. Turn it off to watch that happen."},
            {"target", "target network", 0.f, 1.f, 1.f, 1.f, {"off", "on"}, false,
             "Compute the bootstrap target from a frozen copy of the network, refreshed "
             "periodically. Without it the target moves every time the weights do, which is "
             "the feedback loop that makes naive neural Q-learning diverge."},
            {"speed", "agent steps per tick", 1.f, 200.f, 24.f, 1.f, {}, false,
             "Display rate only.", true},
        };
        surf_.resize(kW, kH);
        view_ = Field(w_, h_);
        buildMaze();
        reset();
    }

    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field&               field()   const override { return view_; }
    const Surface*             surface() const override { return &surf_; }
    std::uint64_t              generation() const override { return gen_; }
    std::vector<Knob>&         knobs()   override { return knobs_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
    }

    [[nodiscard]] std::string subtitle() const override {
        char b[160];
        const int nn = greedy_path(false), tt = greedy_path(true);
        std::snprintf(b, sizeof b,
            "episode %d  ·  network %s  ·  table %s  ·  optimal %d",
            episode_, nn < 0 ? "lost" : (std::to_string(nn) + " steps").c_str(),
            tt < 0 ? "lost" : (std::to_string(tt) + " steps").c_str(), shortest_path());
        return b;
    }

    void reset() override {
        rng_.reseed(runSeed(0x0DEE5Eull));
        net_.init({kInputs, 24, 24, kActions}, 20240, MLP::Out::Linear);
        target_ = net_;
        table_.assign(std::size_t(w_) * h_ * kActions, 0.0f);
        replay_.clear(); replayAt_ = 0;
        ax_ = tx_ = sx_; ay_ = ty_ = sy_;
        episode_ = 0; steps_ = 0; tsteps_ = 0; gen_ = 0; sinceSync_ = 0;
        publish();
    }

    void step() override {
        const int n = std::max(1, int(knob("speed") + 0.5f));
        for (int i = 0; i < n; ++i) { neuralStep(); tableStep(); }
        ++gen_;
        publish();
    }

    std::vector<Metric> metrics() const override {
        const int opt = shortest_path();
        const int nn = greedy_path(false), tt = greedy_path(true);
        return {
            // Reported as "how much worse than optimal", so 1.0 is perfect and
            // the two agents are directly comparable.
            Metric{ "network: optimal/actual", nn < 0 ? 0.0 : double(opt) / double(nn), 1.0 },
            Metric{ "table:   optimal/actual", tt < 0 ? 0.0 : double(opt) / double(tt), 1.0 },
            Metric{ "episodes", double(episode_), 0.0, Metric::Neither },
        };
    }

    bool poke(float nx, float ny) override {
        const int x = std::clamp(int(nx * w_), 0, w_ - 1), y = std::clamp(int(ny * h_), 0, h_ - 1);
        if (cell_[idx(x,y)] == Wall) return false;
        ax_ = x; ay_ = y; publish();
        return true;
    }

    [[nodiscard]] const char* epoch_name() const override { return "episode"; }
    [[nodiscard]] int epoch_count() const override { return episode_; }
    bool advance_epoch() override {
        const int start = episode_;
        for (int guard = 0; guard < 200000 && episode_ == start; ++guard) { neuralStep(); tableStep(); }
        publish();
        return true;
    }

    // ── measurement ────────────────────────────────────────────────────────
    [[nodiscard]] int episodes() const { return episode_; }
    [[nodiscard]] int shortest_path() const {
        std::vector<int> dist(std::size_t(w_) * h_, -1);
        std::vector<int> q; q.push_back(idx(sx_, sy_)); dist[std::size_t(idx(sx_,sy_))] = 0;
        for (std::size_t i = 0; i < q.size(); ++i) {
            const int c = q[i], cx = c % w_, cy = c / w_;
            if (cx == gx_ && cy == gy_) return dist[std::size_t(c)];
            for (int a = 0; a < kActions; ++a) {
                int nx = cx, ny = cy; move(a, nx, ny);
                if ((nx == cx && ny == cy) || dist[idx(nx,ny)] >= 0) continue;
                dist[idx(nx,ny)] = dist[std::size_t(c)] + 1; q.push_back(idx(nx,ny));
            }
        }
        return -1;
    }
    [[nodiscard]] int greedy_path(bool tabular, int limit = 400) const {
        int x = sx_, y = sy_;
        for (int i = 0; i < limit; ++i) {
            if (x == gx_ && y == gy_) return i;
            const int a = tabular ? bestTable(x, y) : bestNet(x, y);
            int nx = x, ny = y; move(a, nx, ny);
            if (nx == x && ny == y) return -1;
            x = nx; y = ny;
        }
        return -1;
    }
    // Print the first few greedy moves with their Q-values. When a policy just
    // "does not work", the question is always whether it is choosing a blocked
    // action, oscillating, or simply valuing the wrong direction.
    void debug_walk(int n) {
        int x = sx_, y = sy_;
        for (int i = 0; i < n; ++i) {
            const auto q = net_.forward(encode(x, y));
            const int a = bestNet(x, y);
            int nx = x, ny = y; move(a, nx, ny);
            std::printf("  (%2d,%2d) Q = [%6.3f %6.3f %6.3f %6.3f]  -> %s%s\n", x, y,
                        double(q[0]), double(q[1]), double(q[2]), double(q[3]),
                        a==0?"up":a==1?"right":a==2?"down":"left",
                        (nx==x&&ny==y) ? "   BLOCKED" : "");
            if (nx == x && ny == y) break;
            x = nx; y = ny;
        }
    }

    void train(int episodes) {
        const int start = episode_;
        for (int guard = 0; guard < 4000000 && episode_ - start < episodes; ++guard) {
            neuralStep(); tableStep();
        }
    }

private:
    enum Cell : std::uint8_t { Open = 0, Wall = 1, Hazard = 2, Goal = 3 };
    static constexpr int kBands = 10, kInputs = 6, kW = 760, kH = 380;
    static constexpr std::size_t kReplayCap = 4000;
    struct Step { std::vector<float> s, s2; int a; float r; bool done; };

    [[nodiscard]] int idx(int x, int y) const { return y * w_ + x; }
    // Which independent run this is. Same seed, same run; different seed, an
    // independent sample of the same configuration.
    [[nodiscard]] std::uint64_t runSeed(std::uint64_t base) const {
        return mix_seed(base, int(knob("seed") + 0.5f));
    }
    [[nodiscard]] float knob(const char* k) const {
        for (auto& kn : knobs_) if (kn.key == k) return kn.value;
        return 0.f;
    }
    void move(int a, int& x, int& y) const {
        int nx = x, ny = y;
        switch (a) { case Up: --ny; break; case Right: ++nx; break;
                     case Down: ++ny; break; case Left: --nx; break; default: break; }
        if (nx < 0 || ny < 0 || nx >= w_ || ny >= h_) return;
        if (cell_[idx(nx,ny)] == Wall) return;
        x = nx; y = ny;
    }
    [[nodiscard]] bool terminal(int x, int y) const {
        return cell_[idx(x,y)] == Goal || cell_[idx(x,y)] == Hazard;
    }
    [[nodiscard]] float rewardFor(int x, int y) const {
        switch (Cell(cell_[idx(x,y)])) {
            case Goal: return 1.0f; case Hazard: return -1.0f; default: return -0.01f;
        }
    }
    // The state the network sees: position, plus which moves this square
    // affords. The wall flags are what let it generalise instead of having to
    // memorise every square separately.
    [[nodiscard]] std::vector<float> encode(int x, int y) const {
        std::vector<float> v(kInputs, 0.0f);
        v[0] = float(x) / float(w_ - 1);
        v[1] = float(y) / float(h_ - 1);
        for (int a = 0; a < kActions; ++a) {
            int nx = x, ny = y; move(a, nx, ny);
            v[std::size_t(2 + a)] = (nx == x && ny == y) ? 0.0f : 1.0f;
        }
        return v;
    }
    [[nodiscard]] int bestNet(int x, int y) const {
        MLP& n = const_cast<MLP&>(net_);
        const auto& q = n.forward(encode(x, y));
        int best = 0;
        for (int a = 1; a < kActions; ++a) if (q[std::size_t(a)] > q[std::size_t(best)]) best = a;
        return best;
    }
    [[nodiscard]] int bestTable(int x, int y) const {
        int best = 0;
        for (int a = 1; a < kActions; ++a)
            if (table_[(std::size_t(idx(x,y)))*kActions + std::size_t(a)]
              > table_[(std::size_t(idx(x,y)))*kActions + std::size_t(best)]) best = a;
        return best;
    }

    void learnFrom(const Step& t) {
        MLP& n = net_;
        const auto& q = n.forward(t.s);
        std::vector<float> target(q.begin(), q.end());     // only the taken action changes
        const float boot = t.done ? t.r : t.r + 0.95f * maxTargetFor(t.s2);
        target[std::size_t(t.a)] = boot;
        n.forward(t.s);
        n.zero_grad();
        n.backward(target);
        n.apply(knob("lr"), 1);
    }
    [[nodiscard]] float maxTargetFor(const std::vector<float>& s2) {
        MLP& n = (knob("target") > 0.5f) ? target_ : net_;
        const auto& q = n.forward(s2);
        float best = q[0];
        for (std::size_t a = 1; a < q.size(); ++a) best = std::max(best, q[a]);
        return best;
    }

    void neuralStep() {
        const float eps = knob("epsilon");
        const int x = ax_, y = ay_;
        const int a = (rng_.unit() < eps) ? int(rng_.unit() * float(kActions)) % kActions
                                          : bestNet(x, y);
        int nx = x, ny = y; move(a, nx, ny);
        const float r = rewardFor(nx, ny);
        const bool done = terminal(nx, ny);

        Step t{ encode(x, y), encode(nx, ny), a, r, done };
        if (knob("replay") > 0.5f) {
            if (replay_.size() < kReplayCap) replay_.push_back(t);
            else { replay_[replayAt_] = t; replayAt_ = (replayAt_ + 1) % kReplayCap; }
            // A small batch of remembered transitions, drawn at random. Training
            // only on the newest step means training on a sequence of almost
            // identical states, which is what makes it chase its own tail.
            for (int k = 0; k < 8 && !replay_.empty(); ++k)
                learnFrom(replay_[std::size_t(rng_.unit() * float(replay_.size())) % replay_.size()]);
        } else {
            learnFrom(t);
        }

        if (++sinceSync_ >= 200 && knob("target") > 0.5f) { target_ = net_; sinceSync_ = 0; }

        ax_ = nx; ay_ = ny; ++steps_;
        if (done || steps_ > 600) { ax_ = sx_; ay_ = sy_; steps_ = 0; ++episode_; }
    }

    // The control: ordinary tabular Q-learning, same maze, same reward, same
    // exploration rate, stepping at the same pace.
    void tableStep() {
        const float eps = knob("epsilon"), alpha = 0.2f, gamma = 0.95f;
        const int x = tx_, y = ty_;
        const int a = (rng_.unit() < eps) ? int(rng_.unit() * float(kActions)) % kActions
                                          : bestTable(x, y);
        int nx = x, ny = y; move(a, nx, ny);
        const float r = rewardFor(nx, ny);
        float best = 0.0f;
        if (!terminal(nx, ny)) {
            best = table_[(std::size_t(idx(nx,ny)))*kActions];
            for (int b = 1; b < kActions; ++b)
                best = std::max(best, table_[(std::size_t(idx(nx,ny)))*kActions + std::size_t(b)]);
        }
        float& cell = table_[(std::size_t(idx(x,y)))*kActions + std::size_t(a)];
        cell += alpha * ((terminal(nx,ny) ? r : r + gamma * best) - cell);
        tx_ = nx; ty_ = ny; ++tsteps_;
        if (terminal(nx,ny) || tsteps_ > 600) { tx_ = sx_; ty_ = sy_; tsteps_ = 0; }
    }

    void buildMaze() {
        cell_.assign(std::size_t(w_) * h_, Open);
        for (int y = 2; y < h_ - 2; ++y) if (y != h_/2) cell_[idx(w_/3, y)] = Wall;
        for (int y = 2; y < h_ - 2; ++y) if (y != 2 && y != h_-3) cell_[idx(2*w_/3, y)] = Wall;
        cell_[idx(w_/3 + 2, h_/2)] = Hazard;
        sx_ = 1; sy_ = h_/2; gx_ = w_-2; gy_ = h_/2;
        cell_[idx(gx_, gy_)] = Goal;
    }

    // ── drawing ────────────────────────────────────────────────────────────
    void px(int x, int y, Rgb c, float a) {
        if (x < 0 || y < 0 || x >= kW || y >= kH || a <= 0.f) return;
        std::uint8_t* p = &surf_.rgba[(std::size_t(y)*kW + x)*4];
        const float k = std::min(1.0f, a);
        p[0] = std::uint8_t(p[0] + (float(c.r)-float(p[0]))*k);
        p[1] = std::uint8_t(p[1] + (float(c.g)-float(p[1]))*k);
        p[2] = std::uint8_t(p[2] + (float(c.b)-float(p[2]))*k);
        p[3] = 255;
    }
    void box(int x0, int y0, int x1, int y1, Rgb c) {
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) px(x, y, c, 1.0f);
    }
    void lineTo(float x0, float y0, float x1, float y1, float wide, Rgb c, float a) {
        const int steps = std::max(1, int(std::max(std::fabs(x1-x0), std::fabs(y1-y0))));
        for (int i = 0; i <= steps; ++i) {
            const float t = float(i)/float(steps);
            const int cx = int(x0 + (x1-x0)*t), cy = int(y0 + (y1-y0)*t);
            const int r = int(wide);
            for (int oy = -r; oy <= r; ++oy) for (int ox = -r; ox <= r; ++ox)
                if (ox*ox + oy*oy <= r*r + r) px(cx+ox, cy+oy, c, a);
        }
    }

    void publish() {
        // The index field: the maze, coloured by the NETWORK's value estimate.
        float lo = 1e9f, hi = -1e9f;
        std::vector<float> val(std::size_t(w_) * h_, 0.0f);
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                if (cell_[idx(x,y)] != Open) continue;
                const auto& q = net_.forward(encode(x, y));
                float best = q[0];
                for (int a = 1; a < kActions; ++a) best = std::max(best, q[std::size_t(a)]);
                val[std::size_t(idx(x,y))] = best;
                lo = std::min(lo, best); hi = std::max(hi, best);
            }
        const float span = std::max(1e-6f, hi - lo);
        const std::uint8_t wallIdx = std::uint8_t(1 + kBands);
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                std::uint8_t v;
                switch (Cell(cell_[idx(x,y)])) {
                    case Wall:   v = wallIdx; break;
                    case Hazard: v = std::uint8_t(wallIdx+1); break;
                    case Goal:   v = std::uint8_t(wallIdx+2); break;
                    default:
                        v = std::uint8_t(1 + std::clamp(int((val[std::size_t(idx(x,y))]-lo)/span
                                                            * float(kBands-1)), 0, kBands-1));
                }
                view_.set(x, y, v);
            }
        view_.set(ax_, ay_, std::uint8_t(wallIdx+3));

        // The surface: maze on the left, the policy network on the right.
        for (int i = 0; i < kW*kH; ++i) {
            std::uint8_t* p = &surf_.rgba[std::size_t(i)*4];
            p[0]=12; p[1]=15; p[2]=20; p[3]=255;
        }
        const int cell = std::min((kH - 40) / h_, (kW/2 - 40) / w_);
        const int ox = 24, oy = (kH - cell*h_) / 2;
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                Rgb c{18,22,28};
                switch (Cell(cell_[idx(x,y)])) {
                    case Wall:   c = Rgb{60,70,84}; break;
                    case Hazard: c = Rgb{217,83,79}; break;
                    case Goal:   c = Rgb{123,216,143}; break;
                    default: {
                        const float t = (val[std::size_t(idx(x,y))] - lo) / span;
                        c = Rgb{ std::uint8_t(20 + 222*t), std::uint8_t(48 + 145*t),
                                 std::uint8_t(70 + 8*t) };
                    }
                }
                box(ox + x*cell, oy + y*cell, ox + (x+1)*cell - 1, oy + (y+1)*cell - 1, c);
            }
        box(ox + ax_*cell + 1, oy + ay_*cell + 1, ox + (ax_+1)*cell - 2, oy + (ay_+1)*cell - 2,
            Rgb{255,255,255});

        // The policy network, drawn from its live weights.
        const int nx0 = kW/2 + 30, nw = kW - nx0 - 30;
        float wmax = 1e-4f;
        for (int l = 0; l + 1 < net_.layers(); ++l)
            for (int i = 0; i < net_.units(l); ++i)
                for (int j = 0; j < net_.units(l+1); ++j)
                    wmax = std::max(wmax, std::fabs(net_.weight(l,i,j)));
        auto pos = [&](int l, int i, float& X, float& Y) {
            const int L = net_.layers(), n = net_.units(l);
            X = float(nx0) + float(nw) * float(l) / float(L - 1);
            Y = 30.0f + (float(kH) - 60.0f) * (float(i) + 0.5f) / float(n);
        };
        for (int l = 0; l + 1 < net_.layers(); ++l)
            for (int i = 0; i < net_.units(l); ++i)
                for (int j = 0; j < net_.units(l+1); ++j) {
                    const float w = net_.weight(l,i,j), m = std::fabs(w)/wmax;
                    if (m < 0.12f) continue;                 // 24x24 is a lot of lines
                    float x0,y0,x1,y1; pos(l,i,x0,y0); pos(l+1,j,x1,y1);
                    lineTo(x0,y0,x1,y1, 0.5f + m*1.6f,
                           w >= 0 ? Rgb{90,209,196} : Rgb{217,83,79}, 0.10f + 0.5f*m);
                }
        net_.forward(encode(ax_, ay_));
        for (int l = 0; l < net_.layers(); ++l)
            for (int i = 0; i < net_.units(l); ++i) {
                float X, Y; pos(l, i, X, Y);
                const float a = std::clamp(net_.activation(l,i)*0.5f + 0.5f, 0.f, 1.f);
                lineTo(X, Y, X, Y, 4.0f,
                       Rgb{ std::uint8_t(30+200*a), std::uint8_t(36+215*a), std::uint8_t(44+195*a) },
                       1.0f);
            }
    }

    Provenance about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    int w_, h_;
    Field   view_;
    Surface surf_;
    std::vector<std::uint8_t> cell_;
    MLP     net_, target_;
    std::vector<float> table_;            // the control
    std::vector<Step>  replay_;
    std::size_t replayAt_ = 0;
    int sx_=1, sy_=1, gx_=2, gy_=2;
    int ax_=1, ay_=1, tx_=1, ty_=1;
    int episode_=0, steps_=0, tsteps_=0, sinceSync_=0;
    Rng rng_{0x0DEE5Eull};
    std::uint64_t gen_ = 0;
};

inline SimPtr make_neuralq() { return std::make_unique<NeuralQ>(); }

} // namespace bench
