// Jolt / FlyGym groundwork checks — compileable scaffolding only.
#include "physics/jolt_body_notes.hpp"
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
    check(std::string(kJoltLabBody).find("jolt") != std::string::npos
          && std::string(kFlyGymBody).find("mujoco") != std::string::npos,
          "body labels name separate Jolt and FlyGym worlds");

    SphereWorld world;
    SphereBodyDesc a; a.radius = 0.5f; a.mass = 1.f;
    SphereBodyDesc bad; bad.radius = 0.f;
    check(world.add_sphere(a) && !world.add_sphere(bad) && world.count() == 1,
          "sphere stub accepts a valid body and rejects zero radius");
    world.step();
    check(world.tick() == 1, "sphere stub advances a fixed-step clock");

    std::printf("%d jolt-stub checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
