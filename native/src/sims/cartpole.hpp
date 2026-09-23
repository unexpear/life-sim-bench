// cartpole.hpp — the canonical control benchmark, with both agents on it.
//
//   Barto, A. G., Sutton, R. S. & Anderson, C. W. "Neuronlike adaptive elements
//   that can solve difficult learning control problems", IEEE Transactions on
//   Systems, Man, and Cybernetics SMC-13 (1983) 834-846.
//
// Balance a pole on a cart by pushing the cart left or right. The published
// constants are used exactly — cart 1.0 kg, pole 0.1 kg, half-length 0.5 m,
// force 10 N, timestep 0.02 s, failure past 12 degrees or 2.4 m — so the
// dynamics are checkable against the equations rather than tuned until they
// look right. The self-test does check them, by hand, at a state where the
// algebra collapses to something you can compute on paper.
//
// The equations themselves are the FRICTIONLESS form: the paper's appendix also
// has cart-track and pole-cart friction, and advance() has neither. See the note
// there for why that is the form to implement and not a shortcut.
//
// WHY THIS AND NOT ANOTHER MAZE. The maze comparison went to the table: it has
// a convergence proof, the network does not, and the table won every time. Here
// the state is four REAL numbers, so a table can only play at all by chopping
// the continuum into boxes — which is what Barto's original did.
//
// I expected that to cap the table and hand the win to the network. It does
// not, and the measurement says so plainly. Over 700 episodes at the default
// seed:
//
//     bins   3      4      6      8     12
//     mean 106    122     86     72     56
//     best 500    481    163    134    114
//
// The table is not limited by being coarse — FOUR boxes per dimension is its
// best setting, and finer is monotonically worse from there, because more boxes
// means most of them are never visited at all. The real cost of a table on a
// continuous problem is not resolution, it is that this number exists at all
// and matters this much. My first default of 6 sat in the bad region and
// produced a "table plateaus around 100" story that was an artifact of my own
// choice.

#pragma once
#include "../sim.hpp"
#include "../learn/mlp.hpp"
#include "../render/voxel.hpp"     // Surface
#include "../rng.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace bench {

class CartPole final : public Sim {
public:
    // The published constants, named as in the paper.
    static constexpr float kGravity   = 9.8f;
    static constexpr float kMassCart  = 1.0f;
    static constexpr float kMassPole  = 0.1f;
    static constexpr float kLength    = 0.5f;     // HALF the pole's length
    static constexpr float kForce     = 10.0f;
    static constexpr float kTau       = 0.02f;    // seconds per step
    static constexpr float kThetaFail = 12.0f * 3.14159265f / 180.0f;
    static constexpr float kXFail     = 2.4f;

    struct State { float x = 0, xd = 0, th = 0, thd = 0; };

    CartPole() {
        about_ = Provenance{
            "Cart-pole (vs a discretised table)", "1983",
            "Andrew Barto, Richard Sutton & Charles Anderson",
            "Barto, A. G., Sutton, R. S. & Anderson, C. W. \"Neuronlike adaptive elements that "
            "can solve difficult learning control problems\", IEEE Trans. SMC-13 (1983) 834-846",
            Replication::No,
            "No. It balances a pole; it does not copy itself.",
            "Push the cart left or right to keep the pole up. The state is four real numbers, so "
            "unlike the maze there is no table that simply holds it - the tabular agent has to "
            "chop the continuum into boxes first, which is what Barto's original did and is a "
            "lossy choice the network never makes. Both run at once. An episode counts as solved "
            "at 195 steps, the long-standing threshold."
        };
        pal_ = { {{8,11,14},"empty"}, {{90,209,196},"network"}, {{242,193,78},"table"},
                 {{60,70,84},"track"} };
        knobs_ = {
            {"seed", "run seed", 1.f, 40.f, 1.f, 1.f, {}, true,
             "Which run this is. The same seed reproduces the identical run; different "
             "seeds are independent runs of the SAME configuration. Every measurement in "
             "this project needed several — a single run cannot tell a real effect from a "
             "lucky one."},
            {"lr", "network learning rate", 0.001f, 0.1f, 0.01f, 0.001f, {}, false,
             "Gradient step for the Q-network."},
            {"epsilon", "exploration", 0.0f, 0.6f, 0.10f, 0.01f, {}, false,
             "Shared by both agents, so the comparison is like for like."},
            {"bins", "table bins per dimension", 3.f, 12.f, 4.f, 1.f, {}, true,
             "How finely the tabular agent chops each of the four state variables — and the "
             "single most important number here. Measured over 700 episodes at the default "
             "seed: at 4 bins the table averages 122 steps and reaches 481; at 6 it averages "
             "86; at 12, only 56. "
             "Too few boxes and distinct states become indistinguishable; too many and most of "
             "them are never visited at all. That choice is the real cost of putting a table on "
             "a continuous problem, and the network is never asked to make it."},
            {"speed", "steps per tick", 1.f, 400.f, 60.f, 1.f, {}, false, "Display rate only.", true},
        };
        surf_.resize(kW, kH);
        view_ = Field(96, 48);
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
        std::snprintf(b, sizeof b,
            "episode %d  ·  network %d steps (best %d)  ·  table %d (best %d)  ·  solved at 195",
            episode_, lastN_, bestN_, lastT_, bestT_);
        return b;
    }

    void reset() override {
        rng_.reseed(runSeed(0xCA47Full));
        bins_ = std::max(3, int(knob("bins") + 0.5f));
        net_.init({4, 24, 24, 2}, 8931, MLP::Out::Linear);
        target_ = net_;
        table_.assign(std::size_t(bins_) * bins_ * bins_ * bins_ * 2, 0.0f);
        replay_.clear(); replayAt_ = 0; sinceSync_ = 0;
        sN_ = start(); sT_ = start();
        stepsN_ = stepsT_ = 0; lastN_ = lastT_ = 0; bestN_ = bestT_ = 0;
        episode_ = 0; gen_ = 0;
        histN_.clear(); histT_.clear();
        publish();
    }

    void step() override {
        const int n = std::max(1, int(knob("speed") + 0.5f));
        for (int i = 0; i < n; ++i) { netStep(); tabStep(); }
        ++gen_;
        publish();
    }

    std::vector<Metric> metrics() const override {
        return {
            Metric{ "network: steps balanced", double(lastN_), 500.0 },
            Metric{ "table:   steps balanced", double(lastT_), 500.0 },
            Metric{ "episodes", double(episode_), 0.0, Metric::Neither },
        };
    }

    bool poke(float, float) override {          // shove the pole
        sN_.thd += 0.6f; sT_.thd += 0.6f;
        publish();
        return true;
    }

    // One epoch is one episode of the network agent — a pole falling is the
    // only boundary in this task that means anything.
    [[nodiscard]] const char* epoch_name() const override { return "episode"; }
    [[nodiscard]] int epoch_count() const override { return episode_; }
    bool advance_epoch() override {
        const int start = episode_;
        for (int guard = 0; guard < 200000 && episode_ == start; ++guard) { netStep(); tabStep(); }
        publish();
        return true;
    }

    // ── the published dynamics, in their frictionless form ─────────────────
    //
    // The 1983 appendix carries two friction terms this does not: mu_c = 0.0005
    // between cart and track, and mu_p = 0.000002 between pole and cart. What is
    // below is the frictionless simplification — the same equations Sutton's
    // pole.c uses. It is not "the paper exactly", and saying so here is cheaper
    // than someone finding out from the paper.
    static State advance(State s, bool pushRight) {
        const float force = pushRight ? kForce : -kForce;
        const float ct = std::cos(s.th), st = std::sin(s.th);
        const float totalMass = kMassCart + kMassPole;
        const float poleML    = kMassPole * kLength;
        const float temp = (force + poleML * s.thd * s.thd * st) / totalMass;
        const float thacc = (kGravity * st - ct * temp) /
                            (kLength * (4.0f / 3.0f - kMassPole * ct * ct / totalMass));
        const float xacc = temp - poleML * thacc * ct / totalMass;
        State n;
        n.x  = s.x  + kTau * s.xd;
        n.xd = s.xd + kTau * xacc;
        n.th = s.th + kTau * s.thd;
        n.thd = s.thd + kTau * thacc;
        return n;
    }
    static bool failed(const State& s) {
        return std::fabs(s.x) > kXFail || std::fabs(s.th) > kThetaFail;
    }

    // ── measurement ────────────────────────────────────────────────────────
    [[nodiscard]] int best_network() const { return bestN_; }
    [[nodiscard]] int best_table()   const { return bestT_; }
    [[nodiscard]] int episodes()     const { return episode_; }
    [[nodiscard]] double mean_last(bool tabular, int k) const {
        const auto& h = tabular ? histT_ : histN_;
        if (h.empty()) return 0.0;
        const std::size_t n = std::min(h.size(), std::size_t(k));
        double s = 0;
        for (std::size_t i = h.size() - n; i < h.size(); ++i) s += double(h[i]);
        return s / double(n);
    }
    void train(int episodes) {
        const int start = episode_;
        for (int guard = 0; guard < 8000000 && episode_ - start < episodes; ++guard) {
            netStep(); tabStep();
        }
    }

private:
    static constexpr int kW = 760, kH = 380;
    static constexpr std::size_t kReplayCap = 6000;
    struct Trans { std::vector<float> s, s2; int a; float r; bool done; };

    // Which independent run this is. Same seed, same run; different seed, an
    // independent sample of the same configuration.
    [[nodiscard]] std::uint64_t runSeed(std::uint64_t base) const {
        return mix_seed(base, int(knob("seed") + 0.5f));
    }
    [[nodiscard]] float knob(const char* k) const {
        for (auto& kn : knobs_) if (kn.key == k) return kn.value;
        return 0.f;
    }
    State start() {
        State s;
        s.x  = (rng_.unit() - 0.5f) * 0.1f;
        s.xd = (rng_.unit() - 0.5f) * 0.1f;
        s.th = (rng_.unit() - 0.5f) * 0.1f;
        s.thd= (rng_.unit() - 0.5f) * 0.1f;
        return s;
    }
    static std::vector<float> encode(const State& s) {
        // Scaled to roughly unit range so no one input dominates the first layer.
        return { s.x / kXFail, s.xd / 2.0f, s.th / kThetaFail, s.thd / 2.5f };
    }
    [[nodiscard]] std::size_t box(const State& s) const {
        auto b = [&](float v, float lo, float hi) {
            const float t = (v - lo) / (hi - lo);
            return std::size_t(std::clamp(int(t * float(bins_)), 0, bins_ - 1));
        };
        return ((b(s.x, -kXFail, kXFail) * std::size_t(bins_)
               + b(s.xd, -2.0f, 2.0f)) * std::size_t(bins_)
               + b(s.th, -kThetaFail, kThetaFail)) * std::size_t(bins_)
               + b(s.thd, -2.5f, 2.5f);
    }
    [[nodiscard]] int bestNet(const State& s) const {
        MLP& n = const_cast<MLP&>(net_);
        const auto& q = n.forward(encode(s));
        return q[1] > q[0] ? 1 : 0;
    }
    [[nodiscard]] int bestTab(const State& s) const {
        const std::size_t k = box(s) * 2;
        return table_[k + 1] > table_[k] ? 1 : 0;
    }

    void learnFrom(const Trans& t) {
        const auto& q = net_.forward(t.s);
        std::vector<float> target(q.begin(), q.end());
        float boot = t.r;
        if (!t.done) {
            MLP& tn = target_;
            const auto& q2 = tn.forward(t.s2);
            boot = t.r + 0.99f * std::max(q2[0], q2[1]);
        }
        target[std::size_t(t.a)] = boot;
        net_.forward(t.s);
        net_.zero_grad();
        net_.backward(target);
        net_.apply(knob("lr"), 1);
    }

    void netStep() {
        const int a = (rng_.unit() < knob("epsilon")) ? (rng_.unit() < 0.5f ? 0 : 1)
                                                      : bestNet(sN_);
        const State n = advance(sN_, a == 1);
        const bool done = failed(n) || stepsN_ + 1 >= 500;
        const float r = failed(n) ? -1.0f : 0.02f;     // upright is worth something each tick

        Trans t{ encode(sN_), encode(n), a, r, failed(n) };
        if (replay_.size() < kReplayCap) replay_.push_back(t);
        else { replay_[replayAt_] = t; replayAt_ = (replayAt_ + 1) % kReplayCap; }
        for (int k = 0; k < 4 && !replay_.empty(); ++k)
            learnFrom(replay_[std::size_t(rng_.unit() * float(replay_.size())) % replay_.size()]);
        if (++sinceSync_ >= 500) { target_ = net_; sinceSync_ = 0; }

        sN_ = n; ++stepsN_;
        if (done) {
            lastN_ = stepsN_; bestN_ = std::max(bestN_, stepsN_);
            histN_.push_back(stepsN_);
            if (histN_.size() > 4000) histN_.erase(histN_.begin());
            stepsN_ = 0; sN_ = start(); ++episode_;
        }
    }

    void tabStep() {
        const float alpha = 0.15f, gamma = 0.99f;
        const int a = (rng_.unit() < knob("epsilon")) ? (rng_.unit() < 0.5f ? 0 : 1)
                                                      : bestTab(sT_);
        const State n = advance(sT_, a == 1);
        const bool dead = failed(n);
        const bool done = dead || stepsT_ + 1 >= 500;
        const float r = dead ? -1.0f : 0.02f;
        const std::size_t k = box(sT_) * 2 + std::size_t(a);
        float best = 0.0f;
        if (!dead) {
            const std::size_t k2 = box(n) * 2;
            best = std::max(table_[k2], table_[k2 + 1]);
        }
        table_[k] += alpha * ((dead ? r : r + gamma * best) - table_[k]);
        sT_ = n; ++stepsT_;
        if (done) {
            lastT_ = stepsT_; bestT_ = std::max(bestT_, stepsT_);
            histT_.push_back(stepsT_);
            if (histT_.size() > 4000) histT_.erase(histT_.begin());
            stepsT_ = 0; sT_ = start();
        }
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
    void box2(int x0, int y0, int x1, int y1, Rgb c) {
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) px(x, y, c, 1.f);
    }
    void thick(float x0, float y0, float x1, float y1, int r, Rgb c) {
        const int steps = std::max(1, int(std::max(std::fabs(x1-x0), std::fabs(y1-y0))));
        for (int i = 0; i <= steps; ++i) {
            const float t = float(i)/float(steps);
            const int cx = int(x0 + (x1-x0)*t), cy = int(y0 + (y1-y0)*t);
            for (int oy=-r; oy<=r; ++oy) for (int ox=-r; ox<=r; ++ox)
                if (ox*ox+oy*oy <= r*r+r) px(cx+ox, cy+oy, c, 1.f);
        }
    }
    void drawCart(const State& s, int baseY, Rgb c) {
        const float scale = float(kW) * 0.36f / kXFail;
        const int cx = kW/2 + int(s.x * scale);
        box2(kW/2 - int(kXFail*scale), baseY, kW/2 + int(kXFail*scale), baseY+2, Rgb{60,70,84});
        box2(cx-22, baseY-12, cx+22, baseY, c);
        const float len = 92.0f;
        thick(float(cx), float(baseY-12),
              float(cx) + std::sin(s.th)*len, float(baseY-12) - std::cos(s.th)*len, 2, c);
    }
    void plot(const std::vector<int>& h, Rgb c, int L, int T, int W, int H) {
        if (h.size() < 2) return;
        const double n = double(h.size() - 1);
        int px0=-1, py0=-1;
        for (std::size_t i = 0; i < h.size(); ++i) {
            const int x = L + int(double(W) * double(i) / n);
            const int y = T + H - int(double(H) * std::min(1.0, double(h[i]) / 500.0));
            if (px0 >= 0) {
                const int st = std::max(1, std::max(std::abs(x-px0), std::abs(y-py0)));
                for (int k = 0; k <= st; ++k)
                    px(px0 + (x-px0)*k/st, py0 + (y-py0)*k/st, c, 1.f);
            }
            px0 = x; py0 = y;
        }
    }

    void publish() {
        for (int i = 0; i < kW*kH; ++i) {
            std::uint8_t* p = &surf_.rgba[std::size_t(i)*4];
            p[0]=12; p[1]=15; p[2]=20; p[3]=255;
        }
        drawCart(sN_, 150, Rgb{90,209,196});      // network
        drawCart(sT_, 300, Rgb{242,193,78});      // table
        // Survival curves underneath, with the 195-step solved line marked.
        const int L = 40, T = 320, W = kW - L - 20, H = 50;
        const int solved = T + H - int(double(H) * 195.0 / 500.0);
        for (int x = L; x < L + W; x += 3) px(x, solved, Rgb{110,120,90}, 1.f);
        plot(histT_, Rgb{242,193,78}, L, T, W, H);
        plot(histN_, Rgb{90,209,196}, L, T, W, H);

        view_.fill(0);
        for (int i = 0; i < view_.w; ++i) {
            auto band = [&](const std::vector<int>& h, std::uint8_t idx) {
                if (h.empty()) return;
                const std::size_t k = std::size_t(double(i)/double(view_.w-1) * double(h.size()-1));
                const int y = std::clamp(view_.h - 1 - int(double(h[k])/500.0*double(view_.h-1)),
                                         0, view_.h-1);
                view_.set(i, y, idx);
            };
            band(histT_, 2);
            band(histN_, 1);
        }
    }

    Provenance about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    Field   view_;
    Surface surf_;
    MLP     net_, target_;
    std::vector<float> table_;
    std::vector<Trans> replay_;
    std::size_t replayAt_ = 0;
    State   sN_, sT_;
    std::vector<int> histN_, histT_;
    int bins_ = 6, stepsN_ = 0, stepsT_ = 0, lastN_ = 0, lastT_ = 0;
    int bestN_ = 0, bestT_ = 0, episode_ = 0, sinceSync_ = 0;
    Rng rng_{0xCA47Full};
    std::uint64_t gen_ = 0;
};

inline SimPtr make_cartpole() { return std::make_unique<CartPole>(); }

} // namespace bench
