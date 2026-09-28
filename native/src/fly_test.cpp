// Fly actor contract + registered Fly Arena host.
// No Brian2 runtime and no FlyWire data are required for these checks.
#include "actors/fly_world.hpp"
#include "sims/fly_arena.hpp"
#include <cmath>
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

    check(Identity::contract_version == Observation::version
          && std::string(Identity::backend) == "reactive-stub"
          && Identity::dataset_hash[0] == '\0',
          "Identity names the synthetic backend and leaves dataset_hash empty");

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

    // Same controller instance reused across two host scenes (reusable-actor gate).
    ReactiveController shared;
    shared.reset(2);
    World food(Scene::Food);
    food.reset(Config{});
    World rocks(Scene::Obstacles);
    Config oc; oc.seed = 2; oc.obstacles = 8;
    rocks.reset(oc);
    bool both = true;
    for (int i = 0; i < 120; ++i) {
        if (!food.advance(shared.act(food.observe()))) both = false;
        if (!rocks.advance(shared.act(rocks.observe()))) both = false;
    }
    check(both && food.travelled > 0 && rocks.travelled > 0
          && std::string(shared.backend()) == Identity::backend,
          "one ReactiveController drives Food and Obstacles hosts without invalid actions");

    // Light and wind scenes construct and step.
    for (Scene sc : {Scene::Light, Scene::Wind}) {
        World w(sc);
        Config c; c.seed = 3;
        if (sc == Scene::Light) c.seek_light = true;
        w.reset(c);
        ReactiveController ctrl;
        ctrl.reset(3);
        if (sc == Scene::Light) { ctrl.odor_gain = 0; ctrl.light_gain = 1; }
        bool ok = true;
        for (int i = 0; i < 90; ++i)
            if (!w.advance(ctrl.act(w.observe()))) ok = false;
        check(ok && w.travelled > 0,
              std::string(sc == Scene::Light ? "Light" : "Wind")
              + " scene steps with the reactive controller");
    }

    // Stale tick is rejected.
    {
        World w(Scene::Food);
        w.reset(Config{});
        ReactiveController ctrl; ctrl.reset(1);
        Action a = ctrl.act(w.observe());
        a.tick = w.tick + 9;
        const auto t0 = w.tick;
        check(!w.advance(a) && w.tick == t0, "a stale action tick is rejected");
    }

    // Registered FlyArena sim: construct, step, save/reopen scene.
    {
        auto sim = bench::make_fly_arena();
        const auto g0 = sim->generation();
        sim->step();
        check(sim->generation() > g0, "FlyArena advances generation on step");
        check(sim->field().w == bench::FlyArena::kViewW
              && sim->field().h == bench::FlyArena::kViewH
              && sim->palette().size() >= 6,
              "FlyArena publishes a painted field and legend");
        auto* arena = static_cast<bench::FlyArena*>(sim.get());
        for (int i = 0; i < 40; ++i) sim->step();
        const auto travelled = arena->world().travelled;
        const auto tick_saved = arena->world().tick;
        const auto x_saved = arena->world().body.position.x;
        const std::string after = arena->saveScene();
        auto sim2 = bench::make_fly_arena();
        auto* a2 = static_cast<bench::FlyArena*>(sim2.get());
        check(a2->loadScene(after)
              && a2->world().tick == tick_saved
              && std::fabs(a2->world().travelled - travelled) < 1e-9
              && a2->world().body.position.x == x_saved,
              "FlyArena save/load restores tick, travel and body position");
        check(a2->poke(0.8f, 0.5f)
              && std::fabs(a2->world().config.source_x - 80.0) < 1.0,
              "FlyArena poke relocates the synthetic source");
    }

    // Food plume: odor seeking closes on the source; odor-blind does not as tightly.
    {
        World seek(Scene::Food);
        seek.reset(Config{});
        ReactiveController on; on.reset(1);
        const double d0 = distance(seek.body.position, seek.target());
        for (int i = 0; i < 3600 && !seek.reached; ++i)
            seek.advance(on.act(seek.observe()));
        World blind(Scene::Food);
        blind.reset(Config{});
        ReactiveController off; off.reset(1); off.odor_gain = 0;
        for (int i = 0; i < 3600; ++i)
            blind.advance(off.act(blind.observe()));
        const double d_seek = distance(seek.body.position, seek.target());
        const double d_blind = distance(blind.body.position, blind.target());
        check(seek.reached && seek.first_arrival > 0 && d_seek < 5.0 && d_seek < d0 * 0.2,
              "food plume odor-seeking walk reaches the source (within 5 mm)");
        check(d_seek + 8.0 < d_blind,
              "odor seeking ends closer to the source than the same walk with odor_gain 0");
    }

    std::printf("%d fly checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}