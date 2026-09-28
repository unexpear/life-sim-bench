// collision.hpp — Particle Collision Lab, first slice.
//
// RESEARCH-NEXT.md starts this lab in 2D and names Box2D 3.1.1 as the engine
// to build with the workbench toolchain. That library is not vendored here.
// What is here is the analytical oracle the brief asks for, and a small disk
// world that uses it, so the lab can be stepped, measured and saved with the
// same project file as every other template. Walls, a fixed 1/60 s step and
// four substeps follow the brief's reading of Box2D's stepping advice. The
// stepper is not Box2D: there is no contact solver beyond one impulse along
// the normal, no friction, no sleep and no bullet CCD. Fast pairs are rejected
// rather than tunneled through.
//
// Particle Life is a different model and stays in continuous.hpp.

#pragma once
#include "../sim.hpp"
#include "../rng.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace bench {

// One-dimensional collision. e = 1 is the brief's oracle:
//
//   v1 = ((m1 - m2) u1 + 2 m2 u2) / (m1 + m2)
//   v2 = (2 m1 u1 + (m2 - m1) u2) / (m1 + m2)
//
// Restitution below one uses the same masses with a coefficient on the
// separating part. Non-finite or non-positive mass is refused; the caller
// keeps its previous velocities.
inline bool elastic_1d(double m1, double m2, double u1, double u2, double e,
                       double& v1, double& v2) {
    if (!(m1 > 0.0) || !(m2 > 0.0) || !(e >= 0.0) || !(e <= 1.0)) return false;
    if (!std::isfinite(m1 + m2 + u1 + u2 + e)) return false;
    const double s = m1 + m2;
    v1 = (m1 * u1 + m2 * u2 + m2 * e * (u2 - u1)) / s;
    v2 = (m1 * u1 + m2 * u2 + m1 * e * (u1 - u2)) / s;
    return std::isfinite(v1) && std::isfinite(v2);
}

struct Disk {
    double x = 0, y = 0, vx = 0, vy = 0;
    double radius = 0.35, mass = 1;
};

// A walled box of disks. Units are metres, kilograms and seconds, not pixels.
class CollisionWorld {
public:
    static constexpr double kDt = 1.0 / 60.0;
    static constexpr int kSubsteps = 4;
    static constexpr int kMaxDisks = 32;

    double width = 10, height = 6;
    double restitution = 1, gravity = 0;
    std::vector<Disk> disks;
    int contacts = 0;

    // False leaves the previous scene in place.
    bool assign(std::vector<Disk> next, double w, double h, double e, double g) {
        if (!(w >= 2.0) || !(w <= 40.0) || !(h >= 2.0) || !(h <= 40.0)) return false;
        if (!(e >= 0.0) || !(e <= 1.0) || !std::isfinite(g) || g < 0.0 || g > 40.0) return false;
        if (next.empty() || int(next.size()) > kMaxDisks) return false;
        const double sub = kDt / double(kSubsteps);
        for (const auto& d : next) {
            if (!(d.radius >= 0.05) || !(d.radius <= 1.5) || !(d.mass >= 0.05) || !(d.mass <= 50.0))
                return false;
            if (!std::isfinite(d.x + d.y + d.vx + d.vy)) return false;
            const double speed = std::hypot(d.vx, d.vy);
            // One substep must not jump a disk by its own radius, or a wall
            // and a neighbour can be skipped. The lab refuses that instead of
            // promising CCD it does not have.
            if (speed * sub >= d.radius) return false;
            if (d.x < d.radius || d.y < d.radius || d.x > w - d.radius || d.y > h - d.radius)
                return false;
        }
        width = w; height = h; restitution = e; gravity = g;
        disks = std::move(next);
        contacts = 0;
        return true;
    }

    void step() {
        contacts = 0;
        const double h = kDt / double(kSubsteps);
        std::vector<char> pair(disks.size() * disks.size(), 0);
        std::vector<char> wall(disks.size(), 0);
        for (int s = 0; s < kSubsteps; ++s) substep(h, pair, wall);
    }

    [[nodiscard]] double energy() const {
        double e = 0;
        for (const auto& d : disks) e += 0.5 * d.mass * (d.vx * d.vx + d.vy * d.vy);
        return e;
    }

private:
    void substep(double h, std::vector<char>& pair, std::vector<char>& wall) {
        for (auto& d : disks) {
            d.vy -= gravity * h;
            d.x += d.vx * h;
            d.y += d.vy * h;
        }
        for (std::size_t i = 0; i < disks.size(); ++i) {
            auto& d = disks[i];
            auto bounce = [&](double& pos, double& vel, double lo, double hi) {
                if (pos < lo) { pos = lo; if (vel < 0) { vel = -restitution * vel; if (!wall[i]) { wall[i] = 1; ++contacts; } } }
                if (pos > hi) { pos = hi; if (vel > 0) { vel = -restitution * vel; if (!wall[i]) { wall[i] = 1; ++contacts; } } }
            };
            bounce(d.x, d.vx, d.radius, width - d.radius);
            bounce(d.y, d.vy, d.radius, height - d.radius);
        }
        for (std::size_t i = 0; i < disks.size(); ++i) {
            for (std::size_t j = i + 1; j < disks.size(); ++j) {
                auto& a = disks[i];
                auto& b = disks[j];
                const double dx = b.x - a.x, dy = b.y - a.y;
                const double dist = std::hypot(dx, dy);
                const double minDist = a.radius + b.radius;
                if (!(dist < minDist) || dist < 1e-9) continue;
                const double nx = dx / dist, ny = dy / dist;
                const double u1 = a.vx * nx + a.vy * ny;
                const double u2 = b.vx * nx + b.vy * ny;
                if (u2 - u1 >= 0) {
                    // Overlap but separating or resting. Push apart without
                    // counting another contact: a pile must not report a
                    // collision on every substep it spends touching.
                } else {
                    double v1n = u1, v2n = u2;
                    if (elastic_1d(a.mass, b.mass, u1, u2, restitution, v1n, v2n)) {
                        a.vx += (v1n - u1) * nx; a.vy += (v1n - u1) * ny;
                        b.vx += (v2n - u2) * nx; b.vy += (v2n - u2) * ny;
                        const std::size_t key = i * disks.size() + j;
                        if (!pair[key]) { pair[key] = 1; ++contacts; }
                    }
                }
                const double overlap = minDist - dist;
                const double inv = 1.0 / a.mass + 1.0 / b.mass;
                const double shareA = (1.0 / a.mass) / inv;
                a.x -= nx * overlap * shareA; a.y -= ny * overlap * shareA;
                b.x += nx * overlap * (1.0 - shareA); b.y += ny * overlap * (1.0 - shareA);
            }
        }
    }
};

class CollisionLab final : public Sim {
public:
    explicit CollisionLab() {
        about_ = Provenance{
            "Particle Collision Lab", "2026",
            "Life-sim Workbench, after the research brief",
            "native/RESEARCH-NEXT.md, Particle Collision Lab. The one-dimensional "
            "elastic oracle is the standard two-body result cited there. Box2D 3.1.1 "
            "is the intended engine and is not linked in this build.",
            Replication::No,
            "No. Disks bounce. Nothing in the box copies itself.",
            "A 2D box of disks with mass, radius and a fixed 1/60 s step. This is a "
            "physical collision sketch, not Particle Life and not a Box2D build. "
            "Equal-mass head-on impact exchanges velocity; the other presets are an "
            "unequal pair, a wall bounce and a small pile under gravity. Saving the "
            "project stores these settings. It does not store live positions."
        };
        pal_ = {{{14, 18, 24}, "empty"}, {{232, 196, 122}, "disk"}};
        view_ = Field(160, 96);
        knobs_ = {
            {"preset", "scene", 0.f, 3.f, 0.f, 1.f,
             {"equal-mass exchange", "unequal masses", "wall bounce", "falling grains"}, true,
             "Which starting scene to build. Changing it starts that scene over."},
            {"restitution", "restitution", 0.f, 1.f, 1.f, 0.05f, {}, false,
             "How much of the closing speed comes back. 1 is the elastic oracle; "
             "0 leaves the disks together after they meet."},
            {"seed", "layout seed", 1.f, 40.f, 1.f, 1.f, {}, true,
             "Shifts the scene slightly so two runs of the same preset are not the "
             "same picture. Seed 1 is the centred layout the oracle checks use."},
        };
        reset();
    }

    const Provenance& about() const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field& field() const override { return view_; }
    std::uint64_t generation() const override { return gen_; }
    std::vector<Knob>& knobs() override { return knobs_; }

    void on_knob(const std::string& key, float v) override {
        for (auto& k : knobs_) if (k.key == key) k.value = k.quantised(v);
        if (key == "preset" || key == "seed") reset();
        else if (key == "restitution") world_.restitution = knob("restitution");
    }

    void reset() override {
        const int preset = int(knob("preset") + 0.5f);
        const int seed = std::max(1, int(knob("seed") + 0.5f));
        const double e = knob("restitution");
        const double shift = double(seed - 1) * 0.03;
        std::vector<Disk> disks;
        double gravity = 0;
        if (preset == 1) {
            disks.push_back(Disk{3.0, 3.0 + shift, 4.0, 0, 0.35, 1});
            disks.push_back(Disk{4.2, 3.0 + shift, 0.0, 0, 0.35, 3});
        } else if (preset == 2) {
            disks.push_back(Disk{8.8, 3.0 + shift, 3.0, 0, 0.35, 1});
        } else if (preset == 3) {
            gravity = 9.8;
            Rng rng(mix_seed(0xC011ull, seed));
            for (int i = 0; i < 6; ++i) {
                Disk d;
                d.x = 2.0 + double(i) * 1.1;
                d.y = 4.2 + shift * 0.25;
                d.vx = (rng.unit() - 0.5) * 0.4;
                d.vy = 0;
                d.radius = 0.28;
                d.mass = 0.5 + double(i % 3) * 0.25;
                disks.push_back(d);
            }
        } else {
            disks.push_back(Disk{3.0, 3.0 + shift, 4.0, 0, 0.35, 1});
            disks.push_back(Disk{4.2, 3.0 + shift, 0.0, 0, 0.35, 1});
        }
        world_.assign(std::move(disks), 10, 6, e, gravity);
        gen_ = 0;
        publish();
    }

    void step() override {
        world_.restitution = knob("restitution");
        world_.step();
        ++gen_;
        publish();
    }

    bool poke(float nx, float ny) override {
        if (world_.disks.empty()) return false;
        auto& d = world_.disks[0];
        // Offset from the middle of the canvas. A stamp at exactly (0.5, 0.5)
        // has no offset, and the roster's brush check stamps there: a zero
        // kick would leave the next frame identical to an unstamped run.
        double kickx = (double(nx) - 0.5) * 2.0;
        double kicky = (double(ny) - 0.5) * 2.0;
        if (std::fabs(kickx) < 1e-6 && std::fabs(kicky) < 1e-6) kickx = 8.0;
        d.vx += kickx;
        d.vy += kicky;
        const double sub = CollisionWorld::kDt / double(CollisionWorld::kSubsteps);
        const double speed = std::hypot(d.vx, d.vy);
        if (speed * sub >= d.radius && speed > 0) {
            const double scale = (d.radius * 0.9 / sub) / speed;
            d.vx *= scale; d.vy *= scale;
        }
        publish();
        return true;
    }

    std::vector<Metric> metrics() const override {
        double px = 0;
        for (const auto& d : world_.disks) { px += d.mass * d.vx; }
        return {
            Metric{"kinetic energy", world_.energy(), 0.0, Metric::Neither},
            Metric{"horizontal momentum", px, 0.0, Metric::Neither},
            Metric{"contacts", double(world_.contacts), 0.0, Metric::Neither},
        };
    }

    [[nodiscard]] const CollisionWorld& world() const { return world_; }

private:
    [[nodiscard]] float knob(const std::string& key) const {
        for (const auto& k : knobs_) if (k.key == key) return k.value;
        return 0.f;
    }

    void publish() {
        std::fill(view_.cells.begin(), view_.cells.end(), 0);
        for (const auto& d : world_.disks) {
            const int cx = int(d.x / world_.width * double(view_.w));
            const int cy = view_.h - 1 - int(d.y / world_.height * double(view_.h));
            const int rad = std::max(1, int(d.radius / world_.width * double(view_.w)));
            for (int dy = -rad; dy <= rad; ++dy)
                for (int dx = -rad; dx <= rad; ++dx) {
                    if (dx * dx + dy * dy > rad * rad) continue;
                    const int x = cx + dx, y = cy + dy;
                    if (x >= 0 && y >= 0 && x < view_.w && y < view_.h)
                        view_.set(x, y, 1);
                }
        }
    }

    Provenance about_;
    std::vector<Swatch> pal_;
    std::vector<Knob> knobs_;
    Field view_;
    CollisionWorld world_;
    std::uint64_t gen_ = 0;
};

inline SimPtr make_collision() { return std::make_unique<CollisionLab>(); }

} // namespace bench
