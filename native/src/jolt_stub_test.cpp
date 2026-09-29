// Jolt / FlyGym groundwork checks — analytic SphereWorld + embodiment boundary.
#include "physics/jolt_body_notes.hpp"
#include <cmath>
#include <cstdio>
#include <string>

int main() {
    using namespace bench::physics;
    int checks = 0, failed = 0;
    auto check = [&](bool ok, const char* text) {
        ++checks;
        if (!ok) { ++failed; std::printf("FAIL %s\n", text); }
        else std::printf("ok   %s\n", text);
    };

    check(!EmbodimentBoundary::jolt_and_mujoco_share_contacts,
          "MuJoCo fly body and Jolt lab do not share contacts");
    check(!EmbodimentBoundary::analytic_sphere_lab_is_flygym,
          "analytic sphere lab is not FlyGym");
    check(std::string(kJoltLabBody).find("sphere") != std::string::npos
          && std::string(kFlyGymBody).find("mujoco") != std::string::npos,
          "body labels name separate Particle Lab and FlyGym worlds");

    SphereWorld world;
    SphereBodyDesc a; a.radius = 0.5f; a.mass = 1.f; a.position = {0, 2, 0};
    SphereBodyDesc bad; bad.radius = 0.f;
    check(world.add_sphere(a) && !world.add_sphere(bad) && world.count() == 1,
          "sphere world accepts a valid body and rejects zero radius");

    // Drop onto floor: should contact and bounce (restitution > 0).
    bool hit_floor = false;
    for (int i = 0; i < 180; ++i) {
        world.step();
        for (const auto& c : world.contacts())
            if (c.b == ContactEvent::kFloor) hit_floor = true;
    }
    check(hit_floor && world.tick() == 180,
          "falling sphere contacts the floor within three seconds");
    check(world.bodies()[0].position.y >= SphereWorld::kFloorY + a.radius - 1e-3f,
          "sphere rests at or above the floor plane");

    // Two spheres collide.
    SphereWorld pair;
    SphereBodyDesc left; left.position = {-1.2f, 1.f, 0}; left.velocity = {2.f, 0, 0};
    left.radius = 0.5f; left.mass = 1.f; left.restitution = 0.9f;
    SphereBodyDesc right; right.position = {1.2f, 1.f, 0}; right.velocity = {-2.f, 0, 0};
    right.radius = 0.5f; right.mass = 1.f; right.restitution = 0.9f;
    check(pair.add_sphere(left) && pair.add_sphere(right), "pair scene accepts two spheres");
    bool hit_pair = false;
    for (int i = 0; i < 60; ++i) {
        pair.step();
        for (const auto& c : pair.contacts())
            if (c.b != ContactEvent::kFloor) hit_pair = true;
    }
    check(hit_pair, "closing spheres generate a sphere-sphere contact");
    check(std::string(SphereWorld::backend()).find("analytic") != std::string::npos,
          "default backend names the analytic sphere lab");
#ifdef BENCH_HAS_JOLT
    check(has_upstream_jolt(), "BENCH_HAS_JOLT reports upstream pin linked");
    std::printf("note  upstream Jolt pin is linked in this build\n");
#else
    check(!has_upstream_jolt(), "default build has no upstream Jolt link");
    std::printf("note  BENCH_WITH_JOLT off — analytic SphereWorld only (pin may still be fetched)\n");
#endif

    std::printf("%d jolt-lab checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
