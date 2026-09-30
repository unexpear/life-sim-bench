// machines.hpp — live machines that grind blocks into sand.
//
// The pictures people know from Lafikobra 3D are baked 3D particle films.
// These three are the same kind of machine, stepped here in 2D: a toothed
// shredder, a press, and a pair of rollers. Blocks go in. Sand comes out.

#pragma once
#include "../sim.hpp"
#include "../render/voxel.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace bench {

class MachineSim final : public Sim {
public:
    enum class Scene { Shredder, Press, Rollers };

    static constexpr int kW = 640;
    static constexpr int kH = 360;
    static constexpr int kCols = 180;
    static constexpr int kRows = 108;

    explicit MachineSim(Scene scene) : scene_(scene) {
        about_ = provenance(scene);
        pal_ = {{{12, 12, 16}, "background"}, {{220, 200, 140}, "sand"}};
        view_ = Field(64, 36);
        surf_.resize(kW, kH);
        grid_.assign(std::size_t(kCols) * kRows, 0);
        knobs_ = knobsFor(scene);
        reset();
    }

    const Provenance& about() const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field& field() const override { return view_; }
    const Surface* surface() const override { return &surf_; }
    std::uint64_t generation() const override { return gen_; }
    std::vector<Knob>& knobs() override { return knobs_; }
    std::string subtitle() const override { return subtitle_; }

    void on_knob(const std::string& key, float v) override {
        bool again = false;
        for (auto& k : knobs_) if (k.key == key) {
            k.value = k.quantised(v);
            again = k.on_reset;
        }
        if (again) reset();
    }

    void reset() override {
        gen_ = 0;
        sand_ = 0;
        fed_ = 0;
        phase_ = 0.f;
        plateY_ = 28;
        wait_ = 0;
        std::fill(grid_.begin(), grid_.end(), 0);
        piece_.clear();
        teeth_.clear();
        buildHousing();
        spawnPiece();
        publish();
    }

    void step() override {
        const int passes = std::max(1, int(knob("speed", 1.f) + 0.5f));
        for (int i = 0; i < passes; ++i) machineStep();
        ++gen_;
        publish();
    }

    bool poke(float nx, float ny) override {
        const int x = std::clamp(int(nx * kCols), 2, kCols - 3);
        const int y = std::clamp(int(ny * kRows), 2, kRows - 3);
        bool wrote = false;
        for (int dy = 0; dy <= 1 && !wrote; ++dy)
            for (int dx = 0; dx <= 1; ++dx) {
                auto& cell = at(x + dx, y + dy);
                if (cell == kEmpty || isPiece(cell)) {
                    cell = sandOf(color_);
                    wrote = true;
                }
            }
        if (!wrote) at(x, y) = sandOf(color_);
        piece_.clear();
        for (int yy = 0; yy < kRows; ++yy)
            for (int xx = 0; xx < kCols; ++xx)
                if (isPiece(at(xx, yy))) piece_.push_back(Cell{xx, yy});
        publish();
        return true;
    }

    std::vector<Metric> metrics() const override {
        return {
            Metric{"sand", double(sand_), 0.0, Metric::Neither},
            Metric{"blocks fed", double(fed_), 0.0, Metric::Neither},
        };
    }

    [[nodiscard]] int sandCount() const { return sand_; }
    [[nodiscard]] int fed() const { return fed_; }

private:
    static constexpr std::uint8_t kEmpty = 0;
    static constexpr std::uint8_t kWall = 1;
    static constexpr std::uint8_t kTooth = 32;

    struct Cell { int x, y; };

    Scene scene_;
    Provenance about_;
    std::vector<Swatch> pal_;
    std::vector<Knob> knobs_;
    Field view_;
    Surface surf_;
    std::string subtitle_;
    std::uint64_t gen_ = 0;
    int sand_ = 0, fed_ = 0, plateY_ = 28, wait_ = 0, color_ = 0;
    float phase_ = 0;
    std::vector<std::uint8_t> grid_;
    std::vector<Cell> piece_;
    std::vector<Cell> teeth_;

    [[nodiscard]] float knob(const std::string& key, float fallback) const {
        for (const auto& k : knobs_) if (k.key == key) return k.value;
        return fallback;
    }
    std::uint8_t& at(int x, int y) { return grid_[std::size_t(y) * kCols + x]; }
    [[nodiscard]] std::uint8_t at(int x, int y) const { return grid_[std::size_t(y) * kCols + x]; }
    [[nodiscard]] bool inside(int x, int y) const {
        return x >= 0 && y >= 0 && x < kCols && y < kRows;
    }
    [[nodiscard]] static bool isSand(std::uint8_t c) { return c >= 2 && c <= 8; }
    [[nodiscard]] static bool isPiece(std::uint8_t c) { return c >= 20 && c <= 26; }
    [[nodiscard]] static std::uint8_t sandOf(int color) { return std::uint8_t(2 + (color % 7)); }
    [[nodiscard]] static std::uint8_t pieceOf(int color) { return std::uint8_t(20 + (color % 7)); }

    static Provenance provenance(Scene scene) {
        const char* title = "Block shredder";
        const char* blurb =
            "Colored blocks fall into a row of moving teeth. Each cell the teeth "
            "touch becomes sand of that color and piles underneath.";
        if (scene == Scene::Press) {
            title = "Block press";
            blurb = "A plate comes down on a block. The crushed layer sprays out "
                    "the sides as sand, then the plate lifts for the next block.";
        } else if (scene == Scene::Rollers) {
            title = "Roller mill";
            blurb = "Blocks drop between two rollers. The nip grinds them, and "
                    "the sand falls out below.";
        }
        return Provenance{
            title, "2026", "Life-sim Workbench",
            "Original machines, 2026. The kind of block-into-powder machine shown "
            "in Lafikobra 3D's films, built here as a live 2D grid rather than a "
            "baked 3D particle shot.",
            Replication::No,
            "No. Blocks are ground into sand. Nothing in the machine copies itself.",
            blurb};
    }

    static std::vector<Knob> knobsFor(Scene scene) {
        const Knob speed{"speed", "machine speed", 1.f, 3.f, 1.f, 1.f, {}, false,
                         "How many machine steps happen before the picture is drawn."};
        if (scene == Scene::Shredder)
            return {
                speed,
                {"teeth", "tooth rate", 0.15f, 1.2f, 0.45f, 0.05f, {}, false,
                 "How fast the teeth walk across the throat."},
            };
        if (scene == Scene::Press)
            return {
                speed,
                {"bite", "plate step", 1.f, 3.f, 1.f, 1.f, {}, false,
                 "How many cells the plate moves each machine step."},
            };
        return {
            speed,
            {"gap", "roller gap", 4.f, 12.f, 6.f, 1.f, {}, true,
             "How wide the opening between the rollers is. Rebuilt when you apply it."},
        };
    }

    void wall(int x, int y) { if (inside(x, y)) at(x, y) = kWall; }

    void buildHousing() {
        const int L = 48, R = 132, floor = 100;
        for (int x = L; x <= R; ++x) { wall(x, floor); wall(x, 4); }
        for (int y = 4; y <= floor; ++y) { wall(L, y); wall(R, y); }
        if (scene_ == Scene::Shredder) {
            for (int y = 16; y <= 46; ++y) {
                const int inset = (46 - y) / 3;
                wall(L + 8 + inset, y);
                wall(R - 8 - inset, y);
            }
            for (int y = 70; y <= floor; ++y) {
                wall(L + 6, y);
                wall(R - 6, y);
            }
        } else if (scene_ == Scene::Press) {
            for (int y = 20; y <= 78; ++y) { wall(70, y); wall(110, y); }
            for (int x = 70; x <= 110; ++x) wall(x, 78);
            for (int y = 78; y <= floor; ++y) { wall(58, y); wall(122, y); }
        } else {
            const int gap = std::max(4, int(knob("gap", 6.f) + 0.5f));
            const int mid = (L + R) / 2;
            for (int y = 36; y <= 70; ++y) {
                wall(mid - gap / 2 - 8, y);
                wall(mid + gap / 2 + 8, y);
            }
            for (int y = 72; y <= floor; ++y) { wall(L + 10, y); wall(R - 10, y); }
        }
    }

    void stampPiece(std::uint8_t v) {
        for (const auto& c : piece_) if (inside(c.x, c.y) && !isSand(at(c.x, c.y)) && at(c.x, c.y) != kWall)
            at(c.x, c.y) = v;
    }

    void spawnAt(int ox, int oy, int shape) {
        static const int shapes[][4][2] = {
            {{0, 0}, {1, 0}, {2, 0}, {3, 0}},
            {{0, 0}, {1, 0}, {0, 1}, {1, 1}},
            {{0, 0}, {1, 0}, {2, 0}, {1, 1}},
            {{0, 0}, {0, 1}, {0, 2}, {1, 2}},
            {{1, 0}, {2, 0}, {0, 1}, {1, 1}},
        };
        const int n = 5;
        const int s = shape % n;
        color_ = (color_ + 1) % 7;
        const std::uint8_t v = pieceOf(color_);
        piece_.clear();
        const int scale = 3;
        for (int i = 0; i < 4; ++i)
            for (int dy = 0; dy < scale; ++dy)
                for (int dx = 0; dx < scale; ++dx) {
                    const int x = ox + shapes[s][i][0] * scale + dx;
                    const int y = oy + shapes[s][i][1] * scale + dy;
                    if (!inside(x, y) || at(x, y) == kWall) continue;
                    piece_.push_back(Cell{x, y});
                    at(x, y) = v;
                }
        if (!piece_.empty()) ++fed_;
    }

    void spawnPiece() {
        if (scene_ == Scene::Press) {
            piece_.clear();
            color_ = (color_ + 1) % 7;
            const std::uint8_t v = pieceOf(color_);
            for (int y = 58; y <= 74; ++y)
                for (int x = 78; x <= 102; ++x) {
                    piece_.push_back(Cell{x, y});
                    at(x, y) = v;
                }
            plateY_ = 26;
            ++fed_;
            return;
        }
        const int shape = int(gen_ / 3);
        spawnAt(scene_ == Scene::Rollers ? 84 : 76, 6, shape);
    }

    bool blocked(int x, int y) const {
        if (!inside(x, y)) return true;
        const std::uint8_t c = at(x, y);
        if (c == kWall || c == kTooth || isSand(c)) return true;
        return false;
    }

    bool canDrop() const {
        for (const auto& c : piece_) {
            const int nx = c.x, ny = c.y + 1;
            if (!blocked(nx, ny)) continue;
            if (!inside(nx, ny) || !isPiece(at(nx, ny))) return false;
        }
        return true;
    }

    void dropPiece() {
        stampPiece(kEmpty);
        for (auto& c : piece_) ++c.y;
        stampPiece(pieceOf(color_));
    }

    void clearTeeth() {
        for (const auto& c : teeth_)
            if (inside(c.x, c.y) && at(c.x, c.y) == kTooth) at(c.x, c.y) = kEmpty;
        teeth_.clear();
    }

    void addTooth(int x, int y) {
        if (!inside(x, y) || at(x, y) != kEmpty) return;
        at(x, y) = kTooth;
        teeth_.push_back(Cell{x, y});
    }

    void placeTeeth() {
        clearTeeth();
        phase_ += knob("teeth", 0.45f);
        if (scene_ == Scene::Shredder) {
            for (int x = 62; x <= 118; ++x) {
                const int n = 2 + int(2.2f + 1.8f * std::sin(phase_ + float(x) * 0.55f));
                for (int k = 0; k < n; ++k) addTooth(x, 50 + k);
            }
        } else if (scene_ == Scene::Rollers) {
            const int gap = std::max(4, int(knob("gap", 6.f) + 0.5f));
            const int mid = 90;
            const float spin = phase_ * 2.f;
            for (int i = 0; i < 10; ++i) {
                const float a = spin + float(i) * 0.628f;
                addTooth(mid - gap / 2 - 6 + int(std::cos(a) * 5.f), 52 + int(std::sin(a) * 6.f));
                addTooth(mid + gap / 2 + 6 + int(std::cos(a + 1.f) * 5.f), 52 + int(std::sin(a + 1.f) * 6.f));
            }
        }
    }

    void becomeSand(int x, int y, int color) {
        if (!inside(x, y) || at(x, y) == kWall) return;
        at(x, y) = sandOf(color);
    }

    void shredTouching() {
        std::vector<Cell> keep;
        keep.reserve(piece_.size());
        for (const auto& c : piece_) {
            bool hit = false;
            for (int dy = -1; dy <= 1 && !hit; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (inside(c.x + dx, c.y + dy) && at(c.x + dx, c.y + dy) == kTooth) hit = true;
            if (scene_ == Scene::Rollers && c.y >= 50 && c.y <= 64) hit = true;
            if (!hit) { keep.push_back(c); continue; }
            at(c.x, c.y) = kEmpty;
            for (int k = -1; k <= 1; ++k) {
                const int sx = std::clamp(c.x + k, 1, kCols - 2);
                int sy = std::min(kRows - 2, std::max(c.y + 6, 70));
                while (sy > 60 && at(sx, sy) != kEmpty) --sy;
                if (at(sx, sy) == kEmpty) becomeSand(sx, sy, color_);
            }
        }
        piece_.swap(keep);
    }

    void crush() {
        const int bite = std::max(1, int(knob("bite", 1.f) + 0.5f));
        plateY_ = std::min(76, plateY_ + bite);
        std::vector<Cell> keep;
        for (const auto& c : piece_) {
            if (c.y > plateY_) { keep.push_back(c); continue; }
            at(c.x, c.y) = kEmpty;
            const int lane = (c.x < 90) ? 61 + (c.x % 7) : 113 + (c.x % 7);
            int y = 80;
            while (y < kRows - 2 && at(lane, y) != kEmpty) ++y;
            if (inside(lane, y) && at(lane, y) == kEmpty) becomeSand(lane, y, color_);
        }
        piece_.swap(keep);
        if (piece_.empty()) plateY_ = 26;
    }

    void fallSand() {
        const int dir = (gen_ & 1u) ? 1 : -1;
        for (int y = kRows - 2; y >= 0; --y) {
            const int x0 = dir > 0 ? 1 : kCols - 2;
            const int x1 = dir > 0 ? kCols - 1 : 0;
            for (int x = x0; x != x1; x += dir) {
                const std::uint8_t g = at(x, y);
                if (!isSand(g)) continue;
                if (at(x, y + 1) == kEmpty) {
                    at(x, y + 1) = g;
                    at(x, y) = kEmpty;
                    continue;
                }
                const int s = ((x + y + int(gen_)) & 1) ? dir : -dir;
                if (x + s > 0 && x + s < kCols - 1 && at(x + s, y) == kEmpty && at(x + s, y + 1) == kEmpty) {
                    at(x + s, y + 1) = g;
                    at(x, y) = kEmpty;
                }
            }
        }
    }

    void machineStep() {
        if (scene_ != Scene::Press) placeTeeth();
        if (scene_ == Scene::Press) crush();
        else shredTouching();
        for (int n = 0; n < 2 && !piece_.empty() && canDrop(); ++n) dropPiece();
        if (scene_ != Scene::Press) shredTouching();
        fallSand();
        if (piece_.empty()) {
            ++wait_;
            if (wait_ >= 8) { wait_ = 0; spawnPiece(); }
        }
    }

    static void colorOf(std::uint8_t c, int& r, int& g, int& b) {
        static const int cols[7][3] = {
            {0, 220, 230}, {240, 210, 40}, {170, 70, 230}, {240, 150, 30},
            {40, 210, 70}, {230, 50, 60}, {50, 90, 230},
        };
        int i = 0;
        if (isSand(c)) i = c - 2;
        else if (isPiece(c)) i = c - 20;
        i %= 7;
        r = cols[i][0]; g = cols[i][1]; b = cols[i][2];
    }

    void plot(int x, int y, int r, int g, int b) {
        if (x < 0 || y < 0 || x >= kW || y >= kH) return;
        auto* p = &surf_.rgba[(std::size_t(y) * kW + std::size_t(x)) * 4];
        p[0] = std::uint8_t(r); p[1] = std::uint8_t(g); p[2] = std::uint8_t(b); p[3] = 255;
    }

    void publish() {
        sand_ = 0;
        auto* p = surf_.rgba.data();
        for (std::size_t i = 0; i < std::size_t(kW) * kH; ++i) {
            p[0] = 12; p[1] = 12; p[2] = 16; p[3] = 255;
            p += 4;
        }
        const int ox = (kW - kCols * 3) / 2;
        const int oy = (kH - kRows * 3) / 2;
        for (int y = 0; y < kRows; ++y)
            for (int x = 0; x < kCols; ++x) {
                const std::uint8_t c = at(x, y);
                if (c == kEmpty) continue;
                int r, g, b;
                if (c == kWall) { r = 36; g = 40; b = 48; }
                else if (c == kTooth) { r = 196; g = 202; b = 210; }
                else { colorOf(c, r, g, b); if (isSand(c)) ++sand_; }
                const int px = ox + x * 3, py = oy + y * 3;
                for (int dy = 0; dy < 3; ++dy)
                    for (int dx = 0; dx < 3; ++dx) plot(px + dx, py + dy, r, g, b);
            }
        if (scene_ == Scene::Press) {
            const int py = oy + plateY_ * 3;
            for (int x = ox + 72 * 3; x <= ox + 108 * 3; ++x)
                for (int t = 0; t < 6; ++t) plot(x, py + t, 210, 214, 220);
        } else if (scene_ == Scene::Rollers) {
            const int gap = std::max(4, int(knob("gap", 6.f) + 0.5f));
            const int mid = 90;
            auto roller = [&](int gx, int gy) {
                const int cx = ox + gx * 3 + 1, cy = oy + gy * 3 + 1;
                for (int dy = -16; dy <= 16; ++dy)
                    for (int dx = -16; dx <= 16; ++dx)
                        if (dx * dx + dy * dy <= 16 * 16) plot(cx + dx, cy + dy, 170, 176, 184);
            };
            roller(mid - gap / 2 - 6, 55);
            roller(mid + gap / 2 + 6, 55);
        }
        char buf[120];
        std::snprintf(buf, sizeof buf, "blocks %d  ·  sand %d", fed_, sand_);
        subtitle_ = buf;
        std::fill(view_.cells.begin(), view_.cells.end(), 0);
        const int n = std::min(view_.w, sand_ / 8);
        for (int i = 0; i < n; ++i) view_.set(i, 0, 1);
    }
};

inline SimPtr make_machine_shredder() { return std::make_unique<MachineSim>(MachineSim::Scene::Shredder); }
inline SimPtr make_machine_press() { return std::make_unique<MachineSim>(MachineSim::Scene::Press); }
inline SimPtr make_machine_rollers() { return std::make_unique<MachineSim>(MachineSim::Scene::Rollers); }

} // namespace bench
