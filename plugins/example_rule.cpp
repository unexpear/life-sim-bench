// example_rule.cpp — a hot-reloadable sim. COPY THIS to start your own.
//
// Build it (from the repo root, with the workbench running or not):
//
//   g++ -std=c++20 -O2 -shared -Inative/src plugins/example_rule.cpp -o plugins/example_rule.dll
//   ...and always with -static -static-libgcc -static-libstdc++, so the plugin
//   cannot bind to a different libstdc++ than the workbench itself uses.
//
// Then press F5 in the workbench, or just leave watch on: it notices the file
// changed, reloads, and swaps the running sim underneath you. Edit `rule()`,
// rebuild, and the picture changes without restarting.
//
// ── the only requirement ────────────────────────────────────────────────────
// Export `bench_create_sim` returning a `new`-ed Sim. Everything else is yours.
// Build it with the same compiler as the workbench - it hands back a C++ object
// with virtual calls, so the ABI has to match.

#include "toolkit.hpp"

using namespace bench;

// ── Larger than Life: Bosco's Rule ──────────────────────────────────────────
// A neighbourhood of radius 5 instead of 1, which is why it grows those bubbly
// coherent blobs instead of Life's confetti. Evans, K. M., "Larger than Life:
// digital creatures in a family of two-dimensional cellular automata" (2001).
// Birth on 34..45 live neighbours in the radius-5 box, survival on 33..57.
class Bosco final : public GridSim {
public:
    explicit Bosco(int size = 256)
        : GridSim(
            Provenance{
                "Bosco's Rule (Larger than Life)", "2001",
                "Kellie Michele Evans",
                "Evans, K. M. \"Larger than Life: digital creatures in a family of "
                "two-dimensional cellular automata\", Discrete Math. Theor. Comput. Sci. (2001)",
                Replication::No,
                "No. It produces coherent moving 'bugs' that look far more alive than anything "
                "in Life, and none of them copy themselves. A good reminder that looking alive "
                "and being self-replicating are unrelated properties.",
                "Life's logic with a radius-5 neighbourhood: born with 34-45 live neighbours in "
                "the 11x11 box, survives with 33-57. The wide neighbourhood is what turns "
                "confetti into coherent blobs with membranes and directed motion."
            },
            std::vector<Swatch>{{{8,11,14},"dead"},{{122,216,138},"alive"}},
            size, size)
    {
        add_knob({"density", "starting density", 0.05f, 0.95f, 0.38f, 0.01f, {}, true,
                  "Fraction alive at the start. Apply setup to seed a new field."});
        reset();
    }

    // Summed-area table so a radius-5 count is O(1) per cell instead of O(121).
    void pre_sweep() override {
        const int w = read().w, h = read().h;
        if (int(sat_.size()) != (w + 1) * (h + 1)) sat_.assign(std::size_t(w + 1) * (h + 1), 0);
        const int W = w + 1;
        for (int y = 0; y < h; ++y) {
            int row = 0;
            for (int x = 0; x < w; ++x) {
                row += read().at(x, y) ? 1 : 0;
                sat_[std::size_t(y + 1) * W + (x + 1)] = sat_[std::size_t(y) * W + (x + 1)] + row;
            }
        }
    }

    std::uint8_t rule(int x, int y) override {
        const int n = box_count(x, y, kR) - (read().at(x, y) ? 1 : 0);
        const bool alive = read().at(x, y) != 0;
        if (alive) return std::uint8_t((n >= 33 && n <= 57) ? 1 : 0);
        return std::uint8_t((n >= 34 && n <= 45) ? 1 : 0);
    }

    void reset() override {
        gen_ = 0;
        cur_.fill(0);
        const float d = knob("density");
        for (auto& c : cur_.cells) c = (rng().unit() < d) ? 1 : 0;
    }

    std::vector<Metric> metrics() const override {
        double alive = 0;
        for (auto c : field().cells) alive += c ? 1 : 0;
        return { {"alive fraction", alive / double(field().cells.size()), 1.0} };
    }

private:
    static constexpr int kR = 5;
    // inclusive box sum, clamped at the edges (this rule is not toroidal here)
    int box_count(int cx, int cy, int r) const {
        const int w = read().w, h = read().h, W = w + 1;
        const int x0 = std::max(0, cx - r), y0 = std::max(0, cy - r);
        const int x1 = std::min(w - 1, cx + r), y1 = std::min(h - 1, cy + r);
        return sat_[std::size_t(y1 + 1) * W + (x1 + 1)]
             - sat_[std::size_t(y0)     * W + (x1 + 1)]
             - sat_[std::size_t(y1 + 1) * W + (x0)]
             + sat_[std::size_t(y0)     * W + (x0)];
    }
    std::vector<int> sat_;
};

// ── the export ──────────────────────────────────────────────────────────────
extern "C" __declspec(dllexport) bench::Sim* bench_create_sim() {
    return new Bosco(256);
}
