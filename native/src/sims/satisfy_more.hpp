// satisfy_more.hpp — nine more scenes from the same reference photos.
//
//   columns  balls fall through pegs; the columns underneath race
//   sides    every bounce adds a side, from a triangle toward a circle
//   square   the bouncing square grows, and its outline stays behind
//   claim    two colours; a dot joins whichever mover touches it
//   glass    shattering a tile speeds the ball up
//   beat     the ball lands on every note; a longer note jumps higher
//   shrink   the square walls step inward on every bounce
//   kaleido  one ball, drawn again at every mirror of the circle
//   escape   a ball that leaves through the gap is replaced by three
//
// Original scenes. The channel name from the photos is not drawn.

#pragma once
#include "../sim.hpp"
#include "../render/voxel.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace bench {

class SatisfyMore final : public Sim {
public:
    enum class Scene { Columns, Sides, Square, Claim, Glass, Beat, Shrink, Kaleido, Escape };

    static constexpr int kW = 640;
    static constexpr int kH = 360;

    explicit SatisfyMore(Scene scene) : scene_(scene) {
        about_ = provenance(scene);
        pal_ = {{{4, 5, 10}, "background"}, {{244, 244, 255}, "progress"}};
        view_ = Field(64, 36);
        surf_.resize(kW, kH);
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
        bounces_ = 0;
        claims_ = 0;
        shattered_ = 0;
        balls_ = 0;
        sides_ = 3;
        lead_ = 0;
        done_ = false;
        speed_ = 8.f;
        half_ = 14.f;
        margin_ = 78.f;
        ang_ = 0.2f;
        phase_ = 0.f;
        bar_ = 0;
        x_ = cx_; y_ = cy_;
        vx_ = 9.f; vy_ = 4.f;
        x2_ = cx_ + 40.f; y2_ = cy_;
        vx2_ = -7.f; vy2_ = 5.f;
        motes_.clear();
        cells_.clear();
        trail_.clear();
        stamps_.clear();
        for (int& c : cols_) c = 0;
        const float seedA = (knob("seed", 1.f) - 1.f) * 0.47f;
        hue0_ = seedA;
        switch (scene_) {
        case Scene::Columns: buildColumns(); break;
        case Scene::Sides:
            x_ = cx_ - 10.f; y_ = cy_ + 8.f;
            vx_ = 11.f * std::cos(0.4f + seedA);
            vy_ = 11.f * std::sin(0.4f + seedA);
            ang_ = seedA;
            break;
        case Scene::Square:
            x_ = 250.f; y_ = 150.f;
            vx_ = 9.f; vy_ = 6.5f;
            ang_ = seedA;
            break;
        case Scene::Claim: buildClaim(); break;
        case Scene::Glass: buildGlass(); break;
        case Scene::Beat: buildBeat(); break;
        case Scene::Shrink:
            x_ = cx_ + 90.f; y_ = cy_;
            vx_ = 13.f; vy_ = 5.f;
            break;
        case Scene::Kaleido:
            x_ = cx_ + 72.f; y_ = cy_ + 20.f;
            vx_ = -5.f * std::cos(seedA);
            vy_ = 8.5f;
            break;
        case Scene::Escape: {
            Mote m;
            m.x = cx_ + 20.f; m.y = cy_;
            m.vx = -8.f; m.vy = 1.1f;
            m.rad = 5.f;
            m.cr = 255; m.cg = 80; m.cb = 140;
            motes_.push_back(m);
            balls_ = 1;
            break;
        }
        }
        publish();
    }

    void step() override {
        if (!done_) {
            switch (scene_) {
            case Scene::Columns: stepColumns(); break;
            case Scene::Sides: stepSides(); break;
            case Scene::Square: stepSquare(); break;
            case Scene::Claim: stepClaim(); break;
            case Scene::Glass: stepGlass(); break;
            case Scene::Beat: stepBeat(); break;
            case Scene::Shrink: stepShrink(); break;
            case Scene::Kaleido: stepKaleido(); break;
            case Scene::Escape: stepEscape(); break;
            }
        }
        ++gen_;
        publish();
    }

    bool poke(float nx, float ny) override {
        const float px = nx * float(kW), py = ny * float(kH);
        switch (scene_) {
        case Scene::Columns: dropColumn(px + 36.f); break;
        case Scene::Sides: sides_ = std::min(48, sides_ + 1); vx_ += 4.f; break;
        case Scene::Square: vx_ += 8.f; vy_ -= 4.f; break;
        case Scene::Claim: claimNear(px, py, 0); break;
        case Scene::Glass: speed_ += 4.f; break;
        case Scene::Beat: phase_ += 0.65f; break;
        case Scene::Shrink: vx_ += 8.f; break;
        case Scene::Kaleido: {
            const float c = std::cos(0.7f), s = std::sin(0.7f);
            const float ox = vx_, oy = vy_;
            vx_ = ox * c - oy * s; vy_ = ox * s + oy * c;
            break;
        }
        case Scene::Escape: spawnEscape(px, py, 3); break;
        }
        publish();
        return true;
    }

    std::vector<Metric> metrics() const override {
        return {
            Metric{"progress", std::clamp(double(progress()), 0.0, 1.0), 1.0, Metric::Higher},
            Metric{"bounces", double(bounces_), 0.0, Metric::Neither},
            Metric{"count", double(count()), 0.0, Metric::Neither},
        };
    }

    [[nodiscard]] int sides() const { return sides_; }
    [[nodiscard]] int bounces() const { return bounces_; }
    [[nodiscard]] int columnBalls() const { int n = 0; for (int c : cols_) n += c; return n; }
    [[nodiscard]] float squareHalf() const { return half_; }
    [[nodiscard]] int claims() const { return claims_; }
    [[nodiscard]] int shattered() const { return shattered_; }
    [[nodiscard]] float glassSpeed() const { return speed_; }
    [[nodiscard]] float wallMargin() const { return margin_; }
    [[nodiscard]] int ballCount() const { return balls_; }
    [[nodiscard]] int count() const {
        switch (scene_) {
        case Scene::Columns: return columnBalls();
        case Scene::Sides: return sides_;
        case Scene::Claim: return claims_;
        case Scene::Glass: return shattered_;
        case Scene::Escape: return balls_;
        default: return bounces_;
        }
    }
    [[nodiscard]] float progress() const {
        switch (scene_) {
        case Scene::Columns: return std::clamp(float(columnBalls()) / 36.f, 0.f, 1.f);
        case Scene::Sides: return std::clamp(float(sides_ - 3) / 45.f, 0.f, 1.f);
        case Scene::Square: return std::clamp((half_ - 14.f) / 70.f, 0.f, 1.f);
        case Scene::Claim: return cells_.empty() ? 0 : std::clamp(float(claims_) / float(cells_.size()), 0.f, 1.f);
        case Scene::Glass: return cells_.empty() ? 0 : std::clamp(float(shattered_) / float(cells_.size()), 0.f, 1.f);
        case Scene::Beat: return std::clamp(float(bounces_) / 24.f, 0.f, 1.f);
        case Scene::Shrink: return std::clamp((margin_ - 78.f) / 90.f, 0.f, 1.f);
        case Scene::Kaleido: return std::clamp(float(bounces_) / 80.f, 0.f, 1.f);
        case Scene::Escape: return std::clamp(float(balls_) / 80.f, 0.f, 1.f);
        }
        return 0;
    }

private:
    struct Mote { float x, y, vx, vy, rad; int cr, cg, cb; bool gone = false; bool out = false; };
    struct Cell { float x, y; int hue; int owner; bool on = true; };
    struct Stamp { float x, y, ang, half; int cr, cg, cb; };

    static constexpr float cx_ = 320.f;
    static constexpr float cy_ = 180.f;
    static constexpr float R_ = 128.f;

    Scene scene_;
    Provenance about_;
    std::vector<Swatch> pal_;
    std::vector<Knob> knobs_;
    Field view_;
    Surface surf_;
    std::string subtitle_;
    std::uint64_t gen_ = 0;
    int bounces_ = 0, claims_ = 0, shattered_ = 0, balls_ = 0, sides_ = 3, lead_ = 0, bar_ = 0;
    bool done_ = false;
    float speed_ = 8, half_ = 14, margin_ = 78, ang_ = 0, phase_ = 0, hue0_ = 0;
    float x_ = 0, y_ = 0, vx_ = 0, vy_ = 0, x2_ = 0, y2_ = 0, vx2_ = 0, vy2_ = 0;
    int cols_[13]{};
    float notes_[12]{};
    std::vector<Mote> motes_;
    std::vector<Cell> cells_;
    std::vector<std::pair<float, float>> trail_;
    std::vector<Stamp> stamps_;

    [[nodiscard]] float knob(const std::string& key, float fallback) const {
        for (const auto& k : knobs_) if (k.key == key) return k.value;
        return fallback;
    }

    static Provenance provenance(Scene scene) {
        const char* title = "Which column fills first";
        const char* blurb =
            "Balls drop through a pegboard into thirteen columns. The stacks "
            "underneath are the race. Spread changes where a drop starts.";
        switch (scene) {
        case Scene::Sides:
            title = "Every bounce adds a side";
            blurb = "The cage starts as a triangle. Each bounce adds one side. "
                    "At 48 sides the cage is the circle. The faint shape is the "
                    "polygon it just left.";
            break;
        case Scene::Square:
            title = "The square grows on each bounce";
            blurb = "A square bounces inside a fixed frame and grows whenever it "
                    "hits a wall. Every outline it had stays on the screen.";
            break;
        case Scene::Claim:
            title = "Dots join the side they touch";
            blurb = "Two movers share a field of dots. A dot takes the colour of "
                    "whichever mover touches it, and keeps that colour.";
            break;
        case Scene::Glass:
            title = "Shattering glass speeds the ball";
            blurb = "The ball breaks every tile it hits, and gets faster each "
                    "time. The frame itself does not add speed.";
            break;
        case Scene::Beat:
            title = "Lands on every beat";
            blurb = "The ball hops from note to note and lands on the beat. "
                    "A taller note is a longer one, and the jump rises with it.";
            break;
        case Scene::Shrink:
            title = "Walls close in on each bounce";
            blurb = "The square starts wide. Every bounce steps all four walls "
                    "inward. The run ends when the ball has almost no room.";
            break;
        case Scene::Kaleido:
            title = "One ball, mirrored";
            blurb = "One ball bounces inside the circle. The picture is that "
                    "same path, copied around the centre. More bounces fill the disc.";
            break;
        case Scene::Escape:
            title = "Each escape spawns three balls";
            blurb = "Balls bounce inside a ring with one gap. A ball that leaves "
                    "through the gap is replaced by three inside. The count is "
                    "capped so the bench can keep drawing them.";
            break;
        case Scene::Columns: break;
        }
        return Provenance{
            title, "2026", "Life-sim Workbench",
            "Original scenes, 2026. No published paper. The rules follow reference "
            "photos of satisfying-physics clips; the pictures were angled photos of a screen.",
            Replication::No,
            "No. Balls, notes and tiles move or change colour. Nothing here copies itself.",
            blurb};
    }

    static std::vector<Knob> knobsFor(Scene scene) {
        const Knob seed{"seed", "start seed", 1.f, 12.f, 1.f, 1.f, {}, true,
                        "Starting direction or which note the tune begins on."};
        switch (scene) {
        case Scene::Columns:
            return {
                {"rate", "steps between drops", 1.f, 6.f, 2.f, 1.f, {}, false,
                 "How often a new ball is released."},
                {"spread", "spread", 0.f, 28.f, 10.f, 1.f, {}, false,
                 "How far left or right a new ball can start."},
            };
        case Scene::Sides:
            return {
                {"speed", "speed", 8.f, 18.f, 12.f, 1.f, {}, false,
                 "Pixels per step. Faster reaches the next side sooner."},
                seed,
            };
        case Scene::Square:
            return {
                {"speed", "speed", 6.f, 16.f, 10.f, 1.f, {}, false,
                 "Pixels per step inside the frame."},
                {"growth", "growth per bounce", 2.f, 10.f, 5.f, 1.f, {}, false,
                 "How many pixels the square's half-size gains on a bounce."},
            };
        case Scene::Claim:
            return {
                {"speed", "speed", 4.f, 14.f, 8.f, 1.f, {}, false,
                 "How fast both movers travel."},
                seed,
            };
        case Scene::Glass:
            return {
                {"speed", "starting speed", 4.f, 12.f, 6.f, 1.f, {}, false,
                 "Speed before the first tile breaks."},
                {"accel", "speed gained", 0.4f, 2.4f, 1.1f, 0.1f, {}, false,
                 "Pixels per step added each time a tile shatters."},
            };
        case Scene::Beat:
            return {
                {"tempo", "steps per note", 3.f, 12.f, 6.f, 1.f, {}, false,
                 "How many steps the ball takes to land on the next note."},
                seed,
            };
        case Scene::Shrink:
            return {
                {"speed", "speed", 8.f, 18.f, 13.f, 1.f, {}, false,
                 "Pixels per step."},
                {"shrink", "walls per bounce", 4.f, 14.f, 8.f, 1.f, {}, false,
                 "How many pixels every wall steps inward on a bounce."},
            };
        case Scene::Kaleido:
            return {
                {"speed", "speed", 5.f, 14.f, 8.f, 1.f, {}, false,
                 "Pixels per step for the one real ball."},
                {"folds", "mirrors", 4.f, 12.f, 8.f, 1.f, {}, false,
                 "How many times the path is copied around the circle."},
            };
        case Scene::Escape:
            return {
                {"speed", "speed", 4.f, 12.f, 7.f, 1.f, {}, false,
                 "Scales every ball's step."},
                {"gap", "gap width", 0.25f, 0.9f, 0.7f, 0.05f, {}, false,
                 "Width of the opening, in radians. Wider means an easier escape."},
            };
        }
        return {};
    }

    void buildColumns() {
        const float left = 168.f, right = 472.f;
        for (int row = 0; row < 8; ++row) {
            const int n = 8;
            const float y = 58.f + float(row) * 24.f;
            const float shift = (row & 1) ? 0.5f : 0.f;
            for (int i = 0; i < n; ++i) {
                Cell c;
                c.x = left + (right - left) * (float(i) + 0.5f + shift) / float(n + 1);
                c.y = y;
                c.on = true;
                cells_.push_back(c);
            }
        }
    }

    void dropColumn(float x) {
        if (int(motes_.size()) > 30) return;
        Mote m;
        m.x = std::clamp(x, 180.f, 460.f);
        m.y = 36.f;
        m.vx = (m.x - 320.f) * 0.02f;
        m.vy = 2.2f;
        m.rad = 3.4f;
        m.cr = 180; m.cg = 240; m.cb = 255;
        motes_.push_back(m);
    }

    void buildClaim() {
        for (int iy = -8; iy <= 8; ++iy)
            for (int ix = -8; ix <= 8; ++ix) {
                Cell c;
                c.x = cx_ + float(ix) * 14.f;
                c.y = cy_ + float(iy) * 14.f;
                if (std::hypot(c.x - cx_, c.y - cy_) > R_ - 12.f) continue;
                c.owner = -1;
                cells_.push_back(c);
            }
        x_ = cx_ - 50.f; y_ = cy_ + 10.f;
        vx_ = 6.f * std::cos(hue0_); vy_ = 6.f * std::sin(hue0_ + 0.6f);
        x2_ = cx_ + 46.f; y2_ = cy_ - 18.f;
        vx2_ = -5.5f; vy2_ = 4.f;
    }

    void claimNear(float x, float y, int owner) {
        int best = -1;
        float bestD = 13.f;
        for (int i = 0; i < int(cells_.size()); ++i) {
            if (cells_[i].owner >= 0) continue;
            const float d = std::hypot(cells_[i].x - x, cells_[i].y - y);
            if (d < bestD) { bestD = d; best = i; }
        }
        if (best < 0) return;
        cells_[best].owner = owner;
        ++claims_;
    }

    void buildGlass() {
        const int cols = 11, rows = 13;
        const float s = 22.f;
        const float ox = cx_ - cols * s * 0.5f;
        const float oy = 28.f;
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c) {
                Cell cell;
                cell.x = ox + (float(c) + 0.5f) * s;
                cell.y = oy + (float(r) + 0.5f) * s;
                cell.hue = r * 3 + c;
                cell.on = true;
                cells_.push_back(cell);
            }
        x_ = cx_ - 20.f; y_ = 40.f;
        vx_ = 0.55f; vy_ = 0.84f;
        speed_ = knob("speed", 6.f);
    }

    void buildBeat() {
        const float shape[12] = {42, 78, 55, 120, 70, 48, 136, 88, 60, 168, 36, 74};
        const int shift = int(knob("seed", 1.f) - 1.f) % 12;
        for (int i = 0; i < 12; ++i) notes_[i] = shape[(i + shift) % 12];
        bar_ = 0;
        phase_ = 0.f;
        placeBeat();
    }

    void placeBeat() {
        const int n = 12;
        const int i0 = bar_ % n;
        const int i1 = (bar_ + 1) % n;
        const float left = 150.f, gap = 28.f, base = 300.f;
        const float x0 = left + float(i0) * gap + 10.f;
        const float x1 = left + float(i1) * gap + 10.f;
        const float y0 = base - notes_[i0];
        const float y1 = base - notes_[i1];
        const float t = std::clamp(phase_, 0.f, 1.f);
        const float apex = 24.f + notes_[i1] * 0.85f;
        x_ = x0 + (x1 - x0) * t;
        y_ = y0 + (y1 - y0) * t - apex * 4.f * t * (1.f - t);
    }

    static void hsv(float h, int& r, int& g, int& b) {
        h -= std::floor(h);
        const float x = 1.f - std::fabs(std::fmod(h * 6.f, 2.f) - 1.f);
        float rf = 0, gf = 0, bf = 0;
        if (h < 1.f / 6.f) { rf = 1; gf = x; }
        else if (h < 2.f / 6.f) { rf = x; gf = 1; }
        else if (h < 3.f / 6.f) { gf = 1; bf = x; }
        else if (h < 4.f / 6.f) { gf = x; bf = 1; }
        else if (h < 5.f / 6.f) { rf = x; bf = 1; }
        else { rf = 1; bf = x; }
        r = int(rf * 255.f); g = int(gf * 255.f); b = int(bf * 255.f);
    }

    void clear() {
        auto* p = surf_.rgba.data();
        const std::size_t n = std::size_t(kW) * kH;
        for (std::size_t i = 0; i < n; ++i) {
            p[0] = 4; p[1] = 5; p[2] = 12; p[3] = 255;
            p += 4;
        }
    }
    void plot(int x, int y, int r, int g, int b) {
        if (x < 0 || y < 0 || x >= kW || y >= kH) return;
        auto* p = &surf_.rgba[(std::size_t(y) * kW + std::size_t(x)) * 4];
        p[0] = std::uint8_t(r); p[1] = std::uint8_t(g); p[2] = std::uint8_t(b); p[3] = 255;
    }
    void disc(float x, float y, float rad, int r, int g, int b) {
        const int R = int(std::ceil(rad));
        const int ix = int(std::lround(x)), iy = int(std::lround(y));
        const float rr = rad * rad;
        for (int dy = -R; dy <= R; ++dy)
            for (int dx = -R; dx <= R; ++dx)
                if (float(dx * dx + dy * dy) <= rr) plot(ix + dx, iy + dy, r, g, b);
    }
    void gloss(float x, float y, float rad, int r, int g, int b) {
        disc(x, y, rad, r, g, b);
        disc(x - rad * 0.28f, y - rad * 0.32f, std::max(1.1f, rad * 0.36f),
             std::min(255, r + 70), std::min(255, g + 70), std::min(255, b + 40));
    }
    void line(float x0, float y0, float x1, float y1, int r, int g, int b) {
        const float dx = x1 - x0, dy = y1 - y0;
        const int n = std::max(1, int(std::hypot(dx, dy)));
        for (int i = 0; i <= n; ++i) {
            const float t = float(i) / float(n);
            plot(int(std::lround(x0 + dx * t)), int(std::lround(y0 + dy * t)), r, g, b);
        }
    }
    void ring(float rad, float thick, int r, int g, int b, float gapAt, float gap) {
        const int n = std::max(24, int(rad * 5.f));
        for (int i = 0; i < n; ++i) {
            const float a = float(i) / float(n) * 6.2831853f;
            if (gap > 0.f) {
                float d = a - gapAt;
                d -= std::floor(d / 6.2831853f) * 6.2831853f;
                if (d > 3.1415926f) d -= 6.2831853f;
                if (std::fabs(d) < gap) continue;
            }
            disc(cx_ + std::cos(a) * rad, cy_ + std::sin(a) * rad, thick, r, g, b);
        }
    }
    void box(float x, float y, int s, int r, int g, int b) {
        const int x0 = int(std::lround(x)) - s / 2, y0 = int(std::lround(y)) - s / 2;
        for (int dy = 0; dy < s; ++dy)
            for (int dx = 0; dx < s; ++dx) plot(x0 + dx, y0 + dy, r, g, b);
    }
    void vbar(float x, float y0, float y1, int w, int r, int g, int b) {
        if (y1 < y0) std::swap(y0, y1);
        const int x0 = int(std::lround(x)) - w / 2;
        const int ya = std::max(0, int(std::floor(y0)));
        const int yb = std::min(kH - 1, int(std::ceil(y1)));
        for (int y = ya; y <= yb; ++y)
            for (int dx = 0; dx < w; ++dx) plot(x0 + dx, y, r, g, b);
    }

    bool hitPolygon(int n, float rot, float radius) {
        const float dx = x_ - cx_, dy = y_ - cy_;
        const float dist = std::hypot(dx, dy);
        if (dist < 1e-3f || n < 3) return false;
        const float tau = 6.2831853f;
        const float sector = tau / float(n);
        const float v0 = -1.5707963f + rot;
        float rel = std::atan2(dy, dx) - v0;
        rel -= std::floor(rel / tau) * tau;
        if (rel < 0.f) rel += tau;
        const float i = std::floor(rel / sector);
        const float flat = v0 + (i + 0.5f) * sector;
        const float nx = std::cos(flat), ny = std::sin(flat);
        const float apothem = radius * std::cos(3.1415926f / float(n));
        const float along = dx * nx + dy * ny;
        const float limit = apothem - 7.f;
        if (along <= limit) return false;
        const float over = along - limit;
        x_ -= nx * over; y_ -= ny * over;
        const float vn = vx_ * nx + vy_ * ny;
        if (vn > 0.f) {
            vx_ -= 2.f * vn * nx;
            vy_ -= 2.f * vn * ny;
            return true;
        }
        return false;
    }

    bool hitCircle(float& x, float& y, float& vx, float& vy, float rad) {
        const float dx = x - cx_, dy = y - cy_;
        const float dist = std::hypot(dx, dy);
        const float lim = rad - 5.f;
        if (dist <= lim || dist < 1e-3f) return false;
        const float nx = dx / dist, ny = dy / dist;
        x = cx_ + nx * lim; y = cy_ + ny * lim;
        const float vn = vx * nx + vy * ny;
        if (vn > 0.f) { vx -= 2.f * vn * nx; vy -= 2.f * vn * ny; return true; }
        return false;
    }

    void poly(int n, float rot, float radius, int r, int g, int b) {
        if (n < 3) return;
        const float tau = 6.2831853f;
        for (int i = 0; i < n; ++i) {
            const float a0 = -1.5707963f + rot + float(i) * tau / float(n);
            const float a1 = -1.5707963f + rot + float(i + 1) * tau / float(n);
            line(cx_ + std::cos(a0) * radius, cy_ + std::sin(a0) * radius,
                 cx_ + std::cos(a1) * radius, cy_ + std::sin(a1) * radius, r, g, b);
        }
    }

    void spinSquare(float x, float y, float half, float ang, int r, int g, int b) {
        float px[4], py[4];
        const float c = std::cos(ang), s = std::sin(ang);
        const float loc[4][2] = {{-half, -half}, {half, -half}, {half, half}, {-half, half}};
        for (int i = 0; i < 4; ++i) {
            px[i] = x + loc[i][0] * c - loc[i][1] * s;
            py[i] = y + loc[i][0] * s + loc[i][1] * c;
        }
        for (int i = 0; i < 4; ++i) line(px[i], py[i], px[(i + 1) % 4], py[(i + 1) % 4], r, g, b);
    }

    void reflectBox(float left, float top, float right, float bottom, bool grow) {
        bool hit = false;
        const float m = half_;
        if (x_ - m < left) { x_ = left + m; if (vx_ < 0) vx_ = -vx_; hit = true; }
        if (x_ + m > right) { x_ = right - m; if (vx_ > 0) vx_ = -vx_; hit = true; }
        if (y_ - m < top) { y_ = top + m; if (vy_ < 0) vy_ = -vy_; hit = true; }
        if (y_ + m > bottom) { y_ = bottom - m; if (vy_ > 0) vy_ = -vy_; hit = true; }
        if (hit && grow) {
            ++bounces_;
            if (scene_ == Scene::Square) half_ = std::min(78.f, half_ + knob("growth", 5.f));
            if (scene_ == Scene::Shrink) margin_ = std::min(168.f, margin_ + knob("shrink", 8.f));
        }
    }

    void stepColumns() {
        const int interval = std::max(1, int(knob("rate", 2.f) + 0.5f));
        if (int(gen_) % interval == 0) {
            const float spread = knob("spread", 10.f);
            dropColumn(320.f + std::sin(float(gen_) * 1.7f + hue0_) * spread);
        }
        for (auto& m : motes_) {
            if (m.gone) continue;
            m.vy += 0.55f;
            if (m.vy > 7.f) m.vy = 7.f;
            m.x += m.vx; m.y += m.vy;
            for (const auto& p : cells_) {
                const float dx = m.x - p.x, dy = m.y - p.y;
                const float d2 = dx * dx + dy * dy;
                if (d2 > 49.f || d2 < 1e-4f) continue;
                const float d = std::sqrt(d2);
                m.vx += (dx / d) * 2.4f;
                if (m.vx > 4.f) m.vx = 4.f;
                if (m.vx < -4.f) m.vx = -4.f;
                m.vy = std::max(m.vy, 1.6f);
            }
            if (m.x < 164.f) { m.x = 164.f; m.vx = std::fabs(m.vx); }
            if (m.x > 476.f) { m.x = 476.f; m.vx = -std::fabs(m.vx); }
            if (m.y > 268.f) {
                int c = int((m.x - 156.f) / 24.f);
                if (c < 0) c = 0;
                if (c > 12) c = 12;
                ++cols_[c];
                m.gone = true;
            }
        }
        lead_ = 0;
        for (int i = 1; i < 13; ++i) if (cols_[i] > cols_[lead_]) lead_ = i;
        if (cols_[lead_] >= 10) done_ = true;
    }

    void stepSides() {
        const float sp = knob("speed", 12.f);
        bool hit = false;
        const float mag = std::hypot(vx_, vy_);
        if (mag > 1e-3f) { vx_ = vx_ / mag * sp; vy_ = vy_ / mag * sp; }
        for (int s = 0; s < 4 && !hit; ++s) {
            x_ += vx_ * 0.25f; y_ += vy_ * 0.25f;
            if (hitPolygon(sides_, ang_, R_)) hit = true;
        }
        if (hit) {
            ++bounces_;
            sides_ = std::min(48, sides_ + 1);
            if (sides_ >= 48) done_ = true;
        }
    }

    void stepSquare() {
        const float sp = knob("speed", 10.f);
        const float mag = std::hypot(vx_, vy_);
        if (mag > 1e-3f) { vx_ = vx_ / mag * sp; vy_ = vy_ / mag * sp; }
        ang_ += 0.05f;
        x_ += vx_; y_ += vy_;
        reflectBox(168.f, 36.f, 472.f, 324.f, true);
        int cr, cg, cb;
        hsv(0.14f + float(bounces_) * 0.06f, cr, cg, cb);
        if (stamps_.size() < 700) stamps_.push_back(Stamp{x_, y_, ang_, half_, cr, cg, cb});
        if (half_ >= 76.f) done_ = true;
    }

    void stepClaim() {
        const float sp = knob("speed", 8.f);
        auto unit = [&](float& vx, float& vy) {
            const float m = std::hypot(vx, vy);
            if (m > 1e-3f) { vx = vx / m * sp; vy = vy / m * sp; }
        };
        unit(vx_, vy_); unit(vx2_, vy2_);
        x_ += vx_; y_ += vy_;
        x2_ += vx2_; y2_ += vy2_;
        hitCircle(x_, y_, vx_, vy_, R_);
        hitCircle(x2_, y2_, vx2_, vy2_, R_);
        claimNear(x_, y_, 0);
        claimNear(x2_, y2_, 1);
        if (!cells_.empty() && claims_ >= int(cells_.size())) done_ = true;
    }

    void stepGlass() {
        speed_ = std::max(speed_, knob("speed", 6.f));
        if (speed_ > 20.f) speed_ = 20.f;
        const float mag = std::hypot(vx_, vy_);
        if (mag < 1e-3f) { vx_ = 0.4f; vy_ = 0.9f; }
        else { vx_ /= mag; vy_ /= mag; }
        x_ += vx_ * speed_; y_ += vy_ * speed_;
        const float left = cx_ - 11 * 11.f, right = cx_ + 11 * 11.f;
        const float top = 28.f, bottom = 28.f + 13 * 22.f;
        if (x_ < left) { x_ = left; vx_ = std::fabs(vx_); }
        if (x_ > right) { x_ = right; vx_ = -std::fabs(vx_); }
        if (y_ < top) { y_ = top; vy_ = std::fabs(vy_); }
        if (y_ > bottom) { y_ = bottom; vy_ = -std::fabs(vy_); }
        for (auto& c : cells_) {
            if (!c.on) continue;
            if (std::fabs(c.x - x_) < 12.f && std::fabs(c.y - y_) < 12.f) {
                c.on = false;
                ++shattered_;
                ++bounces_;
                speed_ += knob("accel", 1.1f);
            }
        }
        if (!cells_.empty() && shattered_ >= int(cells_.size())) done_ = true;
    }

    void stepBeat() {
        const float tempo = std::max(1.f, knob("tempo", 6.f));
        phase_ += 1.f / tempo;
        while (phase_ >= 1.f) { phase_ -= 1.f; ++bar_; ++bounces_; }
        placeBeat();
        trail_.push_back({x_, y_});
        if (trail_.size() > 80) trail_.erase(trail_.begin());
        if (bounces_ >= 36) done_ = true;
    }

    void stepShrink() {
        const float sp = knob("speed", 13.f);
        const float mag = std::hypot(vx_, vy_);
        if (mag > 1e-3f) { vx_ = vx_ / mag * sp; vy_ = vy_ / mag * sp; }
        half_ = 6.f;
        x_ += vx_; y_ += vy_;
        const float left = margin_, top = margin_ * 0.55f;
        const float right = float(kW) - margin_, bottom = float(kH) - margin_ * 0.55f;
        reflectBox(left, top, right, bottom, true);
        trail_.push_back({x_, y_});
        if (trail_.size() > 80) trail_.erase(trail_.begin());
        if (right - left < 50.f) done_ = true;
    }

    void stepKaleido() {
        const float sp = knob("speed", 8.f);
        const float mag = std::hypot(vx_, vy_);
        if (mag > 1e-3f) { vx_ = vx_ / mag * sp; vy_ = vy_ / mag * sp; }
        const float x0 = x_, y0 = y_;
        x_ += vx_; y_ += vy_;
        if (hitCircle(x_, y_, vx_, vy_, R_)) ++bounces_;
        trail_.push_back({x0, y0});
        trail_.push_back({x_, y_});
        if (trail_.size() > 500) trail_.erase(trail_.begin(), trail_.begin() + 2);
    }

    void spawnEscape(float x, float y, int n) {
        static const int cols[][3] = {
            {255, 90, 140}, {80, 220, 255}, {255, 220, 80}, {170, 255, 120},
            {255, 140, 60}, {200, 120, 255}, {120, 255, 210}, {255, 245, 180},
        };
        for (int i = 0; i < n; ++i) {
            if (int(motes_.size()) >= 72) return;
            Mote m;
            const float a = hue0_ + float(balls_ + i) * 1.7f;
            m.x = x; m.y = y;
            m.vx = std::cos(a) * 4.5f;
            m.vy = std::sin(a) * 4.5f;
            m.rad = 5.2f;
            const int c = (balls_ + i) & 7;
            m.cr = cols[c][0]; m.cg = cols[c][1]; m.cb = cols[c][2];
            motes_.push_back(m);
            ++balls_;
        }
    }

    void stepEscape() {
        const float scale = knob("speed", 7.f) / 7.f;
        const float gap = knob("gap", 0.7f);
        const float gapAt = 3.1415926f;
        std::vector<Mote> born;
        for (auto& m : motes_) {
            if (m.gone) continue;
            m.x += m.vx * scale; m.y += m.vy * scale;
            if (m.out) continue;
            const float dx = m.x - cx_, dy = m.y - cy_;
            const float dist = std::hypot(dx, dy);
            if (dist < R_ - 6.f || dist < 1e-3f) continue;
            float ang = std::atan2(dy, dx) - gapAt;
            ang -= std::floor(ang / 6.2831853f) * 6.2831853f;
            if (ang > 3.1415926f) ang -= 6.2831853f;
            const float nx = dx / dist, ny = dy / dist;
            const float vn = m.vx * nx + m.vy * ny;
            if (std::fabs(ang) < gap && vn > 0.f) {
                m.out = true;
                ++bounces_;
                const float ix = cx_ + nx * (R_ - 18.f);
                const float iy = cy_ + ny * (R_ - 18.f);
                for (int i = 0; i < 3; ++i) {
                    if (int(motes_.size()) + int(born.size()) >= 72) break;
                    Mote b;
                    b.x = ix; b.y = iy;
                    const float a = float(i - 1) * 0.9f + 0.4f;
                    b.vx = std::cos(a) * 4.2f;
                    b.vy = std::sin(a) * 4.2f;
                    b.rad = 5.2f;
                    b.cr = 80 + (i * 70) % 180;
                    b.cg = 200;
                    b.cb = 255 - i * 40;
                    born.push_back(b);
                    ++balls_;
                }
            } else if (vn > 0.f) {
                m.x = cx_ + nx * (R_ - 6.f);
                m.y = cy_ + ny * (R_ - 6.f);
                m.vx -= 2.f * vn * nx;
                m.vy -= 2.f * vn * ny;
                // A perfect circle bounce repeats forever and can miss the gap.
                // A small sideways kick lets the orbit drift until it finds the opening.
                m.vx += ny * 0.85f;
                m.vy -= nx * 0.85f;
            }
        }
        for (auto& b : born) motes_.push_back(b);
        if (balls_ >= 72) done_ = true;
    }

    void mirrored(float x0, float y0, float x1, float y1, int r, int g, int b) {
        const int n = std::max(3, int(knob("folds", 8.f) + 0.5f));
        const float tau = 6.2831853f;
        auto rot = [&](float x, float y, float a, float& ox, float& oy) {
            const float dx = x - cx_, dy = y - cy_;
            const float c = std::cos(a), s = std::sin(a);
            ox = cx_ + dx * c - dy * s;
            oy = cy_ + dx * s + dy * c;
        };
        for (int k = 0; k < n; ++k) {
            const float a = float(k) * tau / float(n);
            float ax, ay, bx, by;
            rot(x0, y0, a, ax, ay); rot(x1, y1, a, bx, by);
            line(ax, ay, bx, by, r, g, b);
            rot(x0, 2.f * cy_ - y0, a, ax, ay);
            rot(x1, 2.f * cy_ - y1, a, bx, by);
            line(ax, ay, bx, by, r, g, b);
        }
    }

    void stampField() {
        std::fill(view_.cells.begin(), view_.cells.end(), 0);
        const int n = int(std::lround(std::clamp(progress(), 0.f, 1.f) * float(view_.w)));
        for (int i = 0; i < n && i < view_.w; ++i) view_.set(i, 0, 1);
    }

    void writeSubtitle() {
        char b[180];
        switch (scene_) {
        case Scene::Columns:
            std::snprintf(b, sizeof b, "balls %d  ·  column %d is ahead", columnBalls(), lead_ + 1);
            break;
        case Scene::Sides:
            std::snprintf(b, sizeof b, "sides %d / 48  ·  %d bounces", sides_, bounces_);
            break;
        case Scene::Square:
            std::snprintf(b, sizeof b, "bounces %d  ·  square %d px", bounces_, int(half_ * 2.f));
            break;
        case Scene::Claim:
            std::snprintf(b, sizeof b, "claimed %d / %d", claims_, int(cells_.size()));
            break;
        case Scene::Glass:
            std::snprintf(b, sizeof b, "shattered %d  ·  %.0f px/step", shattered_, double(speed_));
            break;
        case Scene::Beat:
            std::snprintf(b, sizeof b, "note %d  ·  lands on every beat", (bar_ % 12) + 1);
            break;
        case Scene::Shrink:
            std::snprintf(b, sizeof b, "bounces %d  ·  walls closing", bounces_);
            break;
        case Scene::Kaleido:
            std::snprintf(b, sizeof b, "bounces %d  ·  %d mirrors", bounces_,
                          int(knob("folds", 8.f) + 0.5f));
            break;
        case Scene::Escape:
            std::snprintf(b, sizeof b, "balls %d  ·  each escape spawns 3", balls_);
            break;
        }
        subtitle_ = b;
    }

    void publish() {
        clear();
        switch (scene_) {
        case Scene::Columns:
            line(156, 48, 156, 280, 80, 170, 255);
            line(484, 48, 484, 280, 80, 170, 255);
            for (const auto& c : cells_) disc(c.x, c.y, 3.1f, 230, 236, 245);
            for (int i = 0; i < 13; ++i) {
                int r, g, b; hsv(float(i) / 13.f, r, g, b);
                const float x = 168.f + float(i) * 24.f;
                const int h = std::min(cols_[i], 14);
                for (int k = 0; k < h; ++k) disc(x, 328.f - float(k) * 8.f, 3.3f, r, g, b);
            }
            for (const auto& m : motes_) if (!m.gone) gloss(m.x, m.y, m.rad, m.cr, m.cg, m.cb);
            break;
        case Scene::Sides:
            if (sides_ > 3) poly(sides_ - 1, ang_, R_, 70, 80, 110);
            poly(sides_, ang_, R_, 255, 150, 70);
            gloss(x_, y_, 11.f, 255, 236, 210);
            break;
        case Scene::Square:
            line(168, 36, 472, 36, 140, 230, 255);
            line(472, 36, 472, 324, 140, 230, 255);
            line(472, 324, 168, 324, 140, 230, 255);
            line(168, 324, 168, 36, 140, 230, 255);
            for (const auto& s : stamps_) spinSquare(s.x, s.y, s.half, s.ang, s.cr, s.cg, s.cb);
            break;
        case Scene::Claim:
            ring(R_, 2.f, 255, 190, 40, 0, 0);
            for (const auto& c : cells_) {
                if (c.owner < 0) disc(c.x, c.y, 2.4f, 230, 230, 240);
                else if (c.owner == 0) gloss(c.x, c.y, 5.2f, 70, 220, 255);
                else gloss(c.x, c.y, 5.2f, 255, 90, 190);
            }
            gloss(x_, y_, 6.f, 180, 250, 255);
            gloss(x2_, y2_, 6.f, 255, 150, 220);
            break;
        case Scene::Glass:
            for (const auto& c : cells_) {
                if (!c.on) continue;
                int r, g, b; hsv(float(c.hue) * 0.09f, r, g, b);
                box(c.x, c.y, 18, r, g, b);
            }
            disc(x_, y_, 8.f, 40, 80, 40);
            gloss(x_, y_, 4.5f, 170, 255, 140);
            break;
        case Scene::Beat: {
            const float left = 150.f, gap = 28.f, base = 300.f;
            line(left - 8, base, left + 12 * gap, base, 80, 180, 255);
            for (int i = 0; i < 12; ++i) {
                const bool hot = i == ((bar_ + 1) % 12);
                const int r = hot ? 255 : 40, g = hot ? 70 : 190, b = hot ? 90 : 255;
                vbar(left + float(i) * gap + 10.f, base - notes_[i], base, 12, r, g, b);
            }
            for (std::size_t i = 1; i < trail_.size(); ++i)
                line(trail_[i - 1].first, trail_[i - 1].second, trail_[i].first, trail_[i].second,
                     255, 140, 210);
            gloss(x_, y_, 6.f, 255, 230, 245);
            break;
        }
        case Scene::Shrink: {
            const float left = margin_, top = margin_ * 0.55f;
            const float right = float(kW) - margin_, bottom = float(kH) - margin_ * 0.55f;
            line(left, top, right, top, 235, 235, 245);
            line(right, top, right, bottom, 235, 235, 245);
            line(right, bottom, left, bottom, 235, 235, 245);
            line(left, bottom, left, top, 235, 235, 245);
            for (std::size_t i = 1; i < trail_.size(); ++i)
                line(trail_[i - 1].first, trail_[i - 1].second, trail_[i].first, trail_[i].second,
                     255, 90, 30);
            gloss(x_, y_, 7.f, 255, 200, 80);
            break;
        }
        case Scene::Kaleido: {
            ring(R_, 2.1f, 245, 245, 255, 0, 0);
            int r, g, b; hsv(0.10f + float(bounces_) * 0.004f, r, g, b);
            for (std::size_t i = 1; i < trail_.size(); i += 2)
                mirrored(trail_[i - 1].first, trail_[i - 1].second, trail_[i].first, trail_[i].second, r, g, b);
            const int n = std::max(3, int(knob("folds", 8.f) + 0.5f));
            for (int k = 0; k < n; ++k) {
                const float a = float(k) * 6.2831853f / float(n);
                const float dx = x_ - cx_, dy = y_ - cy_;
                const float c = std::cos(a), s = std::sin(a);
                disc(cx_ + dx * c - dy * s, cy_ + dx * s + dy * c, 3.2f, 245, 245, 255);
                disc(cx_ + dx * c + dy * s, cy_ + dx * s - dy * c, 3.2f, 245, 245, 255);
            }
            break;
        }
        case Scene::Escape:
            ring(R_, 2.4f, 255, 50, 120, 3.1415926f, knob("gap", 0.7f));
            for (const auto& m : motes_) if (!m.gone) gloss(m.x, m.y, m.rad, m.cr, m.cg, m.cb);
            break;
        }
        stampField();
        writeSubtitle();
    }
};

inline SimPtr make_satisfy_columns() { return std::make_unique<SatisfyMore>(SatisfyMore::Scene::Columns); }
inline SimPtr make_satisfy_sides() { return std::make_unique<SatisfyMore>(SatisfyMore::Scene::Sides); }
inline SimPtr make_satisfy_square() { return std::make_unique<SatisfyMore>(SatisfyMore::Scene::Square); }
inline SimPtr make_satisfy_claim() { return std::make_unique<SatisfyMore>(SatisfyMore::Scene::Claim); }
inline SimPtr make_satisfy_glass() { return std::make_unique<SatisfyMore>(SatisfyMore::Scene::Glass); }
inline SimPtr make_satisfy_beat() { return std::make_unique<SatisfyMore>(SatisfyMore::Scene::Beat); }
inline SimPtr make_satisfy_shrink() { return std::make_unique<SatisfyMore>(SatisfyMore::Scene::Shrink); }
inline SimPtr make_satisfy_kaleido() { return std::make_unique<SatisfyMore>(SatisfyMore::Scene::Kaleido); }
inline SimPtr make_satisfy_escape() { return std::make_unique<SatisfyMore>(SatisfyMore::Scene::Escape); }

} // namespace bench
