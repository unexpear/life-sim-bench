// The collision lab's oracle, Box2D world, denser cases, editing and scene save.
#include "sims/collision.hpp"
#include "projects.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

int main() {
    int checks = 0, failed = 0;
    auto check = [&](bool ok, const char* text) {
        ++checks;
        if (!ok) { ++failed; std::printf("FAIL %s\n", text); }
        else std::printf("ok   %s\n", text);
    };

    double v1 = 0, v2 = 0;
    check(bench::elastic_1d(1, 1, 4, 0, 1, v1, v2) && std::fabs(v1) < 1e-12 && std::fabs(v2 - 4) < 1e-12,
          "equal masses exchange velocity");
    check(bench::elastic_1d(1, 3, 4, -1, 1, v1, v2), "unequal masses accept a finite impact");
    {
        const double m1 = 1, m2 = 3, u1 = 4, u2 = -1;
        const double want1 = ((m1 - m2) * u1 + 2 * m2 * u2) / (m1 + m2);
        const double want2 = (2 * m1 * u1 + (m2 - m1) * u2) / (m1 + m2);
        check(std::fabs(v1 - want1) < 1e-12 && std::fabs(v2 - want2) < 1e-12,
              "e = 1 matches the published one-dimensional oracle");
    }
    check(bench::elastic_1d(2, 2, 3, 1, 0, v1, v2) && std::fabs(v1 - 2) < 1e-12 && std::fabs(v2 - 2) < 1e-12,
          "e = 0 leaves the pair with the centre-of-mass velocity");
    double keep1 = 9, keep2 = 8;
    check(!bench::elastic_1d(0, 1, 1, 0, 1, keep1, keep2) && keep1 == 9 && keep2 == 8,
          "a non-positive mass is refused and does not write velocities");
    check(!bench::elastic_1d(1, 1, 1, 0, 1.5, keep1, keep2), "restitution above one is refused");

    bench::CollisionWorld box;
    check(box.assign({{3.0, 3.0, 4.0, 0, 0.35, 1}, {4.2, 3.0, 0, 0, 0.35, 1}}, 10, 6, 1, 0),
          "the equal-mass scene is accepted");
    const double e0 = box.energy();
    for (int i = 0; i < 30; ++i) box.step();
    // Box2D is approximate; accept a tight band around the oracle velocities.
    check(std::fabs(box.disks[0].vx) < 0.15 && std::fabs(box.disks[1].vx - 4) < 0.15,
          "after the impact the disks hold near-oracle velocities");
    check(std::fabs(box.disks[0].vy) < 0.05 && std::fabs(box.disks[1].vy) < 0.05,
          "a head-on impact does not invent a large sideways velocity");
    check(std::fabs(box.energy() - e0) / e0 < 0.05, "elastic energy stays within a few percent");

    bench::CollisionWorld glance;
    check(glance.assign({{3.0, 3.0, 3.0, 0.5, 0.35, 1}, {4.3, 3.45, 0, 0.5, 0.35, 2}}, 10, 6, 1, 0),
          "a glancing pair is accepted");
    const double py0 = glance.disks[0].mass * glance.disks[0].vy + glance.disks[1].mass * glance.disks[1].vy;
    for (int i = 0; i < 40; ++i) glance.step();
    const double py1 = glance.disks[0].mass * glance.disks[0].vy + glance.disks[1].mass * glance.disks[1].vy;
    check(std::fabs(glance.disks[0].vx - 3.0) > 1e-3,
          "an offset impact changes the normal velocity");
    check(std::fabs(py1 - py0) < 0.05, "no friction leaves tangential momentum nearly unchanged");

    bench::CollisionWorld wall;
    check(wall.assign({{8.8, 3.0, 3.0, 0, 0.35, 1}}, 10, 6, 1, 0), "a wall approach is accepted");
    bool reversed = false;
    for (int i = 0; i < 40 && !reversed; ++i) {
        wall.step();
        if (wall.disks[0].vx < 0) reversed = true;
    }
    check(reversed && std::fabs(std::fabs(wall.disks[0].vx) - 3) < 0.2,
          "an elastic wall reverses the normal speed and keeps its size");

    bench::CollisionWorld pile;
    check(pile.assign({{3, 4.5, 0.1, 0, 0.3, 1}, {4, 4.2, -0.1, 0, 0.3, 1},
                       {3.4, 2.2, 0, 0, 0.3, 1}, {4.2, 1.6, 0, 0, 0.3, 1}}, 10, 6, 0.2, 9.8),
          "a small pile under gravity is accepted");
    bool finite = true;
    for (int i = 0; i < 180; ++i) {
        pile.step();
        for (const auto& d : pile.disks)
            if (!std::isfinite(d.x + d.y + d.vx + d.vy)) finite = false;
    }
    check(finite, "a resting pile stays finite for three seconds");

    bench::CollisionWorld kept = box;
    const double keptX = kept.disks[0].x;
    check(!kept.assign({{1, 1, 100, 0, 0.35, 1}}, 10, 6, 1, 0) && kept.disks[0].x == keptX,
          "an unsupported speed is refused and the scene is unchanged");

    // Fast disk against a wall (bullet CCD path).
    bench::CollisionWorld fast;
    check(fast.assign({{2.0, 3.0, 25.0, 0, 0.25, 1}}, 10, 6, 1, 0), "a fast wall-bound disk is accepted");
    bool hit = false;
    for (int i = 0; i < 60 && !hit; ++i) {
        fast.step();
        if (fast.disks[0].vx < 0) hit = true;
    }
    check(hit, "a fast disk reverses after hitting the far wall");

    // Dense gas: timing and a memory counter from Box2D.
    std::vector<bench::Disk> gas;
    gas.reserve(120);
    for (int i = 0; i < 120; ++i) {
        bench::Disk d;
        d.radius = 0.16;
        d.mass = 0.35;
        d.x = 0.5 + (i % 12) * 0.7;
        d.y = 0.5 + (i / 12) * 0.7;
        d.vx = ((i * 37) % 11) * 0.15 - 0.75;
        d.vy = ((i * 19) % 11) * 0.15 - 0.75;
        gas.push_back(d);
    }
    bench::CollisionWorld dense;
    check(dense.assign(gas, 12, 10, 0.9, 0), "a 120-disk gas scene is accepted");
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 120; ++i) dense.step();
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const auto counters = dense.box2d_counters();
    std::printf("info dense 120 disks x 120 steps: %.1f ms, box2d bodies=%d bytes~=%d\n",
                ms, counters.bodyCount, counters.byteCount);
    check(ms < 15000.0, "dense gas stays within a generous timing budget");
    check(counters.bodyCount >= 120, "Box2D reports the dynamic bodies");
    bool denseFinite = true;
    for (const auto& d : dense.disks)
        if (!std::isfinite(d.x + d.y + d.vx + d.vy)) denseFinite = false;
    check(denseFinite, "dense gas positions stay finite");

    auto sim = bench::make_collision();
    auto* lab = dynamic_cast<bench::CollisionLab*>(sim.get());
    check(lab && lab->world().disks.size() == 2, "the template opens on the equal-mass exchange");
    std::size_t painted = 0;
    for (auto c : lab->field().cells) if (c) ++painted;
    check(painted > 10, "the disks are on the field the workbench draws");

    // Editing beyond stamp: place and erase.
    lab->on_knob("tool", 1.f);
    check(lab->poke(0.5f, 0.5f), "place tool adds a disk in the middle");
    const auto afterPlace = lab->world().disks.size();
    check(afterPlace == 3, "place increases the disk count");
    lab->on_knob("tool", 2.f);
    check(lab->poke(0.5f, 0.5f), "erase tool removes the nearest disk");
    check(lab->world().disks.size() == afterPlace - 1, "erase decreases the disk count");

    // Scene checkpoint stores more than knobs.
    lab->on_knob("tool", 0.f);
    lab->step(); lab->step();
    const auto scene = lab->saveScene();
    check(scene.find("collision2d-scene") != std::string::npos, "saveScene writes a tagged scene body");
    const double midX = lab->world().disks[0].x;
    auto againLab = bench::make_collision();
    auto* lab2 = dynamic_cast<bench::CollisionLab*>(againLab.get());
    check(lab2 && lab2->loadScene(scene), "loadScene restores a live scene");
    check(std::fabs(lab2->world().disks[0].x - midX) < 1e-6, "restored positions match the checkpoint");
    check(lab2->generation() == lab->generation(), "restored generation matches the checkpoint");

    namespace p = bench::projects;
    p::Document saved;
    saved.name = "Collision check";
    saved.model = "collision2d";
    p::capture_settings(*sim, saved);
    saved.body = lab->saveScene();
    const auto path = std::filesystem::temp_directory_path() / "collision-reopen.benchsim";
    std::string error;
    check(p::save(path, saved, error), "the scene settings fit a version-1 project");
    p::Document back;
    auto again = bench::make_collision();
    check(p::read(path, back, error) && p::apply_settings(*again, back, error),
          "reopening applies those settings to a fresh lab");
    auto* lab3 = dynamic_cast<bench::CollisionLab*>(again.get());
    check(lab3 && lab3->loadScene(back.body), "project body restores the collision scene");
    check(lab3 && lab3->world().disks.size() == lab->world().disks.size(),
          "the reopened lab restores disk count from the scene body");
    std::filesystem::remove(path);

    // Gas / dense presets exist on the lab.
    lab->on_knob("preset", 4.f);
    check(lab->world().disks.size() >= 40, "gas-box preset builds a denser scene");
    lab->on_knob("preset", 5.f);
    check(lab->world().disks.size() >= 40, "dense-pile preset builds a denser scene");

    std::printf("%d collision checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
