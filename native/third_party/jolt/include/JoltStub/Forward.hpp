// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal compileable placeholders for Particle Lab 3D groundwork.
// Not the real Jolt API — names are local so they cannot be mistaken for
// jrouwe/JoltPhysics headers until the pin is fetched and linked.
#pragma once
#include <cstdint>

namespace bench::joltstub {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct SphereBodyDesc {
    Vec3 position{};
    float radius = 1.f;
    float mass = 1.f;
    float restitution = 0.2f;
};

// Host-owned world handle. Real Jolt uses JPH::PhysicsSystem; this stub only
// tracks that a 3D sphere lab needs an isolated world, fixed step, and no
// silent sharing with a MuJoCo fly body.
class SphereWorld {
public:
    static constexpr float kFixedDt = 1.f / 60.f;
    static constexpr int kSubsteps = 1;

    void reset() { count_ = 0; tick_ = 0; }
    bool add_sphere(const SphereBodyDesc& d) {
        if (count_ >= kMax || d.radius <= 0.f || d.mass <= 0.f) return false;
        bodies_[count_++] = d;
        return true;
    }
    void step() { ++tick_; } // no contacts yet — scaffolding only
    [[nodiscard]] std::uint32_t count() const { return count_; }
    [[nodiscard]] std::uint64_t tick() const { return tick_; }

    static constexpr std::uint32_t kMax = 4096;

private:
    SphereBodyDesc bodies_[kMax]{};
    std::uint32_t count_ = 0;
    std::uint64_t tick_ = 0;
};

} // namespace bench::joltstub
