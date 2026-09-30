// satisfy.hpp — six circular scenes reconstructed from reference photos.
//
// The photos are of a screen, taken at an angle, with glare. What they show
// clearly is the rule written beside each circle, not a frame to copy:
//
//   grow     one ball, larger on every bounce, until it meets the ring
//   blocks   rings of squares knocked loose from the outside in; they fall
//            and stack until the centre is gone
//   bowl     capsules pour in and the ring shrinks as the count rises
//   fill     a ball under gravity leaves a trail; the run ends when the
//            disc is covered
//   spiral   a bead runs inward and speeds up until it hits the centre
//   water    a drop follows a sawtooth spiral; each fall adds one ball
//
// These are original scenes. They are not a recording of that clip, and the
// channel name from the photos is not drawn.

#pragma once
#include "../sim.hpp"
#include "../render/voxel.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace bench {

class SatisfySim final : public Sim {
public:
    enum class Scene { Grow, Blocks, Bowl, Fill, Spiral, Water };

    static constexpr int kW = 640;
    static constexpr int kH = 360;

    explicit SatisfySim(Scene scene) : scene_(scene) {
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
        fallen_ = 0;
        water_ = 0;
        done_ = false;
        kick_ = 0;
        coverage_ = 0;
        inkCount_ = 0;
        ballR_ = 8.f;
        bowlR_ = 118.f;
        spiralR_ = 104.f;
        spiralR0_ = 104.f;
        omega_ = 0.28f;
        theta_ = 0.f;
        dropS_ = 0.f;
        angle_ = 0.f;
        x_ = cx_ - 28.f;
        y_ = cy_ - 10.f;
        vx_ = 7.2f;
        vy_ = 0.6f;
        blocks_.clear();
        dots_.clear();
        trail_.clear();
        ticks_.clear();
        ink_.assign(std::size_t(kW) * kH, 0);
        for (float& f : floorY_) f = 338.f;
        const float seedA = (knob("seed", 1.f) - 1.f) * 0.51f;
        const float ca = std::cos(0.35f + seedA), sa = std::sin(0.35f + seedA);
        dirx_ = ca;
        diry_ = sa;
        if (scene_ == Scene::Fill) {
            vx_ = 6.4f * ca;
            vy_ = 6.4f * sa * 0.35f + 0.4f;
            x_ = cx_ - 36.f;
            y_ = cy_ - 24.f;
        }
        if (scene_ == Scene::Blocks) buildBlocks();
        if (scene_ == Scene::Spiral) theta_ = seedA;
        hue0_ = seedA;
        publish();
    }

    void step() override {
        if (!done_) {
            switch (scene_) {
            case Scene::Grow: stepGrow(); break;
            case Scene::Blocks: stepBlocks(); break;
            case Scene::Bowl: stepBowl(); break;
            case Scene::Fill: stepFill(); break;
            case Scene::Spiral: stepSpiral(); break;
            case Scene::Water: stepWater(); break;
            }
        }
        ++gen_;
        publish();
    }

    bool poke(float nx, float ny) override {
        const float px = nx * float(kW), py = ny * float(kH);
        switch (scene_) {
        case Scene::Grow: kick_ += 12.f; break;
        case Scene::Blocks: knockNearest(px, py, true); break;
        case Scene::Bowl: spawnDot(px, py, true); break;
        case Scene::Fill: {
            const float c = std::cos(0.6f), s = std::sin(0.6f);
            const float ox = vx_, oy = vy_;
            vx_ = ox * c - oy * s;
            vy_ = ox * s + oy * c;
            break;
        }
        case Scene::Spiral: omega_ += 0.22f; break;
        case Scene::Water: dropPool(px, py); break;
        }
        publish();
        return true;
    }

    std::vector<Metric> metrics() const override {
        const double p = std::clamp(double(progress()), 0.0, 1.0);
        return {
            Metric{"progress", p, 1.0, Metric::Higher},
            Metric{"bounces", double(bounces_), 0.0, Metric::Neither},
            Metric{"pieces", double(pieceCount()), 0.0, Metric::Neither},
        };
    }

    [[nodiscard]] float ballRadius() const { return ballR_; }
    [[nodiscard]] int bounces() const { return bounces_; }
    [[nodiscard]] int fallen() const { return fallen_; }
    [[nodiscard]] int blockCount() const { return int(blocks_.size()); }
    [[nodiscard]] int pieceCount() const {
        if (scene_ == Scene::Bowl) return int(dots_.size());
        if (scene_ == Scene::Water) return water_;
        if (scene_ == Scene::Blocks) return fallen_;
        return bounces_;
    }
    [[nodiscard]] float bowlRadius() const { return bowlR_; }
    [[nodiscard]] float coverage() const { return coverage_; }
    [[nodiscard]] float spiralRadius() const { return spiralR_; }
    [[nodiscard]] float spiralSpeed() const { return omega_; }
    [[nodiscard]] int waterBalls() const { return water_; }
    [[nodiscard]] float progress() const {
        switch (scene_) {
        case Scene::Grow: return R_ <= 0 ? 0 : std::clamp(ballR_ / R_, 0.f, 1.f);
        case Scene::Blocks: return blocks_.empty() ? 0 : float(fallen_) / float(blocks_.size());
        case Scene::Bowl: return std::clamp(float(dots_.size()) / std::max(1.f, knob("target", 40.f)), 0.f, 1.f);
        case Scene::Fill: return coverage_;
        case Scene::Spiral: return spiralR0_ <= 0 ? 1.f : std::clamp(1.f - spiralR_ / spiralR0_, 0.f, 1.f);
        case Scene::Water: return std::clamp(float(water_) / std::max(1.f, knob("target", 16.f)), 0.f, 1.f);
        }
        return 0;
    }

private:
    struct Block {
        float x, y, vx, vy, rad;
        int ring, cr, cg, cb;
        bool fallen = false;
        bool settled = false;
    };
    struct Dot {
        float x, y, vx, vy, ang, rad;
        int cr, cg, cb;
        bool pill = false;
    };
    struct Tick { float x, y, r, hue; };

    static constexpr float cx_ = 320.f;
    static constexpr float cy_ = 150.f;
    static constexpr float R_ = 112.f;
    static constexpr int kBins = 32;

    Scene scene_;
    Provenance about_;
    std::vector<Swatch> pal_;
    std::vector<Knob> knobs_;
    Field view_;
    Surface surf_;
    std::string subtitle_;
    std::uint64_t gen_ = 0;
    int bounces_ = 0, fallen_ = 0, water_ = 0, inkCount_ = 0;
    bool done_ = false;
    float kick_ = 0, ballR_ = 8, bowlR_ = 118, coverage_ = 0;
    float x_ = 0, y_ = 0, vx_ = 0, vy_ = 0, dirx_ = 1, diry_ = 0;
    float spiralR_ = 104, spiralR0_ = 104, omega_ = 0.28f, theta_ = 0, hue0_ = 0;
    float dropS_ = 0, angle_ = 0;
    float floorY_[kBins]{};
    std::vector<Block> blocks_;
    std::vector<Dot> dots_;
    std::vector<std::pair<float, float>> trail_;
    std::vector<Tick> ticks_;
    std::vector<std::uint8_t> ink_;

    [[nodiscard]] float knob(const std::string& key, float fallback) const {
        for (const auto& k : knobs_) if (k.key == key) return k.value;
        return fallback;
    }

    static Provenance provenance(Scene scene) {
        const char* title = "Ball grows into the circle";
        const char* blurb =
            "One ball bounces inside a ring and grows on every bounce until its "
            "radius meets the circle. Speed and growth are live; the seed only "
            "changes the starting direction.";
        switch (scene) {
        case Scene::Blocks:
            title = "Blocks fall toward the center";
            blurb = "Squares sit in rings. A striker knocks them loose from the outside "
                    "in. Fallen squares drop and stack under the circle. The run is "
                    "finished when the center square is gone.";
            break;
        case Scene::Bowl:
            title = "Bowl closes as it fills";
            blurb = "Capsules drop into a ring. The ring shrinks as the count rises, "
                    "so the bowl closes as it fills. The target is a short bench run "
                    "of that rule, not the multi-thousand count in the reference clip.";
            break;
        case Scene::Fill:
            title = "Trails fill the circle";
            blurb = "A ball falls and bounces inside the circle, leaving a thick trail. "
                    "The run ends when those trails cover the disc. The curves are the "
                    "path under gravity, not straight chords.";
            break;
        case Scene::Spiral:
            title = "Spiral speeds to the center";
            blurb = "A bead follows an inward spiral. It speeds up until it reaches the "
                    "center. The colored ticks are the path already taken.";
            break;
        case Scene::Water:
            title = "Water balls on a spiral";
            blurb = "A drop follows a sawtooth spiral. Each time it falls off the inner "
                    "end, one water ball joins the pool. The reference clip counts into "
                    "the thousands; this target is short enough to watch fill.";
            break;
        case Scene::Grow: break;
        }
        return Provenance{
            title, "2026", "Life-sim Workbench",
            "Original scenes, 2026. No published paper. The rules follow reference "
            "photos of circular satisfying-physics clips; the pictures themselves "
            "were angled photos of a screen.",
            Replication::No,
            "No. Balls, blocks and trails move. Nothing in these scenes copies itself.",
            blurb};
    }

    static std::vector<Knob> knobsFor(Scene scene) {
        const Knob seed{"seed", "start seed", 1.f, 12.f, 1.f, 1.f, {}, true,
                        "Starting direction or colour offset. Same seed, same opening."};
        switch (scene) {
        case Scene::Grow:
            return {
                {"speed", "speed", 8.f, 22.f, 14.f, 1.f, {}, false,
                 "Pixels travelled per step. Higher speed reaches the ring sooner."},
                {"growth", "growth per bounce", 2.f, 14.f, 6.f, 1.f, {}, false,
                 "How much the radius grows on each bounce, in pixels."},
                seed,
            };
        case Scene::Blocks:
            return {
                {"rate", "striker speed", 0.08f, 0.45f, 0.2f, 0.01f, {}, false,
                 "How fast the striker runs around the current outer ring."},
                seed,
            };
        case Scene::Bowl:
            return {
                {"interval", "steps between drops", 1.f, 8.f, 2.f, 1.f, {}, false,
                 "How many steps pass before another capsule appears."},
                {"target", "fill target", 16.f, 64.f, 40.f, 1.f, {}, false,
                 "Capsule count at which the bowl has closed."},
            };
        case Scene::Fill:
            return {
                {"speed", "speed", 5.f, 14.f, 8.f, 1.f, {}, false,
                 "Scales the ball's step. Gravity stays in pixels."},
                {"width", "trail width", 1.5f, 7.f, 3.5f, 0.5f, {}, false,
                 "Thickness of the painted path, in pixels."},
                seed,
            };
        case Scene::Spiral:
            return {
                {"accel", "acceleration", 0.004f, 0.04f, 0.012f, 0.002f, {}, false,
                 "How much the bead speeds up on each step, in radians."},
                seed,
            };
        case Scene::Water:
            return {
                {"speed", "drop speed", 0.03f, 0.12f, 0.06f, 0.01f, {}, false,
                 "How fast the drop runs along the spiral. One full run adds one ball."},
                {"target", "pool target", 8.f, 36.f, 16.f, 1.f, {}, false,
                 "Water balls at which the pool counts as full."},
            };
        }
        return {};
    }

    void buildBlocks() {
        struct Spec { float rad; int n; int r, g, b; };
        const Spec specs[] = {
            {0.f, 1, 255, 120, 40},
            {22.f, 12, 255, 150, 48},
            {42.f, 22, 50, 220, 255},
            {64.f, 32, 255, 214, 48},
            {86.f, 44, 64, 196, 255},
            {108.f, 56, 255, 176, 36},
        };
        const float phase = (knob("seed", 1.f) - 1.f) * 0.2f;
        for (int ring = 0; ring < 6; ++ring) {
            const auto& s = specs[ring];
            for (int i = 0; i < s.n; ++i) {
                const float a = phase + (float(i) + (ring & 1 ? 0.5f : 0.f)) / float(s.n) * 6.2831853f;
                Block b;
                b.x = cx_ + std::cos(a) * s.rad;
                b.y = cy_ + std::sin(a) * s.rad;
                b.vx = b.vy = 0;
                b.rad = s.rad;
                b.ring = ring;
                b.cr = s.r; b.cg = s.g; b.cb = s.b;
                blocks_.push_back(b);
            }
        }
        angle_ = phase;
        x_ = cx_ + std::cos(angle_) * 108.f;
        y_ = cy_ + std::sin(angle_) * 108.f;
    }

    int outerRing() const {
        int ring = -1;
        for (const auto& b : blocks_) if (!b.fallen) ring = std::max(ring, b.ring);
        return ring;
    }

    int binOf(float x) const {
        const float left = cx_ - float(kBins) * 5.5f;
        int b = int((x - left) / 11.f);
        if (b < 0) b = 0;
        if (b >= kBins) b = kBins - 1;
        return b;
    }

    void knockNearest(float px, float py, bool any) {
        const int outer = outerRing();
        int best = -1;
        float bestD = 1e9f;
        for (int i = 0; i < int(blocks_.size()); ++i) {
            const auto& b = blocks_[i];
            if (b.fallen) continue;
            if (!any && b.ring != outer) continue;
            const float d = std::hypot(b.x - px, b.y - py);
            if (d < bestD) { bestD = d; best = i; }
        }
        if (best < 0) return;
        if (!any && bestD > 28.f) return;
        auto& b = blocks_[std::size_t(best)];
        b.fallen = true;
        b.vx = (b.x - cx_) * 0.012f;
        b.vy = 0.9f;
        ++fallen_;
    }

    void stepGrow() {
        const float sp = knob("speed", 14.f) + kick_;
        kick_ *= 0.82f;
        bool grew = false;
        const int sub = 4;
        for (int s = 0; s < sub; ++s) {
            x_ += dirx_ * sp / float(sub);
            y_ += diry_ * sp / float(sub);
            const float dx = x_ - cx_, dy = y_ - cy_;
            const float dist = std::sqrt(dx * dx + dy * dy);
            const float lim = std::max(1.f, R_ - ballR_);
            if (dist > lim && dist > 1e-4f) {
                const float nx = dx / dist, ny = dy / dist;
                x_ = cx_ + nx * lim;
                y_ = cy_ + ny * lim;
                const float vn = dirx_ * nx + diry_ * ny;
                if (vn > 0.f) {
                    dirx_ -= 2.f * vn * nx;
                    diry_ -= 2.f * vn * ny;
                    const float m = std::sqrt(dirx_ * dirx_ + diry_ * diry_);
                    if (m > 1e-4f) { dirx_ /= m; diry_ /= m; }
                    if (!grew) {
                        ballR_ = std::min(R_, ballR_ + knob("growth", 6.f));
                        ++bounces_;
                        grew = true;
                    }
                }
            }
        }
        if (ballR_ >= R_ - 1.5f) { ballR_ = R_; done_ = true; }
        trail_.push_back({x_, y_});
        if (trail_.size() > 36) trail_.erase(trail_.begin());
    }

    void stepBlocks() {
        if (outerRing() >= 0) {
            angle_ += knob("rate", 0.2f);
            const int outer = outerRing();
            float rad = 0;
            for (const auto& b : blocks_) if (!b.fallen && b.ring == outer) { rad = b.rad; break; }
            x_ = cx_ + std::cos(angle_) * rad;
            y_ = cy_ + std::sin(angle_) * rad;
            knockNearest(x_, y_, false);
        }
        for (auto& b : blocks_) {
            if (!b.fallen || b.settled) continue;
            b.vy += 0.32f;
            b.x += b.vx;
            b.y += b.vy;
            const int bin = binOf(b.x);
            if (b.y >= floorY_[bin]) {
                b.y = floorY_[bin];
                b.vx = b.vy = 0;
                b.settled = true;
                floorY_[bin] -= 11.f;
            }
        }
        if (outerRing() < 0) {
            bool airborne = false;
            for (const auto& b : blocks_) if (b.fallen && !b.settled) airborne = true;
            if (!airborne) done_ = true;
        }
    }

    void spawnDot(float x, float y, bool pill) {
        if (int(dots_.size()) >= 72) return;
        static const int cols[][3] = {
            {255, 120, 170}, {120, 220, 255}, {255, 210, 90}, {170, 140, 255},
            {120, 255, 190}, {255, 160, 90}, {255, 245, 180}, {140, 180, 255},
        };
        const int i = int(dots_.size()) % 8;
        Dot d;
        d.x = x; d.y = y;
        d.vx = ((i & 1) ? 0.6f : -0.6f);
        d.vy = 0.4f;
        d.ang = float(i) * 0.4f;
        d.rad = pill ? 6.5f : 5.2f;
        d.cr = cols[i][0]; d.cg = cols[i][1]; d.cb = cols[i][2];
        d.pill = pill;
        dots_.push_back(d);
    }

    void containDots(float wallR, float grav, float bounce) {
        const float target = scene_ == Scene::Bowl ? knob("target", 40.f) : 1e9f;
        bowlR_ = wallR;
        if (scene_ == Scene::Bowl) {
            const float t = std::clamp(float(dots_.size()) / std::max(1.f, target), 0.f, 1.f);
            bowlR_ = R_ * (1.05f - 0.62f * t);
            if (float(dots_.size()) + 0.5f >= target) done_ = true;
        }
        for (int iter = 0; iter < 3; ++iter) {
            for (auto& d : dots_) {
                d.vy += (iter == 0 ? grav : 0.f);
                if (iter == 0) {
                    d.x += d.vx;
                    d.y += d.vy;
                    d.ang += 0.04f + d.vx * 0.02f;
                    const float sp = std::hypot(d.vx, d.vy);
                    if (sp > 7.f) { d.vx *= 7.f / sp; d.vy *= 7.f / sp; }
                }
                const float dx = d.x - cx_, dy = d.y - cy_;
                const float dist = std::sqrt(dx * dx + dy * dy);
                const float lim = std::max(d.rad + 1.f, bowlR_ - d.rad);
                if (dist > lim && dist > 1e-4f) {
                    const float nx = dx / dist, ny = dy / dist;
                    d.x = cx_ + nx * lim;
                    d.y = cy_ + ny * lim;
                    const float vn = d.vx * nx + d.vy * ny;
                    if (vn > 0.f) {
                        d.vx -= (1.f + bounce) * vn * nx;
                        d.vy -= (1.f + bounce) * vn * ny;
                        if (bounce < 0.15f) { d.vx *= 0.55f; d.vy *= 0.55f; }
                    }
                }
            }
            for (std::size_t i = 0; i < dots_.size(); ++i) {
                for (std::size_t j = i + 1; j < dots_.size(); ++j) {
                    float dx = dots_[j].x - dots_[i].x;
                    float dy = dots_[j].y - dots_[i].y;
                    const float minD = dots_[i].rad + dots_[j].rad;
                    const float d2 = dx * dx + dy * dy;
                    if (d2 >= minD * minD || d2 < 1e-6f) continue;
                    const float dist = std::sqrt(d2);
                    const float push = 0.5f * (minD - dist);
                    dx /= dist; dy /= dist;
                    dots_[i].x -= dx * push; dots_[i].y -= dy * push;
                    dots_[j].x += dx * push; dots_[j].y += dy * push;
                }
            }
        }
    }

    void stepBowl() {
        const int interval = std::max(1, int(knob("interval", 2.f) + 0.5f));
        const float target = knob("target", 40.f);
        if (!done_ && float(dots_.size()) < target && int(gen_) % interval == 0)
            spawnDot(cx_ + float(int(gen_) % 7 - 3) * 6.f, cy_ - bowlR_ + 18.f, true);
        containDots(R_, 0.22f, 0.4f);
    }

    void paintDisc(float x, float y, float rad) {
        const int R = int(std::ceil(rad));
        const int ix = int(std::lround(x)), iy = int(std::lround(y));
        const float rr = rad * rad;
        const float lim = (R_ - 2.f) * (R_ - 2.f);
        for (int dy = -R; dy <= R; ++dy)
            for (int dx = -R; dx <= R; ++dx) {
                if (float(dx * dx + dy * dy) > rr) continue;
                const int px = ix + dx, py = iy + dy;
                if (px < 0 || py < 0 || px >= kW || py >= kH) continue;
                const float ox = float(px) - cx_, oy = float(py) - cy_;
                if (ox * ox + oy * oy > lim) continue;
                auto& cell = ink_[std::size_t(py) * kW + px];
                if (cell) continue;
                cell = 1;
                ++inkCount_;
            }
    }

    void stepFill() {
        const float scale = knob("speed", 8.f) / 8.f;
        const float width = knob("width", 3.5f);
        const float x0 = x_, y0 = y_;
        const int sub = 4;
        for (int s = 0; s < sub; ++s) {
            vy_ += 0.22f / float(sub);
            x_ += vx_ * scale / float(sub);
            y_ += vy_ * scale / float(sub);
            const float dx = x_ - cx_, dy = y_ - cy_;
            const float dist = std::sqrt(dx * dx + dy * dy);
            const float lim = R_ - 4.f;
            if (dist > lim && dist > 1e-4f) {
                const float nx = dx / dist, ny = dy / dist;
                x_ = cx_ + nx * lim;
                y_ = cy_ + ny * lim;
                const float vn = vx_ * nx + vy_ * ny;
                if (vn > 0.f) {
                    vx_ -= 2.f * vn * nx;
                    vy_ -= 2.f * vn * ny;
                    const float sp = std::hypot(vx_, vy_);
                    if (sp > 16.f) { vx_ *= 16.f / sp; vy_ *= 16.f / sp; }
                    ++bounces_;
                }
            }
        }
        const float dx = x_ - x0, dy = y_ - y0;
        const int n = std::max(1, int(std::hypot(dx, dy)));
        for (int i = 0; i <= n; ++i) {
            const float t = float(i) / float(n);
            paintDisc(x0 + dx * t, y0 + dy * t, width);
        }
        const float area = 3.1415926f * (R_ - 2.f) * (R_ - 2.f);
        coverage_ = area > 0 ? std::clamp(float(inkCount_) / area, 0.f, 1.f) : 0;
        if (coverage_ >= 0.72f) done_ = true;
    }

    void stepSpiral() {
        if (spiralR_ <= 1.5f) { spiralR_ = 0; done_ = true; x_ = cx_; y_ = cy_; return; }
        omega_ += knob("accel", 0.012f);
        if (omega_ > 1.15f) omega_ = 1.15f;
        // Nine turns across the radius, sampled often enough that the ticks
        // read as rings rather than a handful of spokes.
        constexpr float thetaMax = 56.5f;
        const float dth = omega_;
        int n = std::max(1, int(dth * std::max(spiralR_, 8.f) / 2.6f));
        if (n > 28) n = 28;
        for (int i = 0; i < n; ++i) {
            theta_ += dth / float(n);
            spiralR_ = spiralR0_ * std::max(0.f, 1.f - theta_ / thetaMax);
            const float c = std::cos(theta_), s = std::sin(theta_);
            x_ = cx_ + c * spiralR_;
            y_ = cy_ + s * spiralR_;
            if (ticks_.size() < 2800)
                ticks_.push_back(Tick{x_, y_, spiralR_, std::fmod(theta_ * 0.15915494f + hue0_, 1.f)});
            if (spiralR_ <= 1.5f) { spiralR_ = 0; done_ = true; x_ = cx_; y_ = cy_; break; }
        }
    }

    void spiralAt(float s, float& x, float& y, float& a) const {
        a = s * 2.7f * 6.2831853f;
        const float rad = R_ * (0.96f - 0.74f * s);
        x = cx_ + std::cos(a) * rad;
        y = cy_ + std::sin(a) * rad;
    }

    void dropPool(float x, float y) {
        if (int(dots_.size()) >= 48) { ++water_; return; }
        spawnDot(x, y, false);
        dots_.back().cr = 90; dots_.back().cg = 190; dots_.back().cb = 255;
        dots_.back().vy = 1.2f;
        dots_.back().rad = 5.4f;
        ++water_;
    }

    void stepWater() {
        const float target = knob("target", 16.f);
        if (float(water_) >= target) { done_ = true; containDots(R_ * 0.98f, 0.18f, 0.05f); return; }
        dropS_ += knob("speed", 0.06f);
        if (dropS_ >= 1.f) {
            float x, y, a;
            spiralAt(1.f, x, y, a);
            dropPool(x, y + 8.f);
            dropS_ = 0.f;
        }
        containDots(R_ * 0.98f, 0.2f, 0.05f);
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

    void line(float x0, float y0, float x1, float y1, int r, int g, int b) {
        const float dx = x1 - x0, dy = y1 - y0;
        const int n = std::max(1, int(std::hypot(dx, dy)));
        for (int i = 0; i <= n; ++i) {
            const float t = float(i) / float(n);
            plot(int(std::lround(x0 + dx * t)), int(std::lround(y0 + dy * t)), r, g, b);
        }
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
        disc(x - rad * 0.28f, y - rad * 0.32f, std::max(1.2f, rad * 0.38f),
             std::min(255, r + 70), std::min(255, g + 70), std::min(255, b + 50));
    }

    void ring(float rad, float thick, int r, int g, int b, float gapAt, float gap) {
        const int n = std::max(32, int(rad * 7.f));
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

    void square(float x, float y, int s, int r, int g, int b) {
        const int x0 = int(std::lround(x)) - s / 2;
        const int y0 = int(std::lround(y)) - s / 2;
        for (int dy = 0; dy < s; ++dy)
            for (int dx = 0; dx < s; ++dx) plot(x0 + dx, y0 + dy, r, g, b);
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

    void tri(float x0, float y0, float x1, float y1, float x2, float y2, int r, int g, int b) {
        const int minx = std::max(0, int(std::floor(std::min(x0, std::min(x1, x2)))));
        const int maxx = std::min(kW - 1, int(std::ceil(std::max(x0, std::max(x1, x2)))));
        const int miny = std::max(0, int(std::floor(std::min(y0, std::min(y1, y2)))));
        const int maxy = std::min(kH - 1, int(std::ceil(std::max(y0, std::max(y1, y2)))));
        const float d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
        if (std::fabs(d) < 1e-4f) return;
        for (int y = miny; y <= maxy; ++y)
            for (int x = minx; x <= maxx; ++x) {
                const float w0 = ((y1 - y2) * (float(x) - x2) + (x2 - x1) * (float(y) - y2)) / d;
                const float w1 = ((y2 - y0) * (float(x) - x2) + (x0 - x2) * (float(y) - y2)) / d;
                const float w2 = 1.f - w0 - w1;
                if (w0 >= 0.f && w1 >= 0.f && w2 >= 0.f) plot(x, y, r, g, b);
            }
    }

    void drawTooth(float s) {
        float x, y, a;
        spiralAt(s, x, y, a);
        const float c = std::cos(a), sn = std::sin(a);
        const float tipx = x + c * 13.f, tipy = y + sn * 13.f;
        const float bx = x - c * 2.f, by = y - sn * 2.f;
        tri(tipx, tipy, bx - sn * 5.f, by + c * 5.f, bx + sn * 5.f, by - c * 5.f, 255, 92, 28);
    }

    void stampField() {
        std::fill(view_.cells.begin(), view_.cells.end(), 0);
        const int n = int(std::lround(std::clamp(progress(), 0.f, 1.f) * float(view_.w)));
        for (int i = 0; i < n && i < view_.w; ++i) view_.set(i, 0, 1);
    }

    void writeSubtitle() {
        char b[180];
        const int pct = int(std::lround(std::clamp(progress(), 0.f, 1.f) * 100.f));
        switch (scene_) {
        case Scene::Grow:
            std::snprintf(b, sizeof b, "bounces %d  ·  %d%% of the circle%s",
                          bounces_, pct, done_ ? "  ·  filled" : "");
            break;
        case Scene::Blocks:
            std::snprintf(b, sizeof b, "fallen %d / %d  ·  %d%% to the center",
                          fallen_, int(blocks_.size()), pct);
            break;
        case Scene::Bowl:
            std::snprintf(b, sizeof b, "balls %d / %d  ·  bowl %d px",
                          int(dots_.size()), int(knob("target", 40.f) + 0.5f), int(bowlR_));
            break;
        case Scene::Fill:
            std::snprintf(b, sizeof b, "bounces %d  ·  fill %d%%%s",
                          bounces_, pct, done_ ? "  ·  circle filled" : "");
            break;
        case Scene::Spiral:
            std::snprintf(b, sizeof b, "%.0f px/step  ·  %d%% to the center%s",
                          double(omega_ * std::max(spiralR_, 8.f)), pct, done_ ? "  ·  center" : "");
            break;
        case Scene::Water:
            std::snprintf(b, sizeof b, "water balls %d / %d  ·  every fall adds 1",
                          water_, int(knob("target", 16.f) + 0.5f));
            break;
        }
        subtitle_ = b;
    }

    void publish() {
        clear();
        switch (scene_) {
        case Scene::Grow:
            ring(R_ + 3.f, 3.4f, 90, 60, 150, 0, 0);
            ring(R_, 1.7f, 214, 196, 255, 0, 0);
            for (std::size_t i = 1; i < trail_.size(); ++i)
                line(trail_[i - 1].first, trail_[i - 1].second, trail_[i].first, trail_[i].second,
                     150, 100, 210);
            gloss(x_, y_, ballR_, 206, 186, 255);
            break;
        case Scene::Blocks:
            for (const auto& b : blocks_) square(b.x, b.y, 11, b.cr, b.cg, b.cb);
            if (fallen_ < int(blocks_.size())) {
                const float tx = -std::sin(angle_), ty = std::cos(angle_);
                disc(x_ - tx * 16.f, y_ - ty * 16.f, 2.2f, 180, 60, 10);
                disc(x_ - tx * 8.f, y_ - ty * 8.f, 3.6f, 255, 110, 20);
                disc(x_, y_, 5.2f, 255, 150, 40);
                disc(x_, y_, 2.2f, 255, 236, 200);
            }
            break;
        case Scene::Bowl:
            ring(bowlR_ + 2.f, 3.2f, 30, 120, 130, 0, 0);
            ring(bowlR_, 1.8f, 120, 255, 236, 0, 0);
            for (const auto& d : dots_) {
                if (!d.pill) { gloss(d.x, d.y, d.rad, d.cr, d.cg, d.cb); continue; }
                const float c = std::cos(d.ang), s = std::sin(d.ang);
                gloss(d.x - c * 7.f, d.y - s * 7.f, d.rad, d.cr, d.cg, d.cb);
                gloss(d.x, d.y, d.rad, d.cr, d.cg, d.cb);
                gloss(d.x + c * 7.f, d.y + s * 7.f, d.rad, d.cr, d.cg, d.cb);
            }
            break;
        case Scene::Fill:
            for (int y = 0; y < kH; ++y)
                for (int x = 0; x < kW; ++x)
                    if (ink_[std::size_t(y) * kW + x]) plot(x, y, 236, 240, 255);
            ring(R_, 1.8f, 255, 255, 255, 0, 0);
            gloss(x_, y_, 4.2f, 70, 150, 255);
            break;
        case Scene::Spiral:
            ring(R_ + 4.f, 2.2f, 255, 255, 255, theta_, done_ ? 0.f : 0.45f);
            for (const auto& t : ticks_) {
                int r, g, b; hsv(t.hue, r, g, b);
                const float c = t.r > 1.f ? (t.x - cx_) / t.r : 1.f;
                const float s = t.r > 1.f ? (t.y - cy_) / t.r : 0.f;
                const float inner = std::max(0.f, t.r - 1.2f), outer = t.r + 3.2f;
                const int n = 4;
                for (int i = 0; i <= n; ++i) {
                    const float u = inner + (outer - inner) * float(i) / float(n);
                    plot(int(std::lround(cx_ + c * u)), int(std::lround(cy_ + s * u)), r, g, b);
                }
            }
            disc(cx_, cy_, 6.f + (1.f - std::clamp(spiralR_ / spiralR0_, 0.f, 1.f)) * 10.f, 255, 170, 60);
            if (!done_) gloss(x_, y_, 5.f, 255, 244, 220);
            break;
        case Scene::Water:
            for (int i = 0; i < 90; ++i) {
                float x, y, a;
                spiralAt(float(i) / 90.f, x, y, a);
                disc(x, y, 1.35f, 255, 214, 150);
            }
            for (int i = 0; i < 110; ++i) drawTooth(float(i) / 110.f);
            {
                float dx, dy, a;
                spiralAt(std::clamp(dropS_, 0.f, 1.f), dx, dy, a);
                gloss(dx, dy, 6.5f, 80, 190, 255);
            }
            for (const auto& d : dots_) gloss(d.x, d.y, d.rad, d.cr, d.cg, d.cb);
            break;
        }
        stampField();
        writeSubtitle();
    }
};

inline SimPtr make_satisfy_grow() { return std::make_unique<SatisfySim>(SatisfySim::Scene::Grow); }
inline SimPtr make_satisfy_blocks() { return std::make_unique<SatisfySim>(SatisfySim::Scene::Blocks); }
inline SimPtr make_satisfy_bowl() { return std::make_unique<SatisfySim>(SatisfySim::Scene::Bowl); }
inline SimPtr make_satisfy_fill() { return std::make_unique<SatisfySim>(SatisfySim::Scene::Fill); }
inline SimPtr make_satisfy_spiral() { return std::make_unique<SatisfySim>(SatisfySim::Scene::Spiral); }
inline SimPtr make_satisfy_water() { return std::make_unique<SatisfySim>(SatisfySim::Scene::Water); }

} // namespace bench
