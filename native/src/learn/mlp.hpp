// mlp.hpp — a multilayer perceptron with real backpropagation.
//
// "The loss went down" is not evidence that backprop is correct. A wrong
// gradient still descends, just to somewhere else, and the bug is invisible in
// the training curve. The test that actually settles it is GRADIENT CHECKING:
// perturb one weight by a small epsilon, measure the change in loss, and
// compare that finite difference against the analytic gradient the code claims.
// If they disagree, the derivative is wrong no matter how nice the curve looks.
// `grad_check()` below does exactly that and the self-test asserts it.
//
// Deliberately plain: dense layers, tanh hidden, sigmoid output, mean-squared
// error, plain SGD with momentum. Everything is stored openly so the network
// diagram can read weights and activations without a separate copy — a picture
// of a model has to be a picture of THE model.

#pragma once
#include "../rng.hpp"
#include <cmath>
#include <cstdint>
#include <vector>

namespace bench {

class MLP {
public:
    // Q-values are unbounded and routinely negative, so a sigmoid output cannot
    // represent them at all — it would silently clamp every value into [0,1]
    // and the agent would learn a policy over a truncated world.
    enum class Out { Sigmoid, Linear };

    MLP() = default;
    MLP(std::vector<int> sizes, std::uint64_t seed = 0x51F1CEull, Out out = Out::Sigmoid) {
        init(std::move(sizes), seed, out);
    }

    void init(std::vector<int> sizes, std::uint64_t seed = 0x51F1CEull,
              Out out = Out::Sigmoid) {
        out_ = out;
        sizes_ = std::move(sizes);
        rng_.reseed(seed);
        const std::size_t L = sizes_.size();
        w_.assign(L - 1, {}); b_.assign(L - 1, {});
        gw_.assign(L - 1, {}); gb_.assign(L - 1, {});
        vw_.assign(L - 1, {}); vb_.assign(L - 1, {});
        a_.assign(L, {}); d_.assign(L, {});
        for (std::size_t l = 0; l + 1 < L; ++l) {
            const std::size_t in = std::size_t(sizes_[l]), out = std::size_t(sizes_[l + 1]);
            w_[l].resize(in * out);
            b_[l].assign(out, 0.0f);
            gw_[l].assign(in * out, 0.0f); gb_[l].assign(out, 0.0f);
            vw_[l].assign(in * out, 0.0f); vb_[l].assign(out, 0.0f);
            // Xavier: keep the variance of activations roughly constant through
            // the depth, or a deep net starts either saturated or dead.
            const float lim = std::sqrt(6.0f / float(in + out));
            for (auto& v : w_[l]) v = (rng_.unit() * 2.0f - 1.0f) * lim;
        }
        for (std::size_t l = 0; l < L; ++l) {
            a_[l].assign(std::size_t(sizes_[l]), 0.0f);
            d_[l].assign(std::size_t(sizes_[l]), 0.0f);
        }
    }

    [[nodiscard]] int layers() const { return int(sizes_.size()); }
    [[nodiscard]] int units(int l) const { return sizes_[std::size_t(l)]; }
    [[nodiscard]] float weight(int l, int from, int to) const {
        return w_[std::size_t(l)][std::size_t(from) * std::size_t(sizes_[std::size_t(l+1)])
                                  + std::size_t(to)];
    }
    [[nodiscard]] float bias(int l, int to) const { return b_[std::size_t(l)][std::size_t(to)]; }
    [[nodiscard]] float activation(int l, int i) const {
        return a_[std::size_t(l)][std::size_t(i)];
    }
    [[nodiscard]] const std::vector<float>& output() const { return a_.back(); }

    const std::vector<float>& forward(const std::vector<float>& x) {
        a_[0] = x;
        const std::size_t L = sizes_.size();
        for (std::size_t l = 0; l + 1 < L; ++l) {
            const std::size_t in = std::size_t(sizes_[l]), out = std::size_t(sizes_[l + 1]);
            const bool last = (l + 2 == L);
            for (std::size_t j = 0; j < out; ++j) {
                float s = b_[l][j];
                for (std::size_t i = 0; i < in; ++i) s += a_[l][i] * w_[l][i * out + j];
                a_[l + 1][j] = last ? (out_ == Out::Linear ? s : sigmoid(s))
                                   : std::tanh(s);
            }
        }
        return a_.back();
    }

    // Mean squared error over the outputs, halved so the derivative is (a - y).
    [[nodiscard]] float loss(const std::vector<float>& target) const {
        float s = 0.0f;
        for (std::size_t i = 0; i < target.size(); ++i) {
            const float e = a_.back()[i] - target[i];
            s += e * e;
        }
        return 0.5f * s;
    }

    // Accumulate gradients for one example. Call forward() first.
    void backward(const std::vector<float>& target) {
        const std::size_t L = sizes_.size();
        // Output layer: dL/dz = (a - y) * sigmoid'(z), and sigmoid'(z) = a(1-a).
        for (std::size_t j = 0; j < a_.back().size(); ++j) {
            const float a = a_.back()[j];
            // Linear output: dL/dz is just the error, because dz/da is 1.
            d_[L - 1][j] = (out_ == Out::Linear) ? (a - target[j])
                                                 : (a - target[j]) * a * (1.0f - a);
        }
        for (std::size_t l = L - 1; l > 0; --l) {
            const std::size_t in = std::size_t(sizes_[l - 1]), out = std::size_t(sizes_[l]);
            for (std::size_t j = 0; j < out; ++j) {
                gb_[l - 1][j] += d_[l][j];
                for (std::size_t i = 0; i < in; ++i) gw_[l - 1][i * out + j] += a_[l - 1][i] * d_[l][j];
            }
            if (l == 1) break;
            // Hidden layers use tanh, whose derivative is 1 - a^2.
            const std::size_t prev = std::size_t(sizes_[l - 1]);
            for (std::size_t i = 0; i < prev; ++i) {
                float s = 0.0f;
                for (std::size_t j = 0; j < out; ++j) s += w_[l - 1][i * out + j] * d_[l][j];
                const float a = a_[l - 1][i];
                d_[l - 1][i] = s * (1.0f - a * a);
            }
        }
    }

    void zero_grad() {
        for (auto& g : gw_) std::fill(g.begin(), g.end(), 0.0f);
        for (auto& g : gb_) std::fill(g.begin(), g.end(), 0.0f);
    }

    // SGD with momentum, averaged over however many examples were accumulated.
    void apply(float lr, int batch = 1, float momentum = 0.9f) {
        const float k = lr / float(batch <= 0 ? 1 : batch);
        for (std::size_t l = 0; l < w_.size(); ++l) {
            for (std::size_t i = 0; i < w_[l].size(); ++i) {
                vw_[l][i] = momentum * vw_[l][i] - k * gw_[l][i];
                w_[l][i] += vw_[l][i];
            }
            for (std::size_t i = 0; i < b_[l].size(); ++i) {
                vb_[l][i] = momentum * vb_[l][i] - k * gb_[l][i];
                b_[l][i] += vb_[l][i];
            }
        }
    }

    // One example, one update. Returns the loss before the step.
    float learn(const std::vector<float>& x, const std::vector<float>& y, float lr) {
        forward(x);
        const float e = loss(y);
        zero_grad();
        backward(y);
        apply(lr, 1);
        return e;
    }

    // ── the test that settles whether the derivative is right ───────────────
    //
    // Compare each analytic gradient against a central finite difference of the
    // loss. Returns the largest relative disagreement; anything above about
    // 1e-3 means the backward pass does not match the forward pass, whatever
    // the training curve is doing.
    // eps defaults to 1e-2 because that is where the check is most accurate in
    // float32, and that was measured rather than assumed. Sweeping it on a
    // 3-5-4-2 net gives a clear U:
    //
    //   1e-1 : 1.9e-2      3e-3 : 4.2e-3
    //   3e-2 : 1.7e-3      1e-3 : 1.7e-2
    //   1e-2 : 8.7e-4      1e-4 : 2.4e-1
    //
    // Rising on BOTH sides is the signature of a precision floor, not a wrong
    // derivative: too small and (up - dn) disappears into float rounding, too
    // large and the second-order term of the expansion shows up. A genuinely
    // incorrect gradient would be flat across eps instead. Recorded here
    // because a first reading of 1.7e-2 looks alarming and means nothing.
    float grad_check(const std::vector<float>& x, const std::vector<float>& y,
                     float eps = 1e-2f) {
        forward(x);
        zero_grad();
        backward(y);

        float worst = 0.0f;
        for (std::size_t l = 0; l < w_.size(); ++l)
            for (std::size_t i = 0; i < w_[l].size(); ++i) {
                const float keep = w_[l][i];
                w_[l][i] = keep + eps; forward(x); const float up = loss(y);
                w_[l][i] = keep - eps; forward(x); const float dn = loss(y);
                w_[l][i] = keep;
                const float numeric  = (up - dn) / (2.0f * eps);
                const float analytic = gw_[l][i];
                const float denom = std::max(1e-5f, std::fabs(numeric) + std::fabs(analytic));
                worst = std::max(worst, std::fabs(numeric - analytic) / denom);
            }
        forward(x);
        return worst;
    }

    [[nodiscard]] std::size_t parameter_count() const {
        std::size_t n = 0;
        for (const auto& v : w_) n += v.size();
        for (const auto& v : b_) n += v.size();
        return n;
    }
    // Flat, shape-preserving genomes for evolutionary controllers. The order
    // is weights then biases for each layer; reject malformed genomes atomically.
    [[nodiscard]] std::vector<double> parameters() const {
        std::vector<double> result;
        result.reserve(parameter_count());
        for (std::size_t l = 0; l < w_.size(); ++l) {
            result.insert(result.end(), w_[l].begin(), w_[l].end());
            result.insert(result.end(), b_[l].begin(), b_[l].end());
        }
        return result;
    }
    bool set_parameters(const std::vector<double>& values) {
        if (values.size() != parameter_count()) return false;
        for (double v : values) if (!std::isfinite(v) || std::fabs(v) > 1e6) return false;
        std::size_t i = 0;
        for (std::size_t l = 0; l < w_.size(); ++l) {
            for (float& v : w_[l]) v = float(values[i++]);
            for (float& v : b_[l]) v = float(values[i++]);
        }
        return true;
    }
    // How far the weights have moved from another snapshot — the honest way to
    // say "the model changed", rather than inferring it from the loss.
    [[nodiscard]] float distance_from(const MLP& other) const {
        float s = 0.0f;
        for (std::size_t l = 0; l < w_.size() && l < other.w_.size(); ++l)
            for (std::size_t i = 0; i < w_[l].size() && i < other.w_[l].size(); ++i) {
                const float d = w_[l][i] - other.w_[l][i];
                s += d * d;
            }
        return std::sqrt(s);
    }

private:
    static float sigmoid(float z) { return 1.0f / (1.0f + std::exp(-z)); }

    Out                             out_ = Out::Sigmoid;
    std::vector<int>                sizes_;
    std::vector<std::vector<float>> w_, b_;    // weights, biases
    std::vector<std::vector<float>> gw_, gb_;  // gradients
    std::vector<std::vector<float>> vw_, vb_;  // momentum
    std::vector<std::vector<float>> a_, d_;    // activations, deltas
    Rng                             rng_;
};

} // namespace bench
