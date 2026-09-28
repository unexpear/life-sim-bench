// The collision lab's oracle, and the disk world that is supposed to follow it.
#include "sims/collision.hpp"
#include "projects.hpp"
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
    check(std::fabs(box.disks[0].vx) < 1e-6 && std::fabs(box.disks[1].vx - 4) < 1e-6,
          "after the impact the disks hold the oracle velocities");
    check(std::fabs(box.disks[0].vy) < 1e-9 && std::fabs(box.disks[1].vy) < 1e-9,
          "a head-on impact does not invent a sideways velocity");
    check(std::fabs(box.energy() - e0) < 1e-6, "elastic energy stays put when nothing else acts");

    bench::CollisionWorld glance;
    check(glance.assign({{3.0, 3.0, 3.0, 0.5, 0.35, 1}, {4.3, 3.45, 0, 0.5, 0.35, 2}}, 10, 6, 1, 0),
          "a glancing pair is accepted");
    const double py0 = glance.disks[0].mass * glance.disks[0].vy + glance.disks[1].mass * glance.disks[1].vy;
    for (int i = 0; i < 40; ++i) glance.step();
    const double py1 = glance.disks[0].mass * glance.disks[0].vy + glance.disks[1].mass * glance.disks[1].vy;
    check(std::fabs(glance.disks[0].vx - 3.0) > 1e-3,
          "an offset impact changes the normal velocity");
    check(std::fabs(py1 - py0) < 1e-6, "no friction leaves the tangential momentum unchanged");

    bench::CollisionWorld wall;
    check(wall.assign({{8.8, 3.0, 3.0, 0, 0.35, 1}}, 10, 6, 1, 0), "a wall approach is accepted");
    bool reversed = false;
    for (int i = 0; i < 40 && !reversed; ++i) {
        wall.step();
        if (wall.disks[0].vx < 0) reversed = true;
    }
    check(reversed && std::fabs(std::fabs(wall.disks[0].vx) - 3) < 1e-6,
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
          "a speed the substep cannot resolve is refused and the scene is unchanged");

    bench::CollisionWorld rest;
    check(rest.assign({{4.0, 3.0, 0, 0, 0.4, 1}, {4.5, 3.0, 0, 0, 0.4, 1}}, 10, 6, 1, 0),
          "an overlapping pair at rest is accepted");
    rest.step();
    check(rest.contacts == 0, "separating an overlap that is not closing is not counted as a collision");

    auto sim = bench::make_collision();
    auto* lab = dynamic_cast<bench::CollisionLab*>(sim.get());
    check(lab && lab->world().disks.size() == 2, "the template opens on the equal-mass exchange");
    std::size_t painted = 0;
    for (auto c : lab->field().cells) if (c) ++painted;
    check(painted > 10, "the disks are on the field the workbench draws");
    namespace p = bench::projects;
    p::Document saved;
    saved.name = "Collision check";
    saved.model = "collision2d";
    p::capture_settings(*sim, saved);
    const auto path = std::filesystem::temp_directory_path() / "collision-reopen.benchsim";
    std::string error;
    check(p::save(path, saved, error), "the scene settings fit a version-1 project");
    p::Document back;
    auto again = bench::make_collision();
    check(p::read(path, back, error) && p::apply_settings(*again, back, error),
          "reopening applies those settings to a fresh lab");
    auto* lab2 = dynamic_cast<bench::CollisionLab*>(again.get());
    check(lab2 && lab2->world().disks.size() == lab->world().disks.size()
          && std::fabs(lab2->world().disks[0].vx - lab->world().disks[0].vx) < 1e-9,
          "the reopened lab starts from the same initial velocities, not from mid-run positions");
    std::filesystem::remove(path);

    std::printf("%d collision checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
