// toolkit.hpp — the parts that never change.
//
// A new simulation should be the RULE and nothing else. Everything here is the
// scaffolding around a rule that is identical in every lattice or agent sim
// ever written: double buffering, neighbour counting, toroidal wrapping,
// spatial binning, rasterising agents to pixels.
//
// Nothing in this file is mandatory. Every sim on the bench is free to ignore
// all of it and implement `Sim` directly — VonNeumann does exactly that,
// because a 29-state rule with directional inputs does not fit a neighbour
// count. Use the scaffolding when it fits; drop through it when it does not.
//
//   GridSim   — you write `rule(x, y)`. Buffers, swapping and wrapping are done.
//   AgentSim  — you write `interact()` and `integrate()`. Wrapping, binning and
//               rasterising are done.
//   Bins      — a uniform spatial hash. Turns an O(n^2) particle sim into O(n).

#pragma once
#include "sim.hpp"
#include "parallel.hpp"
#include "rng.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace bench {

// ── neighbourhoods ──────────────────────────────────────────────────────────

// The eight surrounding cells. Toroidal.
inline int moore8(const Field& f, int x, int y, std::uint8_t of) {
    int n = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue;
            if (f.wrap(x + dx, y + dy) == of) ++n;
        }
    return n;
}

// The four orthogonal cells. Toroidal. This is von Neumann's neighbourhood, and
// the reason his automaton needed directional transmission states at all.
inline int neumann4(const Field& f, int x, int y, std::uint8_t of) {
    return (f.wrap(x + 1, y) == of) + (f.wrap(x - 1, y) == of)
         + (f.wrap(x, y + 1) == of) + (f.wrap(x, y - 1) == of);
}

// ── GridSim ─────────────────────────────────────────────────────────────────
// Subclass, implement `rule`, and you have a working cellular automaton with
// correct double buffering. The classic bug this removes is reading from the
// buffer you are writing to, which silently turns a synchronous rule into an
// asynchronous one and changes the physics.
class GridSim : public Sim {
public:
    GridSim(Provenance about, std::vector<Swatch> pal, int w, int h)
        : about_(std::move(about)), pal_(std::move(pal)), cur_(w, h), nxt_(w, h) {}

    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_;   }
    const Field&               field()   const override { return cur_;   }
    std::uint64_t              generation() const override { return gen_; }
    std::vector<Knob>&         knobs() override { return knobs_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
    }

    // Below this many cells a sweep is not worth threading — see step().
    static constexpr std::size_t kParallelCells = 250000;

    // The one thing you have to write.
    //
    // It must be a pure function of the CURRENT field: it may read cur_ freely
    // but must not touch shared mutable state, because sweeps are threaded
    // above kParallelCells. Drawing from rng() here would make the result
    // depend on thread interleaving.
    virtual std::uint8_t rule(int x, int y) = 0;

    // Called before each sweep, if the rule needs precomputed state.
    virtual void pre_sweep() {}

    void step() override {
        pre_sweep();
        // Row-parallel above a size threshold.
        //
        // Double buffering makes this free of any ordering question: rule()
        // only ever reads cur_, and each row writes its own disjoint slice of
        // nxt_, so the result cannot depend on how the work was split. Verified
        // rather than assumed — no rule() in the roster draws from the shared
        // rng_, which is the one thing that would make a split observable, and
        // the self-test compares a threaded sweep with a serial one cell for
        // cell.
        //
        // The threshold is measured, not guessed. This project previously
        // recorded that stepping a lattice was NOT worth threading, which was
        // true of the sizes it ran at: cost is dead linear at about 85,000
        // cells per millisecond, so 256 squared is 0.7 ms and threading it is
        // all overhead. At 1024 squared it is 12 ms and at 2048 squared 49 ms,
        // which is a fifth of a second per frame at ten steps. The decision
        // changed because the sizes did.
        const std::size_t cells = std::size_t(cur_.w) * std::size_t(cur_.h);
        const unsigned workers = (cells >= kParallelCells) ? 0u : 1u;
        parallel_for(std::size_t(cur_.h), [&](std::size_t row) {
            const int y = int(row);
            for (int x = 0; x < cur_.w; ++x) nxt_.set(x, y, rule(x, y));
        }, workers);
        cur_.cells.swap(nxt_.cells);
        ++gen_;
    }

    // Handy defaults; override freely.
    void reset() override { cur_.fill(0); gen_ = 0; }
    Field* editable() override { return &cur_; }
    bool poke(float nx, float ny) override {
        const int cx = int(nx * cur_.w), cy = int(ny * cur_.h);
        for (int dy = -3; dy <= 3; ++dy)
            for (int dx = -3; dx <= 3; ++dx)
                if (rng_.unit() < 0.6f)
                    cur_.set((cx + dx + cur_.w) % cur_.w, (cy + dy + cur_.h) % cur_.h, 1);
        return true;
    }

protected:
    [[nodiscard]] const Field& read() const { return cur_; }   // read the current state
    Field&                     write()      { return nxt_; }   // write the next state
    Rng&                       rng()        { return rng_;  }
    void                       add_knob(Knob k) { knobs_.push_back(std::move(k)); }
    [[nodiscard]] float        knob(const char* key) const {
        for (auto& kn : knobs_) if (kn.key == key) return kn.value;
        return 0.f;
    }
    void                       bump() { ++gen_; }
protected:
    Provenance          about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    Field               cur_, nxt_;
    Rng                 rng_{0x5EEDu};
    std::uint64_t       gen_ = 0;
};

// ── Bins ────────────────────────────────────────────────────────────────────
// Uniform spatial hash over a torus. Build it once per step, then ask for the
// candidates near a point instead of testing all n. This is the difference
// between 500 particles and 50,000.
class Bins {
public:
    void build(const std::vector<float>& px, const std::vector<float>& py,
               float w, float h, float cell) {
        w_ = w; h_ = h; cell_ = std::max(cell, 1e-3f);
        nx_ = std::max(1, int(w_ / cell_));
        ny_ = std::max(1, int(h_ / cell_));
        heads_.assign(std::size_t(nx_) * ny_, -1);
        next_.assign(px.size(), -1);
        for (std::size_t i = 0; i < px.size(); ++i) {
            const int c = cell_of(px[i], py[i]);
            next_[i] = heads_[c];
            heads_[c] = int(i);
        }
    }

    // Visit every particle in the 3x3 block of cells around (x, y), wrapping.
    // NB: not called `near` — <windows.h> defines that as a macro.
    template <class F>
    void each_near(float x, float y, F&& fn) const {
        const int cx = clampi(int(x / cell_), nx_), cy = clampi(int(y / cell_), ny_);
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int gx = (cx + dx + nx_) % nx_, gy = (cy + dy + ny_) % ny_;
                for (int i = heads_[std::size_t(gy) * nx_ + gx]; i >= 0; i = next_[std::size_t(i)])
                    fn(i);
            }
    }
    [[nodiscard]] float cell_size() const { return cell_; }

private:
    static int clampi(int v, int n) { return v < 0 ? 0 : (v >= n ? n - 1 : v); }
    [[nodiscard]] int cell_of(float x, float y) const {
        return clampi(int(y / cell_), ny_) * nx_ + clampi(int(x / cell_), nx_);
    }
    std::vector<int> heads_, next_;
    float w_ = 1, h_ = 1, cell_ = 1;
    int   nx_ = 1, ny_ = 1;
};

// ── AgentSim ────────────────────────────────────────────────────────────────
// Positions, wrapping, binning and rasterising, done. You write the forces.
class AgentSim : public Sim {
public:
    AgentSim(Provenance about, std::vector<Swatch> pal, int n, int w, int h)
        : about_(std::move(about)), pal_(std::move(pal)), view_(w, h),
          n_(n), w_(float(w)), h_(float(h)) {
        px_.resize(n_); py_.resize(n_); vx_.assign(n_, 0.f); vy_.assign(n_, 0.f);
        kind_.assign(n_, 0);
    }

    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_;   }
    const Field&               field()   const override { return view_;  }
    std::uint64_t              generation() const override { return gen_; }
    std::vector<Knob>&         knobs() override { return knobs_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
    }

    // The two things you write.
    virtual void interact() = 0;                 // accumulate forces into vx_/vy_
    virtual void integrate() { advance(); }      // default: move and wrap

    void step() override { interact(); integrate(); publish(); ++gen_; }

    void reset() override {
        gen_ = 0;
        for (int i = 0; i < n_; ++i) {
            px_[i] = rng_.unit() * w_; py_[i] = rng_.unit() * h_;
            vx_[i] = vy_[i] = 0.f;
        }
        publish();
    }

protected:
    void advance() {
        for (int i = 0; i < n_; ++i) {
            px_[i] = wrap_pos(px_[i] + vx_[i], w_);
            py_[i] = wrap_pos(py_[i] + vy_[i], h_);
        }
    }
    void publish() {
        view_.fill(0);
        for (int i = 0; i < n_; ++i) {
            const int ix = int(px_[i]), iy = int(py_[i]);
            if (ix >= 0 && iy >= 0 && ix < view_.w && iy < view_.h)
                view_.set(ix, iy, std::uint8_t(1 + kind_[i]));
        }
    }
    void rebin(float radius) { bins_.build(px_, py_, w_, h_, radius); }

    void  add_knob(Knob k) { knobs_.push_back(std::move(k)); }
    [[nodiscard]] float knob(const char* key) const {
        for (auto& kn : knobs_) if (kn.key == key) return kn.value;
        return 0.f;
    }

    Provenance          about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    Field               view_;
    Bins                bins_;
    int                 n_;
    float               w_, h_;
    std::vector<float>  px_, py_, vx_, vy_;
    std::vector<std::uint8_t> kind_;
    Rng                 rng_{0xA9E27u};
    std::uint64_t       gen_ = 0;
};

} // namespace bench
