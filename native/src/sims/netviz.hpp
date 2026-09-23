// netviz.hpp — a picture of a network that is a picture of THE network.
//
// Every edge here is read straight out of the live MLP as it trains: thickness
// is |w|, colour is the sign of w, and each node's fill is its current
// activation for the example being shown. Nothing is cached, smoothed or
// re-derived, because a diagram of a model that is not wired to the model is a
// drawing of an idea, and this bench has already been caught doing exactly that
// once with the fake 3D.
//
// One transform is unavoidable and is declared in the blurb: hidden units are
// tanh, so their -1..1 is drawn as (a+1)/2. Inputs and the sigmoid output are
// already 0..1 and are drawn untouched — see render().
//
// You can take hold of the nodes and move them. Layout is presentation and
// nothing else — dragging a node changes where an edge is drawn and cannot
// change what the network computes, which is worth being able to confirm by
// hand.
//
// The rendered image goes through Sim::surface(). The coarse Field published
// alongside is the WEIGHT MATRIX as a heatmap, so the workbench timeline
// records the weights over training and scrubbing it rewinds the model's
// history rather than a picture of it.

#pragma once
#include "../sim.hpp"
#include "../learn/mlp.hpp"
#include "../render/voxel.hpp"     // for Surface
#include <algorithm>
#include <cmath>
#include <vector>

namespace bench {

class NetViz final : public Sim {
public:
    NetViz() {
        about_ = Provenance{
            "Neural network, live", "1986",
            "Rumelhart, Hinton & Williams (backpropagation)",
            "Rumelhart, D. E., Hinton, G. E. & Williams, R. J. \"Learning representations by "
            "back-propagating errors\", Nature 323 (1986) 533-536",
            Replication::No,
            "No. It learns; it does not copy itself. The bench keeps those apart because "
            "conflating them is the single most common overclaim in this whole field.",
            "A multilayer perceptron learning XOR, drawn from its own weights while it trains. "
            "Edge thickness is the size of a weight and colour is its sign; a node's brightness "
            "is its activation on the example currently being shown. Inputs and the output unit "
            "run 0 to 1 and are drawn straight; the hidden units are tanh and run -1 to 1, so "
            "their brightness is (a+1)/2 - a hidden unit at mid grey is sitting at zero, not at "
            "its floor. Drag the nodes about - layout is presentation, and moving one cannot "
            "change what the network computes."
        };
        pal_ = { {{8,11,14}, "zero"} };
        for (int i = 1; i <= kBands; ++i) {              // negative weights
            const float t = float(i) / float(kBands);
            pal_.push_back({{ std::uint8_t(20 + 197*t), std::uint8_t(24 + 59*t),
                              std::uint8_t(30 + 49*t) }, i == kBands ? "negative" : "" });
        }
        for (int i = 1; i <= kBands; ++i) {              // positive weights
            const float t = float(i) / float(kBands);
            pal_.push_back({{ std::uint8_t(20 + 70*t), std::uint8_t(24 + 185*t),
                              std::uint8_t(30 + 166*t) }, i == kBands ? "positive" : "" });
        }
        knobs_ = {
            {"seed", "run seed", 1.f, 40.f, 1.f, 1.f, {}, true,
             "Which run this is. The same seed reproduces the identical run; different "
             "seeds are independent runs of the SAME configuration. Every measurement in "
             "this project needed several — a single run cannot tell a real effect from a "
             "lucky one."},
            {"lr", "learning rate", 0.01f, 2.0f, 0.5f, 0.01f, {}, false,
             "Step size for gradient descent. Too small and it crawls."},
            {"hidden", "hidden units", 2.f, 12.f, 4.f, 1.f, {}, true,
             "Width of the hidden layer. XOR needs at least 2 — with one hidden unit the "
             "network is effectively linear and cannot represent it at all. Rebuilds the net."},
            {"speed", "training steps per tick", 1.f, 400.f, 40.f, 1.f, {}, false,
             "Epochs per simulation tick. Display rate only; it does not change the learning.", true},
            {"showcase", "example being shown", 0.f, 3.f, 0.f, 1.f,
             {"0 XOR 0 = 0", "0 XOR 1 = 1", "1 XOR 0 = 1", "1 XOR 1 = 0"}, false,
             "Which input the node brightnesses are showing. The weights are the same either "
             "way — this only changes which activation pattern is drawn."},
        };
        surf_.resize(kW, kH);
        view_ = Field(48, 24);
        build();
    }

    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field&               field()   const override { return view_; }
    const Surface*             surface() const override { return &surf_; }
    std::uint64_t              generation() const override { return gen_; }
    std::vector<Knob>&         knobs()   override { return knobs_; }

    [[nodiscard]] std::string subtitle() const override {
        char b[128];
        std::snprintf(b, sizeof b, "2-%d-1  ·  loss %.5f  ·  moved %.2f from init",
                      hidden_, double(loss_), double(drift_));
        return b;
    }

    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        if (k == "hidden" && int(v + 0.5f) != hidden_) build();
        if (k == "showcase") { render(); }
    }

    void reset() override { build(); }

    void step() override {
        const int epochs = std::max(1, int(knob("speed") + 0.5f));
        const float lr = knob("lr");
        for (int e = 0; e < epochs; ++e) {
            loss_ = 0.0f;
            net_.zero_grad();
            for (int i = 0; i < 4; ++i) {
                net_.forward(X[i]);
                loss_ += net_.loss(Y[i]);
                net_.backward(Y[i]);
            }
            net_.apply(lr, 4);
            ++epoch_;
        }
        drift_ = net_.distance_from(init_);
        ++gen_;
        render();
    }

    std::vector<Metric> metrics() const override {
        int right = 0;
        MLP& n = const_cast<MLP&>(net_);
        for (int i = 0; i < 4; ++i) right += ((n.forward(X[i])[0] > 0.5f) == (Y[i][0] > 0.5f)) ? 1 : 0;
        return {
            Metric{ "loss",                double(loss_), 0.0, Metric::Lower },
            Metric{ "cases correct (of 4)", double(right), 4.0 },
            // How far the weights have moved from where they started. This is
            // the honest answer to "has the model changed" — the loss can sit
            // still while the weights keep travelling.
            Metric{ "weight travel",       double(drift_), 0.0, Metric::Neither },
        };
    }

    // One epoch is 100 training epochs: a single gradient step is invisible,
    // so the useful unit is a block big enough to move the loss.
    [[nodiscard]] const char* epoch_name() const override { return "100 epochs"; }
    [[nodiscard]] int epoch_count() const override { return epoch_ / 100; }
    bool advance_epoch() override {
        const float lr = knob("lr");
        for (int e = 0; e < 100; ++e) {
            loss_ = 0.0f;
            net_.zero_grad();
            for (int i = 0; i < 4; ++i) { net_.forward(X[i]); loss_ += net_.loss(Y[i]); net_.backward(Y[i]); }
            net_.apply(lr, 4);
            ++epoch_;
        }
        drift_ = net_.distance_from(init_);
        render();
        return true;
    }

    // ── dragging nodes ─────────────────────────────────────────────────────
    bool drag_begin(float nx, float ny) override {
        const float px = nx * float(kW), py = ny * float(kH);
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            const float dx = nodes_[i].x - px, dy = nodes_[i].y - py;
            if (dx*dx + dy*dy <= (kNodeR + 6) * (kNodeR + 6)) { held_ = int(i); return true; }
        }
        held_ = -1;
        return false;                      // nothing grabbed: let the click through
    }
    void drag_move(float nx, float ny) override {
        if (held_ < 0) return;
        nodes_[std::size_t(held_)].x = std::clamp(nx * float(kW), 12.0f, float(kW) - 12.0f);
        nodes_[std::size_t(held_)].y = std::clamp(ny * float(kH), 12.0f, float(kH) - 12.0f);
        render();
    }
    void drag_end() override { held_ = -1; }

    // Start the model again from a different random initialisation. Re-running
    // the layout was the obvious "stamp" and it is a no-op unless a node has
    // been dragged. Reinitialising is the useful action anyway: it shows how
    // much of where a network ends up is decided by where it started.
    bool poke(float, float) override {
        seed_ += 1013904223u;
        net_.init({2, hidden_, 1}, runSeed(seed_));
        init_ = net_;
        epoch_ = 0; loss_ = 0.0f; drift_ = 0.0f;
        render();
        return true;
    }

    // ── for the tests ──────────────────────────────────────────────────────
    [[nodiscard]] float loss() const { return loss_; }
    [[nodiscard]] int   epoch() const { return epoch_; }
    [[nodiscard]] int   cases_correct() {
        int r = 0;
        for (int i = 0; i < 4; ++i) r += ((net_.forward(X[i])[0] > 0.5f) == (Y[i][0] > 0.5f)) ? 1 : 0;
        return r;
    }
    [[nodiscard]] std::size_t node_count() const { return nodes_.size(); }
    [[nodiscard]] std::pair<float,float> node_pos(std::size_t i) const {
        return { nodes_[i].x, nodes_[i].y };
    }
    [[nodiscard]] const MLP& net() const { return net_; }

private:
    struct Node { float x, y; int layer, index; };
    static constexpr int kW = 660, kH = 420;
    static constexpr float kNodeR = 15.0f;
    static constexpr int kBands = 6;

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
        hidden_ = std::max(1, int(knob("hidden") + 0.5f));
        net_.init({2, hidden_, 1}, runSeed(seed_));
        init_ = net_;                    // a snapshot to measure travel against
        epoch_ = 0; gen_ = 0; loss_ = 0.0f; drift_ = 0.0f;
        layout();
        render();
    }

    void layout() {
        nodes_.clear();
        const int L = net_.layers();
        for (int l = 0; l < L; ++l) {
            const int n = net_.units(l);
            const float x = 90.0f + (float(kW) - 180.0f) * float(l) / float(std::max(1, L - 1));
            for (int i = 0; i < n; ++i) {
                const float y = float(kH) * (float(i) + 1.0f) / float(n + 1);
                nodes_.push_back(Node{ x, y, l, i });
            }
        }
    }
    [[nodiscard]] int nodeIndex(int layer, int i) const {
        int k = 0;
        for (int l = 0; l < layer; ++l) k += net_.units(l);
        return k + i;
    }

    // ── drawing ────────────────────────────────────────────────────────────
    void px(int x, int y, Rgb c, float a) {
        if (x < 0 || y < 0 || x >= kW || y >= kH || a <= 0.0f) return;
        std::uint8_t* p = &surf_.rgba[(std::size_t(y) * kW + x) * 4];
        const float k = std::min(1.0f, a);
        p[0] = std::uint8_t(p[0] + (float(c.r) - float(p[0])) * k);
        p[1] = std::uint8_t(p[1] + (float(c.g) - float(p[1])) * k);
        p[2] = std::uint8_t(p[2] + (float(c.b) - float(p[2])) * k);
        p[3] = 255;
    }
    // Thick line by distance to the segment, so edges of different weight are
    // genuinely different widths rather than stippled.
    void line(float x0, float y0, float x1, float y1, float wide, Rgb c, float alpha) {
        const float dx = x1 - x0, dy = y1 - y0;
        const float len2 = dx*dx + dy*dy;
        const int minX = std::max(0, int(std::floor(std::min(x0, x1) - wide - 1)));
        const int maxX = std::min(kW - 1, int(std::ceil (std::max(x0, x1) + wide + 1)));
        const int minY = std::max(0, int(std::floor(std::min(y0, y1) - wide - 1)));
        const int maxY = std::min(kH - 1, int(std::ceil (std::max(y0, y1) + wide + 1)));
        for (int y = minY; y <= maxY; ++y)
            for (int x = minX; x <= maxX; ++x) {
                const float fx = float(x) + 0.5f - x0, fy = float(y) + 0.5f - y0;
                float t = len2 > 1e-6f ? (fx*dx + fy*dy) / len2 : 0.0f;
                t = std::clamp(t, 0.0f, 1.0f);
                const float px_ = fx - t*dx, py_ = fy - t*dy;
                const float d = std::sqrt(px_*px_ + py_*py_);
                if (d > wide + 1.0f) continue;
                px(x, y, c, alpha * std::clamp(wide + 1.0f - d, 0.0f, 1.0f));
            }
    }
    void disc(float cx, float cy, float r, Rgb c, float alpha) {
        const int minX = std::max(0, int(cx - r - 1)), maxX = std::min(kW - 1, int(cx + r + 1));
        const int minY = std::max(0, int(cy - r - 1)), maxY = std::min(kH - 1, int(cy + r + 1));
        for (int y = minY; y <= maxY; ++y)
            for (int x = minX; x <= maxX; ++x) {
                const float dx = float(x) + 0.5f - cx, dy = float(y) + 0.5f - cy;
                const float d = std::sqrt(dx*dx + dy*dy);
                if (d > r + 1.0f) continue;
                px(x, y, c, alpha * std::clamp(r + 1.0f - d, 0.0f, 1.0f));
            }
    }

    void render() {
        std::fill(surf_.rgba.begin(), surf_.rgba.end(), std::uint8_t(0));
        for (int i = 0; i < kW * kH; ++i) {
            std::uint8_t* p = &surf_.rgba[std::size_t(i) * 4];
            p[0] = 10; p[1] = 13; p[2] = 17; p[3] = 255;
        }

        const int shown = std::clamp(int(knob("showcase") + 0.5f), 0, 3);
        net_.forward(X[std::size_t(shown)]);

        // Scale thickness against the LARGEST weight present, so the picture
        // stays readable whether the net has just started or has trained for an
        // hour. Absolute widths would either vanish early or saturate later.
        float wmax = 1e-4f;
        for (int l = 0; l + 1 < net_.layers(); ++l)
            for (int i = 0; i < net_.units(l); ++i)
                for (int j = 0; j < net_.units(l + 1); ++j)
                    wmax = std::max(wmax, std::fabs(net_.weight(l, i, j)));

        for (int l = 0; l + 1 < net_.layers(); ++l)
            for (int i = 0; i < net_.units(l); ++i)
                for (int j = 0; j < net_.units(l + 1); ++j) {
                    const float w = net_.weight(l, i, j);
                    const float m = std::fabs(w) / wmax;
                    const Node& A = nodes_[std::size_t(nodeIndex(l, i))];
                    const Node& B = nodes_[std::size_t(nodeIndex(l + 1, j))];
                    const Rgb c = (w >= 0.0f) ? Rgb{90, 209, 196} : Rgb{217, 83, 79};
                    line(A.x, A.y, B.x, B.y, 0.4f + m * 3.6f, c, 0.15f + 0.75f * m);
                }

        for (const Node& nd : nodes_) {
            const float a = net_.activation(nd.layer, nd.index);
            // Hidden units are tanh and can be negative, so -1..1 is remapped to
            // 0..1 for brightness — stated in the blurb rather than hidden.
            //
            // The remap applies to the hidden layer ONLY. The inputs are 0 or 1
            // and the output unit is sigmoid, both already in 0..1, and putting
            // them through it cost the output its dark end: after 400 steps of
            // the default run the answer to 0 XOR 0 is 0.0014 and was drawn at
            // t = 0.5007, mid grey, indistinguishable from a hidden unit sitting
            // at exactly zero. A confident "no" now reads as dark, which is what
            // the blurb has always said brightness means.
            const bool hidden = (nd.layer > 0 && nd.layer + 1 < net_.layers());
            const float t = std::clamp(hidden ? (a * 0.5f + 0.5f) : a, 0.0f, 1.0f);
            disc(nd.x, nd.y, kNodeR + 2.0f, Rgb{34, 44, 54}, 1.0f);
            disc(nd.x, nd.y, kNodeR,
                 Rgb{ std::uint8_t(20 + 200*t), std::uint8_t(24 + 220*t), std::uint8_t(30 + 200*t) },
                 1.0f);
        }
        publishWeights();
    }

    // The coarse Field is the weight matrix, so the workbench timeline records
    // the WEIGHTS as they train and scrubbing rewinds the model, not a picture.
    void publishWeights() {
        view_.fill(0);
        float wmax = 1e-4f;
        for (int l = 0; l + 1 < net_.layers(); ++l)
            for (int i = 0; i < net_.units(l); ++i)
                for (int j = 0; j < net_.units(l + 1); ++j)
                    wmax = std::max(wmax, std::fabs(net_.weight(l, i, j)));
        int col = 0;
        for (int l = 0; l + 1 < net_.layers(); ++l) {
            for (int i = 0; i < net_.units(l) && col < view_.w; ++i, ++col)
                for (int j = 0; j < net_.units(l + 1) && j < view_.h; ++j) {
                    const float w = net_.weight(l, i, j);
                    const int band = std::clamp(int(std::fabs(w) / wmax * float(kBands)), 0, kBands);
                    if (band == 0) continue;
                    view_.set(col, j, std::uint8_t(w >= 0 ? kBands + band : band));
                }
            ++col;
        }
    }

    Provenance about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    MLP    net_, init_;
    Field  view_;
    Surface surf_;
    std::vector<Node> nodes_;
    int    hidden_ = 4, epoch_ = 0, held_ = -1;
    std::uint64_t seed_ = 7;
    float  loss_ = 0.0f, drift_ = 0.0f;
    std::uint64_t gen_ = 0;

    inline static const std::vector<std::vector<float>> X{{0,0},{0,1},{1,0},{1,1}};
    inline static const std::vector<std::vector<float>> Y{{0},  {1},  {1},  {0}};
};

inline SimPtr make_netviz() { return std::make_unique<NetViz>(); }

} // namespace bench
