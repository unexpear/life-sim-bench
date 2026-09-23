// continual.hpp — transfer, forgetting and affordance, measured.
//
// The experiment runs itself, in two phases, with a CONTROL:
//
//   phase 1   train one network on task A
//   phase 2   carry on training that same network on task B, and at the same
//             time train a SECOND network on B from scratch
//
// Three things are measured every epoch and drawn as curves:
//
//   · accuracy on B of the transferred net   — versus
//   · accuracy on B of the from-scratch net  — the control, without which
//     "it learned B quickly" means nothing at all
//   · accuracy on A of the transferred net   — which falls during phase 2.
//     That fall is catastrophic forgetting. It is a curve, not an assertion.
//
//   McCloskey, M. & Cohen, N. J. "Catastrophic Interference in Connectionist
//   Networks", Psychology of Learning and Motivation 24 (1989) 109-165.
//   French, R. M. "Catastrophic forgetting in connectionist networks",
//   Trends in Cognitive Sciences 3 (1999) 128-135.
//
// The AFFORDANCE task is the same machinery pointed at a different question:
// instead of predicting a label, the network predicts which MOVES ARE POSSIBLE
// from a square — what the environment affords. Ground truth is the maze's own
// walls, so accuracy is checkable rather than eyeballed.

#pragma once
#include "../sim.hpp"
#include "../learn/mlp.hpp"
#include "../render/voxel.hpp"     // Surface
#include <algorithm>
#include <cmath>
#include <vector>

namespace bench {

class Continual final : public Sim {
public:
    Continual() {
        about_ = Provenance{
            "Transfer & catastrophic forgetting", "1989",
            "Michael McCloskey & Neal Cohen",
            "McCloskey, M. & Cohen, N. J. \"Catastrophic Interference in Connectionist "
            "Networks\", Psychology of Learning and Motivation 24 (1989) 109-165",
            Replication::No,
            "No. This is a learning experiment, not a self-replicator, and the bench keeps the "
            "two apart no matter how much the vocabulary overlaps.",
            "Train a network on task A, then on task B, and watch what happens to A. The drop "
            "is catastrophic forgetting - the network has no mechanism for keeping A, so B "
            "simply overwrites the weights that held it. A second network learns B from scratch "
            "as a control: without it, 'the transferred net learned B fast' is not a claim you "
            "can check."
        };
        pal_ = { {{8,11,14},"empty"}, {{90,209,196},"task B (transferred)"},
                 {{242,193,78},"task B (from scratch)"}, {{217,83,79},"task A, being forgotten"} };
        knobs_ = {
            {"seed", "run seed", 1.f, 40.f, 1.f, 1.f, {}, true,
             "Which run this is. The same seed reproduces the identical run; different "
             "seeds are independent runs of the SAME configuration. Every measurement in "
             "this project needed several — a single run cannot tell a real effect from a "
             "lucky one."},
            {"taskB", "task B", 0.f, 3.f, 0.f, 1.f,
             {"XOR of the OTHER two bits", "XNOR of the same bits", "OR of the same bits",
              "affordance: which moves are legal"}, true,
             "What to train second. The relationship between A and B decides everything, and "
             "not in the order you would guess: the opposite function overwrites A hardest, a "
             "DISJOINT one next, an overlapping one least. In the three binary modes both tasks "
             "share one output, so once B is learned what is left of A is just how often the two "
             "label an input the same way — 0 of 16 examples for the opposite, 8 of 16 for the "
             "disjoint XOR, 12 of 16 for OR. Measured at phase1=600, read at epoch 1800, seeds "
             "1-5: A ends at 0.00 in four of the five seeds and 0.25 in the fifth for the "
             "opposite, at 0.50 in all five for the disjoint XOR, and at 0.75 in all five for "
             "OR."},
            {"phase1", "epochs on task A", 200.f, 4000.f, 1200.f, 100.f, {}, true,
             "How long to train A before switching. Longer means a deeper A to destroy."},
            {"lr", "learning rate", 0.01f, 1.0f, 0.25f, 0.01f, {}, false,
             "From 0.01 up to the default, larger steps overwrite the old task faster; past "
             "that the net stops learning B at all and A stops falling — forgetting is not a "
             "fixed property of the network, it is a function of how hard you push it."},
            {"speed", "epochs per tick", 1.f, 200.f, 25.f, 1.f, {}, false,
             "Display rate only.", true},
        };
        surf_.resize(kW, kH);
        view_ = Field(64, 32);
        build();
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
        std::snprintf(b, sizeof b, "%s  ·  epoch %d  ·  A %.0f%%  B %.0f%%  (scratch B %.0f%%)",
                      epoch_ < phase1_ ? "learning A" : "learning B", epoch_,
                      100.0 * accA_, 100.0 * accB_, 100.0 * accScratch_);
        return b;
    }

    void reset() override { build(); }

    void step() override {
        const int n = std::max(1, int(knob("speed") + 0.5f));
        const float lr = knob("lr");
        for (int i = 0; i < n; ++i) {
            if (epoch_ < phase1_) trainOn(net_, A_, lr);
            else {
                trainOn(net_, B_, lr);
                trainOn(scratch_, B_, lr);       // the control, same data, same steps
            }
            ++epoch_;
            if ((epoch_ & 3) == 0) sample();
        }
        ++gen_;
        render();
    }

    std::vector<Metric> metrics() const override {
        return {
            Metric{ "accuracy on A", accA_, 1.0 },
            Metric{ "accuracy on B", accB_, 1.0 },
            Metric{ "B from scratch (control)", accScratch_, 1.0 },
        };
    }

    // Re-run the whole experiment from a different initialisation. Rebuilding
    // with the same seed reproduces the identical run, which is correct and
    // useless as a control — the point of re-running is to see whether the
    // forgetting you just watched was a property of the setup or of one draw.
    bool poke(float, float) override {
        seed_ += 2654435761u;
        build();
        return true;
    }

    // One epoch is 100 training epochs — the natural unit here is not one
    // gradient step, which is invisible, but a block big enough to move the
    // curve.
    [[nodiscard]] const char* epoch_name() const override { return "100 epochs"; }
    [[nodiscard]] int epoch_count() const override { return epoch_ / 100; }
    bool advance_epoch() override {
        const float lr = knob("lr");
        for (int i = 0; i < 100; ++i) {
            if (epoch_ < phase1_) trainOn(net_, A_, lr);
            else { trainOn(net_, B_, lr); trainOn(scratch_, B_, lr); }
            ++epoch_;
            if ((epoch_ & 3) == 0) sample();
        }
        render();
        return true;
    }

    // ── what the tests measure ─────────────────────────────────────────────
    [[nodiscard]] double acc_a() const { return accA_; }
    [[nodiscard]] double acc_b() const { return accB_; }
    [[nodiscard]] double acc_scratch() const { return accScratch_; }
    [[nodiscard]] int    phase1() const { return phase1_; }
    void run_to(int epochs) {
        const float lr = knob("lr");
        while (epoch_ < epochs) {
            if (epoch_ < phase1_) trainOn(net_, A_, lr);
            else { trainOn(net_, B_, lr); trainOn(scratch_, B_, lr); }
            ++epoch_;
            if ((epoch_ & 3) == 0) sample();
        }
        render();
    }
    // Epochs the transferred net needed to first clear a threshold on B, and
    // the same for the control. The comparison IS the transfer measurement.
    //
    // Answered from the climb staircase rather than by scanning the history,
    // because the history is a sliding window: it used to return int(i)*4 for
    // history index i, and once sample() started dropping the front that index
    // stopped meaning an epoch. The same crossing in the same run reported 312
    // epochs when asked at epoch 800 and 0 when asked again at 4000. The
    // staircase keeps the answer instead of searching for it.
    [[nodiscard]] int epochs_to(double threshold, bool scratch) const {
        const auto& c = scratch ? climbScratch_ : climbB_;
        for (const auto& rung : c) if (rung.acc >= threshold) return rung.epoch;
        return -1;
    }

private:
    struct Set { std::vector<std::vector<float>> x, y; };
    // One step of a curve's running maximum: the height, and the epoch it was
    // first reached at. See climb() below.
    struct Rung { double acc; int epoch; };
    static constexpr int kW = 680, kH = 380;

    // Which independent run this is. Same seed, same run; different seed, an
    // independent sample of the same configuration.
    [[nodiscard]] std::uint64_t runSeed(std::uint64_t base) const {
        return mix_seed(base, int(knob("seed") + 0.5f));
    }
    [[nodiscard]] float knob(const char* key) const {
        for (auto& kn : knobs_) if (kn.key == key) return kn.value;
        return 0.f;
    }

    void build() {
        phase1_ = int(knob("phase1") + 0.5f);
        buildTasks();
        const int in = int(A_.x[0].size()), out = int(A_.y[0].size());
        net_.init({in, 10, out}, runSeed(seed_));
        scratch_.init({in, 10, out}, runSeed(seed_));   // same start, so only the history differs
        epoch_ = 0; gen_ = 0;
        accA_ = accB_ = accScratch_ = 0.0;
        histA_.clear(); histB_.clear(); histScratch_.clear();
        climbB_.clear(); climbScratch_.clear(); dropped_ = 0;
        sample();
        render();
    }

    void buildTasks() {
        A_ = Set{}; B_ = Set{};
        const int mode = int(knob("taskB") + 0.5f);
        if (mode == 3) {
            // AFFORDANCE. Inputs are a square's coordinates; outputs are which
            // of the four moves that square affords. Ground truth is the maze
            // itself, so accuracy is a fact rather than an impression.
            buildMaze();
            for (int y = 0; y < kMazeH; ++y)
                for (int x = 0; x < kMazeW; ++x) {
                    if (maze_[std::size_t(y) * kMazeW + x]) continue;
                    std::vector<float> in{ float(x) / float(kMazeW - 1),
                                           float(y) / float(kMazeH - 1) };
                    std::vector<float> out(4, 0.0f);
                    const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
                    for (int a = 0; a < 4; ++a) {
                        const int nx = x + dx[a], ny = y + dy[a];
                        const bool legal = nx >= 0 && ny >= 0 && nx < kMazeW && ny < kMazeH
                                        && !maze_[std::size_t(ny) * kMazeW + nx];
                        out[std::size_t(a)] = legal ? 1.0f : 0.0f;
                    }
                    B_.x.push_back(in); B_.y.push_back(out);
                }
            // Task A for this mode is a simpler question on the same inputs:
            // "is this square in the left half" — something to forget.
            for (std::size_t i = 0; i < B_.x.size(); ++i) {
                A_.x.push_back(B_.x[i]);
                A_.y.push_back(std::vector<float>(4, B_.x[i][0] < 0.5f ? 1.0f : 0.0f));
            }
            return;
        }
        // Four binary inputs, sixteen examples. A is XOR of the first two bits.
        for (int m = 0; m < 16; ++m) {
            std::vector<float> in{ float((m >> 0) & 1), float((m >> 1) & 1),
                                   float((m >> 2) & 1), float((m >> 3) & 1) };
            const float a = float(int(in[0]) ^ int(in[1]));
            float b = 0.0f;
            switch (mode) {
                case 0: b = float(int(in[2]) ^ int(in[3])); break;   // disjoint bits
                case 1: b = 1.0f - a;                        break;   // the exact opposite
                default: b = float(int(in[0]) | int(in[1])); break;   // overlapping
            }
            A_.x.push_back(in); A_.y.push_back({a});
            B_.x.push_back(in); B_.y.push_back({b});
        }
    }

    void buildMaze() {
        maze_.assign(std::size_t(kMazeW) * kMazeH, 0);
        for (int y = 2; y < kMazeH - 2; ++y) if (y != kMazeH / 2) maze_[std::size_t(y)*kMazeW + kMazeW/3] = 1;
        for (int x = 3; x < kMazeW - 3; ++x) if (x != kMazeW / 2) maze_[std::size_t(kMazeH/3)*kMazeW + x] = 1;
    }

    static void trainOn(MLP& net, const Set& s, float lr) {
        net.zero_grad();
        for (std::size_t i = 0; i < s.x.size(); ++i) {
            net.forward(s.x[i]);
            net.backward(s.y[i]);
        }
        net.apply(lr, int(s.x.size()));
    }
    static double accuracy(MLP& net, const Set& s) {
        if (s.x.empty()) return 0.0;
        std::size_t right = 0, total = 0;
        for (std::size_t i = 0; i < s.x.size(); ++i) {
            const auto& o = net.forward(s.x[i]);
            for (std::size_t j = 0; j < o.size(); ++j) {
                right += ((o[j] > 0.5f) == (s.y[i][j] > 0.5f)) ? 1 : 0;
                ++total;
            }
        }
        return double(right) / double(total);
    }

    void sample() {
        accA_ = accuracy(net_, A_);
        accB_ = accuracy(net_, B_);
        accScratch_ = accuracy(scratch_, B_);
        histA_.push_back(accA_);
        histB_.push_back(accB_);
        histScratch_.push_back(accScratch_);
        climb(climbB_, accB_, epoch_);
        climb(climbScratch_, accScratch_, epoch_);
        if (histA_.size() > kMaxPoints) {          // keep it bounded, like everything else here
            histA_.erase(histA_.begin());
            histB_.erase(histB_.begin());
            histScratch_.erase(histScratch_.begin());
            ++dropped_;                            // so an index can still be turned into an epoch
        }
    }

    // Every time a curve sets a new high, remember the height and the epoch it
    // first happened at. The first sample to clear any threshold is necessarily
    // a new high — everything before it was below the threshold, and therefore
    // below it — so this staircase answers epochs_to() exactly as a scan of the
    // untrimmed history would, and it survives the window sliding.
    static void climb(std::vector<Rung>& c, double acc, int epoch) {
        if (c.empty() || acc > c.back().acc) c.push_back(Rung{acc, epoch});
    }

    // ── the chart ──────────────────────────────────────────────────────────
    void px(int x, int y, Rgb c, float a) {
        if (x < 0 || y < 0 || x >= kW || y >= kH || a <= 0.0f) return;
        std::uint8_t* p = &surf_.rgba[(std::size_t(y) * kW + x) * 4];
        const float k = std::min(1.0f, a);
        p[0] = std::uint8_t(p[0] + (float(c.r) - float(p[0])) * k);
        p[1] = std::uint8_t(p[1] + (float(c.g) - float(p[1])) * k);
        p[2] = std::uint8_t(p[2] + (float(c.b) - float(p[2])) * k);
        p[3] = 255;
    }
    void plot(const std::vector<double>& v, Rgb c, int L, int T, int W, int H) {
        if (v.size() < 2) return;
        const double n = double(v.size() - 1);
        int px0 = -1, py0 = -1;
        for (std::size_t i = 0; i < v.size(); ++i) {
            const int x = L + int(double(W) * double(i) / n);
            const int y = T + int(double(H) * (1.0 - std::clamp(v[i], 0.0, 1.0)));
            if (px0 >= 0) {
                const int steps = std::max(1, std::max(std::abs(x - px0), std::abs(y - py0)));
                for (int k = 0; k <= steps; ++k) {
                    const int ix = px0 + (x - px0) * k / steps;
                    const int iy = py0 + (y - py0) * k / steps;
                    px(ix, iy, c, 1.0f); px(ix, iy - 1, c, 0.6f); px(ix, iy + 1, c, 0.6f);
                }
            }
            px0 = x; py0 = y;
        }
    }

    void render() {
        for (int i = 0; i < kW * kH; ++i) {
            std::uint8_t* p = &surf_.rgba[std::size_t(i) * 4];
            p[0] = 12; p[1] = 15; p[2] = 20; p[3] = 255;
        }
        const int L = 54, T = 30, W = kW - L - 20, H = kH - T - 40;
        for (int i = 0; i <= 4; ++i) {                       // gridlines at 0/25/50/75/100%
            const int y = T + H * i / 4;
            for (int x = L; x < L + W; ++x) px(x, y, Rgb{40, 50, 60}, 1.0f);
        }
        for (int y = T; y < T + H; ++y) px(L, y, Rgb{60, 74, 88}, 1.0f);

        // Where phase 2 began: everything to the right of this line is the
        // network being trained on B, and the red curve falling away is A
        // being destroyed.
        //
        // The switch is an index into the samples STILL HELD, so the samples
        // already dropped have to come off it. Without that subtraction the
        // line sat at a fixed fraction of the chart while the curve underneath
        // it scrolled away, and past 2560 epochs it marked nothing. When the
        // switch itself has scrolled off there is no line to draw: every sample
        // on screen is then phase 2.
        if (histA_.size() > 1) {
            const double total = double(histA_.size() - 1);
            const double at = double(phase1_ / 4) - double(dropped_);
            if (at >= 0.0 && at <= total) {
                const int x = L + int(double(W) * at / total);
                for (int y = T; y < T + H; y += 4) px(x, y, Rgb{150, 150, 90}, 0.9f);
            }
        }
        plot(histScratch_, Rgb{242, 193, 78}, L, T, W, H);   // control
        plot(histB_,       Rgb{ 90, 209, 196}, L, T, W, H);  // transferred, on B
        plot(histA_,       Rgb{217,  83,  79}, L, T, W, H);  // A, being forgotten
        publishBands();
    }

    // The index field carries the three curves as bands, so the workbench
    // timeline records the EXPERIMENT rather than a picture of it.
    void publishBands() {
        view_.fill(0);
        const int n = view_.w;
        for (int i = 0; i < n && !histA_.empty(); ++i) {
            const std::size_t k = std::size_t(double(i) / double(n - 1) * double(histA_.size() - 1));
            auto put = [&](double v, std::uint8_t idx) {
                const int y = std::clamp(int((1.0 - v) * double(view_.h - 1)), 0, view_.h - 1);
                view_.set(i, y, idx);
            };
            put(histScratch_[k], 2);
            put(histB_[k], 1);
            put(histA_[k], 3);
        }
    }

    static constexpr int kMazeW = 15, kMazeH = 11;
    static constexpr std::size_t kMaxPoints = 640;

    Provenance about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    MLP     net_, scratch_;
    Set     A_, B_;
    std::vector<std::uint8_t> maze_;
    Field   view_;
    Surface surf_;
    std::vector<double> histA_, histB_, histScratch_;
    std::vector<Rung>   climbB_, climbScratch_;
    std::size_t dropped_ = 0;             // samples the sliding window has discarded
    double  accA_ = 0, accB_ = 0, accScratch_ = 0;
    int     epoch_ = 0, phase1_ = 1200;
    std::uint64_t seed_ = 4242;
    std::uint64_t gen_ = 0;
};

inline SimPtr make_continual() { return std::make_unique<Continual>(); }

} // namespace bench
