// SPDX-License-Identifier: GPL-3.0-or-later
// Body interface notes: Jolt Particle Lab 3D vs FlyGym/MuJoCo detailed fly.
#pragma once
#include "../../third_party/jolt/include/JoltStub/Forward.hpp"
#include <string_view>

namespace bench::physics {

// Particle Lab 3D (future): spheres and static obstacles in a Jolt world.
inline constexpr std::string_view kJoltLabBody = "sphere-3d-jolt-stub";

// Detailed fly body (research): FlyGym / NeuroMechFly owns the articulated
// fly and its contacting surroundings in MuJoCo. Do not drop that body into
// a Jolt world and expect correct shared contacts (RESEARCH-NEXT.md).
inline constexpr std::string_view kFlyGymBody = "flygym-mujoco-separate-world";

struct EmbodimentBoundary {
    static constexpr bool jolt_and_mujoco_share_contacts = false;
    static constexpr const char* note =
        "Use Jolt only for Particle Lab 3D spheres/obstacles. "
        "Use FlyGym/MuJoCo only inside its own world for a detailed fly. "
        "Cross-engine coupling needs separate force/contact/timing validation.";
};

using SphereWorld = bench::joltstub::SphereWorld;
using SphereBodyDesc = bench::joltstub::SphereBodyDesc;

} // namespace bench::physics
