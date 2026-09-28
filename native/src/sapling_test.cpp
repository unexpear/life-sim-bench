// Wood comes back. A town that only ever cuts the trunks it was given runs out.
#include "sims/voxelcity.hpp"
#include <cstdio>
#include <memory>

namespace {

void clear_band(bench::VoxelCity& city, int x0, int x1, int z0, int z1, int y0, int y1) {
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y) {
                const auto b = city.world().at(x, y, z);
                if (b == bench::Wood || b == bench::Leaves || b == bench::Sapling
                    || b == bench::SaplingAged || (y > y0 && y <= y1))
                    city.edit_block(x, y, z, std::uint8_t(bench::Air));
            }
}

std::unique_ptr<bench::VoxelCity> town() {
    auto s = std::make_unique<bench::VoxelCity>();
    s->on_knob("size", 0.f);
    s->on_knob("agents", 1.f);
    s->on_knob("seed", 3.f);
    s->on_knob("policy", 1.f);
    return s;
}

} // namespace

int main() {
    using namespace bench;
    int checks = 0, failed = 0;
    auto check = [&](bool ok, const std::string& text) {
        ++checks;
        if (!ok) { ++failed; std::printf("FAIL %s\n", text.c_str()); }
        else std::printf("ok   %s\n", text.c_str());
    };

    auto s = town();
    const int H = s->world().height();
    clear_band(*s, 0, 50, 0, 24, 16, 40);
    for (int z = 1; z <= 12; ++z)
        for (int x = 1; x <= 40; ++x)
            s->edit_block(x, 20, z, std::uint8_t(Grass));

    int decayed = 0, saplings = 0;
    for (int i = 0; i < 400; ++i) {
        const int x = 1 + (i % 40);
        const int z = 1 + (i / 40);
        s->edit_block(x, 22, z, std::uint8_t(Leaves));
        s->random_tick_for_test(x, 22, z);
        if (s->world().at(x, 22, z) == Air) ++decayed;
        if (s->world().at(x, 21, z) == Sapling) ++saplings;
    }
    check(decayed == 400, "a leaf with no log within four blocks rots on its random tick ("
          + std::to_string(decayed) + " of 400)");
    check(saplings >= 5 && saplings <= 45,
          "oak leaves plant a sapling about one time in twenty (" + std::to_string(saplings)
          + " of 400; the published chance is 0.05)");

    s->edit_block(8, 22, 12, std::uint8_t(Leaves));
    s->edit_block(9, 22, 12, std::uint8_t(Wood));
    s->random_tick_for_test(8, 22, 12);
    check(s->world().at(8, 22, 12) == Leaves, "a leaf beside a log does not rot");

    int gx = -1, gz = -1, gy = -1;
    for (int z = 4; z < 40 && gx < 0; ++z)
        for (int x = 4; x < 40 && gx < 0; ++x) {
            const int y = s->world().surface(x, z);
            if (y > 4 && y + 16 < H && s->world().at(x, y, z) == Grass) { gx = x; gz = z; gy = y; }
        }
    check(gx >= 0, "the town has a grass column tall enough for a tree");
    for (int y = gy + 1; y < gy + 16 && y < H; ++y)
        for (int dz = -4; dz <= 4; ++dz)
            for (int dx = -4; dx <= 4; ++dx)
                s->edit_block(gx + dx, y, gz + dz, std::uint8_t(Air));
    s->edit_block(gx, gy + 1, gz, std::uint8_t(Sapling));
    const int light = s->light().internal_light(gx, gy + 2, gz, s->sky_darken_now());
    check(light >= 9, "the cell above a surface sapling is bright enough to grow (light "
          + std::to_string(light) + ")");
    int grewAt = -1;
    for (int t = 1; t <= 500 && grewAt < 0; ++t) {
        s->random_tick_for_test(gx, gy + 1, gz);
        bool trunk = false;
        for (int y = gy + 1; y < gy + 12 && y < H; ++y)
            if (s->world().at(gx, y, gz) == Wood) trunk = true;
        if (trunk) grewAt = t;
    }
    check(grewAt >= 2 && grewAt <= 500,
          "daylight grows a sapling into wood, and not on the first tick (tick "
          + std::to_string(grewAt) + "; two stage rolls of 1/7)");

    int wood = 0;
    for (int y = 0; y < H; ++y) if (s->world().at(gx, y, gz) == Wood) ++wood;
    for (int y = gy + 1; y < H; ++y)
        for (int dz = -3; dz <= 3; ++dz)
            for (int dx = -3; dx <= 3; ++dx)
                if (s->world().at(gx + dx, y, gz + dz) == Wood)
                    s->edit_block(gx + dx, y, gz + dz, std::uint8_t(Air));
    int replanted = 0;
    for (int i = 0; i < 80 && replanted == 0; ++i) {
        s->edit_block(gx, gy + 3, gz, std::uint8_t(Leaves));
        s->random_tick_for_test(gx, gy + 3, gz);
        if (s->world().at(gx, gy + 1, gz) == Sapling) replanted = 1;
    }
    check(replanted == 1, "after the trunk is gone, rotting leaves plant another sapling");
    int cameBack = -1;
    for (int t = 1; t <= 500 && cameBack < 0; ++t) {
        if (s->world().at(gx, gy + 1, gz) != Sapling && s->world().at(gx, gy + 1, gz) != SaplingAged)
            break;
        s->random_tick_for_test(gx, gy + 1, gz);
        for (int y = gy + 1; y < gy + 12 && y < H; ++y)
            if (s->world().at(gx, y, gz) == Wood) cameBack = t;
    }
    check(cameBack > 0, "and that sapling grows a new trunk, so the column has wood again");
    (void)wood;

    auto dark = town();
    const int dx = 12, dy = 8, dz = 12;
    for (int y = dy - 1; y <= dy + 3; ++y)
        for (int z = dz - 1; z <= dz + 1; ++z)
            for (int x = dx - 1; x <= dx + 1; ++x)
                dark->edit_block(x, y, z, std::uint8_t(Stone));
    dark->edit_block(dx, dy, dz, std::uint8_t(Air));
    dark->edit_block(dx, dy - 1, dz, std::uint8_t(Dirt));
    dark->edit_block(dx, dy, dz, std::uint8_t(Sapling));
    const int gloom = dark->light().internal_light(dx, dy + 1, dz, dark->sky_darken_now());
    for (int t = 0; t < 80; ++t) dark->random_tick_for_test(dx, dy, dz);
    check(gloom < 9 && dark->world().at(dx, dy, dz) == Sapling,
          "a sapling under stone, below light 9, does not advance (" + std::to_string(gloom) + ")");

    std::printf("%d sapling checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
