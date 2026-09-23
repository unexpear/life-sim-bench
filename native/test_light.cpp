// test_light.cpp — asserts every constant and every behaviour that
// src/world/light.hpp publishes, and prints what it measured.
//
// The load-bearing test is REBUILD EQUIVALENCE: after any sequence of
// incremental edits, the light array must be byte-for-byte identical to a full
// recompute of the same world. That is the only check that actually catches a
// broken removal pass — a naive engine passes every hand-written spot check and
// then leaves a stale halo where a torch used to be. It is run twice: on a small
// hand-built world where the failures are legible, and on a real generated
// 96x128x96 world with 600 random edits where they are not.
//
// build (one line; a trailing backslash here is a -Wcomment warning, and this
// project builds at zero warnings):
//   g++ -std=c++20 -O2 -Wall -Wextra -Isrc test_light.cpp -o test_light.exe -static -static-libgcc -static-libstdc++

#include "world/light.hpp"
#include "world/blockworld.hpp"
#include "rng.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace bench;

static int g_checks = 0;
static void ok(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { std::printf("  FAIL: %s\n", what); std::fflush(stdout); std::abort(); }
}

// A flat test world: bedrock at 0, stone up to groundTop, open sky above.
// Hand-built rather than generated, so every expected number below can be
// worked out on paper.
static void build_flat(BlockWorld& w, int groundTop) {
    for (int z = 0; z < w.n(); ++z)
        for (int x = 0; x < w.n(); ++x) {
            w.set(x, 0, z, Bedrock);
            for (int y = 1; y <= groundTop; ++y) w.set(x, y, z, Stone);
            for (int y = groundTop + 1; y < w.height(); ++y) w.set(x, y, z, Air);
        }
    for (int z = 0; z < w.n(); ++z) for (int x = 0; x < w.n(); ++x) w.refresh_column(x, z);
}

// Byte-for-byte against a full recompute. Returns the number of differing cells
// and reports the first one, because "3 cells wrong" and "300000 cells wrong"
// are different bugs.
static std::size_t diff_vs_rebuild(const BlockWorld& w, const LightEngine& inc, const char* tag) {
    LightEngine fresh(w);
    fresh.rebuild();
    const auto& a = inc.raw();
    const auto& b = fresh.raw();
    std::size_t bad = 0;
    std::size_t first = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) { if (bad == 0) first = i; ++bad; }
    if (bad) {
        const int n = w.n();
        const int x = int(first % std::size_t(n));
        const int z = int((first / std::size_t(n)) % std::size_t(n));
        const int y = int(first / (std::size_t(n) * std::size_t(n)));
        std::printf("  %s: %zu cells differ; first at (%d,%d,%d) inc sky=%d blk=%d "
                    "rebuild sky=%d blk=%d\n",
                    tag, bad, x, y, z, a[first] >> 4, a[first] & 15, b[first] >> 4, b[first] & 15);
    }
    return bad;
}

// ── 1. published constants ──────────────────────────────────────────────────
static void test_constants() {
    std::printf("[1] constants (Minecraft Java Edition)\n");

    ok(kLightMax == 15, "kLightMax == 15");
    std::printf("  light levels          : 0..%d          [minecraft.wiki/w/Light]\n", kLightMax);

    ok(kTorchLight == 14, "torch 14");
    ok(kLavaLight == 15, "lava 15");
    ok(kGlowstoneLight == 15, "glowstone 15");
    ok(kLitFurnaceLight == 13, "lit furnace 13");
    std::printf("  emission torch        : %d             [wiki/Light, Java]\n", kTorchLight);
    std::printf("  emission lava         : %d             [wiki/Light, Java]\n", kLavaLight);
    std::printf("  emission glowstone    : %d             [wiki/Light, Java]\n", kGlowstoneLight);
    std::printf("  emission lit furnace  : %d             [wiki/Furnace, Java]\n", kLitFurnaceLight);

    ok(light_emission(Torch) == kTorchLight, "light_emission(Torch)");
    ok(light_emission(Lava) == kLavaLight, "light_emission(Lava)");
    ok(light_emission(Stone) == 0, "stone emits nothing");
    ok(light_emission(Furnace) == 0, "unlit furnace emits nothing");

    ok(light_opacity(Air) == 0, "air opacity 0");
    ok(light_opacity(Torch) == 0, "torch opacity 0");
    ok(light_opacity(Wheat) == 0, "crops opacity 0");
    ok(light_opacity(Chest) == 0, "chest opacity 0");
    ok(light_opacity(Water) == 1, "water opacity 1");
    ok(light_opacity(Leaves) == 1, "leaves opacity 1");
    ok(light_opacity(Lava) == 1, "lava opacity 1");
    ok(light_opacity(Stone) == 15, "stone opaque");
    ok(light_opacity(Bedrock) == 15, "bedrock opaque");
    std::printf("  opacity 0 / 1 / 15    : air,torch,crop,chest / water,leaves,lava / full cubes\n");

    ok(kSkyDarkenMidnight == 11, "midnight darkening 11");
    ok(15 - kSkyDarkenMidnight == 4, "sky 15 -> internal 4 at midnight");
    std::printf("  midnight sky darken   : %d  (wiki: sky 15 reads as internal 4)\n",
                kSkyDarkenMidnight);

    ok(kHostileMaxBlockLight == 0, "hostile needs block light 0");
    ok(kHostileMaxInternalSky == 7, "hostile needs internal sky <= 7");
    ok(kSpawnLightRollSides == 8, "spawn roll is 0..7");
    std::printf("  hostile spawn (1.18+) : block light == %d and internal sky <= %d\n",
                kHostileMaxBlockLight, kHostileMaxInternalSky);
}

// ── 2. block light falls off by taxicab distance ────────────────────────────
static void test_block_light_falloff() {
    std::printf("[2] block light: taxicab falloff from a torch\n");
    const int G = 10;
    BlockWorld w(48, 64);
    build_flat(w, G);
    LightEngine L(w);
    L.rebuild();

    const int tx = 24, ty = G + 1, tz = 24;
    w.set(tx, ty, tz, Torch);
    L.block_changed(tx, ty, tz);

    ok(L.block_light(tx, ty, tz) == 14, "torch cell is 14");
    // minecraft.wiki/w/Light, verbatim: "If a torch with light level 14 is placed
    // on the floor, the light level of the adjacent floor blocks in all four
    // directions is 13, while the diagonal blocks in all four directions have a
    // light level of 12."
    for (int d = 0; d < 4; ++d) {
        const int ax = tx + (d == 0) - (d == 1);
        const int az = tz + (d == 2) - (d == 3);
        ok(L.block_light(ax, ty, az) == 13, "adjacent is 13");
    }
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sz = -1; sz <= 1; sz += 2)
            ok(L.block_light(tx + sx, ty, tz + sz) == 12, "diagonal is 12");
    std::printf("  torch=%d adjacent=%d diagonal=%d   (wiki worked example)\n",
                L.block_light(tx, ty, tz), L.block_light(tx + 1, ty, tz),
                L.block_light(tx + 1, ty, tz + 1));

    std::printf("  falloff along +x      :");
    for (int d = 0; d <= 15; ++d) {
        const int got = L.block_light(tx + d, ty, tz);
        const int want = (14 - d > 0) ? 14 - d : 0;
        ok(got == want, "taxicab falloff");
        std::printf(" %d", got);
    }
    std::printf("\n  reach                 : last lit cell at d=13 (value 1), d=14 is 0\n");
    ok(L.block_light(tx + 13, ty, tz) == 1, "d=13 is 1");
    ok(L.block_light(tx + 14, ty, tz) == 0, "d=14 is 0");
}

// ── 3. sky light: free fall, sideways cost, filters ─────────────────────────
static void test_sky_light() {
    std::printf("[3] sky light: free fall, sideways cost, water filter\n");
    const int G = 10;
    BlockWorld w(48, 64);
    build_flat(w, G);
    LightEngine L(w);
    L.rebuild();

    for (int y = G + 1; y < w.height(); ++y) ok(L.sky(5, y, 5) == 15, "open column is 15");
    ok(L.sky(5, G, 5) == 0, "opaque ground top is 0");
    ok(L.sky_top(5, 5) == G, "sky_top is the ground");
    std::printf("  open column           : sky=15 for y=%d..%d, ground top y=%d is 0\n",
                G + 1, w.height() - 1, G);

    // A single 1x1 roof tile 9 blocks up. The cell under it is lit only from the
    // four sides, which cost one level: 14.
    w.set(20, 20, 20, Stone);
    L.block_changed(20, 20, 20);
    ok(L.sky(20, 20, 20) == 0, "roof tile itself is dark");
    ok(L.sky(20, 19, 20) == 14, "one block under a 1x1 roof is 14");
    ok(L.sky(20, 18, 20) == 14, "and so is the next one down (side-lit)");
    std::printf("  under a 1x1 roof      : %d  (side-lit, one level lost)\n", L.sky(20, 19, 20));

    // A 7x7 roof. The centre is 4 steps from the nearest open column: 15-4 = 11.
    for (int dz = -3; dz <= 3; ++dz)
        for (int dx = -3; dx <= 3; ++dx) {
            w.set(30 + dx, 20, 30 + dz, Stone);
            L.block_changed(30 + dx, 20, 30 + dz);
        }
    ok(L.sky(30, 19, 30) == 11, "centre of a 7x7 roof is 11");
    ok(L.sky(27, 19, 30) == 14, "edge under a 7x7 roof is 14");
    std::printf("  under a 7x7 roof      : edge=%d ... centre=%d  (15 - taxicab to open sky)\n",
                L.sky(27, 19, 30), L.sky(30, 19, 30));
    ok(diff_vs_rebuild(w, L, "roofs") == 0, "roofs match a full rebuild");

    // Water: opacity 1, so the free fall stops at the surface and every further
    // block costs one. This is why the sea gets dark with depth.
    BlockWorld ww(48, 64);
    build_flat(ww, G);
    for (int y = G + 1; y <= G + 6; ++y) ww.set(10, y, 10, Water);
    // walls, so the column is not simply re-lit from the side
    for (int y = G + 1; y <= G + 6; ++y) {
        ww.set(9, y, 10, Stone); ww.set(11, y, 10, Stone);
        ww.set(10, y, 9, Stone); ww.set(10, y, 11, Stone);
    }
    LightEngine LW(ww);
    LW.rebuild();
    // Six water blocks, y = G+1 .. G+6. k counts down from the surface; k=6
    // would be the stone floor, which is opaque and 0 — not part of the claim.
    std::printf("  water column top-down :");
    for (int k = 0; k <= 5; ++k) {
        const int y = G + 6 - k;
        const int got = LW.sky(10, y, 10);
        const int want = 14 - k;      // surface 14, then -1 per block
        ok(got == want, "water attenuates sky by 1 per block");
        std::printf(" %d", got);
    }
    std::printf("   (surface 14, then -1/block; light never recovers to 15)\n");
    ok(LW.sky(10, G + 7, 10) == 15, "air above the water is still 15");
    ok(LW.block_light(10, G + 3, 10) == 0, "water does not emit");
}

// ── 4. removal: the part naive engines get wrong ────────────────────────────
static void test_removal() {
    std::printf("[4] removal (the decrease queue)\n");
    const int G = 10;
    BlockWorld w(48, 64);
    build_flat(w, G);
    LightEngine L(w);
    L.rebuild();

    // 4a. one torch, placed then broken: everything must return to zero.
    w.set(24, G + 1, 24, Torch);   L.block_changed(24, G + 1, 24);
    ok(L.block_light(24, G + 1, 24) == 14, "torch lit");
    w.set(24, G + 1, 24, Air);     L.block_changed(24, G + 1, 24);
    std::size_t lit = 0;
    for (int y = 0; y < w.height(); ++y)
        for (int z = 0; z < w.n(); ++z)
            for (int x = 0; x < w.n(); ++x) if (L.block_light(x, y, z) != 0) ++lit;
    ok(lit == 0, "no block light survives the torch");
    std::printf("  torch placed+broken   : %zu lit cells left (want 0)\n", lit);

    // 4b. two torches six apart, break one. The classic failure: the removal
    // wave erases the survivor's field and never puts it back.
    w.set(20, G + 1, 24, Torch);   L.block_changed(20, G + 1, 24);
    w.set(26, G + 1, 24, Torch);   L.block_changed(26, G + 1, 24);
    ok(L.block_light(23, G + 1, 24) == 11, "midpoint is 11 with both torches");
    w.set(20, G + 1, 24, Air);     L.block_changed(20, G + 1, 24);
    const int mid = L.block_light(23, G + 1, 24);
    ok(mid == 11, "midpoint still 11 from the surviving torch");
    ok(L.block_light(26, G + 1, 24) == 14, "survivor still 14");
    ok(L.block_light(20, G + 1, 24) == 8, "dead torch's cell now lit only by the survivor");
    std::printf("  break 1 of 2 torches  : midpoint=%d (want 11), survivor=%d, "
                "dead site=%d (want 8 = 14-6)\n",
                mid, L.block_light(26, G + 1, 24), L.block_light(20, G + 1, 24));
    ok(diff_vs_rebuild(w, L, "two torches") == 0, "two-torch removal matches rebuild");

    // 4c. sky column collapse and restore. This is the case the extra DOWN/15
    // removal rule exists for: without it the shaft stays at 15 under a roof.
    BlockWorld s(48, 64);
    build_flat(s, G);
    // a 5x5 pit dug from the surface down to y=2, so there is a deep open shaft
    for (int dz = -2; dz <= 2; ++dz)
        for (int dx = -2; dx <= 2; ++dx)
            for (int y = 2; y <= G; ++y) s.set(24 + dx, y, 24 + dz, Air);
    LightEngine LS(s);
    LS.rebuild();
    ok(LS.sky(24, 2, 24) == 15, "bottom of an open shaft is 15");
    ok(LS.sky_top(24, 24) == 1, "sky_top is the last stone under the pit");
    std::printf("  open 5x5 shaft        : floor sky=%d, sky_top=%d\n",
                LS.sky(24, 2, 24), LS.sky_top(24, 24));

    // lid the whole pit
    for (int dz = -2; dz <= 2; ++dz)
        for (int dx = -2; dx <= 2; ++dx) {
            s.set(24 + dx, G + 1, 24 + dz, Stone);
            LS.block_changed(24 + dx, G + 1, 24 + dz);
        }
    ok(LS.sky(24, 2, 24) == 0, "lidded shaft floor goes dark");
    ok(LS.sky(24, G, 24) == 0, "lidded shaft top goes dark");
    std::printf("  lidded                : floor sky=%d, just under the lid=%d (want 0,0)\n",
                LS.sky(24, 2, 24), LS.sky(24, G, 24));
    ok(diff_vs_rebuild(s, LS, "lidded shaft") == 0, "lidded shaft matches rebuild");

    // take the lid off again
    for (int dz = -2; dz <= 2; ++dz)
        for (int dx = -2; dx <= 2; ++dx) {
            s.set(24 + dx, G + 1, 24 + dz, Air);
            LS.block_changed(24 + dx, G + 1, 24 + dz);
        }
    ok(LS.sky(24, 2, 24) == 15, "shaft floor is 15 again");
    std::printf("  unlidded              : floor sky=%d (want 15)\n", LS.sky(24, 2, 24));
    ok(diff_vs_rebuild(s, LS, "unlidded shaft") == 0, "unlidded shaft matches rebuild");

    // 4d. The worst case the complexity claim rests on: a 1x1 shaft 99 blocks
    // deep, capped by a single block. One edit has to collapse the whole column
    // and then refill it from the sides. This is the only update in the engine
    // whose cost scales with world HEIGHT rather than with the light radius, so
    // it is the number worth measuring rather than asserting from theory.
    BlockWorld d(48, 128);
    build_flat(d, 100);
    for (int y = 2; y <= 100; ++y) d.set(24, y, 24, Air);
    LightEngine LD(d);
    LD.rebuild();
    ok(LD.sky(24, 2, 24) == 15, "deep shaft floor is 15");
    d.set(24, 101, 24, Stone);
    LD.block_changed(24, 101, 24);
    const std::uint64_t capNodes = LD.last_nodes();
    // The shaft is walled in stone on all four sides, so nothing refills it:
    // every one of the 99 cells must go to 0, which is exactly what the naive
    // removal rule fails to do (15 < 15 is false, so it calls them survivors).
    ok(LD.sky(24, 2, 24) == 0, "capped deep shaft goes dark at the bottom");
    ok(LD.sky(24, 100, 24) == 0, "and dark at the top, right under the cap");
    int stillLit = 0;
    for (int y = 2; y <= 100; ++y) if (LD.sky(24, y, 24) != 0) ++stillLit;
    ok(stillLit == 0, "no cell of the capped shaft keeps its sky light");
    ok(diff_vs_rebuild(d, LD, "capped deep shaft") == 0, "capped deep shaft matches rebuild");
    d.set(24, 101, 24, Air);
    LD.block_changed(24, 101, 24);
    const std::uint64_t uncapNodes = LD.last_nodes();
    ok(LD.sky(24, 2, 24) == 15, "uncapped deep shaft is 15 again");
    ok(diff_vs_rebuild(d, LD, "uncapped deep shaft") == 0, "uncapped deep shaft matches rebuild");
    std::printf("  99-deep 1x1 shaft     : cap costs %llu cells, uncap %llu "
                "(world is %zu cells)\n",
                (unsigned long long)capNodes, (unsigned long long)uncapNodes, d.volume());
    ok(capNodes < 30000 && uncapNodes < 30000, "column collapse stays bounded by h*15^2");
}

// ── 5. the hostile-spawn predicate ──────────────────────────────────────────
static void test_spawn() {
    std::printf("[5] hostile spawn predicate (Java 1.18+: block light 0)\n");
    const int G = 10;
    BlockWorld w(48, 64);
    build_flat(w, G);
    // a sealed 1x2 corridor in the stone, floor y=3, headroom y=4 and y=5
    for (int x = 8; x <= 40; ++x) { w.set(x, 4, 8, Air); w.set(x, 5, 8, Air); }
    LightEngine L(w);
    L.rebuild();

    ok(L.sky(20, 4, 8) == 0, "sealed corridor has no sky light");
    ok(L.block_light(20, 4, 8) == 0, "and no block light");
    ok(L.hostile_can_spawn(20, 4, 8, kSkyDarkenNoon), "dark cave spawns at noon");
    ok(L.hostile_can_spawn(20, 4, 8, kSkyDarkenMidnight), "dark cave spawns at midnight");
    std::printf("  sealed cave, no torch : sky=0 block=0 -> spawn=%d (want 1, day or night)\n",
                int(L.hostile_can_spawn(20, 4, 8, kSkyDarkenNoon)));

    w.set(8, 4, 8, Torch);
    L.block_changed(8, 4, 8);
    std::printf("  torch at x=8, spawn ok:");
    for (int d = 0; d <= 15; ++d) {
        const bool sp = L.light_allows_hostile_spawn(8 + d, 4, 8, kSkyDarkenNoon);
        const int bl = L.block_light(8 + d, 4, 8);
        ok(sp == (bl == 0), "spawn allowed exactly where block light is 0");
        std::printf(" %d", int(sp));
    }
    std::printf("\n");
    ok(L.block_light(21, 4, 8) == 1 && !L.light_allows_hostile_spawn(21, 4, 8, 0),
       "13 blocks out: light 1, no spawn");
    ok(L.block_light(22, 4, 8) == 0 && L.light_allows_hostile_spawn(22, 4, 8, 0),
       "14 blocks out: light 0, spawn");
    std::printf("  a torch (14) suppresses spawning out to taxicab distance 13; "
                "at 14 the light is 0 again\n");

    // Surface: sky light does not change at night, the internal value does.
    const int sx = 40, sy = G + 1, sz = 40;
    ok(L.sky(sx, sy, sz) == 15, "surface sky is 15 always");
    ok(L.internal_sky(sx, sy, sz, kSkyDarkenNoon) == 15, "noon internal sky 15");
    ok(L.internal_sky(sx, sy, sz, kSkyDarkenMidnight) == 4, "midnight internal sky 4");
    ok(!L.hostile_can_spawn(sx, sy, sz, kSkyDarkenNoon), "no surface spawn at noon");
    ok(L.hostile_can_spawn(sx, sy, sz, kSkyDarkenMidnight), "surface spawns at midnight");
    std::printf("  surface sky=15        : internal noon=%d -> spawn=%d | "
                "internal midnight=%d -> spawn=%d\n",
                L.internal_sky(sx, sy, sz, kSkyDarkenNoon),
                int(L.hostile_can_spawn(sx, sy, sz, kSkyDarkenNoon)),
                L.internal_sky(sx, sy, sz, kSkyDarkenMidnight),
                int(L.hostile_can_spawn(sx, sy, sz, kSkyDarkenMidnight)));

    // Footing. A mob needs a solid top surface below and two free blocks.
    ok(!L.spawnable_footing(20, 5, 8), "no headroom one below the ceiling");
    ok(L.spawnable_footing(20, 4, 8), "corridor floor is spawnable");
    ok(!L.spawnable_footing(20, 2, 8), "solid stone is not spawnable");
    std::printf("  footing               : corridor floor=1, head slot=0, solid rock=0\n");

    // The randomised roll, driven by an Rng the caller owns: same seed, same run.
    Rng a(0xBEEF1234ull), b(0xBEEF1234ull);
    int hitsA = 0, hitsB = 0;
    for (int i = 0; i < 1000; ++i) {
        hitsA += int(L.roll_hostile_spawn(30, 4, 8, kSkyDarkenNoon, a));
        hitsB += int(L.roll_hostile_spawn(30, 4, 8, kSkyDarkenNoon, b));
    }
    ok(hitsA == hitsB, "same seed, same rolls");
    ok(hitsA == 1000, "internal light 0 always passes the 0..7 roll");
    std::printf("  roll (light 0, n=1000): %d successes, reproducible=%d\n",
                hitsA, int(hitsA == hitsB));
    int hitsLit = 0;
    Rng c(0xBEEF1234ull);
    for (int i = 0; i < 1000; ++i) hitsLit += int(L.roll_hostile_spawn(21, 4, 8, 0, c));
    ok(hitsLit == 0, "block light 1 never spawns in 1.18+");
    std::printf("  roll (block light 1)  : %d successes (want 0)\n", hitsLit);
}

// ── 6. soak: real world, random edits, must equal a full rebuild ────────────
static void test_soak_and_performance() {
    std::printf("[6] soak + performance on a generated 96x128x96 world\n");
    BlockWorld w(96, 128);
    w.generate(20260814ull);
    const std::size_t vol = w.volume();

    LightEngine L(w);
    auto t0 = std::chrono::steady_clock::now();
    L.rebuild();
    auto t1 = std::chrono::steady_clock::now();
    const double rebuildMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("  volume                : %zu cells (%.2f MB of light)\n", vol, double(vol) / 1e6);
    std::printf("  full rebuild          : %.1f ms, %llu cells written\n",
                rebuildMs, (unsigned long long)L.last_nodes());

    // Sanity on the generated world before we start breaking it.
    std::size_t sky15 = 0, litByBlocks = 0;
    for (int z = 0; z < w.n(); ++z)
        for (int x = 0; x < w.n(); ++x)
            if (L.sky(x, w.height() - 1, z) == 15) ++sky15;
    for (std::size_t i = 0; i < vol; ++i) if ((L.raw()[i] & 15) != 0) ++litByBlocks;
    ok(sky15 == std::size_t(w.n()) * std::size_t(w.n()), "every column sees the sky at the top");
    ok(litByBlocks > 0, "generated lava lights something");
    std::printf("  top layer all sky 15  : %zu / %d columns\n",
                sky15, w.n() * w.n());
    std::printf("  block-lit cells       : %zu (from generated lava at Y1-10)\n", litByBlocks);

    // 600 random edits with an explicitly seeded Rng, then byte-compare.
    Rng r(0x5EED0F11ull);
    const std::uint8_t palette[6] = { Air, Stone, Torch, Water, Leaves, Lava };
    std::uint64_t maxNodes = 0, sumNodes = 0;
    const int kEdits = 600;
    auto t2 = std::chrono::steady_clock::now();
    for (int e = 0; e < kEdits; ++e) {
        const int x = int(r.below(std::uint32_t(w.n())));
        const int z = int(r.below(std::uint32_t(w.n())));
        const int y = 1 + int(r.below(std::uint32_t(w.height() - 2)));
        w.set(x, y, z, palette[r.below(6)]);
        w.refresh_column(x, z);
        L.block_changed(x, y, z);
        maxNodes = std::max(maxNodes, L.last_nodes());
        sumNodes += L.last_nodes();
    }
    auto t3 = std::chrono::steady_clock::now();
    const double editMs = std::chrono::duration<double, std::milli>(t3 - t2).count();

    std::printf("  %d random edits      : %.2f ms total, %.4f ms each\n", kEdits, editMs,
                editMs / kEdits);
    std::printf("  cells written / edit  : mean %.1f, max %llu  (volume is %zu, "
                "so max is %.4f%% of it)\n",
                double(sumNodes) / kEdits, (unsigned long long)maxNodes, vol,
                100.0 * double(maxNodes) / double(vol));
    ok(maxNodes < vol / 8, "an incremental update is nowhere near O(volume)");

    const std::size_t bad = diff_vs_rebuild(w, L, "soak");
    ok(bad == 0, "600 incremental edits equal a full rebuild, byte for byte");
    std::printf("  incremental == rebuild: %zu differing cells out of %zu\n", bad, vol);

    // Torch churn: the operation an agent actually performs, timed on its own.
    Rng r2(0xC0FFEEull);
    std::uint64_t churnMax = 0, churnSum = 0;
    const int kChurn = 2000;
    auto t4 = std::chrono::steady_clock::now();
    for (int e = 0; e < kChurn; ++e) {
        const int x = int(r2.below(std::uint32_t(w.n())));
        const int z = int(r2.below(std::uint32_t(w.n())));
        int y = w.surface(x, z) + 1;
        if (y < 1 || y >= w.height() - 1) y = w.height() / 2;
        const std::uint8_t was = w.at(x, y, z);
        w.set(x, y, z, Torch);  L.block_changed(x, y, z);
        churnMax = std::max(churnMax, L.last_nodes()); churnSum += L.last_nodes();
        w.set(x, y, z, was);    L.block_changed(x, y, z);
        churnMax = std::max(churnMax, L.last_nodes()); churnSum += L.last_nodes();
    }
    auto t5 = std::chrono::steady_clock::now();
    const double churnMs = std::chrono::duration<double, std::milli>(t5 - t4).count();
    std::printf("  %d torch place+break: %.2f ms (%.4f ms per update), "
                "mean %.1f cells, max %llu\n",
                kChurn, churnMs, churnMs / (2 * kChurn),
                double(churnSum) / (2 * kChurn), (unsigned long long)churnMax);
    ok(churnMax < 40000, "a torch update stays inside its light radius");
    ok(diff_vs_rebuild(w, L, "churn") == 0, "torch churn equals a full rebuild");
}

// ── 7. does the per-edit cost care how big the world is? ────────────────────
// The whole complexity claim is "an update is bounded by the light radius, not
// by the world". That is only worth believing if it is measured at both ends of
// the size range this project supports.
static void test_scaling() {
    std::printf("[7] per-edit cost vs world size (the claim: it does not scale with n)\n");
    const int sizes[2] = { 96, 256 };
    double perEdit[2] = { 0.0, 0.0 };
    double meanCells[2] = { 0.0, 0.0 };
    for (int s = 0; s < 2; ++s) {
        BlockWorld w(sizes[s], 128);
        w.generate(4242ull);
        LightEngine L(w);
        auto t0 = std::chrono::steady_clock::now();
        L.rebuild();
        auto t1 = std::chrono::steady_clock::now();

        Rng r(0xA11CE0ull);
        std::uint64_t sum = 0;
        const int kEdits = 4000;
        auto t2 = std::chrono::steady_clock::now();
        for (int e = 0; e < kEdits; ++e) {
            const int x = int(r.below(std::uint32_t(w.n())));
            const int z = int(r.below(std::uint32_t(w.n())));
            int y = w.surface(x, z) + 1;
            if (y < 1 || y >= w.height() - 1) y = w.height() / 2;
            w.set(x, y, z, Torch); L.block_changed(x, y, z);
            w.set(x, y, z, Air);   L.block_changed(x, y, z);
            sum += L.last_nodes();
        }
        auto t3 = std::chrono::steady_clock::now();
        perEdit[s] = std::chrono::duration<double, std::milli>(t3 - t2).count() / (2 * kEdits);
        meanCells[s] = double(sum) / kEdits;
        std::printf("  %3dx128x%-3d (%8zu cells): rebuild %6.1f ms | %.4f ms/edit | "
                    "mean %.0f cells/edit\n",
                    sizes[s], sizes[s], w.volume(),
                    std::chrono::duration<double, std::milli>(t1 - t0).count(),
                    perEdit[s], meanCells[s]);
    }
    // 256^2 is 7.1x the area of 96^2. If the update were O(volume) this ratio
    // would be ~7; the assertion is deliberately loose (cache behaviour at 8 MB
    // is genuinely worse than at 1 MB) but nowhere near linear.
    const double ratio = perEdit[1] / perEdit[0];
    std::printf("  world grew 7.1x in volume; time per edit grew %.2fx "
                "(O(volume) would be ~7.1x)\n", ratio);
    ok(ratio < 3.0, "update cost does not scale with world volume");
}

int main() {
    std::printf("light.hpp — sky light and block light, Minecraft Java Edition rules\n\n");
    test_constants();          std::printf("\n");
    test_block_light_falloff();std::printf("\n");
    test_sky_light();          std::printf("\n");
    test_removal();            std::printf("\n");
    test_spawn();              std::printf("\n");
    test_soak_and_performance();std::printf("\n");
    test_scaling();
    std::printf("\nall %d assertions passed\n", g_checks);
    return 0;
}
