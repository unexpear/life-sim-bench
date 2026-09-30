// sand.hpp — three sand scenes, stepped live.
//
// Grains pour and pile, an hourglass drains, and a ball of sand is held by
// links to its neighbours until those links stretch and break. A few hundred
// grains is enough to watch, so nothing is precomputed.

#pragma once
#include "../sim.hpp"
#include "../render/voxel.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace bench {

class SandSim final : public Sim {
public:
    enum class Scene { Pour, Hourglass, Ball };

    static constexpr int kW = 640;
    static constexpr int kH = 360;
    static constexpr int kCols = 200;
    static constexpr int kRows = 110;

    explicit SandSim(Scene scene) : scene_(scene) {
        about_ = provenance(scene);
        pal_ = {{{8, 8, 12}, "background"}, {{244, 220, 160}, "sand"}};
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
        lower_ = 0;
        linksAlive_ = 0;
        std::fill(grid_.begin(), grid_.end(), 0);
        grains_.clear();
        links_.clear();
        if (scene_ == Scene::Hourglass) buildHourglass();
        if (scene_ == Scene::Ball) buildBall();
        publish();
    }

    void step() override {
        const int passes = std::max(1, int(knob("flow", 2.f) + 0.5f));
        for (int i = 0; i < passes; ++i) {
            if (scene_ == Scene::Pour) spawnPour();
            if (scene_ == Scene::Ball) stepBall();
            else stepGrid();
        }
        ++gen_;
        publish();
    }

    bool poke(float nx, float ny) override {
        if (scene_ == Scene::Ball) {
            const float px = nx * float(kW), py = ny * float(kH);
            for (auto& g : grains_) {
                if (std::hypot(g.x - px, g.y - py) < 28.f) {
                    g.vx += (px - g.x) * 0.05f;
                    g.vy += 2.f;
                }
            }
            for (auto& L : links_) {
                if (!L.alive) continue;
                const auto& a = grains_[L.a];
                if (std::hypot(a.x - px, a.y - py) < 36.f) L.alive = false;
            }
        } else {
            const int x = std::clamp(int(nx * kCols), 2, kCols - 3);
            const int y = std::clamp(int(ny * kRows), 2, kRows - 3);
            bool wrote = false;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    auto& cell = at(x + dx, y + dy);
                    if (cell == kEmpty) { cell = shade(x + dx, y + dy); wrote = true; }
                }
            if (!wrote) {
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) {
                        auto& cell = at(x + dx, y + dy);
                        if (cell > kWall) cell = kEmpty;
                    }
            }
        }
        publish();
        return true;
    }

    std::vector<Metric> metrics() const override {
        return {
            Metric{"progress", std::clamp(double(progress()), 0.0, 1.0), 1.0, Metric::Higher},
            Metric{"grains", double(sand_), 0.0, Metric::Neither},
            Metric{"links", double(linksAlive_), 0.0, Metric::Neither},
        };
    }

    [[nodiscard]] int sandCount() const { return sand_; }
    [[nodiscard]] int lowerSand() const { return lower_; }
    [[nodiscard]] int linksAlive() const { return linksAlive_; }
    [[nodiscard]] float meanY() const {
        if (grains_.empty()) return 0;
        double s = 0;
        for (const auto& g : grains_) s += g.y;
        return float(s / double(grains_.size()));
    }
    [[nodiscard]] float progress() const {
        if (scene_ == Scene::Ball)
            return links_.empty() ? 1.f : 1.f - float(linksAlive_) / float(links_.size());
        if (scene_ == Scene::Hourglass)
            return sand_ == 0 ? 0 : std::clamp(float(lower_) / float(sand_), 0.f, 1.f);
        return std::clamp(float(sand_) / 500.f, 0.f, 1.f);
    }

private:
    struct Grain { float x, y, vx, vy; int cr, cg, cb; };
    struct Link { int a, b; float rest; bool alive; };

    static constexpr int kEmpty = 0;
    static constexpr int kWall = 1;

    Scene scene_;
    Provenance about_;
    std::vector<Swatch> pal_;
    std::vector<Knob> knobs_;
    Field view_;
    Surface surf_;
    std::string subtitle_;
    std::uint64_t gen_ = 0;
    int sand_ = 0, lower_ = 0, linksAlive_ = 0;
    std::vector<std::uint8_t> grid_;
    std::vector<Grain> grains_;
    std::vector<Link> links_;

    [[nodiscard]] float knob(const std::string& key, float fallback) const {
        for (const auto& k : knobs_) if (k.key == key) return k.value;
        return fallback;
    }
    std::uint8_t& at(int x, int y) { return grid_[std::size_t(y) * kCols + x]; }
    std::uint8_t at(int x, int y) const { return grid_[std::size_t(y) * kCols + x]; }

    static std::uint8_t shade(int x, int y) {
        const int h = (x * 3 + y * 5) % 5;
        return std::uint8_t(2 + h);
    }

    static Provenance provenance(Scene scene) {
        const char* title = "Sand pour";
        const char* blurb =
            "Grains drop from a spout and pile with a slope. It runs while you watch.";
        if (scene == Scene::Hourglass) {
            title = "Sand hourglass";
            blurb = "Sand starts in the upper bulb and drains through the neck. "
                    "The neck width is the whole difference between a trickle and a pour.";
        } else if (scene == Scene::Ball) {
            title = "Linked sand ball";
            blurb = "Grains in a ball start linked to nearby grains. A link breaks when "
                    "it stretches past the break amount, and the ball slumps into a pile.";
        }
        return Provenance{
            title, "2026", "Life-sim Workbench",
            "Original scenes, 2026. Grains, a neck, and breakable neighbour links. "
            "Stepped live; a bake is not required at this size.",
            Replication::No,
            "No. Grains fall and links break. Nothing in the pile copies itself.",
            blurb};
    }

    static std::vector<Knob> knobsFor(Scene scene) {
        const Knob flow{"flow", "passes per step", 1.f, 4.f, 3.f, 1.f, {}, false,
                        "How many grain updates happen before the picture is drawn."};
        const Knob seed{"seed", "start seed", 1.f, 12.f, 1.f, 1.f, {}, true,
                        "Shifts the spout, the colours, or the ball."};
        if (scene == Scene::Pour)
            return {
                {"rate", "grains per pass", 1.f, 8.f, 3.f, 1.f, {}, false,
                 "How many new grains the spout releases each pass."},
                {"spread", "spout spread", 0.f, 18.f, 6.f, 1.f, {}, false,
                 "How wide the stream is, in cells."},
                flow,
            };
        if (scene == Scene::Hourglass)
            return {
                {"gap", "neck width", 1.f, 10.f, 3.f, 1.f, {}, true,
                 "Cells open at the neck. Rebuilt when you apply it."},
                flow,
                seed,
            };
        return {
            {"break", "break stretch", 0.08f, 0.5f, 0.22f, 0.01f, {}, false,
             "A link breaks after it has stretched by this fraction of its rest length."},
            {"range", "link range", 8.f, 18.f, 12.f, 1.f, {}, true,
             "How far a grain looks for a neighbour to link to, in pixels. Rebuilt on apply."},
            seed,
        };
    }

    void buildHourglass() {
        const int gap = std::max(1, int(knob("gap", 3.f) + 0.5f));
        const float midY = kRows * 0.5f;
        const float midX = kCols * 0.5f;
        for (int y = 0; y < kRows; ++y)
            for (int x = 0; x < kCols; ++x) {
                const float t = std::fabs(float(y) - midY) / midY;
                const float half = float(gap) * 0.5f + t * t * 78.f;
                const bool wall = x == 0 || y == 0 || x == kCols - 1 || y == kRows - 1
                    || std::fabs(float(x) - midX) > half;
                if (wall) at(x, y) = kWall;
                else if (y > 6 && y < int(midY) - 3)
                    at(x, y) = shade(x + int(knob("seed", 1.f)), y);
            }
    }

    void buildBall() {
        const float range = knob("range", 12.f);
        const float seedA = (knob("seed", 1.f) - 1.f) * 0.4f;
        const float rad = 4.6f;
        const float ballR = 52.f;
        const float ox = 320.f, oy = 168.f;
        for (float y = oy - ballR; y <= oy + ballR; y += rad * 1.72f) {
            const int row = int((y - (oy - ballR)) / (rad * 1.72f));
            for (float x = ox - ballR; x <= ox + ballR; x += rad * 2.f) {
                const float xx = x + ((row & 1) ? rad : 0.f);
                if (std::hypot(xx - ox, y - oy) > ballR) continue;
                const float t = std::hypot(xx - ox, y - oy) / ballR;
                Grain g;
                g.x = xx; g.y = y;
                g.vx = g.vy = 0;
                g.cr = 255;
                g.cg = std::clamp(int(245 - t * 110 + seedA * 40.f), 30, 255);
                g.cb = std::clamp(int(190 - t * 150), 20, 255);
                grains_.push_back(g);
            }
        }
        const int n = int(grains_.size());
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                const float dx = grains_[j].x - grains_[i].x;
                const float dy = grains_[j].y - grains_[i].y;
                const float d = std::hypot(dx, dy);
                if (d > 1.f && d <= range) links_.push_back(Link{i, j, d, true});
            }
        linksAlive_ = int(links_.size());
    }

    void spawnPour() {
        const int rate = std::max(1, int(knob("rate", 3.f) + 0.5f));
        const float spread = knob("spread", 6.f);
        const float seedA = (knob("seed", 1.f) - 1.f);
        const int spout = kCols / 2;
        for (int i = 0; i < rate; ++i) {
            const int x = std::clamp(spout + int(std::sin(float(gen_) * 1.7f + seedA + float(i)) * spread), 1, kCols - 2);
            if (at(x, 1) == kEmpty) at(x, 1) = shade(x, int(gen_) + i);
            if (at(x, 2) == kEmpty) at(x, 2) = shade(x + 1, int(gen_) + i);
        }
    }

    void stepGrid() {
        const int dir = (gen_ & 1u) ? 1 : -1;
        for (int y = kRows - 2; y >= 0; --y) {
            const int x0 = dir > 0 ? 1 : kCols - 2;
            const int x1 = dir > 0 ? kCols - 1 : 0;
            for (int x = x0; x != x1; x += dir) {
                const std::uint8_t g = at(x, y);
                if (g <= kWall) continue;
                if (at(x, y + 1) == kEmpty) {
                    at(x, y + 1) = g;
                    at(x, y) = kEmpty;
                    continue;
                }
                const int s = ((x + y + int(gen_)) & 1) ? dir : -dir;
                if (x + s > 0 && x + s < kCols - 1 && at(x + s, y) == kEmpty
                    && at(x + s, y + 1) == kEmpty) {
                    at(x + s, y + 1) = g;
                    at(x, y) = kEmpty;
                }
            }
        }
    }

    void stepBall() {
        const float brk = knob("break", 0.08f);
        for (int sub = 0; sub < 4; ++sub) {
            for (auto& g : grains_) g.vy += 0.16f;
            for (auto& L : links_) {
                if (!L.alive) continue;
                auto& a = grains_[L.a];
                auto& b = grains_[L.b];
                const float dx = b.x - a.x, dy = b.y - a.y;
                const float d = std::hypot(dx, dy);
                if (d < 1e-3f) continue;
                if ((d - L.rest) / L.rest > brk) { L.alive = false; continue; }
                const float f = (d - L.rest) * 0.35f;
                const float nx = dx / d, ny = dy / d;
                a.vx += nx * f; a.vy += ny * f;
                b.vx -= nx * f; b.vy -= ny * f;
            }
            for (auto& g : grains_) {
                if (g.vx > 6.f) g.vx = 6.f;
                if (g.vx < -6.f) g.vx = -6.f;
                if (g.vy > 8.f) g.vy = 8.f;
                g.x += g.vx * 0.25f;
                g.y += g.vy * 0.25f;
                if (g.y > 312.f) { g.y = 312.f; g.vy *= -0.08f; g.vx *= 0.75f; }
                if (g.x < 48.f) { g.x = 48.f; g.vx = std::fabs(g.vx) * 0.4f; }
                if (g.x > 592.f) { g.x = 592.f; g.vx = -std::fabs(g.vx) * 0.4f; }
            }
            separate();
        }
        linksAlive_ = 0;
        for (const auto& L : links_) if (L.alive) ++linksAlive_;
    }

    void separate() {
        const float minD = 7.2f;
        const int n = int(grains_.size());
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) {
                float dx = grains_[j].x - grains_[i].x;
                float dy = grains_[j].y - grains_[i].y;
                const float d2 = dx * dx + dy * dy;
                if (d2 >= minD * minD || d2 < 1e-6f) continue;
                const float d = std::sqrt(d2);
                const float push = 0.5f * (minD - d);
                dx /= d; dy /= d;
                grains_[i].x -= dx * push; grains_[i].y -= dy * push;
                grains_[j].x += dx * push; grains_[j].y += dy * push;
            }
        }
    }

    static void grainColor(std::uint8_t id, int& r, int& g, int& b) {
        switch (id) {
        case 2: r = 255; g = 214; b = 90; break;
        case 3: r = 255; g = 168; b = 48; break;
        case 4: r = 255; g = 236; b = 170; break;
        case 5: r = 240; g = 140; b = 36; break;
        default: r = 255; g = 196; b = 70; break;
        }
    }

    void recount() {
        sand_ = 0;
        lower_ = 0;
        if (scene_ == Scene::Ball) {
            sand_ = int(grains_.size());
            return;
        }
        for (int y = 0; y < kRows; ++y)
            for (int x = 0; x < kCols; ++x) {
                if (at(x, y) <= kWall) continue;
                ++sand_;
                if (y > kRows / 2) ++lower_;
            }
    }

    void clear() {
        auto* p = surf_.rgba.data();
        const std::size_t n = std::size_t(kW) * kH;
        for (std::size_t i = 0; i < n; ++i) {
            p[0] = 10; p[1] = 10; p[2] = 14; p[3] = 255;
            p += 4;
        }
    }
    void plot(int x, int y, int r, int g, int b) {
        if (x < 0 || y < 0 || x >= kW || y >= kH) return;
        auto* p = &surf_.rgba[(std::size_t(y) * kW + std::size_t(x)) * 4];
        p[0] = std::uint8_t(r); p[1] = std::uint8_t(g); p[2] = std::uint8_t(b); p[3] = 255;
    }
    void block(int x, int y, int s, int r, int g, int b) {
        for (int dy = 0; dy < s; ++dy)
            for (int dx = 0; dx < s; ++dx) plot(x + dx, y + dy, r, g, b);
    }
    void disc(float x, float y, float rad, int r, int g, int b) {
        const int R = int(std::ceil(rad));
        const int ix = int(std::lround(x)), iy = int(std::lround(y));
        for (int dy = -R; dy <= R; ++dy)
            for (int dx = -R; dx <= R; ++dx)
                if (dx * dx + dy * dy <= int(rad * rad + 0.5f)) plot(ix + dx, iy + dy, r, g, b);
    }

    void drawGrid() {
        const int ox = (kW - kCols * 3) / 2;
        const int oy = (kH - kRows * 3) / 2;
        for (int y = 0; y < kRows; ++y)
            for (int x = 0; x < kCols; ++x) {
                const std::uint8_t c = at(x, y);
                if (c == kEmpty) continue;
                int r, g, b;
                if (c == kWall) { r = 28; g = 30; b = 36; }
                else grainColor(c, r, g, b);
                block(ox + x * 3, oy + y * 3, 3, r, g, b);
            }
    }

    void stampField() {
        std::fill(view_.cells.begin(), view_.cells.end(), 0);
        const int n = int(std::lround(std::clamp(progress(), 0.f, 1.f) * float(view_.w)));
        for (int i = 0; i < n && i < view_.w; ++i) view_.set(i, 0, 1);
    }

    void publish() {
        recount();
        clear();
        if (scene_ == Scene::Ball) {
            for (int x = 40; x < 600; ++x) plot(x, 318, 80, 70, 60);
            for (const auto& g : grains_) disc(g.x, g.y, 4.2f, g.cr, g.cg, g.cb);
        } else drawGrid();
        char b[160];
        if (scene_ == Scene::Ball)
            std::snprintf(b, sizeof b, "grains %d  ·  links %d / %d",
                          sand_, linksAlive_, int(links_.size()));
        else if (scene_ == Scene::Hourglass)
            std::snprintf(b, sizeof b, "grains %d  ·  %d in the lower bulb", sand_, lower_);
        else
            std::snprintf(b, sizeof b, "grains %d  ·  pouring", sand_);
        subtitle_ = b;
        stampField();
    }
};

inline SimPtr make_sand_pour() { return std::make_unique<SandSim>(SandSim::Scene::Pour); }
inline SimPtr make_sand_hourglass() { return std::make_unique<SandSim>(SandSim::Scene::Hourglass); }
inline SimPtr make_sand_ball() { return std::make_unique<SandSim>(SandSim::Scene::Ball); }

} // namespace bench
