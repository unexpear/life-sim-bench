#include "registry.hpp"
#include "sims/satisfy.hpp"
#include "sims/satisfy_more.hpp"
#include "sims/sand.hpp"
#include "sims/machines.hpp"
#include "render/png.hpp"
#include "workflows.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {

int g_checks = 0, g_failed = 0;

void check(bool ok, const std::string& name) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL %s\n", name.c_str());
    }
}

void save(const bench::SatisfySim& sim, const std::filesystem::path& path) {
    const auto* s = sim.surface();
    check(s && bench::write_png(path.string().c_str(), s->w, s->h, s->rgba.data()),
          "write " + path.filename().string());
}

} // namespace

int main(int argc, char** argv) {
    using bench::SatisfySim;
    {
        SatisfySim sim(SatisfySim::Scene::Grow);
        const float r0 = sim.ballRadius();
        check(sim.poke(0.5f, 0.5f), "grow accepts a kick");
        for (int i = 0; i < 80; ++i) sim.step();
        check(sim.bounces() >= 1, "grow ball bounces");
        check(sim.ballRadius() > r0 + 1.f, "grow ball grows on a bounce");
        check(sim.progress() > 0.f && sim.progress() <= 1.f, "grow progress stays in range");
    }
    {
        SatisfySim sim(SatisfySim::Scene::Blocks);
        check(sim.blockCount() > 20, "blocks build the rings");
        for (int i = 0; i < 70; ++i) sim.step();
        check(sim.fallen() >= 8, "striker knocks blocks loose");
        check(sim.fallen() < sim.blockCount(), "the center is still there early on");
    }
    {
        SatisfySim sim(SatisfySim::Scene::Bowl);
        const float r0 = sim.bowlRadius();
        for (int i = 0; i < 36; ++i) sim.step();
        check(sim.pieceCount() >= 8, "bowl receives capsules");
        check(sim.bowlRadius() < r0 - 4.f, "bowl radius shrinks as it fills");
    }
    {
        SatisfySim sim(SatisfySim::Scene::Fill);
        sim.step();
        const float c0 = sim.coverage();
        for (int i = 0; i < 40; ++i) sim.step();
        check(sim.bounces() >= 1, "fill ball bounces");
        check(sim.coverage() > c0, "trails cover more of the disc");
        check(sim.coverage() < 1.f, "the disc is not already full");
    }
    {
        SatisfySim sim(SatisfySim::Scene::Spiral);
        const float r0 = sim.spiralRadius();
        const float w0 = sim.spiralSpeed();
        for (int i = 0; i < 40; ++i) sim.step();
        check(sim.spiralRadius() < r0 - 10.f, "spiral moves inward");
        check(sim.spiralSpeed() > w0, "spiral speeds up");
    }
    {
        SatisfySim sim(SatisfySim::Scene::Water);
        for (int i = 0; i < 80; ++i) sim.step();
        check(sim.waterBalls() >= 2, "falls add water balls");
    }
    {
        bench::SatisfyMore sim(bench::SatisfyMore::Scene::Columns);
        for (int i = 0; i < 90; ++i) sim.step();
        const int caught = sim.columnBalls();
        check(caught >= 5, "columns catch fallen balls (" + std::to_string(caught) + ")");
    }
    {
        bench::SatisfyMore sim(bench::SatisfyMore::Scene::Sides);
        for (int i = 0; i < 40; ++i) sim.step();
        check(sim.bounces() >= 1, "polygon ball bounces");
        check(sim.sides() > 3, "a bounce adds a side");
        check(sim.sides() <= 48, "sides stop at the circle");
    }
    {
        bench::SatisfyMore sim(bench::SatisfyMore::Scene::Square);
        const float h0 = sim.squareHalf();
        for (int i = 0; i < 40; ++i) sim.step();
        check(sim.bounces() >= 1, "the square bounces");
        check(sim.squareHalf() > h0, "the square grows on a bounce");
    }
    {
        bench::SatisfyMore sim(bench::SatisfyMore::Scene::Claim);
        for (int i = 0; i < 40; ++i) sim.step();
        check(sim.claims() >= 4, "movers claim dots they touch");
    }
    {
        bench::SatisfyMore sim(bench::SatisfyMore::Scene::Glass);
        const float s0 = sim.glassSpeed();
        for (int i = 0; i < 24; ++i) sim.step();
        check(sim.shattered() >= 2, "the ball shatters tiles");
        check(sim.glassSpeed() > s0, "shattering speeds the ball up");
    }
    {
        bench::SatisfyMore sim(bench::SatisfyMore::Scene::Beat);
        for (int i = 0; i < 20; ++i) sim.step();
        check(sim.bounces() >= 2, "the ball lands on more than one note");
    }
    {
        bench::SatisfyMore sim(bench::SatisfyMore::Scene::Shrink);
        const float m0 = sim.wallMargin();
        for (int i = 0; i < 40; ++i) sim.step();
        check(sim.bounces() >= 1, "the closing box bounces");
        check(sim.wallMargin() > m0, "a bounce moves the walls in");
    }
    {
        bench::SatisfyMore sim(bench::SatisfyMore::Scene::Kaleido);
        for (int i = 0; i < 30; ++i) sim.step();
        check(sim.bounces() >= 1, "the mirrored ball bounces");
    }
    {
        bench::SatisfyMore sim(bench::SatisfyMore::Scene::Escape);
        for (int i = 0; i < 80; ++i) sim.step();
        check(sim.ballCount() >= 4, "an escape spawns three more balls");
    }
    {
        bench::SandSim sim(bench::SandSim::Scene::Pour);
        for (int i = 0; i < 25; ++i) sim.step();
        check(sim.sandCount() > 40, "the spout releases sand");
    }
    {
        bench::SandSim sim(bench::SandSim::Scene::Hourglass);
        const int before = sim.lowerSand();
        for (int i = 0; i < 40; ++i) sim.step();
        check(sim.lowerSand() > before, "sand drains into the lower bulb");
    }
    {
        bench::SandSim sim(bench::SandSim::Scene::Ball);
        const float y0 = sim.meanY();
        const int links0 = sim.linksAlive();
        check(links0 > 10, "the sand ball starts linked");
        for (int i = 0; i < 20; ++i) sim.step();
        check(sim.meanY() > y0 + 2.f, "the sand ball falls");
    }
    {
        bench::MachineSim sim(bench::MachineSim::Scene::Shredder);
        for (int i = 0; i < 70; ++i) sim.step();
        check(sim.sandCount() > 0, "the shredder turns a block into sand");
    }
    {
        bench::MachineSim sim(bench::MachineSim::Scene::Press);
        for (int i = 0; i < 50; ++i) sim.step();
        check(sim.sandCount() > 0, "the press crushes sand out of the block");
    }
    {
        bench::MachineSim sim(bench::MachineSim::Scene::Rollers);
        for (int i = 0; i < 80; ++i) sim.step();
        check(sim.sandCount() > 0, "the rollers grind out sand");
    }
    {
        const char* ids[] = {
            "satisfy-grow", "satisfy-blocks", "satisfy-bowl", "satisfy-fill",
            "satisfy-spiral", "satisfy-water", "satisfy-columns", "satisfy-sides",
            "satisfy-square", "satisfy-claim", "satisfy-glass", "satisfy-beat",
            "satisfy-shrink", "satisfy-kaleido", "satisfy-escape",
            "sand-pour", "sand-hourglass", "sand-ball",
            "machine-shred", "machine-press", "machine-rollers"};
        for (const char* id : ids) {
            const bench::Entry* found = nullptr;
            for (const auto& e : bench::registry()) if (e.id == id) found = &e;
            check(found != nullptr, std::string(id) + " is registered");
            if (!found) continue;
            auto sim = found->make();
            check(bench::catalog_title(id) == sim->about().title, std::string(id) + " title matches");
            const auto before = sim->surface()->rgba;
            check(sim->poke(0.5f, 0.42f), std::string(id) + " poke");
            sim->step();
            auto fresh = found->make();
            fresh->step();
            check(sim->surface()->rgba != fresh->surface()->rgba, std::string(id) + " poke changes the picture");
            check(before != sim->surface()->rgba || sim->field().cells != fresh->field().cells,
                  std::string(id) + " is not a blank frame");
        }
    }

    {
        const char* ids[] = {"sand-pour", "sand-hourglass", "sand-ball",
                             "machine-shred", "machine-press", "machine-rollers"};
        auto mixRun = [](const bench::Entry& e, const bench::Knob& spec, float v) {
            auto s = e.make();
            bool reset = false;
            for (const auto& k : s->knobs()) if (k.key == spec.key) reset = k.on_reset;
            s->on_knob(spec.key, v);
            if (reset) s->reset();
            for (int i = 0; i < 24; ++i) s->step();
            std::uint64_t h = 1469598103934665603ull;
            auto mix = [&](std::uint8_t b) { h ^= b; h *= 1099511628211ull; };
            for (auto c : s->field().cells) mix(c);
            if (const auto* sf = s->surface()) for (auto b : sf->rgba) mix(b);
            return h;
        };
        for (const char* id : ids) {
            const bench::Entry* e = nullptr;
            for (const auto& row : bench::registry()) if (row.id == id) e = &row;
            if (!e) continue;
            const auto knobs = e->make()->knobs();
            for (const auto& k : knobs) {
                if (k.display_only) continue;
                const auto lo = mixRun(*e, k, k.quantised(k.min));
                const auto hi = mixRun(*e, k, k.quantised(k.max));
                check(lo != hi, std::string(id) + " knob '" + k.key + "' changes the outcome");
            }
        }
    }

    if (argc > 1) {
        const std::filesystem::path dir(argv[1]);
        std::filesystem::create_directories(dir);
        const SatisfySim::Scene scenes[] = {
            SatisfySim::Scene::Grow, SatisfySim::Scene::Blocks, SatisfySim::Scene::Bowl,
            SatisfySim::Scene::Fill, SatisfySim::Scene::Spiral, SatisfySim::Scene::Water};
        const char* names[] = {"grow", "blocks", "bowl", "fill", "spiral", "water"};
        const int frames[] = {50, 90, 40, 48, 70, 80};
        for (int i = 0; i < 6; ++i) {
            SatisfySim sim(scenes[i]);
            for (int s = 0; s < frames[i]; ++s) sim.step();
            save(sim, dir / (std::string(names[i]) + ".png"));
        }
        const bench::SatisfyMore::Scene more[] = {
            bench::SatisfyMore::Scene::Columns, bench::SatisfyMore::Scene::Sides,
            bench::SatisfyMore::Scene::Square, bench::SatisfyMore::Scene::Claim,
            bench::SatisfyMore::Scene::Glass, bench::SatisfyMore::Scene::Beat,
            bench::SatisfyMore::Scene::Shrink, bench::SatisfyMore::Scene::Kaleido,
            bench::SatisfyMore::Scene::Escape};
        const char* moreNames[] = {"columns", "sides", "square", "claim", "glass",
                                   "beat", "shrink", "kaleido", "escape"};
        const int moreFrames[] = {70, 28, 48, 50, 28, 36, 36, 90, 140};
        for (int i = 0; i < 9; ++i) {
            bench::SatisfyMore sim(more[i]);
            for (int s = 0; s < moreFrames[i]; ++s) sim.step();
            const auto* shot = sim.surface();
            check(shot && bench::write_png((dir / (std::string(moreNames[i]) + ".png")).string().c_str(),
                                            shot->w, shot->h, shot->rgba.data()),
                  std::string("write ") + moreNames[i]);
        }
        const bench::SandSim::Scene sandScenes[] = {
            bench::SandSim::Scene::Pour, bench::SandSim::Scene::Hourglass, bench::SandSim::Scene::Ball};
        const char* sandNames[] = {"sand-pour", "sand-hourglass", "sand-ball"};
        const int sandFrames[] = {55, 60, 6};
        for (int i = 0; i < 3; ++i) {
            bench::SandSim sim(sandScenes[i]);
            for (int s = 0; s < sandFrames[i]; ++s) sim.step();
            const auto* shot = sim.surface();
            check(shot && bench::write_png((dir / (std::string(sandNames[i]) + ".png")).string().c_str(),
                                            shot->w, shot->h, shot->rgba.data()),
                  std::string("write ") + sandNames[i]);
        }
        const bench::MachineSim::Scene machineScenes[] = {
            bench::MachineSim::Scene::Shredder, bench::MachineSim::Scene::Press,
            bench::MachineSim::Scene::Rollers};
        const char* machineNames[] = {"shredder", "press", "rollers"};
        const int machineFrames[] = {55, 40, 70};
        for (int i = 0; i < 3; ++i) {
            bench::MachineSim sim(machineScenes[i]);
            for (int s = 0; s < machineFrames[i]; ++s) sim.step();
            const auto* shot = sim.surface();
            check(shot && bench::write_png((dir / (std::string(machineNames[i]) + ".png")).string().c_str(),
                                            shot->w, shot->h, shot->rgba.data()),
                  std::string("write ") + machineNames[i]);
        }
    }

    std::printf("%d satisfy checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
