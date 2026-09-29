// SPDX-License-Identifier: GPL-3.0-or-later
// SphereWorld for Particle Lab 3D groundwork.
// Default path: local analytic sphere/floor contacts (not FlyGym, not MuJoCo).
// When BENCH_HAS_JOLT is defined (CMake BENCH_WITH_JOLT=ON + fetched pin), the
// same host API can later wrap JPH::PhysicsSystem — see jolt_body_notes.hpp.
// Names under bench::joltstub stay local so they are not mistaken for upstream
// jrouwe/JoltPhysics headers until the pin is linked.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace bench::joltstub {

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}
    Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 normalized() const {
        const float L = length();
        return L > 1e-8f ? Vec3{x / L, y / L, z / L} : Vec3{};
    }
};

struct SphereBodyDesc {
    Vec3 position{};
    Vec3 velocity{};
    float radius = 1.f;
    float mass = 1.f;
    float restitution = 0.2f;
};

struct ContactEvent {
    std::uint32_t a = 0, b = 0; // b == kFloor when sphere-floor
    Vec3 normal{};
    float penetration = 0;
    static constexpr std::uint32_t kFloor = 0xffffffffu;
};

// Host-owned world. Real Jolt uses JPH::PhysicsSystem; this implementation
// advances a fixed-step analytic sphere lab with pairwise + floor contacts.
// It does NOT share contacts with a MuJoCo/FlyGym fly body.
class SphereWorld {
public:
    static constexpr float kFixedDt = 1.f / 60.f;
    static constexpr int kSubsteps = 1;
    static constexpr std::uint32_t kMax = 4096;
    static constexpr float kFloorY = 0.f;
    static constexpr float kGravityY = -9.81f;

    void reset() {
        bodies_.clear();
        contacts_.clear();
        tick_ = 0;
        kinetic_ = 0;
    }

    bool add_sphere(const SphereBodyDesc& d) {
        if (bodies_.size() >= kMax || d.radius <= 0.f || d.mass <= 0.f
            || !std::isfinite(d.radius) || !std::isfinite(d.mass))
            return false;
        if (!std::isfinite(d.position.x) || !std::isfinite(d.position.y)
            || !std::isfinite(d.position.z))
            return false;
        bodies_.push_back(d);
        return true;
    }

    void step() {
        contacts_.clear();
        const float dt = kFixedDt;
        for (auto& b : bodies_) {
            b.velocity.y += kGravityY * dt;
            b.position = b.position + b.velocity * dt;
        }
        // Sphere–floor
        for (std::uint32_t i = 0; i < bodies_.size(); ++i) {
            auto& b = bodies_[i];
            const float pen = b.radius - (b.position.y - kFloorY);
            if (pen > 0.f) {
                b.position.y += pen;
                if (b.velocity.y < 0.f)
                    b.velocity.y = -b.velocity.y * b.restitution;
                contacts_.push_back({i, ContactEvent::kFloor, {0, 1, 0}, pen});
            }
        }
        // Sphere–sphere (sequential impulses, one pass — scaffolding honesty)
        for (std::uint32_t i = 0; i < bodies_.size(); ++i) {
            for (std::uint32_t j = i + 1; j < bodies_.size(); ++j) {
                auto& a = bodies_[i];
                auto& b = bodies_[j];
                Vec3 d = b.position - a.position;
                const float dist = d.length();
                const float min_dist = a.radius + b.radius;
                if (dist <= 1e-8f || dist >= min_dist) continue;
                const float pen = min_dist - dist;
                const Vec3 n = d.normalized();
                const float inv_m = 1.f / a.mass + 1.f / b.mass;
                a.position = a.position - n * (pen * (1.f / a.mass) / inv_m);
                b.position = b.position + n * (pen * (1.f / b.mass) / inv_m);
                const float closing = (b.velocity.x - a.velocity.x) * n.x
                    + (b.velocity.y - a.velocity.y) * n.y
                    + (b.velocity.z - a.velocity.z) * n.z;
                if (closing < 0.f) {
                    const float e = std::min(a.restitution, b.restitution);
                    const float jimp = -(1.f + e) * closing / inv_m;
                    a.velocity = a.velocity - n * (jimp / a.mass);
                    b.velocity = b.velocity + n * (jimp / b.mass);
                }
                contacts_.push_back({i, j, n, pen});
            }
        }
        kinetic_ = 0;
        for (const auto& b : bodies_) {
            const float sp2 = b.velocity.x * b.velocity.x
                + b.velocity.y * b.velocity.y
                + b.velocity.z * b.velocity.z;
            kinetic_ += 0.5f * b.mass * sp2;
        }
        ++tick_;
    }

    [[nodiscard]] std::uint32_t count() const {
        return static_cast<std::uint32_t>(bodies_.size());
    }
    [[nodiscard]] std::uint64_t tick() const { return tick_; }
    [[nodiscard]] float kinetic_energy() const { return kinetic_; }
    [[nodiscard]] std::uint32_t contact_count() const {
        return static_cast<std::uint32_t>(contacts_.size());
    }
    [[nodiscard]] const std::vector<SphereBodyDesc>& bodies() const { return bodies_; }
    [[nodiscard]] const std::vector<ContactEvent>& contacts() const { return contacts_; }
    [[nodiscard]] static constexpr const char* backend() { return "analytic-sphere-lab"; }

private:
    std::vector<SphereBodyDesc> bodies_;
    std::vector<ContactEvent> contacts_;
    std::uint64_t tick_ = 0;
    float kinetic_ = 0;
};

} // namespace bench::joltstub
