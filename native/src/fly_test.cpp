// The fly actor headers are research stubs. They are not a template.
#include "actors/fly_world.hpp"
#include <cstdio>
#include <string>

int main() {
    using namespace bench::fly;
    int checks = 0, failed = 0;
    auto check = [&](bool ok, const std::string& text) {
        ++checks;
        if (!ok) { ++failed; std::printf("FAIL %s\n", text.c_str()); }
        else std::printf("ok   %s\n", text.c_str());
    };

    World world(Scene::Food);
    world.reset(Config{});
    ReactiveController brain;
    brain.reset(1);
    const auto before = world.observe();
    Action bad = brain.act(before);
    bad.forward = 2;
    const auto tick = world.tick;
    const auto place = world.body.position;
    check(!world.advance(bad) && world.tick == tick && world.body.position.x == place.x
          && world.body.position.y == place.y,
          "an action outside 0..1 speed does not move the body or advance time");

    Action good = brain.act(world.observe());
    check(valid(good, world.tick) && world.advance(good) && world.tick == tick + 1,
          "the reactive controller's own action is accepted and advances one tick");

    bool inside = true;
    for (int i = 0; i < 300; ++i) {
        if (!world.advance(brain.act(world.observe()))) inside = false;
        const auto& p = world.body.position;
        if (p.x < Body::radius || p.y < Body::radius
            || p.x > World::width - Body::radius || p.y > World::height - Body::radius)
            inside = false;
    }
    check(inside && world.tick == tick + 301 && world.travelled > 0,
          "three hundred steps stay inside the arena and the body actually walks");

    std::printf("%d fly-stub checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
