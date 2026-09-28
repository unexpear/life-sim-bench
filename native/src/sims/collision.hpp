// collision.hpp â€” Particle Collision Lab (Box2D 3.1.1).
//
// RESEARCH-NEXT.md starts this lab in 2D and names Box2D 3.1.1 as the engine
// to build with the workbench toolchain. That library is vendored under
// native/third_party/box2d and linked here. The one-dimensional elastic oracle
// stays as an analytical check; the world itself is stepped by Box2D with a
// fixed 1/60 s step and four substeps. Particle Life stays in continuous.hpp.

#pragma once
#include "../sim.hpp"
#include "../rng.hpp"
#include <box2d/box2d.h>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace bench {

// One-dimensional collision. e = 1 is the brief's oracle:
//
//   v1 = ((m1 - m2) u1 + 2 m2 u2) / (m1 + m2)
//   v2 = (2 m1 u1 + (m2 - m1) u2) / (m1 + m2)
//
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

// A walled box of disks backed by Box2D. Units are metres, kilograms, seconds.
class CollisionWorld {
public:
    static constexpr double kDt = 1.0 / 60.0;
    static constexpr int kSubsteps = 4;
    static constexpr int kMaxDisks = 512;

    double width = 10, height = 6;
    double restitution = 1, gravity = 0;
    std::vector<Disk> disks;
    int contacts = 0;

    CollisionWorld() = default;
    CollisionWorld(const CollisionWorld& other) { *this = other; }
    CollisionWorld& operator=(const CollisionWorld& other) {
        if (this == &other) return *this;
        clear_world();
        assign(other.disks, other.width, other.height, other.restitution, other.gravity);
        contacts = other.contacts;
        return *this;
    }
    CollisionWorld(CollisionWorld&& other) noexcept { move_from(std::move(other)); }
    CollisionWorld& operator=(CollisionWorld&& other) noexcept {
        if (this == &other) return *this;
        clear_world();
        move_from(std::move(other));
        return *this;
    }
    ~CollisionWorld() { clear_world(); }

    // False leaves the previous scene in place.
    bool assign(std::vector<Disk> next, double w, double h, double e, double g) {
        if (!(w >= 2.0) || !(w <= 80.0) || !(h >= 2.0) || !(h <= 80.0)) return false;
        if (!(e >= 0.0) || !(e <= 1.0) || !std::isfinite(g) || g < 0.0 || g > 40.0) return false;
        if (next.empty() || int(next.size()) > kMaxDisks) return false;
        for (const auto& d : next) {
            if (!(d.radius >= 0.05) || !(d.radius <= 1.5) || !(d.mass >= 0.05) || !(d.mass <= 50.0))
                return false;
            if (!std::isfinite(d.x + d.y + d.vx + d.vy)) return false;
            const double speed = std::hypot(d.vx, d.vy);
            // Box2D handles wall CCD for ordinary and bullet bodies; still refuse
            // speeds that would cross much of the box in one step.
            if (speed > 60.0) return false;
            if (d.x < d.radius || d.y < d.radius || d.x > w - d.radius || d.y > h - d.radius)
                return false;
        }
        if (!rebuild(std::move(next), w, h, e, g)) return false;
        contacts = 0;
        return true;
    }

    void step() {
        if (!b2World_IsValid(world_)) return;
        // Keep shape restitution in sync if the knob changed after assign.
        apply_materials();
        b2World_SetGravity(world_, b2Vec2{0.f, float(-gravity)});
        b2World_Step(world_, float(kDt), kSubsteps);
        const b2ContactEvents ev = b2World_GetContactEvents(world_);
        // beginCount is new contacts this step; resting piles do not re-fire it.
        contacts = ev.beginCount;
        sync_from_bodies();
    }

    [[nodiscard]] double energy() const {
        double e = 0;
        for (const auto& d : disks) e += 0.5 * d.mass * (d.vx * d.vx + d.vy * d.vy);
        return e;
    }

    [[nodiscard]] b2Counters box2d_counters() const {
        if (!b2World_IsValid(world_)) return {};
        return b2World_GetCounters(world_);
    }

    bool add_disk(Disk d) {
        if (int(disks.size()) >= kMaxDisks) return false;
        auto next = disks;
        next.push_back(d);
        return assign(std::move(next), width, height, restitution, gravity);
    }

    bool remove_nearest(double x, double y) {
        if (disks.empty()) return false;
        std::size_t best = 0;
        double bestD = 1e300;
        for (std::size_t i = 0; i < disks.size(); ++i) {
            const double dd = std::hypot(disks[i].x - x, disks[i].y - y);
            if (dd < bestD) { bestD = dd; best = i; }
        }
        if (bestD > disks[best].radius * 1.5) return false;
        auto next = disks;
        next.erase(next.begin() + std::ptrdiff_t(best));
        if (next.empty()) return false;
        return assign(std::move(next), width, height, restitution, gravity);
    }

    int nearest_index(double x, double y) const {
        if (disks.empty()) return -1;
        int best = 0;
        double bestD = 1e300;
        for (int i = 0; i < int(disks.size()); ++i) {
            const double dd = std::hypot(disks[std::size_t(i)].x - x, disks[std::size_t(i)].y - y);
            if (dd < bestD) { bestD = dd; best = i; }
        }
        return bestD <= disks[std::size_t(best)].radius * 1.5 ? best : -1;
    }

    bool set_disk_velocity(int index, double vx, double vy) {
        if (index < 0 || index >= int(disks.size()) || !b2World_IsValid(world_)) return false;
        if (!std::isfinite(vx + vy) || std::hypot(vx, vy) > 60.0) return false;
        disks[std::size_t(index)].vx = vx;
        disks[std::size_t(index)].vy = vy;
        b2Body_SetLinearVelocity(bodies_[std::size_t(index)], b2Vec2{float(vx), float(vy)});
        b2Body_SetAwake(bodies_[std::size_t(index)], true);
        return true;
    }

    bool move_disk(int index, double x, double y) {
        if (index < 0 || index >= int(disks.size()) || !b2World_IsValid(world_)) return false;
        auto& d = disks[std::size_t(index)];
        if (x < d.radius || y < d.radius || x > width - d.radius || y > height - d.radius) return false;
        if (!std::isfinite(x + y)) return false;
        d.x = x; d.y = y;
        b2Body_SetTransform(bodies_[std::size_t(index)], b2Vec2{float(x), float(y)}, b2Rot_identity);
        b2Body_SetAwake(bodies_[std::size_t(index)], true);
        return true;
    }

private:
    b2WorldId world_ = b2_nullWorldId;
    b2BodyId ground_ = b2_nullBodyId;
    std::vector<b2BodyId> bodies_;
    std::vector<b2ShapeId> shapes_;

    void move_from(CollisionWorld&& other) noexcept {
        world_ = other.world_; other.world_ = b2_nullWorldId;
        ground_ = other.ground_; other.ground_ = b2_nullBodyId;
        bodies_ = std::move(other.bodies_);
        shapes_ = std::move(other.shapes_);
        width = other.width; height = other.height;
        restitution = other.restitution; gravity = other.gravity;
        disks = std::move(other.disks);
        contacts = other.contacts;
    }

    void clear_world() {
        bodies_.clear();
        shapes_.clear();
        ground_ = b2_nullBodyId;
        if (b2World_IsValid(world_)) {
            b2DestroyWorld(world_);
            world_ = b2_nullWorldId;
        }
    }

    void apply_materials() {
        for (auto shape : shapes_) {
            if (!b2Shape_IsValid(shape)) continue;
            b2Shape_SetFriction(shape, 0.f);
            b2Shape_SetRestitution(shape, float(restitution));
        }
    }

    void sync_from_bodies() {
        for (std::size_t i = 0; i < bodies_.size(); ++i) {
            if (!b2Body_IsValid(bodies_[i])) continue;
            const b2Vec2 p = b2Body_GetPosition(bodies_[i]);
            const b2Vec2 v = b2Body_GetLinearVelocity(bodies_[i]);
            disks[i].x = p.x; disks[i].y = p.y;
            disks[i].vx = v.x; disks[i].vy = v.y;
        }
    }

    bool rebuild(std::vector<Disk> next, double w, double h, double e, double g) {
        clear_world();
        b2WorldDef def = b2DefaultWorldDef();
        def.gravity = b2Vec2{0.f, float(-g)};
        def.enableSleep = false;
        // Ideal elastic presets should bounce even at low approach speed.
        def.restitutionThreshold = 0.0f;
        def.workerCount = 1;
        world_ = b2CreateWorld(&def);
        if (!b2World_IsValid(world_)) return false;

        b2BodyDef groundDef = b2DefaultBodyDef();
        groundDef.type = b2_staticBody;
        ground_ = b2CreateBody(world_, &groundDef);
        b2ShapeDef wallShape = b2DefaultShapeDef();
        wallShape.material.friction = 0.f;
        wallShape.material.restitution = float(e);
        wallShape.filter.categoryBits = 1;
        wallShape.filter.maskBits = UINT64_MAX;
        const float t = 0.5f;
        const float fw = float(w), fh = float(h);
        const b2Polygon floor = b2MakeOffsetBox(fw * 0.5f + t, t, b2Vec2{fw * 0.5f, -t}, b2Rot_identity);
        const b2Polygon ceil  = b2MakeOffsetBox(fw * 0.5f + t, t, b2Vec2{fw * 0.5f, fh + t}, b2Rot_identity);
        const b2Polygon left  = b2MakeOffsetBox(t, fh * 0.5f + t, b2Vec2{-t, fh * 0.5f}, b2Rot_identity);
        const b2Polygon right = b2MakeOffsetBox(t, fh * 0.5f + t, b2Vec2{fw + t, fh * 0.5f}, b2Rot_identity);
        b2CreatePolygonShape(ground_, &wallShape, &floor);
        b2CreatePolygonShape(ground_, &wallShape, &ceil);
        b2CreatePolygonShape(ground_, &wallShape, &left);
        b2CreatePolygonShape(ground_, &wallShape, &right);

        bodies_.clear();
        shapes_.clear();
        bodies_.reserve(next.size());
        shapes_.reserve(next.size());
        for (const auto& d : next) {
            b2BodyDef bd = b2DefaultBodyDef();
            bd.type = b2_dynamicBody;
            bd.position = b2Vec2{float(d.x), float(d.y)};
            bd.linearVelocity = b2Vec2{float(d.vx), float(d.vy)};
            bd.linearDamping = 0.f;
            bd.angularDamping = 0.f;
            bd.enableSleep = false;
            bd.isAwake = true;
            bd.fixedRotation = false;
            // Fast disks get bullet CCD against walls/static; pairwise bullet CCD
            // is still not guaranteed by Box2D.
            bd.isBullet = std::hypot(d.vx, d.vy) > 8.0;
            b2BodyId body = b2CreateBody(world_, &bd);
            b2ShapeDef sd = b2DefaultShapeDef();
            const float area = float(B2_PI * d.radius * d.radius);
            sd.density = area > 0.f ? float(d.mass) / area : 1.f;
            sd.material.friction = 0.f;
            sd.material.restitution = float(e);
            sd.enableContactEvents = true;
            sd.enableHitEvents = true;
            b2Circle circle{{0.f, 0.f}, float(d.radius)};
            b2ShapeId shape = b2CreateCircleShape(body, &sd, &circle);
            // Honour the requested mass exactly (density*area can drift in float).
            b2MassData mass = b2Body_GetMassData(body);
            mass.mass = float(d.mass);
            mass.center = b2Vec2{0.f, 0.f};
            mass.rotationalInertia = float(0.5 * d.mass * d.radius * d.radius);
            b2Body_SetMassData(body, mass);
            bodies_.push_back(body);
            shapes_.push_back(shape);
        }

        width = w; height = h; restitution = e; gravity = g;
        disks = std::move(next);
        sync_from_bodies();
        return true;
    }
};

class CollisionLab final : public Sim {
public:
    explicit CollisionLab() {
        about_ = Provenance{
            "Particle Collision Lab", "2026",
            "Life-sim Workbench, after the research brief",
            "native/RESEARCH-NEXT.md, Particle Collision Lab. Steps with vendored "
            "Box2D 3.1.1. The one-dimensional elastic oracle remains the analytical "
            "check for equal-mass exchange.",
            Replication::No,
            "No. Disks bounce. Nothing in the box copies itself.",
            "A 2D box of disks with mass, radius and a fixed 1/60 s step, stepped by "
            "Box2D. Presets cover equal-mass exchange, unequal masses, a wall bounce, "
            "falling grains, a gas box and a denser pile. Edit tools place, erase, "
            "kick and drag disks. Saving the project stores knobs and the live scene."
        };
        pal_ = {{{14, 18, 24}, "empty"}, {{232, 196, 122}, "disk"}};
        view_ = Field(160, 96);
        knobs_ = {
            {"preset", "scene", 0.f, 5.f, 0.f, 1.f,
             {"equal-mass exchange", "unequal masses", "wall bounce", "falling grains",
              "gas box", "dense pile"}, true,
             "Which starting scene to build. Changing it starts that scene over."},
            {"restitution", "restitution", 0.f, 1.f, 1.f, 0.05f, {}, false,
             "How much of the closing speed comes back. 1 is the elastic oracle; "
             "0 leaves the disks together after they meet."},
            {"seed", "layout seed", 1.f, 40.f, 1.f, 1.f, {}, true,
             "Shifts the scene slightly so two runs of the same preset are not the "
             "same picture. Seed 1 is the centred layout the oracle checks use."},
            // Edit tools only change poke/drag; they do not change a free run.
            // display_only: the dead-knob self-test must not demand a stepped-field
            // difference from parameters that only affect the stamp.
            {"tool", "edit tool", 0.f, 3.f, 0.f, 1.f,
             {"kick", "place", "erase", "drag"}, false,
             "Stamp action: kick the nearest disk, place a new one, erase one, or "
             "drag to move. Dragging also aims a kick when the tool is kick.", true},
            {"radius", "place radius", 0.15f, 0.8f, 0.35f, 0.05f, {}, false,
             "Radius used when the place tool adds a disk.", true},
            {"mass", "place mass", 0.25f, 8.f, 1.f, 0.25f, {}, false,
             "Mass used when the place tool adds a disk.", true},
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
        double w = 10, h = 6;
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
        } else if (preset == 4) {
            // Sparse gas: many small disks, no gravity.
            w = 16; h = 10;
            Rng rng(mix_seed(0x6A5ull, seed));
            for (int i = 0; i < 80; ++i) {
                Disk d;
                d.radius = 0.18;
                d.mass = 0.4;
                d.x = d.radius + 0.2 + rng.unit() * (w - 2 * d.radius - 0.4);
                d.y = d.radius + 0.2 + rng.unit() * (h - 2 * d.radius - 0.4);
                const double ang = rng.unit() * 6.283185307179586;
                const double sp = 1.0 + rng.unit() * 2.5;
                d.vx = std::cos(ang) * sp;
                d.vy = std::sin(ang) * sp;
                disks.push_back(d);
            }
        } else if (preset == 5) {
            gravity = 9.8;
            w = 12; h = 10;
            Rng rng(mix_seed(0xD3ADull, seed));
            int n = 0;
            for (int row = 0; row < 10 && n < 120; ++row) {
                for (int col = 0; col < 12 && n < 120; ++col) {
                    Disk d;
                    d.radius = 0.22;
                    d.mass = 0.6 + 0.15 * double(n % 4);
                    d.x = 1.0 + double(col) * 0.85 + (row % 2) * 0.4;
                    d.y = 2.0 + double(row) * 0.7 + shift * 0.1;
                    if (d.x < d.radius || d.x > w - d.radius || d.y < d.radius || d.y > h - d.radius)
                        continue;
                    d.vx = (rng.unit() - 0.5) * 0.2;
                    d.vy = 0;
                    disks.push_back(d);
                    ++n;
                }
            }
        } else {
            disks.push_back(Disk{3.0, 3.0 + shift, 4.0, 0, 0.35, 1});
            disks.push_back(Disk{4.2, 3.0 + shift, 0.0, 0, 0.35, 1});
        }
        world_.assign(std::move(disks), w, h, e, gravity);
        gen_ = 0;
        drag_ = -1;
        publish();
    }

    void step() override {
        world_.restitution = knob("restitution");
        world_.step();
        ++gen_;
        publish();
    }

    bool poke(float nx, float ny) override {
        const double x = double(nx) * world_.width;
        const double y = (1.0 - double(ny)) * world_.height;
        const int tool = int(knob("tool") + 0.5f);
        if (tool == 1) {
            Disk d;
            d.x = x; d.y = y;
            d.vx = 0; d.vy = 0;
            d.radius = knob("radius");
            d.mass = knob("mass");
            if (d.x < d.radius || d.y < d.radius || d.x > world_.width - d.radius
                || d.y > world_.height - d.radius) return false;
            if (!world_.add_disk(d)) return false;
            publish();
            return true;
        }
        if (tool == 2) {
            if (!world_.remove_nearest(x, y)) return false;
            publish();
            return true;
        }
        // kick (0) or drag-tool stamp fallback: impulse toward click from centre,
        // or a default rightward kick when the stamp is dead-centre.
        int idx = world_.nearest_index(x, y);
        if (idx < 0 && !world_.disks.empty()) idx = 0;
        if (idx < 0) return false;
        auto& d = world_.disks[std::size_t(idx)];
        double kickx = (double(nx) - 0.5) * 2.0;
        double kicky = (0.5 - double(ny)) * 2.0;
        if (std::fabs(kickx) < 1e-6 && std::fabs(kicky) < 1e-6) kickx = 8.0;
        return world_.set_disk_velocity(idx, d.vx + kickx, d.vy + kicky) ? (publish(), true) : false;
    }

    bool drag_begin(float nx, float ny) override {
        const double x = double(nx) * world_.width;
        const double y = (1.0 - double(ny)) * world_.height;
        drag_ = world_.nearest_index(x, y);
        drag_x_ = x; drag_y_ = y;
        return drag_ >= 0;
    }

    void drag_move(float nx, float ny) override {
        if (drag_ < 0) return;
        const double x = double(nx) * world_.width;
        const double y = (1.0 - double(ny)) * world_.height;
        const int tool = int(knob("tool") + 0.5f);
        if (tool == 3) {
            world_.move_disk(drag_, x, y);
            world_.set_disk_velocity(drag_, 0, 0);
        } else {
            // Aim: leave the disk in place and set velocity along the drag.
            const double vx = (x - drag_x_) * 4.0;
            const double vy = (y - drag_y_) * 4.0;
            world_.set_disk_velocity(drag_, vx, vy);
        }
        publish();
    }

    void drag_end() override { drag_ = -1; }

    std::vector<Metric> metrics() const override {
        double px = 0;
        for (const auto& d : world_.disks) { px += d.mass * d.vx; }
        return {
            Metric{"kinetic energy", world_.energy(), 0.0, Metric::Neither},
            Metric{"horizontal momentum", px, 0.0, Metric::Neither},
            Metric{"contacts", double(world_.contacts), 0.0, Metric::Neither},
            Metric{"disks", double(world_.disks.size()), 0.0, Metric::Neither},
        };
    }

    [[nodiscard]] const CollisionWorld& world() const { return world_; }
    CollisionWorld& world() { return world_; }

    // Live scene for project save/reopen (stored in the document body field).
    std::string saveScene() const {
        std::ostringstream out;
        out.precision(17);
        out << "collision2d-scene 1\n"
            << "width " << world_.width << "\n"
            << "height " << world_.height << "\n"
            << "restitution " << world_.restitution << "\n"
            << "gravity " << world_.gravity << "\n"
            << "generation " << gen_ << "\n"
            << "disks " << world_.disks.size() << "\n";
        for (const auto& d : world_.disks)
            out << d.x << ' ' << d.y << ' ' << d.vx << ' ' << d.vy << ' '
                << d.radius << ' ' << d.mass << '\n';
        out << "end\n";
        return out.str();
    }

    bool loadScene(const std::string& data) {
        std::istringstream in(data);
        std::string tag; int version = 0;
        if (!(in >> tag >> version) || tag != "collision2d-scene" || version != 1) return false;
        double w = 0, h = 0, e = 0, g = 0;
        std::uint64_t generation = 0;
        std::size_t count = 0;
        std::string key;
        while (in >> key) {
            if (key == "end") break;
            if (key == "width") { if (!(in >> w)) return false; }
            else if (key == "height") { if (!(in >> h)) return false; }
            else if (key == "restitution") { if (!(in >> e)) return false; }
            else if (key == "gravity") { if (!(in >> g)) return false; }
            else if (key == "generation") { if (!(in >> generation)) return false; }
            else if (key == "disks") {
                if (!(in >> count) || count == 0 || count > std::size_t(CollisionWorld::kMaxDisks))
                    return false;
                std::vector<Disk> disks(count);
                for (std::size_t i = 0; i < count; ++i) {
                    if (!(in >> disks[i].x >> disks[i].y >> disks[i].vx >> disks[i].vy
                              >> disks[i].radius >> disks[i].mass)) return false;
                }
                if (!world_.assign(std::move(disks), w, h, e, g)) return false;
                for (auto& k : knobs_) if (k.key == "restitution") k.value = float(e);
                gen_ = generation;
                publish();
                return true;
            } else return false;
        }
        return false;
    }

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
    int drag_ = -1;
    double drag_x_ = 0, drag_y_ = 0;
};

inline SimPtr make_collision() { return std::make_unique<CollisionLab>(); }

} // namespace bench
