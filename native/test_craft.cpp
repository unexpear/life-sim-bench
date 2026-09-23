// test_craft.cpp — asserts every published constant in world/craft.hpp and
// prints what it measured.
//
// The house rule is that a claim is worth what its test is worth, so this file
// does not merely re-state the tables. Where craft.hpp implements a formula it
// is checked against the independently published table (armour durability is a
// product; the damage-reduction formula has two published forms that must agree;
// block-break times must reproduce the wiki's stone and obsidian rows). Where a
// number is a bare constant it is asserted and printed so a regression is loud.
//
// It also measures the two places this file knowingly disagrees with the rest of
// the project — gold's speed-versus-harvest split, and blockworld's rounding of
// break times where the game ceils — so those divergences are on the record with
// a size attached rather than being discovered later as "a bug".
//
// build:
//   g++ -std=c++20 -O2 -Wall -Wextra -Isrc test_craft.cpp -o test_craft.exe
//       -static -static-libgcc -static-libstdc++
// (all on one line; the continuation is split here only for reading)

#include "world/craft.hpp"
#include "world/blockworld.hpp"
#include "rng.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace bench;
namespace cr = bench::craft;

static int g_checks = 0;
static int g_fail   = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            std::printf("  FAIL line %d: %s\n", __LINE__, #cond);              \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                  \
    do {                                                                       \
        ++g_checks;                                                            \
        const double _a = double(a), _b = double(b);                           \
        if (!(std::fabs(_a - _b) <= (eps))) {                                  \
            std::printf("  FAIL line %d: %s = %.6f, expected %.6f\n",          \
                        __LINE__, #a, _a, _b);                                 \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

static void rule(const char* title) {
    std::printf("\n== %s ", title);
    for (int i = int(std::string(title).size()); i < 66; ++i) std::putchar('=');
    std::putchar('\n');
}

// ── 1. tool statistics ──────────────────────────────────────────────────────

static void test_tools() {
    rule("tool durability, mining speed, harvest level");

    // minecraft.wiki, Durability / Breaking / Golden Pickaxe (Java Edition).
    const int   dur[6]   = { 59, 131, 250, 32, 1561, 2031 };
    const float speed[6] = { 2.0f, 4.0f, 6.0f, 12.0f, 8.0f, 9.0f };
    const int   level[6] = { 0, 1, 2, 0, 3, 4 };

    std::printf("  %-10s %10s %8s %8s\n", "material", "durability", "speed", "level");
    for (int m = 0; m < cr::kMaterials; ++m) {
        const cr::Material mat = cr::Material(m);
        std::printf("  %-10s %10d %8.0f %8d\n", cr::material_name(mat),
                    cr::tool_durability(mat), double(cr::tool_mining_speed(mat)),
                    cr::tool_harvest_level(mat));
        CHECK(cr::tool_durability(mat) == dur[m]);
        CHECK_NEAR(cr::tool_mining_speed(mat), speed[m], 1e-6);
        CHECK(cr::tool_harvest_level(mat) == level[m]);
    }

    // The two facts about gold that a single "tier" integer cannot hold.
    CHECK(cr::tool_mining_speed(cr::Material::Gold)
        > cr::tool_mining_speed(cr::Material::Diamond));
    CHECK(cr::tool_harvest_level(cr::Material::Gold)
       == cr::tool_harvest_level(cr::Material::Wood));
    CHECK(cr::tool_durability(cr::Material::Gold)
        < cr::tool_durability(cr::Material::Wood));
    std::printf("  gold: speed %.0f (fastest, beats diamond's %.0f) but harvest level %d"
                " (== wood) and %d uses (< wood's %d)\n",
                double(cr::tool_mining_speed(cr::Material::Gold)),
                double(cr::tool_mining_speed(cr::Material::Diamond)),
                cr::tool_harvest_level(cr::Material::Gold),
                cr::tool_durability(cr::Material::Gold),
                cr::tool_durability(cr::Material::Wood));

    // blockworld.hpp numbers its tiers from the bare hand, so every level is
    // one higher over there. If that ever stops being true, drops_for() below
    // silently starts lying.
    CHECK(cr::blockworld_tier(cr::Material::Wood)    == 1);
    CHECK(cr::blockworld_tier(cr::Material::Stone)   == 2);
    CHECK(cr::blockworld_tier(cr::Material::Iron)    == 3);
    CHECK(cr::blockworld_tier(cr::Material::Diamond) == 4);
    CHECK(cr::blockworld_tier(cr::Material::Gold)    == 1);

    rule("tool attack damage and attack speed");
    // minecraft.wiki, Damage (Java Edition).
    const float dmg[5][6] = {                        // wood stone iron gold diam neth
        { 2.0f, 3.0f, 4.0f, 2.0f, 5.0f, 6.0f },      // pickaxe
        { 7.0f, 9.0f, 9.0f, 7.0f, 9.0f, 10.0f },     // axe
        { 2.5f, 3.5f, 4.5f, 2.5f, 5.5f, 6.5f },      // shovel
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },      // hoe
        { 4.0f, 5.0f, 6.0f, 4.0f, 7.0f, 8.0f },      // sword
    };
    const float spd[5][6] = {
        { 1.2f, 1.2f, 1.2f, 1.2f, 1.2f, 1.2f },
        { 0.8f, 0.8f, 0.9f, 1.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 1.0f, 2.0f, 3.0f, 1.0f, 4.0f, 4.0f },
        { 1.6f, 1.6f, 1.6f, 1.6f, 1.6f, 1.6f },
    };
    std::printf("  damage (attack speed) by kind x material\n");
    std::printf("  %-8s", "");
    for (int m = 0; m < cr::kMaterials; ++m)
        std::printf("%14s", cr::material_name(cr::Material(m)));
    std::putchar('\n');
    for (int k = 0; k < cr::kToolKinds; ++k) {
        std::printf("  %-8s", cr::tool_kind_name(cr::ToolKind(k)));
        for (int m = 0; m < cr::kMaterials; ++m) {
            const cr::ToolKind K = cr::ToolKind(k);
            const cr::Material M = cr::Material(m);
            std::printf("%9.1f(%.1f)", double(cr::tool_attack_damage(K, M)),
                        double(cr::tool_attack_speed(K, M)));
            CHECK_NEAR(cr::tool_attack_damage(K, M), dmg[k][m], 1e-5);
            CHECK_NEAR(cr::tool_attack_speed(K, M), spd[k][m], 1e-5);
        }
        std::putchar('\n');
    }
    // Damage per second is where the axe/sword tradeoff actually lives.
    const float swordDps = cr::tool_attack_damage(cr::ToolKind::Sword, cr::Material::Diamond)
                         * cr::tool_attack_speed(cr::ToolKind::Sword, cr::Material::Diamond);
    const float axeDps   = cr::tool_attack_damage(cr::ToolKind::Axe, cr::Material::Diamond)
                         * cr::tool_attack_speed(cr::ToolKind::Axe, cr::Material::Diamond);
    std::printf("  measured diamond sword DPS %.2f vs diamond axe DPS %.2f\n",
                double(swordDps), double(axeDps));
    CHECK(swordDps > axeDps);   // 11.2 vs 9.0
}

// ── 2. armour ───────────────────────────────────────────────────────────────

static void test_armour() {
    rule("armour points, toughness, durability");

    // minecraft.wiki, Armor / Durability (Java Edition).
    const int pts[6][4] = {
        { 1, 3, 2, 1 }, { 2, 5, 4, 1 }, { 2, 6, 5, 2 },
        { 2, 5, 3, 1 }, { 3, 8, 6, 3 }, { 3, 8, 6, 3 },
    };
    const int dur[6][4] = {
        {  55,  80,  75,  65 },   // leather
        { 165, 240, 225, 195 },   // chainmail
        { 165, 240, 225, 195 },   // iron
        {  77, 112, 105,  91 },   // gold
        { 363, 528, 495, 429 },   // diamond
        { 407, 592, 555, 481 },   // netherite
    };
    const int setPts[6]     = { 7, 12, 15, 11, 20, 20 };
    const float setTough[6] = { 0, 0, 0, 0, 8.0f, 12.0f };

    std::printf("  %-10s %28s %28s %6s %6s\n", "material",
                "points  (H  C  L  B)", "durability (H   C   L   B)", "set", "tough");
    for (int m = 0; m < cr::kArmourMaterials; ++m) {
        const cr::ArmourMaterial M = cr::ArmourMaterial(m);
        std::printf("  %-10s        ", cr::armour_material_name(M));
        for (int s = 0; s < cr::kArmourSlots; ++s) {
            std::printf("%3d", cr::armour_points(M, cr::ArmourSlot(s)));
            CHECK(cr::armour_points(M, cr::ArmourSlot(s)) == pts[m][s]);
        }
        std::printf("          ");
        for (int s = 0; s < cr::kArmourSlots; ++s) {
            std::printf("%4d", cr::armour_durability(M, cr::ArmourSlot(s)));
            // This is the real assertion: craft.hpp stores factor x multiplier,
            // NOT the 24 published products. If either half is wrong the whole
            // row goes wrong at once and is impossible to miss.
            CHECK(cr::armour_durability(M, cr::ArmourSlot(s)) == dur[m][s]);
        }
        std::printf(" %5d %6.0f\n", cr::full_set_points(M),
                    double(cr::full_set_toughness(M)));
        CHECK(cr::full_set_points(M) == setPts[m]);
        CHECK_NEAR(cr::full_set_toughness(M), setTough[m], 1e-5);
    }
    std::printf("  armour durability factors %d/%d/%d/%d/%d/%d x slot multipliers"
                " %d/%d/%d/%d reproduce all 24 published values\n",
                cr::armour_durability_factor(cr::ArmourMaterial::Leather),
                cr::armour_durability_factor(cr::ArmourMaterial::Chainmail),
                cr::armour_durability_factor(cr::ArmourMaterial::Iron),
                cr::armour_durability_factor(cr::ArmourMaterial::Gold),
                cr::armour_durability_factor(cr::ArmourMaterial::Diamond),
                cr::armour_durability_factor(cr::ArmourMaterial::Netherite),
                cr::armour_slot_multiplier(cr::ArmourSlot::Helmet),
                cr::armour_slot_multiplier(cr::ArmourSlot::Chestplate),
                cr::armour_slot_multiplier(cr::ArmourSlot::Leggings),
                cr::armour_slot_multiplier(cr::ArmourSlot::Boots));

    // Gold sits below chainmail overall despite being the rarer metal — worth
    // asserting, because it looks like a table error until you check it.
    CHECK(cr::full_set_points(cr::ArmourMaterial::Gold)
        < cr::full_set_points(cr::ArmourMaterial::Chainmail));

    // Knockback resistance: 0.1 per netherite piece, 0.4 for a set = the
    // published 40%.
    float kb = 0.0f;
    for (int s = 0; s < cr::kArmourSlots; ++s)
        kb += cr::armour_knockback_resistance(cr::ArmourMaterial::Netherite);
    CHECK_NEAR(kb, 0.4f, 1e-5);
    CHECK_NEAR(cr::armour_knockback_resistance(cr::ArmourMaterial::Diamond), 0.0f, 1e-9);
    std::printf("  netherite knockback resistance: %.1f per piece, %.1f (%.0f%%) for a set\n",
                double(cr::armour_knockback_resistance(cr::ArmourMaterial::Netherite)),
                double(kb), double(kb * 100.0f));

    rule("armour damage reduction formula");
    // Published worked results.
    CHECK_NEAR(cr::damage_after_armour(10.0f, 20.0f, 8.0f), 3.0f, 1e-4);   // full diamond
    CHECK_NEAR(cr::damage_after_armour(10.0f, 15.0f, 0.0f), 6.0f, 1e-4);   // full iron
    CHECK_NEAR(cr::damage_after_armour( 4.0f,  7.0f, 0.0f), 3.2f, 1e-4);   // full leather
    CHECK_NEAR(cr::damage_after_armour(10.0f,  0.0f, 0.0f), 10.0f, 1e-4);  // none
    std::printf("  10 dmg vs full diamond (20 pts, 8 tough) -> %.3f\n",
                double(cr::damage_after_armour(10.0f, 20.0f, 8.0f)));
    std::printf("  10 dmg vs full iron    (15 pts, 0 tough) -> %.3f\n",
                double(cr::damage_after_armour(10.0f, 15.0f, 0.0f)));
    std::printf("   4 dmg vs full leather ( 7 pts, 0 tough) -> %.3f\n",
                double(cr::damage_after_armour(4.0f, 7.0f, 0.0f)));

    // The 4/5 x points floor: a huge hit can never strip armour past 80% of its
    // nominal value.
    const float huge = cr::damage_after_armour(1000.0f, 20.0f, 8.0f);
    CHECK_NEAR(huge, 840.0f, 0.05);
    std::printf("  1000 dmg vs full diamond -> %.1f  (floor holds at %.0f%% reduction"
                " = 4/5 x 20 points)\n", double(huge), double(100.0f - huge / 10.0f));

    // What toughness actually buys, measured rather than asserted from memory.
    const float withT    = cr::damage_after_armour(20.0f, 20.0f, 8.0f);
    const float withoutT = cr::damage_after_armour(20.0f, 20.0f, 0.0f);
    std::printf("  20 dmg vs 20 points: %.2f with 8 toughness, %.2f with 0"
                " -> toughness saves %.2f HP on a big hit\n",
                double(withT), double(withoutT), double(withoutT - withT));
    CHECK(withT < withoutT);
    CHECK_NEAR(withT, 8.0f, 1e-4);
    CHECK_NEAR(withoutT, 12.0f, 1e-4);

    // The wiki publishes the same rule twice, as a subtraction and as a
    // percentage. They must be the same function; a transcription slip in
    // either would otherwise never show up.
    int sweep = 0;
    double worst = 0.0;
    for (int d = 1; d <= 60; ++d)
        for (int p = 0; p <= 20; ++p)
            for (int t = 0; t <= 12; t += 2) {
                const float dmg = float(d) * 0.5f;
                const float a = cr::damage_after_armour(dmg, float(p), float(t));
                const float b = dmg * (1.0f - cr::armour_reduction_fraction(dmg, float(p), float(t)));
                worst = std::max(worst, double(std::fabs(a - b)));
                ++sweep;
            }
    CHECK(worst < 1e-4);
    std::printf("  swept %d (damage, points, toughness) triples: the subtraction form and"
                " the percentage form agree to %.2e\n", sweep, worst);
}

// ── 3. block breaking ───────────────────────────────────────────────────────

static void test_breaking() {
    rule("block breaking: reproducing the published tables");

    // minecraft.wiki, Stone: the whole tool row, in seconds.
    struct Row { cr::Material m; int ticks; double seconds; };
    const Row stone[] = {
        { cr::Material::Wood,      23, 1.15 },
        { cr::Material::Stone,     12, 0.60 },
        { cr::Material::Iron,       8, 0.40 },
        { cr::Material::Gold,       4, 0.20 },
        { cr::Material::Diamond,    6, 0.30 },
        { cr::Material::Netherite,  5, 0.25 },
    };
    std::printf("  stone (hardness 1.5), pickaxe:\n");
    for (const Row& r : stone) {
        const int t = cr::break_ticks_for(Stone, cr::ToolKind::Pickaxe, r.m);
        std::printf("    %-10s %4d ticks = %.2f s   (published %.2f s)\n",
                    cr::material_name(r.m), t, t / 20.0, r.seconds);
        CHECK(t == r.ticks);
        CHECK_NEAR(t / 20.0, r.seconds, 1e-9);
    }
    // By hand, stone is not harvestable, so the divisor is 100 not 30.
    const int byHand = cr::break_ticks(1.5f, cr::kBareHandSpeed, false);
    std::printf("    %-10s %4d ticks = %.2f s   (published 7.50 s)\n",
                "bare hand", byHand, byHand / 20.0);
    CHECK(byHand == 150);

    // minecraft.wiki, Obsidian: hardness 50, diamond 9.4 s, netherite 8.35 s.
    const int obsD = cr::break_ticks_for(Obsidian, cr::ToolKind::Pickaxe, cr::Material::Diamond);
    const int obsN = cr::break_ticks_for(Obsidian, cr::ToolKind::Pickaxe, cr::Material::Netherite);
    std::printf("  obsidian (hardness 50): diamond %d ticks = %.2f s (published 9.40),"
                " netherite %d ticks = %.2f s (published 8.35)\n",
                obsD, obsD / 20.0, obsN, obsN / 20.0);
    CHECK(obsD == 188);
    CHECK(obsN == 167);

    // Coal ore, hardness 3.0, wooden pickaxe: published 2.25 s. 3.0 * 30 / 2 =
    // 45 ticks. (This assertion first read 90, from dropping the speed divisor;
    // the printed seconds disagreed with the published 2.25 immediately, which
    // is the entire reason the measurement is printed next to its source.)
    const int coalW = cr::break_ticks_for(Coal, cr::ToolKind::Pickaxe, cr::Material::Wood);
    std::printf("  coal ore (hardness 3.0) with a wooden pickaxe: %d ticks = %.2f s"
                " (published 2.25)\n", coalW, coalW / 20.0);
    CHECK(coalW == 45);
    CHECK_NEAR(coalW / 20.0, 2.25, 1e-9);

    // The ceiling is load-bearing. 1.5 * 30 / 2 = 22.5, and the game runs 23.
    CHECK(cr::break_ticks(1.5f, 2.0f, true) == 23);
    CHECK(cr::break_ticks(1.5f, 4.0f, true) == 12);   // 11.25 -> 12
    std::printf("  ceiling check: 1.5*30/2 = 22.5 -> %d ticks; 1.5*30/4 = 11.25 -> %d ticks\n",
                cr::break_ticks(1.5f, 2.0f, true), cr::break_ticks(1.5f, 4.0f, true));

    rule("measured divergence from blockworld.hpp");

    // GOLD. blockworld's break_ticks takes a harvest tier and derives speed from
    // it, so it necessarily treats a golden pickaxe as a wooden one.
    const int mineGold  = cr::break_ticks_for(Stone, cr::ToolKind::Pickaxe, cr::Material::Gold);
    const int benchGold = bench::break_ticks(Stone, cr::blockworld_tier(cr::Material::Gold));
    std::printf("  stone with a GOLDEN pickaxe: craft.hpp %d ticks (speed 12),"
                " blockworld %d ticks (speed 2 from tier 1) -> %.1fx apart\n",
                mineGold, benchGold, double(benchGold) / double(mineGold));
    CHECK(mineGold == 4);
    CHECK(benchGold == 23);
    CHECK(benchGold > mineGold);

    // And gold cannot harvest what its speed lets it chew through.
    CHECK(cr::drops_for(DiamondOre, cr::ToolKind::Pickaxe, cr::Material::Gold)  == false);
    CHECK(cr::drops_for(DiamondOre, cr::ToolKind::Pickaxe, cr::Material::Iron)  == true);
    CHECK(cr::drops_for(Obsidian,   cr::ToolKind::Pickaxe, cr::Material::Gold)  == false);
    CHECK(cr::drops_for(Obsidian,   cr::ToolKind::Pickaxe, cr::Material::Diamond) == true);
    std::printf("  golden pickaxe on diamond ore: %d ticks and NO drop; iron pickaxe:"
                " %d ticks and a drop\n",
                cr::break_ticks_for(DiamondOre, cr::ToolKind::Pickaxe, cr::Material::Gold),
                cr::break_ticks_for(DiamondOre, cr::ToolKind::Pickaxe, cr::Material::Iron));

    // TWO more divergences, which have to be separated or they are indis-
    // tinguishable noise. This block first asserted "differences are at most 1
    // tick" and measured 1750, which is how the second one below was found.
    //
    //   (a) rounding. blockworld does int(seconds*20 + 0.5) — round to nearest.
    //       The game accumulates damage and breaks when it reaches 1, i.e. ceil.
    //       Worth at most one tick.
    //
    //   (b) the wrong-tool penalty. blockworld's `correct` is "is this a pickaxe
    //       at all" (r.pickaxe ? tier >= 1 : true), which is true for EVERY
    //       material, so it never charges the 5x penalty to an under-tier
    //       pickaxe. The game charges by whether the tool can HARVEST: a wooden
    //       pickaxe on obsidian is divisor 100, not 30. That is a factor of
    //       10/3 on exactly the blocks where the tier gate bites, and it is a
    //       model difference rather than a rounding one.
    //
    // Gold and netherite are excluded: gold is the speed divergence already
    // measured above, and blockworld's tool_speed() has no entry for tier 5 so
    // netherite falls through to speed 1 over there.
    int agree = 0, agreeDiffer = 0, maxRound = 0;
    int gated = 0;
    double minRatio = 1e9, maxRatio = 0.0;
    for (int b = 0; b < kBlocks; ++b) {
        const BlockRule r = block_rule(b);
        if (r.hardness < 0.0f) continue;                     // bedrock: never breaks
        for (int m = 0; m < cr::kMaterials; ++m) {
            const cr::Material M = cr::Material(m);
            if (M == cr::Material::Gold || M == cr::Material::Netherite) continue;
            const int a = cr::break_ticks_for(b, cr::ToolKind::Pickaxe, M);
            const int c = bench::break_ticks(b, cr::blockworld_tier(M));
            // blockworld says "correct tool"; craft.hpp says "can harvest".
            const bool bwCorrect = r.pickaxe ? (cr::blockworld_tier(M) >= 1) : true;
            const bool canHarvest = cr::drops_for(b, cr::ToolKind::Pickaxe, M);
            if (bwCorrect == canHarvest) {
                ++agree;
                if (a != c) { ++agreeDiffer; maxRound = std::max(maxRound, std::abs(a - c)); }
            } else {
                ++gated;
                CHECK(a > c);          // the penalty always makes craft.hpp slower
                const double ratio = double(a) / double(c);
                minRatio = std::min(minRatio, ratio);
                maxRatio = std::max(maxRatio, ratio);
            }
        }
    }
    std::printf("  (a) rounding: %d of %d pairs where the harvest verdict agrees differ,"
                " by at most %d tick(s)\n", agreeDiffer, agree, maxRound);
    std::printf("  (b) tier gate: %d pairs where blockworld calls an under-tier pickaxe"
                " 'correct' and the game does not; craft.hpp is %.2fx-%.2fx slower there"
                " (the 100/30 = 3.33 wrong-tool divisor)\n", gated, minRatio, maxRatio);
    CHECK(agree > 0);
    CHECK(maxRound <= 1);              // rounding, and only rounding
    CHECK(gated > 0);
    CHECK(minRatio > 3.0 && maxRatio < 3.5);
}

// ── 4. recipes ──────────────────────────────────────────────────────────────

static void test_recipes() {
    rule("crafting recipes");

    struct Want { cr::ItemId out; int makes; cr::Station st; };
    const Want w[] = {
        { cr::ItemId::Planks,        4, cr::Station::Inventory },
        { cr::ItemId::Stick,         4, cr::Station::Inventory },
        { cr::ItemId::CraftingTable, 1, cr::Station::Inventory },
        { cr::ItemId::Torch,         4, cr::Station::Inventory },
        { cr::ItemId::Shears,        1, cr::Station::Inventory },
        { cr::ItemId::FlintAndSteel, 1, cr::Station::Inventory },
        { cr::ItemId::Furnace,       1, cr::Station::CraftingTable },
        { cr::ItemId::Chest,         1, cr::Station::CraftingTable },
        { cr::ItemId::WoodenDoor,    3, cr::Station::CraftingTable },
        { cr::ItemId::Bed,           1, cr::Station::CraftingTable },
        { cr::ItemId::Boat,          1, cr::Station::CraftingTable },
        { cr::ItemId::Bucket,        1, cr::Station::CraftingTable },
        { cr::ItemId::Bread,         1, cr::Station::CraftingTable },
    };
    for (const Want& want : w) {
        const cr::Recipe r = cr::recipe_for(want.out);
        CHECK(r.ok());
        CHECK(r.out.n == want.makes);
        CHECK(r.station == want.st);
        std::printf("  %-16s makes %d  <-", cr::item_name(want.out).c_str(), r.out.n);
        for (int i = 0; i < r.ins; ++i)
            std::printf("  %d %s", r.in[i].n, cr::item_name(r.in[i].id).c_str());
        std::printf("   [%s]\n",
                    r.station == cr::Station::Inventory ? "2x2 inventory" : "crafting table");
    }

    // The published ingredient counts, spelled out.
    CHECK(cr::recipe_for(cr::ItemId::Planks).in[0].n == 1);        // 1 log -> 4 planks
    CHECK(cr::recipe_for(cr::ItemId::Stick).in[0].n == 2);         // 2 planks -> 4 sticks
    CHECK(cr::recipe_for(cr::ItemId::CraftingTable).in[0].n == 4);
    CHECK(cr::recipe_for(cr::ItemId::Furnace).in[0].n == 8);       // 8 cobblestone
    CHECK(cr::recipe_for(cr::ItemId::Chest).in[0].n == 8);         // 8 planks
    CHECK(cr::recipe_for(cr::ItemId::WoodenDoor).in[0].n == 6);    // 6 planks -> 3 doors
    CHECK(cr::recipe_for(cr::ItemId::Boat).in[0].n == 5);          // 5 planks
    CHECK(cr::recipe_for(cr::ItemId::Bucket).in[0].n == 3);        // 3 iron
    CHECK(cr::recipe_for(cr::ItemId::Shears).in[0].n == 2);        // 2 iron
    CHECK(cr::recipe_for(cr::ItemId::Bread).in[0].n == 3);         // 3 wheat
    CHECK(cr::recipe_for(cr::ItemId::Bed).in[0].n == 3);           // 3 wool
    CHECK(cr::recipe_for(cr::ItemId::Bed).in[1].n == 3);           // + 3 planks
    CHECK(cr::recipe_for(cr::ItemId::Torch).out.n == 4);

    rule("tool and armour recipes");
    std::printf("  material units + sticks per tool:\n");
    const int matCost[5]   = { 3, 3, 1, 2, 2 };
    const int stickCost[5] = { 2, 2, 2, 2, 1 };
    for (int k = 0; k < cr::kToolKinds; ++k) {
        const cr::ToolKind K = cr::ToolKind(k);
        const cr::Recipe r = cr::tool_recipe(K, cr::Material::Iron);
        std::printf("    %-8s %d iron ingot(s) + %d stick(s)   [%s]\n",
                    cr::tool_kind_name(K), r.in[0].n, r.in[1].n,
                    r.station == cr::Station::CraftingTable ? "crafting table" : "?");
        CHECK(r.in[0].n == matCost[k]);
        CHECK(r.in[1].n == stickCost[k]);
        // Every tool needs the 3x3 grid: the shovel is one column wide but
        // three rows tall, so even it does not fit the inventory.
        CHECK(r.station == cr::Station::CraftingTable);
    }
    std::printf("  material units per armour piece: ");
    const int armCost[4] = { 5, 8, 7, 4 };
    int setCost = 0;
    for (int s = 0; s < cr::kArmourSlots; ++s) {
        const cr::Recipe r = cr::armour_recipe(cr::ArmourSlot(s), cr::ArmourMaterial::Diamond);
        std::printf("%s %d  ", cr::armour_slot_name(cr::ArmourSlot(s)), r.in[0].n);
        CHECK(r.in[0].n == armCost[s]);
        setCost += r.in[0].n;
    }
    std::printf("-> a full set costs %d units\n", setCost);
    CHECK(setCost == 24);

    // Chainmail has no crafting recipe in Java Edition. Saying so is the honest
    // answer; inventing one would be worse than useless.
    for (int s = 0; s < cr::kArmourSlots; ++s)
        CHECK(!cr::armour_recipe(cr::ArmourSlot(s), cr::ArmourMaterial::Chainmail).ok());
    std::printf("  chainmail: not craftable (trade/loot only) — 4 slots correctly report"
                " no recipe\n");

    // Netherite is smithed, not crafted.
    const cr::Recipe np = cr::tool_recipe(cr::ToolKind::Pickaxe, cr::Material::Netherite);
    CHECK(np.station == cr::Station::SmithingTable);
    CHECK(np.in[0].id == cr::tool_item(cr::ToolKind::Pickaxe, cr::Material::Diamond));
    CHECK(np.in[1].id == cr::ItemId::NetheriteIngot && np.in[1].n == 1);
    std::printf("  netherite pickaxe: %s + %d %s at a smithing table\n",
                cr::item_name(np.in[0].id).c_str(), np.in[1].n,
                cr::item_name(np.in[1].id).c_str());

    // Packed tool/armour ids must round-trip, or every lookup above is luck.
    int roundTrips = 0;
    for (int k = 0; k < cr::kToolKinds; ++k)
        for (int m = 0; m < cr::kMaterials; ++m) {
            const cr::ItemId id = cr::tool_item(cr::ToolKind(k), cr::Material(m));
            CHECK(cr::is_tool(id) && !cr::is_armour(id));
            CHECK(cr::ix(cr::tool_kind_of(id)) == k);
            CHECK(cr::ix(cr::tool_material_of(id)) == m);
            ++roundTrips;
        }
    for (int s = 0; s < cr::kArmourSlots; ++s)
        for (int m = 0; m < cr::kArmourMaterials; ++m) {
            const cr::ItemId id = cr::armour_item(cr::ArmourSlot(s), cr::ArmourMaterial(m));
            CHECK(cr::is_armour(id) && !cr::is_tool(id));
            CHECK(cr::ix(cr::armour_slot_of(id)) == s);
            CHECK(cr::ix(cr::armour_material_of(id)) == m);
            ++roundTrips;
        }
    std::printf("  packed item ids: %d tool/armour ids round-trip through"
                " is_*/kind_of/material_of\n", roundTrips);

    // Walk the whole book. 4 chainmail pieces are legitimately un-craftable.
    int ok = 0, notCraftable = 0;
    for (int i = 0; i < cr::kRecipeCount; ++i) {
        const cr::Recipe r = cr::recipe_at(i);
        if (r.ok()) { ++ok; CHECK(r.ins >= 1); CHECK(r.in[0].n > 0); }
        else ++notCraftable;
    }
    std::printf("  recipe book: %d entries, %d craftable, %d intentionally not\n",
                cr::kRecipeCount, ok, notCraftable);
    CHECK(cr::kRecipeCount == 70);
    CHECK(notCraftable == 4);
}

// ── 5. a cost planner, to prove the book composes ───────────────────────────
//
// Expands a want down to items that have no recipe, rounding each craft up to a
// whole batch — you cannot craft 2/4 of a stick recipe, you craft the batch and
// keep the change. This exists to show the lookup functions are enough on their
// own: nothing here duplicates a recipe.

static void expand(cr::ItemId want, int n, std::vector<cr::Stack>& raw, int depth = 0) {
    if (depth > 16) return;                       // recipes here are acyclic; belt and braces
    const cr::Recipe r = cr::recipe_for(want);
    if (!r.ok() || r.station == cr::Station::SmithingTable) {
        for (cr::Stack& s : raw) if (s.id == want) { s.n += n; return; }
        raw.push_back({ want, n });
        return;
    }
    const int batches = (n + r.out.n - 1) / r.out.n;
    for (int i = 0; i < r.ins; ++i)
        expand(r.in[i].id, r.in[i].n * batches, raw, depth + 1);
}

static int raw_count(const std::vector<cr::Stack>& raw, cr::ItemId id) {
    for (const cr::Stack& s : raw) if (s.id == id) return s.n;
    return 0;
}

static void test_planner() {
    rule("cost planning through recipe_for() alone");

    {   // A diamond pickaxe is 3 diamonds and, once the stick batch is rounded
        // up, exactly one log.
        std::vector<cr::Stack> raw;
        expand(cr::tool_item(cr::ToolKind::Pickaxe, cr::Material::Diamond), 1, raw);
        std::printf("  1 diamond pickaxe =");
        for (const cr::Stack& s : raw)
            std::printf("  %d %s", s.n, cr::item_name(s.id).c_str());
        std::putchar('\n');
        CHECK(raw_count(raw, cr::ItemId::Diamond) == 3);
        CHECK(raw_count(raw, cr::ItemId::OakLog) == 1);
    }
    {   // The full five-tool diamond kit: 3+3+1+2+2 = 11 diamonds, 2+2+2+2+1 = 9
        // sticks -> 3 stick batches -> 6 planks -> 2 logs.
        std::vector<cr::Stack> raw;
        for (int k = 0; k < cr::kToolKinds; ++k)
            expand(cr::tool_item(cr::ToolKind(k), cr::Material::Diamond), 1, raw);
        std::printf("  full diamond tool kit (5 tools) =");
        for (const cr::Stack& s : raw)
            std::printf("  %d %s", s.n, cr::item_name(s.id).c_str());
        std::putchar('\n');
        CHECK(raw_count(raw, cr::ItemId::Diamond) == 11);
        CHECK(raw_count(raw, cr::ItemId::OakLog) == 5);   // one batch per tool, no sharing
    }
    {   // Armour shares nothing but diamonds, so this is the clean 24.
        std::vector<cr::Stack> raw;
        for (int s = 0; s < cr::kArmourSlots; ++s)
            expand(cr::armour_item(cr::ArmourSlot(s), cr::ArmourMaterial::Diamond), 1, raw);
        std::printf("  full diamond armour set =");
        for (const cr::Stack& s : raw)
            std::printf("  %d %s", s.n, cr::item_name(s.id).c_str());
        std::putchar('\n');
        CHECK(raw_count(raw, cr::ItemId::Diamond) == 24);
    }
    {   // 16 torches: 4 batches -> 4 coal and 4 sticks -> 1 stick batch -> 2
        // planks -> 1 log.
        std::vector<cr::Stack> raw;
        expand(cr::ItemId::Torch, 16, raw);
        std::printf("  16 torches =");
        for (const cr::Stack& s : raw)
            std::printf("  %d %s", s.n, cr::item_name(s.id).c_str());
        std::putchar('\n');
        CHECK(raw_count(raw, cr::ItemId::Coal) == 4);
        CHECK(raw_count(raw, cr::ItemId::OakLog) == 1);
    }
}

// ── 6. smelting ─────────────────────────────────────────────────────────────

static void test_smelting() {
    rule("smelting: cycle time, fuel table, recipes");

    CHECK(cr::kSmeltTicks == 200);
    CHECK(cr::kBlastFurnaceTicks == 100);
    std::printf("  furnace cycle %d ticks = %.1f s per item (blast furnace/smoker %d)\n",
                cr::kSmeltTicks, cr::kSmeltTicks / 20.0, cr::kBlastFurnaceTicks);
    CHECK(cr::smelt_ticks_for(8) == 1600);

    struct F { cr::ItemId id; int ticks; double items; };
    const F fuels[] = {
        { cr::ItemId::LavaBucket,     20000, 100.0 },
        { cr::ItemId::BlockOfCoal,    16000,  80.0 },
        { cr::ItemId::DriedKelpBlock,  4000,  20.0 },
        { cr::ItemId::BlazeRod,        2400,  12.0 },
        { cr::ItemId::Coal,            1600,   8.0 },
        { cr::ItemId::Charcoal,        1600,   8.0 },
        { cr::ItemId::Boat,            1200,   6.0 },
        { cr::ItemId::OakLog,           300,   1.5 },
        { cr::ItemId::Planks,           300,   1.5 },
        { cr::ItemId::CraftingTable,    300,   1.5 },
        { cr::ItemId::Chest,            300,   1.5 },
        { cr::ItemId::WoodenDoor,       200,   1.0 },
        { cr::ItemId::WoodenSlab,       150,   0.75 },
        { cr::ItemId::Stick,            100,   0.5 },
        { cr::ItemId::Sapling,          100,   0.5 },
        { cr::ItemId::Wool,             100,   0.5 },
        { cr::ItemId::Bamboo,            50,   0.25 },
    };
    std::printf("  %-18s %8s %8s %10s\n", "fuel", "ticks", "seconds", "items");
    for (const F& f : fuels) {
        std::printf("  %-18s %8d %8.1f %10.2f\n", cr::item_name(f.id).c_str(),
                    cr::fuel_burn_ticks(f.id), cr::fuel_burn_ticks(f.id) / 20.0,
                    double(cr::items_smelted(f.id)));
        CHECK(cr::fuel_burn_ticks(f.id) == f.ticks);
        CHECK_NEAR(cr::items_smelted(f.id), f.items, 1e-6);
        CHECK(cr::is_fuel(f.id));
    }
    // A wooden tool is one smelt; a stone one is not fuel at all.
    CHECK(cr::fuel_burn_ticks(cr::tool_item(cr::ToolKind::Pickaxe, cr::Material::Wood)) == 200);
    CHECK(cr::fuel_burn_ticks(cr::tool_item(cr::ToolKind::Pickaxe, cr::Material::Stone)) == 0);
    CHECK(!cr::is_fuel(cr::ItemId::IronIngot));
    CHECK(!cr::is_fuel(cr::ItemId::Cobblestone));
    std::printf("  wooden pickaxe as fuel: %d ticks (exactly 1 item); stone pickaxe: %d\n",
                cr::fuel_burn_ticks(cr::tool_item(cr::ToolKind::Pickaxe, cr::Material::Wood)),
                cr::fuel_burn_ticks(cr::tool_item(cr::ToolKind::Pickaxe, cr::Material::Stone)));

    // Fuel accounting rounds up: you cannot feed the furnace part of a coal.
    CHECK(cr::fuel_units_for(8, cr::ItemId::Coal) == 1);
    CHECK(cr::fuel_units_for(9, cr::ItemId::Coal) == 2);
    CHECK(cr::fuel_units_for(64, cr::ItemId::Coal) == 8);
    CHECK(cr::fuel_units_for(100, cr::ItemId::LavaBucket) == 1);
    CHECK(cr::fuel_units_for(101, cr::ItemId::LavaBucket) == 2);
    std::printf("  fuel accounting: 8 items = 1 coal, 9 items = 2 coal,"
                " 64 items = 8 coal, 100 items = 1 lava bucket\n");

    rule("smelting recipes");
    struct S { cr::ItemId in; cr::ItemId out; double xp; };
    const S rec[] = {
        { cr::ItemId::RawIron,     cr::ItemId::IronIngot,      0.7 },
        { cr::ItemId::RawGold,     cr::ItemId::GoldIngot,      1.0 },
        { cr::ItemId::IronOre,     cr::ItemId::IronIngot,      0.7 },
        { cr::ItemId::GoldOre,     cr::ItemId::GoldIngot,      1.0 },
        { cr::ItemId::AncientDebris, cr::ItemId::NetheriteScrap, 2.0 },
        { cr::ItemId::Sand,        cr::ItemId::Glass,          0.1 },
        { cr::ItemId::Cobblestone, cr::ItemId::Stone,          0.1 },
        { cr::ItemId::OakLog,      cr::ItemId::Charcoal,       0.15 },
        { cr::ItemId::Cactus,      cr::ItemId::GreenDye,       1.0 },
        { cr::ItemId::ClayBall,    cr::ItemId::Brick,          0.3 },
        { cr::ItemId::Kelp,        cr::ItemId::DriedKelp,      0.1 },
        { cr::ItemId::RawBeef,     cr::ItemId::Steak,          0.35 },
        { cr::ItemId::RawPorkchop, cr::ItemId::CookedPorkchop, 0.35 },
        { cr::ItemId::Potato,      cr::ItemId::BakedPotato,    0.35 },
        { cr::ItemId::RawSalmon,   cr::ItemId::CookedSalmon,   0.35 },
    };
    for (const S& s : rec) {
        const cr::SmeltResult r = cr::smelt(s.in);
        std::printf("  %-16s -> %-16s  %.2f xp\n", cr::item_name(s.in).c_str(),
                    cr::item_name(r.out).c_str(), double(r.xp));
        CHECK(r.out == s.out);
        CHECK_NEAR(r.xp, s.xp, 1e-5);
    }
    CHECK(!cr::smeltable(cr::ItemId::Diamond));
    CHECK(!cr::smeltable(cr::ItemId::Stick));
    CHECK(cr::smelt(cr::ItemId::Diamond).out == cr::ItemId::None);
    std::printf("  non-smeltables (diamond, stick) correctly report nothing\n");

    // Cooking every raw food yields a strictly better food. That is the whole
    // reason a furnace is worth building, so it is worth asserting.
    const cr::ItemId raws[] = {
        cr::ItemId::RawBeef, cr::ItemId::RawPorkchop, cr::ItemId::RawChicken,
        cr::ItemId::RawMutton, cr::ItemId::RawRabbit, cr::ItemId::RawCod,
        cr::ItemId::RawSalmon, cr::ItemId::Potato,
    };
    std::printf("  cooking gain (hunger, saturation):\n");
    for (cr::ItemId r : raws) {
        const cr::ItemId c = cr::smelt(r).out;
        const cr::FoodStats a = cr::food_stats(r), b = cr::food_stats(c);
        std::printf("    %-14s %d/%.1f  ->  %-16s %d/%.1f\n",
                    cr::item_name(r).c_str(), a.hunger, double(a.saturation()),
                    cr::item_name(c).c_str(), b.hunger, double(b.saturation()));
        CHECK(b.hunger > a.hunger);
        CHECK(b.saturation() > a.saturation());
    }
}

// ── 7. food ─────────────────────────────────────────────────────────────────

static void test_food() {
    rule("food: hunger, saturation modifier, saturation restored");

    // minecraft.wiki, Food (Java Edition). `sat` is the published saturation
    // RESTORED; craft.hpp stores only hunger and the game's saturationModifier
    // and derives this, so a wrong modifier fails here rather than hiding.
    struct FRow { cr::ItemId id; int hunger; double sat; };
    const FRow f[] = {
        { cr::ItemId::Bread,          5,  6.0 },
        { cr::ItemId::Apple,          4,  2.4 },
        { cr::ItemId::GoldenApple,    4,  9.6 },
        { cr::ItemId::MelonSlice,     2,  1.2 },
        { cr::ItemId::Cookie,         2,  0.4 },
        { cr::ItemId::RottenFlesh,    4,  0.8 },
        { cr::ItemId::Carrot,         3,  3.6 },
        { cr::ItemId::Potato,         1,  0.6 },
        { cr::ItemId::BakedPotato,    5,  6.0 },
        { cr::ItemId::DriedKelp,      1,  0.6 },
        { cr::ItemId::RawBeef,        3,  1.8 },
        { cr::ItemId::Steak,          8, 12.8 },
        { cr::ItemId::RawPorkchop,    3,  1.8 },
        { cr::ItemId::CookedPorkchop, 8, 12.8 },
        { cr::ItemId::RawChicken,     2,  1.2 },
        { cr::ItemId::CookedChicken,  6,  7.2 },
        { cr::ItemId::RawMutton,      2,  1.2 },
        { cr::ItemId::CookedMutton,   6,  9.6 },
        { cr::ItemId::RawRabbit,      3,  1.8 },
        { cr::ItemId::CookedRabbit,   5,  6.0 },
        { cr::ItemId::RawCod,         2,  0.4 },
        { cr::ItemId::CookedCod,      5,  6.0 },
        { cr::ItemId::RawSalmon,      2,  0.4 },
        { cr::ItemId::CookedSalmon,   6,  9.6 },
    };
    std::printf("  %-16s %7s %6s %11s\n", "food", "hunger", "mod", "saturation");
    for (const FRow& row : f) {
        const cr::FoodStats s = cr::food_stats(row.id);
        std::printf("  %-16s %7d %6.1f %11.2f\n", cr::item_name(row.id).c_str(),
                    s.hunger, double(s.mod), double(s.saturation()));
        CHECK(s.hunger == row.hunger);
        CHECK(s.edible());
        // The identity that catches a lost factor of two, checked for every food.
        CHECK_NEAR(s.saturation(), row.sat, 1e-4);
        CHECK_NEAR(s.saturation(), double(s.hunger) * double(s.mod) * 2.0, 1e-4);
    }
    std::printf("  all %d foods satisfy saturation == hunger x modifier x 2\n",
                int(sizeof(f) / sizeof(f[0])));

    // The row that a sorted, merged wiki table makes it easy to misread.
    CHECK(cr::food_stats(cr::ItemId::CookedMutton).hunger == 6);
    CHECK(cr::food_stats(cr::ItemId::CookedSalmon).hunger == 6);
    CHECK(cr::food_stats(cr::ItemId::Steak).hunger == 8);
    std::printf("  cooked mutton and cooked salmon are 6 hunger / 9.6 saturation —"
                " they share a merged saturation row with the golden apples, they are"
                " not 8 like steak\n");

    CHECK(!cr::edible(cr::ItemId::Diamond));
    CHECK(!cr::edible(cr::ItemId::IronIngot));

    // Eating, with the two caps the game applies. The two caps bite in
    // different places and it is easy to assume the wrong one is active — a
    // first pass here expected 20 saturation from one steak at 18/20 hunger,
    // which is wrong: the steak only carries 12.8, so the hunger cap binds and
    // the saturation cap does not.
    cr::Nourishment n{ 18, 0.0f };
    n = cr::eat(n, cr::ItemId::Steak);          // +8 hunger (6 wasted), +12.8 saturation
    std::printf("  steak at 18/20 hunger:      -> hunger %d, saturation %.1f"
                "  (hunger cap wastes %d; saturation is under its cap)\n",
                n.hunger, double(n.saturation), 18 + 8 - cr::kMaxHunger);
    CHECK(n.hunger == cr::kMaxHunger);          // 26 capped to 20
    CHECK_NEAR(n.saturation, 12.8f, 1e-4);      // 12.8 < 20, so no clipping

    n = cr::eat(n, cr::ItemId::Steak);          // now the saturation cap binds
    std::printf("  a second steak on top of it: -> hunger %d, saturation %.1f"
                "  (saturation cap binds: 25.6 clipped to the hunger of %d)\n",
                n.hunger, double(n.saturation), n.hunger);
    CHECK(n.hunger == cr::kMaxHunger);
    CHECK_NEAR(n.saturation, 20.0f, 1e-4);      // saturation can never exceed hunger

    cr::Nourishment m{ 4, 0.0f };
    m = cr::eat(m, cr::ItemId::Steak);
    std::printf("  steak at 4/20 hunger:       -> hunger %d, saturation %.1f"
                "  (12.8 clipped to the new hunger of 12)\n",
                m.hunger, double(m.saturation));
    CHECK(m.hunger == 12);
    CHECK_NEAR(m.saturation, 12.0f, 1e-4);      // 12.8 clipped to hunger 12

    cr::Nourishment k{ 0, 0.0f };
    k = cr::eat(k, cr::ItemId::Diamond);        // inedible: no change at all
    CHECK(k.hunger == 0);
}

// ── 8. the world bridge, end to end ─────────────────────────────────────────

static void test_world_bridge() {
    rule("bridge to BlockWorld: mine, smelt, craft");

    BlockWorld w(96, 128);
    w.generate(20260814ull);
    const std::size_t iron    = w.count(IronOre);
    const std::size_t coal    = w.count(Coal);
    const std::size_t diamond = w.count(DiamondOre);
    const std::size_t wood    = w.count(Wood);
    std::printf("  world 96x128x96 seed 20260814: %zu iron ore, %zu coal ore,"
                " %zu diamond ore, %zu wood\n", iron, coal, diamond, wood);
    CHECK(iron > 0 && coal > 0 && diamond > 0 && wood > 0);

    // Block -> item, so a caller holding a Block can ask what it smelts into.
    CHECK(cr::item_from_block(IronOre) == cr::ItemId::IronOre);
    CHECK(cr::item_from_block(Wood) == cr::ItemId::OakLog);
    CHECK(cr::item_from_block(Stone) == cr::ItemId::Cobblestone);   // stone drops cobble
    CHECK(cr::smelt(cr::item_from_block(IronOre)).out == cr::ItemId::IronIngot);
    CHECK(cr::smelt(cr::item_from_block(Wood)).out == cr::ItemId::Charcoal);
    CHECK(cr::smelt(cr::item_from_block(Stone)).out == cr::ItemId::Stone);

    // The whole first hour, priced in ticks. A stone pickaxe is needed to
    // harvest iron ore at all; a wooden one breaks the block and drops nothing.
    CHECK(cr::drops_for(IronOre, cr::ToolKind::Pickaxe, cr::Material::Wood) == false);
    CHECK(cr::drops_for(IronOre, cr::ToolKind::Pickaxe, cr::Material::Stone) == true);

    const int mineOne = cr::break_ticks_for(IronOre, cr::ToolKind::Pickaxe, cr::Material::Stone);
    const int mine3   = mineOne * 3;
    const int smelt3  = cr::smelt_ticks_for(3);
    const int coalNeed = cr::fuel_units_for(3, cr::ItemId::Coal);
    std::printf("  iron pickaxe from scratch: mine 3 iron ore with a stone pickaxe"
                " = 3 x %d = %d ticks, smelt = %d ticks, fuel = %d coal\n",
                mineOne, mine3, smelt3, coalNeed);
    CHECK(mineOne == cr::break_ticks(3.0f, 4.0f, true));   // 3.0 * 30 / 4 = 22.5 -> 23
    CHECK(mineOne == 23);
    CHECK(smelt3 == 600);
    CHECK(coalNeed == 1);

    std::vector<cr::Stack> raw;
    expand(cr::tool_item(cr::ToolKind::Pickaxe, cr::Material::Iron), 1, raw);
    std::printf("  1 iron pickaxe =");
    for (const cr::Stack& s : raw)
        std::printf("  %d %s", s.n, cr::item_name(s.id).c_str());
    std::printf("   (the ingots come from the furnace, not the bench)\n");
    CHECK(raw_count(raw, cr::ItemId::IronIngot) == 3);
    CHECK(raw_count(raw, cr::ItemId::OakLog) == 1);

    // Durability as a budget: how much stone one pickaxe of each material can
    // clear before it breaks, and how long that takes.
    std::printf("  stone cleared per pickaxe before it breaks:\n");
    for (int m = 0; m < cr::kMaterials; ++m) {
        const cr::Material M = cr::Material(m);
        const int uses  = cr::tool_durability(M);
        const int ticks = cr::break_ticks_for(Stone, cr::ToolKind::Pickaxe, M);
        std::printf("    %-10s %5d blocks in %7d ticks (%.1f min), %.1f blocks/s\n",
                    cr::material_name(M), uses, uses * ticks, uses * ticks / 1200.0,
                    20.0 / double(ticks));
        CHECK(uses > 0 && ticks > 0);
    }
    // Gold's whole character in one comparison: fastest per block, worst total.
    const int goldTotal = cr::tool_durability(cr::Material::Gold)
                        * cr::break_ticks_for(Stone, cr::ToolKind::Pickaxe, cr::Material::Gold);
    const int woodTotal = cr::tool_durability(cr::Material::Wood)
                        * cr::break_ticks_for(Stone, cr::ToolKind::Pickaxe, cr::Material::Wood);
    CHECK(goldTotal < woodTotal);
    std::printf("  a golden pickaxe is the fastest per block and still clears less stone"
                " in total than a wooden one (%d ticks of work vs %d)\n",
                goldTotal, woodTotal);
}

// ── 9. determinism and cost ─────────────────────────────────────────────────

static void test_determinism_and_cost() {
    rule("determinism and per-call cost");

    // Nothing here is random, but a caller may drive it from an Rng. Same seed,
    // same stream of answers — checked by hashing two independent passes.
    auto pass = [](std::uint64_t seed) {
        Rng r(seed);
        std::uint64_t h = 1469598103934665603ull;
        for (int i = 0; i < 20000; ++i) {
            const int b = int(r.below(kBlocks));
            const cr::Material m = cr::Material(r.below(cr::kMaterials));
            const cr::ToolKind k = cr::ToolKind(r.below(cr::kToolKinds));
            const int t = cr::break_ticks_for(b, k, m);
            const int d = cr::drops_for(b, k, m) ? 1 : 0;
            h = (h ^ std::uint64_t(t + 1)) * 1099511628211ull;
            h = (h ^ std::uint64_t(d)) * 1099511628211ull;
        }
        return h;
    };
    const std::uint64_t a = pass(0xC0FFEEull);
    const std::uint64_t b = pass(0xC0FFEEull);
    const std::uint64_t c = pass(0xC0FFEFull);
    std::printf("  seed 0xC0FFEE twice -> %016llx / %016llx; different seed -> %016llx\n",
                (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
    CHECK(a == b);
    CHECK(a != c);

    // Cost. Everything in craft.hpp is O(1) — a switch or a fixed-size table
    // index — so the only number worth reporting is nanoseconds per call.
    const int N = 2000000;
    volatile long long sink = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        const int b = i % kBlocks;
        const cr::Material m = cr::Material(i % cr::kMaterials);
        sink += cr::break_ticks_for(b, cr::ToolKind::Pickaxe, m);
    }
    auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        const cr::Recipe r = cr::recipe_for(cr::tool_item(cr::ToolKind(i % cr::kToolKinds),
                                                          cr::Material(i % cr::kMaterials)));
        sink += r.in[0].n;
    }
    auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) sink += cr::fuel_burn_ticks(cr::ItemId(i % 90));
    auto t3 = std::chrono::steady_clock::now();

    const double ns1 = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
    const double ns2 = std::chrono::duration<double, std::nano>(t2 - t1).count() / N;
    const double ns3 = std::chrono::duration<double, std::nano>(t3 - t2).count() / N;
    std::printf("  %d calls each: break_ticks_for %.2f ns, recipe_for(tool) %.2f ns,"
                " fuel_burn_ticks %.2f ns  (sink %lld)\n",
                N, ns1, ns2, ns3, (long long)sink);
    std::printf("  all O(1): no loop over blocks, no allocation, nothing that scales"
                " with world volume. A 256x128x256 world is 8.4M voxels; this file"
                " never touches one of them.\n");
    CHECK(ns1 < 500.0 && ns2 < 500.0 && ns3 < 500.0);
}

// ── main ────────────────────────────────────────────────────────────────────

int main() {
    std::printf("test_craft — crafting, smelting, gear and food tables\n");
    std::printf("constants: Minecraft Java Edition, minecraft.wiki, read 2026-08\n");

    test_tools();
    test_armour();
    test_breaking();
    test_recipes();
    test_planner();
    test_smelting();
    test_food();
    test_world_bridge();
    test_determinism_and_cost();

    std::printf("\n%s: %d checks, %d failed\n", g_fail ? "FAILED" : "PASSED",
                g_checks, g_fail);
    return g_fail ? 1 : 0;
}
