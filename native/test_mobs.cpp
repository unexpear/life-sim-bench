// test_mobs.cpp — asserts what src/world/mobs.hpp publishes, and prints what it
// measured getting there.
//
// Build:
//   g++ -std=c++20 -O2 -Wall -Wextra -Isrc test_mobs.cpp -o test_mobs.exe
//       -static -static-libgcc -static-libstdc++
// Delete the exe before rebuilding. A stale binary has produced false results
// in this project more than once.
//
// Every number printed here is measured by the run that prints it. Where a
// section asserts a Minecraft constant, the constant's source is in mobs.hpp at
// the point of definition; this file only checks that the code agrees with it.

#include "world/mobs.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace bench;

// ── check harness ───────────────────────────────────────────────────────────
// Not <cassert>: assert() vanishes under -DNDEBUG, and a test that can be
// compiled into a no-op is not a test.
static int g_checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            std::printf("\nFAIL  %s:%d\n      %s\n", __FILE__, __LINE__, #cond);\
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        ++g_checks;                                                            \
        const double va = double(a), vb = double(b);                           \
        if (!(std::fabs(va - vb) <= double(tol))) {                            \
            std::printf("\nFAIL  %s:%d\n      %s = %.6f, expected %.6f +-%.6f\n",\
                        __FILE__, __LINE__, #a, va, vb, double(tol));          \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

static void head(const char* s) { std::printf("\n== %s ==\n", s); }

// ── worlds to test against ──────────────────────────────────────────────────

// Flat grass at groundY, stone below, bedrock at 0, air above. The heightmap is
// refreshed at the end because BlockWorld::set() does not maintain it and
// raw_sky_light() reads it.
static void make_flat(BlockWorld& w, int groundY) {
    for (int z = 0; z < w.n(); ++z)
        for (int x = 0; x < w.n(); ++x) {
            w.set(x, 0, z, Bedrock);
            for (int y = 1; y < groundY; ++y) w.set(x, y, z, Stone);
            w.set(x, groundY, z, Grass);
            for (int y = groundY + 1; y < w.height(); ++y) w.set(x, y, z, Air);
        }
    for (int z = 0; z < w.n(); ++z)
        for (int x = 0; x < w.n(); ++x) w.refresh_column(x, z);
}

// ── 1. the published mob table ──────────────────────────────────────────────

static void test_table() {
    head("mob table (minecraft.wiki, Java Edition)");
    std::printf("%-9s %-8s %6s %7s %6s %6s %6s %-6s %s\n",
                "mob", "category", "health", "attack", "speed", "xpLo", "xpHi",
                "burns", "drops");
    for (int k = 0; k < kMobKinds; ++k) {
        const MobType& t = mob_type(k);
        std::printf("%-9s %-8s %6.0f %7.1f %6.2f %6d %6d %-6s ",
                    t.name, t.cat == CatMonster ? "monster" : "creature",
                    double(t.health), double(t.attack), double(t.speed),
                    t.xpLo, t.xpHi, t.burnsInDaylight ? "yes" : "no");
        for (int i = 0; i < t.dropCount; ++i)
            std::printf("%s %d-%d%s ", mob_item_name(t.drops[std::size_t(i)].item),
                        int(t.drops[std::size_t(i)].lo), int(t.drops[std::size_t(i)].hi),
                        t.drops[std::size_t(i)].playerKillOnly ? "(player kill)" : "");
        std::printf("\n");
    }

    // Health.
    CHECK(mob_type(MobZombie).health   == 20.0f);
    CHECK(mob_type(MobSkeleton).health == 20.0f);
    CHECK(mob_type(MobCreeper).health  == 20.0f);
    CHECK(mob_type(MobSpider).health   == 16.0f);
    CHECK(mob_type(MobCow).health      == 10.0f);
    CHECK(mob_type(MobPig).health      == 10.0f);
    CHECK(mob_type(MobSheep).health    ==  8.0f);
    CHECK(mob_type(MobChicken).health  ==  4.0f);

    // movement_speed attribute.
    CHECK(mob_type(MobZombie).speed  == 0.23f);
    CHECK(mob_type(MobSpider).speed  == 0.30f);
    CHECK(mob_type(MobCow).speed     == 0.20f);
    CHECK(mob_type(MobPig).speed     == 0.25f);
    CHECK(mob_type(MobSheep).speed   == 0.23f);
    CHECK(mob_type(MobChicken).speed == 0.25f);
    // Skeleton and creeper are the two UNVERIFIED speeds — asserted so a change
    // is deliberate, not because the number is trusted.
    CHECK(mob_type(MobSkeleton).speed == 0.25f);
    CHECK(mob_type(MobCreeper).speed  == 0.25f);

    // XP: 5 for the four hostiles, 1-3 for the four animals.
    for (int k : {MobZombie, MobSkeleton, MobCreeper, MobSpider}) {
        CHECK(mob_type(k).xpLo == 5 && mob_type(k).xpHi == 5);
        CHECK(mob_type(k).cat == CatMonster);
        CHECK(mob_hostile(k));
    }
    for (int k : {MobCow, MobPig, MobSheep, MobChicken}) {
        CHECK(mob_type(k).xpLo == 1 && mob_type(k).xpHi == 3);
        CHECK(mob_type(k).cat == CatCreature);
        CHECK(!mob_hostile(k));
    }

    // Daylight burning: undead only.
    CHECK(mob_type(MobZombie).burnsInDaylight);
    CHECK(mob_type(MobSkeleton).burnsInDaylight);
    CHECK(!mob_type(MobCreeper).burnsInDaylight);
    CHECK(!mob_type(MobSpider).burnsInDaylight);
    for (int k : {MobCow, MobPig, MobSheep, MobChicken})
        CHECK(!mob_type(k).burnsInDaylight);

    // Follow range: zombie 35, the rest 16.
    CHECK(mob_type(MobZombie).followRange == 35.0f);
    CHECK(mob_type(MobSkeleton).followRange == 16.0f);
    std::printf("follow range: zombie %.0f, skeleton %.0f (creeper/spider %.0f "
                "unverified)\n", double(mob_type(MobZombie).followRange),
                double(mob_type(MobSkeleton).followRange),
                double(mob_type(MobCreeper).followRange));
}

// ── 2. the difficulty formula ───────────────────────────────────────────────

static void test_difficulty() {
    head("difficulty: min(D, 0.5D+1) / D / 1.5D");
    const float z = mob_type(MobZombie).attack;   // 3 on Normal
    const float s = mob_type(MobSpider).attack;   // 2 on Normal
    std::printf("zombie base %.1f -> easy %.2f normal %.2f hard %.2f\n",
                double(z), double(damage_to_player(z, DiffEasy)),
                double(damage_to_player(z, DiffNormal)),
                double(damage_to_player(z, DiffHard)));
    std::printf("spider base %.1f -> easy %.2f normal %.2f hard %.2f\n",
                double(s), double(damage_to_player(s, DiffEasy)),
                double(damage_to_player(s, DiffNormal)),
                double(damage_to_player(s, DiffHard)));

    // The published per-mob tables the formula has to reproduce.
    CHECK_NEAR(damage_to_player(z, DiffEasy),   2.5, 1e-6);
    CHECK_NEAR(damage_to_player(z, DiffNormal), 3.0, 1e-6);
    CHECK_NEAR(damage_to_player(z, DiffHard),   4.5, 1e-6);
    CHECK_NEAR(damage_to_player(s, DiffEasy),   2.0, 1e-6);
    CHECK_NEAR(damage_to_player(s, DiffNormal), 2.0, 1e-6);
    CHECK_NEAR(damage_to_player(s, DiffHard),   3.0, 1e-6);
    CHECK(damage_to_player(z, DiffPeaceful) == 0.0f);

    // Easy is a min, not a halving: below D = 2 the "+1" wins and Easy equals
    // Normal. That is why the spider takes the same 2 on both.
    CHECK_NEAR(damage_to_player(1.0f, DiffEasy), 1.0, 1e-6);
    CHECK_NEAR(damage_to_player(8.0f, DiffEasy), 5.0, 1e-6);
    std::printf("easy is a min, not a halving: D=1 -> %.2f, D=8 -> %.2f\n",
                double(damage_to_player(1.0f, DiffEasy)),
                double(damage_to_player(8.0f, DiffEasy)));
}

// ── 3. the explosion formula ────────────────────────────────────────────────

static void test_explosion() {
    head("creeper explosion: (impact^2+impact)/2 * 7 * 2*power + 1");
    const float maxDmg = explosion_damage(kCreeperPower, 0.0f, 1.0f);
    const float charged = explosion_damage(kChargedCreeperPower, 0.0f, 1.0f);
    std::printf("power %.0f, point blank, full exposure: %.0f\n",
                double(kCreeperPower), double(maxDmg));
    std::printf("charged (power %.0f): %.0f\n", double(kChargedCreeperPower),
                double(charged));
    std::printf("hard difficulty on the same blast: %.1f\n",
                double(damage_to_player(maxDmg, DiffHard)));
    CHECK(maxDmg == 43.0f);                        // the published maximum
    CHECK(charged == 85.0f);
    CHECK_NEAR(damage_to_player(maxDmg, DiffHard), 64.5, 1e-4);   // creeper article

    // Range is 2*power; at or beyond it the blast does nothing.
    CHECK(explosion_damage(kCreeperPower, 6.0f, 1.0f) == 0.0f);
    CHECK(explosion_damage(kCreeperPower, 5.99f, 1.0f) > 0.0f);
    CHECK(explosion_damage(kCreeperPower, 0.0f, 0.0f) == 0.0f);
    std::printf("damage vs distance (power 3, exposure 1): ");
    for (float d = 0.0f; d <= 6.0f; d += 1.0f)
        std::printf("%.0fm=%.0f ", double(d),
                    double(explosion_damage(kCreeperPower, d, 1.0f)));
    std::printf("\n");
    // Monotone in distance — a blast must never hurt more further away.
    float prev = 1e9f;
    for (int i = 0; i <= 60; ++i) {
        const float d = explosion_damage(kCreeperPower, float(i) * 0.1f, 1.0f);
        CHECK(d <= prev);
        prev = d;
    }
}

// ── 4. the mob cap ──────────────────────────────────────────────────────────

static void test_cap() {
    head("mob cap: base * spawnableChunks / 289");
    std::printf("17x17 chunks = %d\n", kMagicChunkCount);
    CHECK(kMagicChunkCount == 289);
    CHECK(mob_cap(CatMonster,  kMagicChunkCount) == 70);
    CHECK(mob_cap(CatCreature, kMagicChunkCount) == 10);
    std::printf("one player (289 chunks): monster %d, creature %d\n",
                mob_cap(CatMonster, 289), mob_cap(CatCreature, 289));
    std::printf("578 chunks: monster %d, creature %d\n",
                mob_cap(CatMonster, 578), mob_cap(CatCreature, 578));
    std::printf("300 chunks: monster %d, creature %d (integer division, as in "
                "the game)\n", mob_cap(CatMonster, 300), mob_cap(CatCreature, 300));
    CHECK(mob_cap(CatMonster, 578) == 140);
    CHECK(mob_cap(CatCreature, 578) == 20);
    CHECK(mob_cap(CatMonster, 300) == 72);
    CHECK(mob_cap(CatCreature, 300) == 10);
    CHECK(mob_cap(CatMonster, 0) == 0);
}

// ── 5. light ────────────────────────────────────────────────────────────────

static void test_light() {
    head("light");
    BlockWorld w(64, 128);
    make_flat(w, 63);

    LightField lf;
    lf.reset(w.n());
    lf.add_source(32, 64, 32, LightField::kTorchLight);
    std::printf("torch light level: %d\n", LightField::kTorchLight);
    CHECK(LightField::kTorchLight == 14);
    CHECK(lf.block_light(32, 64, 32) == 14);
    CHECK(lf.block_light(33, 64, 32) == 13);
    CHECK(lf.block_light(32, 64, 45) == 1);
    CHECK(lf.block_light(32, 64, 46) == 0);
    std::printf("taxicab falloff: d=0 -> %d, d=1 -> %d, d=13 -> %d, d=14 -> %d\n",
                lf.block_light(32, 64, 32), lf.block_light(33, 64, 32),
                lf.block_light(32, 64, 45), lf.block_light(32, 64, 46));

    // A query must reach a source in a neighbouring 16-cell.
    LightField lf2;
    lf2.reset(w.n());
    lf2.add_source(15, 64, 15, LightField::kTorchLight);   // cell (0,0)
    CHECK(lf2.block_light(17, 64, 17) == 10);              // cell (1,1)
    std::printf("cross-cell query (cell 0,0 source seen from cell 1,1): %d\n",
                lf2.block_light(17, 64, 17));

    // rebuild() finds torches in the world, and is the file's only O(volume) pass.
    w.set(10, 64, 10, Torch);
    w.set(20, 64, 20, Torch);
    LightField lf3;
    const auto t0 = std::chrono::steady_clock::now();
    lf3.rebuild(w);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("rebuild over %dx%dx%d = %zu blocks: %d sources, %.2f ms "
                "(setup only, never per tick)\n", w.n(), w.height(), w.n(),
                w.volume(), lf3.sources(), ms);
    CHECK(lf3.sources() == 2);
    CHECK(lf3.block_light(10, 64, 10) == 14);

    // Sky light and the day cycle.
    std::printf("sky darken: noon(6000)=%d dusk(12500)=%d night(18000)=%d "
                "dawn(23500)=%d\n", sky_darken(6000), sky_darken(12500),
                sky_darken(18000), sky_darken(23500));
    CHECK(sky_darken(6000)  == 0);
    CHECK(sky_darken(18000) == kNightSkyDarken);
    CHECK(sky_darken(0)     == 0);
    CHECK(sky_darken(-1000) == sky_darken(23000));   // wraps
    CHECK(raw_sky_light(w, 32, 64, 32) == 15);       // open sky above grass
    CHECK(raw_sky_light(w, 32, 60, 32) == 0);        // buried in stone
    CHECK(internal_sky_light(w, 32, 64, 32, 6000)  == 15);
    CHECK(internal_sky_light(w, 32, 64, 32, 18000) == 4);
    std::printf("internal sky light on open grass: noon %d, midnight %d\n",
                internal_sky_light(w, 32, 64, 32, 6000),
                internal_sky_light(w, 32, 64, 32, 18000));

    // Burn window, from the daylight-cycle table.
    CHECK(undead_burn_window(kUndeadBurnFrom));
    CHECK(undead_burn_window(kUndeadBurnTo - 1));
    CHECK(!undead_burn_window(kUndeadBurnTo));
    CHECK(!undead_burn_window(18000));
    CHECK(!undead_burn_window(kUndeadBurnFrom - 1));
    int burnTicks = 0;
    for (int t = 0; t < kDayLength; ++t) if (undead_burn_window(t)) ++burnTicks;
    std::printf("undead burn window: ticks %d..%d, %d of %d ticks (%.1f%% of "
                "the day)\n", kUndeadBurnFrom, kUndeadBurnTo, burnTicks,
                kDayLength, 100.0 * burnTicks / kDayLength);
    CHECK(burnTicks == (kDayLength - kUndeadBurnFrom) + kUndeadBurnTo);
}

// ── 6. spawn rules ──────────────────────────────────────────────────────────

static void test_spawn_rules() {
    head("spawn rules");
    BlockWorld w(64, 128);
    make_flat(w, 63);
    LightField dark;
    dark.reset(w.n());

    const int gy = 64;   // the block a mob's feet occupy, grass at 63

    // Hostiles: block light 0 and internal sky light <= 7.
    CHECK(spawn_allowed(CatMonster, w, dark, 32, gy, 32, 18000));   // night
    CHECK(!spawn_allowed(CatMonster, w, dark, 32, gy, 32, 6000));   // noon
    std::printf("monster on open grass: night %s, noon %s\n",
                spawn_allowed(CatMonster, w, dark, 32, gy, 32, 18000) ? "yes" : "no",
                spawn_allowed(CatMonster, w, dark, 32, gy, 32, 6000)  ? "yes" : "no");

    // One torch, 1.18+ rule: ANY block light stops a hostile spawn. Torch level
    // 14, taxicab falloff, so the block at distance 14 is the first one clear.
    LightField lit;
    lit.reset(w.n());
    lit.add_source(32, gy, 32, LightField::kTorchLight);
    int firstClear = -1;
    for (int d = 0; d <= 20; ++d)
        if (spawn_allowed(CatMonster, w, lit, 32 + d, gy, 32, 18000)) { firstClear = d; break; }
    std::printf("torch at distance 0..20 (night): first spawnable block at "
                "d=%d, block light there = %d\n",
                firstClear, lit.block_light(32 + firstClear, gy, 32));
    CHECK(firstClear == 14);
    CHECK(lit.block_light(32 + firstClear, gy, 32) == kMonsterMaxBlockLight);
    CHECK(kMonsterMaxBlockLight == 0);
    // Under the pre-1.18 rule (combined light <= 7) the first clear block would
    // have been d=7. The 1.18 change doubled a torch's spawn-proofing radius,
    // and that is the whole reason this file says which version it models.

    // Animals: grass floor, light >= 9.
    CHECK(spawn_allowed(CatCreature, w, dark, 32, gy, 32, 6000));    // noon
    CHECK(!spawn_allowed(CatCreature, w, dark, 32, gy, 32, 18000));  // midnight
    std::printf("animal on open grass: noon %s, midnight %s (needs light >= %d)\n",
                spawn_allowed(CatCreature, w, dark, 32, gy, 32, 6000)  ? "yes" : "no",
                spawn_allowed(CatCreature, w, dark, 32, gy, 32, 18000) ? "yes" : "no",
                kAnimalMinLight);
    // ...and a torch is enough for them at night: light 14 >= 9.
    CHECK(spawn_allowed(CatCreature, w, lit, 32, gy, 32, 18000));

    // Animals need GRASS, not merely a solid floor.
    w.set(40, 63, 40, Stone);
    w.refresh_column(40, 40);
    CHECK(!spawn_allowed(CatCreature, w, dark, 40, gy, 40, 6000));
    CHECK(spawn_allowed(CatMonster, w, dark, 40, gy, 40, 18000));
    std::printf("stone floor: animal %s, monster %s\n",
                spawn_allowed(CatCreature, w, dark, 40, gy, 40, 6000)  ? "yes" : "no",
                spawn_allowed(CatMonster, w, dark, 40, gy, 40, 18000)  ? "yes" : "no");

    // Two blocks of space. A ceiling one block up kills the spawn.
    w.set(44, gy + 1, 44, Stone);
    w.refresh_column(44, 44);
    CHECK(!spawn_allowed(CatMonster, w, dark, 44, gy, 44, 18000));
    w.set(44, gy + 1, 44, Air);
    w.refresh_column(44, 44);
    CHECK(spawn_allowed(CatMonster, w, dark, 44, gy, 44, 18000));
    std::printf("two blocks of headroom required: 1 block -> no, 2 blocks -> yes\n");

    // No liquid in the body, and no spawning on leaves.
    w.set(46, gy, 46, Water);
    CHECK(!spawn_allowed(CatMonster, w, dark, 46, gy, 46, 18000));
    w.set(46, gy, 46, Air);
    w.set(48, 63, 48, Leaves);
    w.refresh_column(48, 48);
    CHECK(!spawn_allowed(CatMonster, w, dark, 48, gy, 48, 18000));
    std::printf("liquid in the body -> no; leaves as floor -> no\n");

    // No floor at all.
    CHECK(!spawn_allowed(CatMonster, w, dark, 32, 100, 32, 18000));
}

// ── 7. pack spawning ────────────────────────────────────────────────────────

static void test_packs() {
    head("pack spawning");
    BlockWorld w(96, 128);
    make_flat(w, 63);
    LightField dark;
    dark.reset(w.n());

    MobWorld mw(0xB0B1E5ull);
    mw.set_player(48.5f, 64.0f, 48.5f);
    mw.set_world_spawn(48.5f, 64.0f, 48.5f);

    // A cap far above anything we will reach, so packs are not truncated by it
    // and the measurement is of the pack loop alone.
    const int chunks = kMagicChunkCount * 40;   // cap 2800
    SpawnReport total;
    for (int cycle = 0; cycle < 20; ++cycle) {
        const SpawnReport r = mw.spawn_cycle(w, dark, CatMonster, 18000, chunks, 4000);
        total.positions     += r.positions;
        total.packs         += r.packs;
        total.spawned       += r.spawned;
        total.packSizeSum   += r.packSizeSum;
        total.rejectCap     += r.rejectCap;
        total.rejectDistance+= r.rejectDistance;
        total.rejectRules   += r.rejectRules;
        total.maxStepOffset  = std::max(total.maxStepOffset, r.maxStepOffset);
    }
    const double meanPack = double(total.packSizeSum) / double(total.packs);
    std::printf("positions %d, packs %d, mobs %d\n",
                total.positions, total.packs, total.spawned);
    std::printf("rejected: cap %d, distance %d, rules %d\n",
                total.rejectCap, total.rejectDistance, total.rejectRules);
    std::printf("mean mobs per successful pack: %.3f (plains group size is 4,4)\n",
                meanPack);
    std::printf("largest single-member displacement: %d (nextInt(6)-nextInt(6) "
                "gives +-5)\n", total.maxStepOffset);
    CHECK(total.spawned > 1000);
    CHECK(total.maxStepOffset <= 5);
    CHECK(total.maxStepOffset == 5);        // with this many rolls, +-5 must appear
    CHECK(meanPack > 3.0 && meanPack <= 4.0);
    CHECK(total.packs <= total.positions * kPackAttempts);
    std::printf("packs per position <= %d: %.4f\n", kPackAttempts,
                double(total.packs) / double(total.positions));

    // Distance band: nothing within 24 of the player, nothing beyond 128.
    float dmin = 1e9f, dmax = 0.0f;
    for (const Mob& m : mw.mobs()) {
        const float dx = m.x - 48.5f, dy = m.y - 64.0f, dz = m.z - 48.5f;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        dmin = std::min(dmin, d);
        dmax = std::max(dmax, d);
    }
    std::printf("spawn distance from player: min %.2f, max %.2f "
                "(rule: > %.0f and <= %.0f)\n", double(dmin), double(dmax),
                double(kMinSpawnDistance), double(kSpawnRadius));
    CHECK(dmin > kMinSpawnDistance);
    CHECK(dmax <= kSpawnRadius);

    // Only the four plains monsters, in roughly their published weights.
    int byKind[kMobKinds] = {0};
    for (const Mob& m : mw.mobs()) ++byKind[m.kind];
    for (int k : {MobCow, MobPig, MobSheep, MobChicken}) CHECK(byKind[k] == 0);
    std::printf("monster mix (weights creeper/skeleton/spider 100, zombie 95):\n");
    for (int k : {MobCreeper, MobSkeleton, MobSpider, MobZombie})
        std::printf("  %-9s %5d  %5.1f%%\n", mob_name(k), byKind[k],
                    100.0 * byKind[k] / total.spawned);
    // 100/395 = 25.3%, 95/395 = 24.1%. Three sigma on ~1e4 samples is well
    // inside a point, so a two-point window is a real check, not a rubber stamp.
    CHECK_NEAR(100.0 * byKind[MobCreeper] / total.spawned, 25.32, 2.0);
    CHECK_NEAR(100.0 * byKind[MobZombie]  / total.spawned, 24.05, 2.0);

    // Now the cap. Same world, a one-player cap, spawn until it saturates.
    MobWorld capped(0xCA9ull);
    capped.set_player(48.5f, 64.0f, 48.5f);
    capped.set_world_spawn(48.5f, 64.0f, 48.5f);
    for (int cycle = 0; cycle < 20; ++cycle)
        capped.spawn_cycle(w, dark, CatMonster, 18000, kMagicChunkCount, 4000);
    std::printf("with a one-player cap of %d, spawned to %d monsters\n",
                mob_cap(CatMonster, kMagicChunkCount), capped.alive(CatMonster));
    CHECK(capped.alive(CatMonster) == kMonsterCap);

    // Animals: daytime, grass, and a cap of 10.
    MobWorld animals(0xA11ull);
    animals.set_player(48.5f, 64.0f, 48.5f);
    animals.set_world_spawn(48.5f, 64.0f, 48.5f);
    for (int cycle = 0; cycle < 20; ++cycle)
        animals.spawn_cycle(w, dark, CatCreature, 6000, kMagicChunkCount, 4000);
    std::printf("animal cap %d, spawned to %d\n",
                mob_cap(CatCreature, kMagicChunkCount), animals.alive(CatCreature));
    CHECK(animals.alive(CatCreature) == kCreatureCap);
    for (const Mob& m : animals.mobs()) CHECK(!mob_hostile(m.kind));

    // ...and no animals at night, because the light is 4 and they need 9.
    MobWorld nightAnimals(0xA12ull);
    nightAnimals.set_player(48.5f, 64.0f, 48.5f);
    nightAnimals.set_world_spawn(48.5f, 64.0f, 48.5f);
    nightAnimals.spawn_cycle(w, dark, CatCreature, 18000, kMagicChunkCount, 8000);
    std::printf("animals attempted at midnight on unlit grass: %d\n",
                nightAnimals.alive(CatCreature));
    CHECK(nightAnimals.alive(CatCreature) == 0);
}

// ── 8. despawning ───────────────────────────────────────────────────────────

// A world where a mob cannot move: flat grass, with a stone box round one cell
// so the despawn measurement is not corrupted by the mob wandering out of range.
static void make_pen(BlockWorld& w, int gx, int gz, int gy) {
    make_flat(w, gy - 1);
    for (int dy = 0; dy <= 1; ++dy) {
        w.set(gx - 1, gy + dy, gz, Stone);
        w.set(gx + 1, gy + dy, gz, Stone);
        w.set(gx, gy + dy, gz - 1, Stone);
        w.set(gx, gy + dy, gz + 1, Stone);
        w.set(gx - 1, gy + dy, gz - 1, Stone);
        w.set(gx + 1, gy + dy, gz - 1, Stone);
        w.set(gx - 1, gy + dy, gz + 1, Stone);
        w.set(gx + 1, gy + dy, gz + 1, Stone);
    }
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) w.refresh_column(gx + dx, gz + dz);
}

static void test_despawn() {
    head("despawning");
    BlockWorld w(64, 128);
    make_pen(w, 32, 32, 64);
    LightField dark;
    dark.reset(w.n());

    // Beyond 128 blocks: gone on the next tick.
    {
        MobWorld mw(1);
        mw.set_player(32.5f, 64.0f, 32.5f + 200.0f);
        mw.add(MobZombie, 32.5f, 64.0f, 32.5f);
        const TickStats st = mw.tick(w, dark, 18000, DiffNormal);
        std::printf("zombie at 200 blocks: despawned %d on tick 1\n", st.despawned);
        CHECK(st.despawned == 1);
        CHECK(mw.alive() == 0);
    }
    // Just inside 128: survives.
    {
        MobWorld mw(1);
        mw.set_player(32.5f, 64.0f, 32.5f + 120.0f);
        mw.add(MobZombie, 32.5f, 64.0f, 32.5f);
        for (int i = 0; i < 100; ++i) mw.tick(w, dark, 18000, DiffNormal);
        std::printf("zombie at 120 blocks after 100 ticks: alive %d\n", mw.alive());
        CHECK(mw.alive() == 1);
    }
    // Inside 32: the idle clock is held at zero, so it never rolls.
    {
        MobWorld mw(1);
        mw.set_player(32.5f, 64.0f, 32.5f + 20.0f);
        mw.add(MobZombie, 32.5f, 64.0f, 32.5f);
        for (int i = 0; i < 20000; ++i) mw.tick(w, dark, 18000, DiffNormal);
        std::printf("zombie at 20 blocks after 20000 ticks: alive %d "
                    "(inside %.0f blocks the clock resets)\n",
                    mw.alive(), double(kNoDespawnDistance));
        CHECK(mw.alive() == 1);
    }
    // Between 32 and 128: 1/800 per tick, but only after 600 idle ticks.
    {
        const int trials = 400;
        long long sum = 0;
        int earliest = 1 << 30;
        for (int t = 0; t < trials; ++t) {
            MobWorld mw(mix_seed(0xDE5A47ull, t));
            mw.set_player(32.5f, 64.0f, 32.5f + 50.0f);
            mw.add(MobZombie, 32.5f, 64.0f, 32.5f);
            int ticks = 0;
            while (mw.alive() > 0 && ticks < 40000) {
                mw.tick(w, dark, 18000, DiffNormal);
                ++ticks;
            }
            CHECK(mw.alive() == 0);
            sum += ticks;
            earliest = std::min(earliest, ticks);
        }
        const double mean = double(sum) / trials;
        std::printf("zombie at 50 blocks, %d trials: mean %.1f ticks to despawn, "
                    "earliest %d\n", trials, mean, earliest);
        std::printf("expected %d idle + 800 mean geometric = %d\n",
                    kDespawnIdleTicks, kDespawnIdleTicks + kDespawnOneIn);
        CHECK(earliest > kDespawnIdleTicks);
        CHECK(mean > 1100.0 && mean < 1750.0);   // 3 sigma on 400 trials is ~120
    }
    // Animals are persistent: they do not despawn at all.
    {
        MobWorld mw(1);
        mw.set_player(32.5f, 64.0f, 32.5f + 300.0f);
        mw.add(MobCow, 32.5f, 64.0f, 32.5f);
        for (int i = 0; i < 5000; ++i) mw.tick(w, dark, 6000, DiffNormal);
        std::printf("cow at 300 blocks after 5000 ticks: alive %d "
                    "(passive mobs are persistent)\n", mw.alive());
        CHECK(mw.alive() == 1);
    }
}

// ── 9. daylight burning ─────────────────────────────────────────────────────

static void test_burning() {
    head("daylight burning");
    BlockWorld w(64, 128);
    make_flat(w, 63);
    LightField dark;
    dark.reset(w.n());

    // Zombie in the open at noon: 1 HP per second until its 20 are gone.
    {
        MobWorld mw(7);
        mw.set_player(32.5f, 64.0f, 32.5f + 20.0f);   // close, so no despawn
        mw.add(MobZombie, 32.5f, 64.0f, 32.5f);
        int ticks = 0;
        while (mw.alive() > 0 && ticks < 5000) { mw.tick(w, dark, 6000, DiffNormal); ++ticks; }
        const double seconds = ticks / 20.0;
        std::printf("zombie in open sun: dead after %d ticks (%.1f s) for %.0f HP "
                    "at %.0f HP/s\n", ticks, seconds,
                    double(mob_type(MobZombie).health),
                    double(kFireDamage) * 20.0 / kFireDamageEvery);
        CHECK(ticks >= 360 && ticks <= 400);
    }
    // Same zombie under a roof: untouched. The roof covers the whole map on
    // purpose — the first draft roofed only a 5x5 patch, the zombie chased the
    // player straight out from under it and burned, and the test was measuring
    // the chase rather than the roof.
    {
        BlockWorld roofed(64, 128);
        make_flat(roofed, 63);
        for (int z = 0; z < roofed.n(); ++z)
            for (int x = 0; x < roofed.n(); ++x) roofed.set(x, 70, z, Stone);
        for (int z = 0; z < roofed.n(); ++z)
            for (int x = 0; x < roofed.n(); ++x) roofed.refresh_column(x, z);
        MobWorld mw(7);
        mw.set_player(32.5f, 64.0f, 32.5f + 20.0f);
        mw.add(MobZombie, 32.5f, 64.0f, 32.5f);
        for (int i = 0; i < 2000; ++i) mw.tick(roofed, dark, 6000, DiffNormal);
        std::printf("zombie under a roof at noon after 2000 ticks: hp %.1f\n",
                    double(mw.mobs()[0].health));
        CHECK(mw.alive() == 1);
        CHECK(mw.mobs()[0].health == mob_type(MobZombie).health);
    }
    // Zombie in the open at midnight: also untouched.
    {
        MobWorld mw(7);
        mw.set_player(32.5f, 64.0f, 32.5f + 20.0f);
        mw.add(MobZombie, 32.5f, 64.0f, 32.5f);
        for (int i = 0; i < 2000; ++i) mw.tick(w, dark, 18000, DiffNormal);
        std::printf("zombie in the open at midnight after 2000 ticks: hp %.1f\n",
                    double(mw.mobs()[0].health));
        CHECK(mw.mobs()[0].health == mob_type(MobZombie).health);
    }
    // Creeper and spider never burn, whatever the hour.
    {
        MobWorld mw(7);
        mw.set_player(32.5f, 64.0f, 32.5f + 20.0f);
        mw.add(MobCreeper, 20.5f, 64.0f, 20.5f);
        mw.add(MobSpider,  22.5f, 64.0f, 22.5f);
        mw.set_player(40.5f, 64.0f, 40.5f);   // >16 away: they wander, in the open
        for (int i = 0; i < 2000; ++i) mw.tick(w, dark, 6000, DiffNormal);
        int burning = 0;
        for (const Mob& m : mw.mobs()) if (m.fireTicks > 0) ++burning;
        std::printf("creeper and spider in open sun for 2000 ticks: %d burning\n",
                    burning);
        CHECK(burning == 0);
    }
}

// ── 10. the state machine ───────────────────────────────────────────────────

static void test_states() {
    head("state machine");
    BlockWorld w(64, 128);
    make_flat(w, 63);
    LightField dark;
    dark.reset(w.n());

    // Creeper: chase -> fuse at 3 blocks -> boom 30 ticks later.
    {
        MobWorld mw(11);
        mw.set_player(32.5f, 64.0f, 32.5f);
        const int id = mw.add(MobCreeper, 34.0f, 64.0f, 32.5f);   // 1.5 blocks away
        int ignited = -1, blew = -1;
        float dmg = 0.0f;
        for (int t = 1; t <= 100 && blew < 0; ++t) {
            const TickStats st = mw.tick(w, dark, 18000, DiffNormal);
            if (ignited < 0 && mw.mobs()[std::size_t(id)].state == StFuse) ignited = t;
            if (st.explosions > 0) { blew = t; dmg = st.playerDamage; }
        }
        std::printf("creeper lit on tick %d, exploded on tick %d (%d ticks of "
                    "fuse, published %d)\n", ignited, blew, blew - ignited,
                    kCreeperFuseTicks);
        std::printf("damage at 1.5 blocks on Normal: %.0f\n", double(dmg));
        CHECK(ignited == 1);
        CHECK(blew - ignited == kCreeperFuseTicks);
        CHECK_NEAR(dmg, explosion_damage(kCreeperPower, 1.5f, 1.0f), 1e-4);
        CHECK(dmg == 28.0f);
        CHECK(mw.alive() == 0);      // the creeper is consumed by its own blast
    }
    // Creeper: the fuse goes out if the target gets 7 blocks away.
    {
        MobWorld mw(11);
        mw.set_player(32.5f, 64.0f, 32.5f);
        const int id = mw.add(MobCreeper, 34.0f, 64.0f, 32.5f);
        for (int t = 0; t < 5; ++t) mw.tick(w, dark, 18000, DiffNormal);
        CHECK(mw.mobs()[std::size_t(id)].state == StFuse);
        const int fuseLeft = mw.mobs()[std::size_t(id)].fuse;
        mw.set_player(32.5f, 64.0f, 42.5f);          // 10 blocks: > 7, but <= 16
        const TickStats st = mw.tick(w, dark, 18000, DiffNormal);
        std::printf("fuse at %d ticks, player retreats to 10 blocks: state %s, "
                    "fuse %d, explosions %d\n", fuseLeft,
                    mob_state_name(mw.mobs()[std::size_t(id)].state),
                    mw.mobs()[std::size_t(id)].fuse, st.explosions);
        CHECK(st.explosions == 0);
        CHECK(mw.mobs()[std::size_t(id)].fuse == -1);
        CHECK(mw.mobs()[std::size_t(id)].state == StChase);
    }
    // Zombie: chases from inside follow range, ignores from outside.
    {
        MobWorld mw(12);
        mw.set_player(32.5f, 64.0f, 32.5f);
        const int inRange  = mw.add(MobZombie, 32.5f, 64.0f, 62.5f);   // 30 <= 35
        const int outRange = mw.add(MobZombie, 32.5f, 64.0f, 32.5f + 40.0f);
        mw.tick(w, dark, 18000, DiffNormal);
        std::printf("zombie at 30 blocks: %s; at 40 blocks: %s (follow range %.0f)\n",
                    mob_state_name(mw.mobs()[std::size_t(inRange)].state),
                    mob_state_name(mw.mobs()[std::size_t(outRange)].state),
                    double(mob_type(MobZombie).followRange));
        CHECK(mw.mobs()[std::size_t(inRange)].state == StChase);
        CHECK(mw.mobs()[std::size_t(outRange)].state != StChase);
        CHECK(mw.mobs()[std::size_t(outRange)].state != StAttack);
    }
    // Zombie melee: closes, attacks, and the damage matches the difficulty rule.
    {
        {
            MobWorld mw(13);
            mw.set_player(32.5f, 64.0f, 32.5f);
            mw.add(MobZombie, 33.5f, 64.0f, 32.5f);
            float total = 0.0f;
            int attacks = 0;
            for (int t = 0; t < 201; ++t) {
                const TickStats st = mw.tick(w, dark, 18000, DiffNormal);
                total += st.playerDamage;
                attacks += st.attacks;
            }
            std::printf("zombie melee over 201 ticks (Normal): %d attacks, "
                        "%.1f damage, cooldown %d ticks\n", attacks, double(total),
                        kMeleeCooldown);
            // First swing lands on tick 1, then one every kMeleeCooldown ticks.
            CHECK(attacks == (201 - 1) / kMeleeCooldown + 1);
            CHECK_NEAR(total, attacks * 3.0, 1e-3);
        }
        for (Difficulty d : {DiffEasy, DiffNormal, DiffHard}) {
            MobWorld mw(13);
            mw.set_player(32.5f, 64.0f, 32.5f);
            mw.add(MobZombie, 33.5f, 64.0f, 32.5f);
            const TickStats st = mw.tick(w, dark, 18000, d);
            std::printf("  first swing on %s: %.2f\n",
                        d == DiffEasy ? "easy" : d == DiffNormal ? "normal" : "hard",
                        double(st.playerDamage));
            CHECK_NEAR(st.playerDamage, damage_to_player(3.0f, d), 1e-5);
        }
        // Peaceful: no target at all.
        MobWorld mw(13);
        mw.set_player(32.5f, 64.0f, 32.5f);
        mw.add(MobZombie, 33.5f, 64.0f, 32.5f);
        const TickStats st = mw.tick(w, dark, 18000, DiffPeaceful);
        CHECK(st.playerDamage == 0.0f);
        CHECK(st.attacks == 0);
        std::printf("  peaceful: %d attacks\n", st.attacks);
    }
    // Skeleton: fires at 15 blocks on the published interval, damage in range.
    {
        for (Difficulty d : {DiffNormal, DiffHard}) {
            MobWorld mw(14);
            mw.set_player(32.5f, 64.0f, 32.5f);
            mw.add(MobSkeleton, 32.5f, 64.0f, 42.5f);   // 10 blocks: inside 15
            float total = 0.0f;
            int attacks = 0;
            float lo = 0, hi = 0;
            skeleton_arrow_range(d, lo, hi);
            for (int t = 0; t < 600; ++t) {
                const TickStats st = mw.tick(w, dark, 18000, d);
                total += st.playerDamage;
                attacks += st.attacks;
            }
            const double mean = attacks ? double(total) / attacks : 0.0;
            std::printf("skeleton on %s: %d shots in 600 ticks (interval %d), "
                        "mean arrow %.2f, published %.0f-%.0f\n",
                        d == DiffHard ? "hard" : "normal", attacks,
                        skeleton_shoot_interval(d), mean, double(lo), double(hi));
            CHECK(attacks == (600 - 1) / skeleton_shoot_interval(d) + 1);
            CHECK(mean >= double(lo) && mean <= double(hi));
        }
        CHECK(skeleton_shoot_interval(DiffNormal) == 60);
        CHECK(skeleton_shoot_interval(DiffHard)   == 40);
    }
    // Spider: hostile in the dark, neutral in bright light (level 11 threshold).
    {
        LightField bright;
        bright.reset(w.n());
        bright.add_source(32, 64, 32, LightField::kTorchLight);   // 14 at the spider
        MobWorld mw(15);
        mw.set_player(38.5f, 64.0f, 32.5f);
        const int id = mw.add(MobSpider, 32.5f, 64.0f, 32.5f);
        mw.tick(w, bright, 18000, DiffNormal);
        const int litState = mw.mobs()[std::size_t(id)].state;
        MobWorld mw2(15);
        mw2.set_player(38.5f, 64.0f, 32.5f);
        const int id2 = mw2.add(MobSpider, 32.5f, 64.0f, 32.5f);
        mw2.tick(w, dark, 18000, DiffNormal);
        std::printf("spider under a torch (light 14): %s; in the dark: %s "
                    "(hostile while light <= 11)\n",
                    mob_state_name(litState),
                    mob_state_name(mw2.mobs()[std::size_t(id2)].state));
        CHECK(litState != StChase && litState != StAttack);
        CHECK(mw2.mobs()[std::size_t(id2)].state == StChase);
    }
    // Passive mobs never chase.
    {
        MobWorld mw(16);
        mw.set_player(32.5f, 64.0f, 32.5f);
        for (int k : {MobCow, MobPig, MobSheep, MobChicken})
            mw.add(k, 33.5f, 64.0f, 32.5f);
        float dmg = 0.0f;
        for (int t = 0; t < 500; ++t) dmg += mw.tick(w, dark, 6000, DiffNormal).playerDamage;
        int wandered = 0;
        for (const Mob& m : mw.mobs())
            if (m.state == StWander || m.state == StIdle) ++wandered;
        std::printf("4 animals next to the player for 500 ticks: %.1f damage, "
                    "%d idle/wandering\n", double(dmg), wandered);
        CHECK(dmg == 0.0f);
        CHECK(wandered == 4);
    }
}

// ── 11. movement ────────────────────────────────────────────────────────────

static void test_movement() {
    head("movement (greedy, no A*)");
    LightField dark;

    // A finite wall. The mob must slide along it and come round the end.
    {
        BlockWorld w(64, 128);
        make_flat(w, 63);
        for (int z = 12; z <= 20; ++z)
            for (int y = 64; y <= 65; ++y) w.set(20, y, z, Stone);
        for (int z = 12; z <= 20; ++z) w.refresh_column(20, z);
        dark.reset(w.n());

        MobWorld mw(21);
        mw.set_player(30.5f, 64.0f, 16.5f);
        const int id = mw.add(MobZombie, 14.5f, 64.0f, 16.5f);
        int ticksToPass = -1;
        float maxZDev = 0.0f;
        for (int t = 1; t <= 800; ++t) {
            mw.tick(w, dark, 18000, DiffNormal);
            const Mob& cur = mw.mobs()[std::size_t(id)];
            maxZDev = std::max(maxZDev, std::fabs(cur.z - 16.5f));
            if (ticksToPass < 0 && cur.x > 21.0f) ticksToPass = t;
        }
        const Mob& m = mw.mobs()[std::size_t(id)];
        // The straight line from x=14.5 to x=21 is 6.5 blocks, 28 ticks at a
        // zombie's 0.23/tick. The wall spans z 12..20 and the mob starts at
        // z=16.5, so getting round its end costs about 4.5 blocks of sideways
        // travel, another 20 ticks. 48 is therefore the detour's cost, and the
        // z deviation is the direct evidence it detoured rather than clipped
        // through. (The first version of this check asserted > 70 ticks off a
        // botched straight-line estimate and failed on correct behaviour.)
        std::printf("zombie vs a 9-long wall: got past x=21 on tick %d "
                    "(straight line would be 28), sideways detour %.2f blocks, "
                    "final (%.2f, %.0f, %.2f)\n",
                    ticksToPass, double(maxZDev), double(m.x), double(m.y),
                    double(m.z));
        CHECK(ticksToPass > 0);
        CHECK(m.x > 21.0f);
        CHECK(ticksToPass > 40);      // materially slower than the straight line
        CHECK(maxZDev > 3.0f);        // it went round the end of the wall
    }
    // A one-block step: the mob climbs rather than stopping.
    {
        BlockWorld w(64, 128);
        make_flat(w, 63);
        for (int z = 10; z <= 24; ++z) w.set(20, 64, z, Stone);
        for (int z = 10; z <= 24; ++z) w.refresh_column(20, z);
        dark.reset(w.n());

        MobWorld mw(22);
        mw.set_player(26.5f, 64.0f, 16.5f);
        const int id = mw.add(MobZombie, 16.5f, 64.0f, 16.5f);
        float maxY = 64.0f;
        for (int t = 0; t < 300; ++t) {
            mw.tick(w, dark, 18000, DiffNormal);
            maxY = std::max(maxY, mw.mobs()[std::size_t(id)].y);
        }
        const Mob& m = mw.mobs()[std::size_t(id)];
        std::printf("zombie vs a 1-block step spanning the path: climbed to y=%.0f, "
                    "ended at x=%.2f\n", double(maxY), double(m.x));
        CHECK(maxY >= 65.0f);      // it went up
        CHECK(m.x > 20.0f);        // and over
    }
    // Gravity: a mob dropped into the air lands on the ground.
    {
        BlockWorld w(64, 128);
        make_flat(w, 63);
        dark.reset(w.n());
        MobWorld mw(23);
        mw.set_player(32.5f, 64.0f, 34.5f);
        const int id = mw.add(MobZombie, 32.5f, 67.0f, 32.5f);
        for (int t = 0; t < 20; ++t) mw.tick(w, dark, 18000, DiffNormal);
        std::printf("zombie spawned at y=67 over grass at y=63: settled at y=%.0f\n",
                    double(mw.mobs()[std::size_t(id)].y));
        CHECK(mw.mobs()[std::size_t(id)].y == 64.0f);
    }
    // Speed ordering is the game's, even though the scale is the bench's.
    std::printf("step per tick (attribute read as blocks/tick, a BENCH scale): ");
    for (int k = 0; k < kMobKinds; ++k)
        std::printf("%s %.2f  ", mob_name(k), double(mob_step(k)));
    std::printf("\n");
    CHECK(mob_step(MobSpider) > mob_step(MobZombie));
    CHECK(mob_step(MobSkeleton) > mob_step(MobZombie));
    CHECK(mob_step(MobPig) > mob_step(MobCow));
}

// ── 12. drops and XP ────────────────────────────────────────────────────────

static void test_drops() {
    head("drops and XP");
    MobWorld mw(0xD400Bull);
    const int n = 200000;

    // Rotten flesh 0-2. The wiki's "66.67%" is P(count > 0) for that uniform,
    // not a separate roll — so it must fall out of the measurement.
    long long sum = 0;
    int nonzero = 0, mx = 0;
    for (int i = 0; i < n; ++i) {
        const auto d = mw.roll_drops(MobZombie, true);
        sum += d[MiRottenFlesh];
        if (d[MiRottenFlesh] > 0) ++nonzero;
        mx = std::max(mx, d[MiRottenFlesh]);
    }
    const double mean = double(sum) / n, p = double(nonzero) / n;
    std::printf("zombie rotten flesh over %d kills: mean %.4f (expect 1.0), "
                "P(>0) %.4f (expect 0.6667), max %d\n", n, mean, p, mx);
    CHECK_NEAR(mean, 1.0, 0.01);
    CHECK_NEAR(p, 2.0 / 3.0, 0.01);
    CHECK(mx == 2);

    // Skeleton drops two lines, both 0-2.
    long long bones = 0, arrows = 0;
    for (int i = 0; i < n; ++i) {
        const auto d = mw.roll_drops(MobSkeleton, true);
        bones += d[MiBone];
        arrows += d[MiArrow];
        CHECK(d[MiBone] <= 2 && d[MiArrow] <= 2);
    }
    std::printf("skeleton: mean bones %.4f, mean arrows %.4f (both 0-2)\n",
                double(bones) / n, double(arrows) / n);
    CHECK_NEAR(double(bones) / n, 1.0, 0.01);
    CHECK_NEAR(double(arrows) / n, 1.0, 0.01);

    // Spider eye is player-kill only.
    int eyesFromPlayer = 0, eyesOtherwise = 0;
    for (int i = 0; i < 20000; ++i) {
        eyesFromPlayer += mw.roll_drops(MobSpider, true)[MiSpiderEye];
        eyesOtherwise  += mw.roll_drops(MobSpider, false)[MiSpiderEye];
    }
    std::printf("spider eyes: %d from player kills, %d otherwise\n",
                eyesFromPlayer, eyesOtherwise);
    CHECK(eyesFromPlayer > 0);
    CHECK(eyesOtherwise == 0);

    // Animals.
    for (int i = 0; i < 20000; ++i) {
        const auto cow = mw.roll_drops(MobCow, true);
        CHECK(cow[MiBeef] >= 1 && cow[MiBeef] <= 3);
        CHECK(cow[MiLeather] <= 2);
        const auto sh = mw.roll_drops(MobSheep, true);
        CHECK(sh[MiWool] == 1);
        CHECK(sh[MiMutton] >= 1 && sh[MiMutton] <= 2);
        const auto ch = mw.roll_drops(MobChicken, true);
        CHECK(ch[MiChickenRaw] == 1);
        CHECK(ch[MiFeather] <= 2);
        const auto pg = mw.roll_drops(MobPig, true);
        CHECK(pg[MiPorkchop] >= 1 && pg[MiPorkchop] <= 3);
    }
    std::printf("cow beef 1-3 + leather 0-2, sheep mutton 1-2 + wool 1, "
                "chicken 1 + feather 0-2, pig 1-3: all in range over 20000 kills\n");

    // XP.
    long long hostileXp = 0, cowXp = 0;
    int cowLo = 99, cowHi = 0;
    for (int i = 0; i < 20000; ++i) {
        hostileXp += mw.roll_xp(MobZombie, true);
        const int c = mw.roll_xp(MobCow, true);
        cowXp += c;
        cowLo = std::min(cowLo, c);
        cowHi = std::max(cowHi, c);
    }
    std::printf("XP: zombie always %.0f, cow %d-%d mean %.3f (published 1-3)\n",
                double(hostileXp) / 20000, cowLo, cowHi, double(cowXp) / 20000);
    CHECK(hostileXp == 5LL * 20000);
    CHECK(cowLo == 1 && cowHi == 3);
    CHECK_NEAR(double(cowXp) / 20000, 2.0, 0.05);
    CHECK(mw.roll_xp(MobZombie, false) == 0);
    std::printf("no player kill -> %d XP\n", mw.roll_xp(MobZombie, false));
}

// ── 13. determinism ─────────────────────────────────────────────────────────

static double run_signature(std::uint64_t seed) {
    BlockWorld w(96, 128);
    w.generate(4242);
    LightField lf;
    lf.rebuild(w);
    MobWorld mw(seed);
    mw.set_player(48.5f, float(w.surface(48, 48) + 1), 48.5f);
    mw.set_world_spawn(48.5f, float(w.surface(48, 48) + 1), 48.5f);
    double sig = 0.0;
    for (int cycle = 0; cycle < 5; ++cycle) {
        mw.spawn_cycle(w, lf, CatMonster, 18000, kMagicChunkCount, 2000);
        for (int t = 0; t < 100; ++t) mw.tick(w, lf, 18000 + t, DiffNormal);
    }
    for (const Mob& m : mw.mobs())
        sig += double(m.x) * 1.5 + double(m.y) * 2.25 + double(m.z) * 3.125
             + double(m.health) + double(m.kind) * 7.0 + double(m.state) * 11.0;
    return sig;
}

static void test_determinism() {
    head("determinism");
    const double a = run_signature(0x5EEDull);
    const double b = run_signature(0x5EEDull);
    const double c = run_signature(0x5EEEull);
    std::printf("seed 0x5EED run 1: %.6f\nseed 0x5EED run 2: %.6f\n"
                "seed 0x5EEE:       %.6f\n", a, b, c);
    CHECK(a == b);
    CHECK(a != c);
}

// ── 14. performance ─────────────────────────────────────────────────────────

static void test_perf() {
    head("performance");
    BlockWorld w(256, 128);
    make_flat(w, 63);
    LightField lf;
    const auto r0 = std::chrono::steady_clock::now();
    lf.rebuild(w);
    const auto r1 = std::chrono::steady_clock::now();
    std::printf("LightField::rebuild over %zu blocks (256x128x256): %.1f ms "
                "— setup only\n", w.volume(),
                std::chrono::duration<double, std::milli>(r1 - r0).count());

    // A full one-player cap of mobs, all inside 32 blocks so none despawns.
    MobWorld mw(0x9E4Full);
    mw.set_player(128.5f, 64.0f, 128.5f);
    Rng place(99);
    for (int i = 0; i < kMonsterCap; ++i) {
        const float ang = place.unit() * 6.2831853f;
        const float rad = 6.0f + place.unit() * 20.0f;
        mw.add(i % 2 ? MobZombie : MobSpider,
               128.5f + std::cos(ang) * rad, 64.0f, 128.5f + std::sin(ang) * rad);
    }
    for (int i = 0; i < kCreatureCap; ++i)
        mw.add(MobCow, 128.5f + float(i) * 2.0f, 64.0f, 118.5f);

    const int ticks = 200000;
    const int mobs = mw.alive();
    const auto t0 = std::chrono::steady_clock::now();
    double sink = 0.0;
    for (int t = 0; t < ticks; ++t)
        sink += double(mw.tick(w, lf, 18000, DiffNormal).playerDamage);
    const auto t1 = std::chrono::steady_clock::now();
    const double s = std::chrono::duration<double>(t1 - t0).count();
    std::printf("%d mobs x %d ticks in %.3f s = %.0f mob-ticks/s, "
                "%.1f ns per mob-tick\n", mobs, ticks, s,
                double(mobs) * ticks / s, s * 1e9 / (double(mobs) * ticks));
    std::printf("at 20 tps that is %.4f%% of one 50 ms tick budget for a full cap\n",
                100.0 * (s / ticks) / 0.05);
    std::printf("(sink %.0f, so the loop is not optimised away)\n", sink);
    CHECK(mobs == kMonsterCap + kCreatureCap);
    // The tick must be cheap enough that mobs are never the bottleneck.
    CHECK(s / ticks < 0.005);   // under 10% of a 50 ms tick
}

int main() {
    std::printf("test_mobs — src/world/mobs.hpp\n");
    test_table();
    test_difficulty();
    test_explosion();
    test_cap();
    test_light();
    test_spawn_rules();
    test_packs();
    test_despawn();
    test_burning();
    test_states();
    test_movement();
    test_drops();
    test_determinism();
    test_perf();
    std::printf("\nOK — %d checks passed\n", g_checks);
    return 0;
}
