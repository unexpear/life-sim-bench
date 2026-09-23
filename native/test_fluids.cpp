// test_fluids.cpp — the assertions that make src/world/fluids.hpp a claim
// rather than a vibe. Every published constant is checked against a MEASUREMENT
// taken from the running simulation, and every measurement is printed.
//
// Build (delete the exe first — a stale binary has produced false results in
// this project before):
//   rm -f tf.exe
//   g++ -std=c++20 -O2 -Wall -Wextra -Isrc test_fluids.cpp -o tf.exe
//       -static -static-libgcc -static-libstdc++
//   ./tf.exe
//
// ── WHAT IS CITED AND WHAT IS NOT ───────────────────────────────────────────
//
// CITED to minecraft.wiki (Java Edition) and asserted below:
//   water   reach 7, tick delay 5, level +1 per block, drop search 4 from the
//           target cell / 5 from the source, infinite source from 2 sources
//   lava    reach 3 Overworld (delay 30, level +2), reach 7 Nether (delay 10),
//           drop search 2 from the target cell / 3 from the source
//   mixing  lava+water sideways or from above -> cobblestone; water on a lava
//           SOURCE -> obsidian; lava flowing DOWN into water -> stone
//   gravity 0.45 s to fall one metre; entity gravity -0.04, drag 0.98;
//           600-tick despawn; sand/gravel tile-tick delay 2
//
// UNVERIFIED — implemented from FlowingFluid's behaviour, NOT found in
// minecraft.wiki prose, and flagged here so nobody quotes it as gospel:
//   1. spreadToSides forces amount 7 for a FALLING block regardless of dropOff.
//      It sounded like it should make the foot of an Overworld lava fall reach
//      four blocks instead of three. §3 measures it and it does not: the cell
//      written at amount 7 recomputes itself to 8-2 = 6 on its own next tick,
//      30 ticks later, before it spreads anything. So the override exists and
//      is unobservable in the settled state, for both fluids. The assertion
//      below is on the measured reach of 3, not on the guess.
//   2. LavaFluid::getSpreadDelay multiplies the delay by 4 with probability 3/4
//      when a lava block is getting deeper. Timing only — it does not change
//      the final shape, which the determinism test confirms.
//   3. The exact set of blocks a fluid destroys on contact. This world has no
//      waterlogging, so leaves and every other solid block stop fluid dead;
//      vanilla would let water occupy leaves.

#include "world/fluids.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>


using namespace bench;

static int g_checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            std::printf("\n  *** FAIL line %d: %s\n", __LINE__, #cond);        \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

// ── scaffolding ────────────────────────────────────────────────────────────

// A world that is nothing but a stone floor at y=0 and air above it. Ideal for
// reach tests: no slope anywhere, so every direction ties at weight 1000 and
// the fluid must produce the full diamond.
static void flat_floor(BlockWorld& w, std::uint8_t block = Stone) {
    for (int z = 0; z < w.n(); ++z)
        for (int x = 0; x < w.n(); ++x) {
            w.set(x, 0, z, block);
            w.refresh_column(x, z);
        }
}

struct Puddle {
    int cells = 0;      // fluid blocks in the layer, including the centre
    int maxDist = 0;    // furthest Manhattan distance reached
    int spanX = 0;      // point-to-point width along x
    bool levelsOk = true;
    int badX = -1, badZ = -1, badLevel = -1, wantLevel = -1;
};

// Walk one horizontal layer and check that level == distance * dropOff, which
// is the whole level model in one line.
static Puddle measure_puddle(const Fluids& f, const BlockWorld& w, int cx, int cy,
                             int cz, std::uint8_t fluid, int dropOff, int levelOffset = 0) {
    Puddle p;
    int minX = cx, maxX = cx;
    for (int z = 0; z < w.n(); ++z)
        for (int x = 0; x < w.n(); ++x) {
            if (w.at(x, cy, z) != fluid) continue;
            ++p.cells;
            const int d = std::abs(x - cx) + std::abs(z - cz);
            if (d > p.maxDist) p.maxDist = d;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            const int want = (d == 0) ? 0 : levelOffset + d * dropOff;
            if (f.level(x, cy, z) != want && p.levelsOk) {
                p.levelsOk = false;
                p.badX = x; p.badZ = z;
                p.badLevel = f.level(x, cy, z); p.wantLevel = want;
            }
        }
    p.spanX = maxX - minX + 1;
    return p;
}

static int count_at_y(const BlockWorld& w, int y, std::uint8_t block) {
    int n = 0;
    for (int z = 0; z < w.n(); ++z)
        for (int x = 0; x < w.n(); ++x)
            if (w.at(x, y, z) == block) ++n;
    return n;
}

static double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ── 1. the published constants, and the reach they imply ───────────────────

static void test_constants() {
    std::printf("\n== 1. published constants (Java Edition) ==\n");

    std::printf("  water        tickDelay=%d dropOff=%d slopeFind=%d sourceConv=%d\n",
                kWater.tickDelay, kWater.dropOff, kWater.slopeFind, int(kWater.sourceConv));
    CHECK(kWater.tickDelay == 5);
    CHECK(kWater.dropOff == 1);
    CHECK(kWater.slopeFind == 4);
    CHECK(kWater.sourceConv == true);

    std::printf("  lava (OW)    tickDelay=%d dropOff=%d slopeFind=%d sourceConv=%d\n",
                kLavaOverworld.tickDelay, kLavaOverworld.dropOff,
                kLavaOverworld.slopeFind, int(kLavaOverworld.sourceConv));
    CHECK(kLavaOverworld.tickDelay == 30);
    CHECK(kLavaOverworld.dropOff == 2);
    CHECK(kLavaOverworld.slopeFind == 2);
    CHECK(kLavaOverworld.sourceConv == false);

    std::printf("  lava (Nether) tickDelay=%d dropOff=%d slopeFind=%d\n",
                kLavaNether.tickDelay, kLavaNether.dropOff, kLavaNether.slopeFind);
    CHECK(kLavaNether.tickDelay == 10);
    CHECK(kLavaNether.dropOff == 1);
    CHECK(kLavaNether.slopeFind == 4);

    // The reach is derived from dropOff, not stored. It must reproduce all
    // three published numbers or the derivation is wrong.
    std::printf("  derived reach   water=%d  lava(OW)=%d  lava(Nether)=%d\n",
                fluid_reach(kWater), fluid_reach(kLavaOverworld), fluid_reach(kLavaNether));
    CHECK(fluid_reach(kWater) == 7);
    CHECK(fluid_reach(kLavaOverworld) == 3);
    CHECK(fluid_reach(kLavaNether) == 7);

    // "up to 5 blocks away for water ... up to 3 blocks away for lava
    // elsewhere" — the same slopeFind, counted from the source instead.
    std::printf("  drop search from source  water=%d  lava(OW)=%d\n",
                drop_search_range(kWater), drop_search_range(kLavaOverworld));
    CHECK(drop_search_range(kWater) == 5);
    CHECK(drop_search_range(kLavaOverworld) == 3);

    std::printf("  gravity delay=%d ticks  g=%.2f  drag=%.2f  despawn=%d ticks\n",
                kGravityDelay, kFallGravity, kFallDrag, kFallDespawn);
    CHECK(kGravityDelay == 2);
    CHECK(kFallGravity == 0.04);
    CHECK(kFallDrag == 0.98);
    CHECK(kFallDespawn == 600);
}

// ── 2. reach on flat ground, for all three fluid rules ─────────────────────

static void test_flat_reach() {
    std::printf("\n== 2. horizontal reach on a flat floor ==\n");

    // WATER: minecraft.wiki, Water — "7 blocks horizontally from a source block
    // on a flat surface", forming a diamond "spanning fifteen blocks
    // point-to-point". A Manhattan-radius-7 diamond is 1 + 4*(1+..+7) = 113.
    {
        BlockWorld w(32, 32);
        flat_floor(w);
        Fluids f(w);
        f.place_source(16, 1, 16, Water);
        const int t = f.settle();
        const Puddle p = measure_puddle(f, w, 16, 1, 16, Water, kWater.dropOff);
        std::printf("  water   settled in %d ticks: %d cells, reach %d, span %d, "
                    "levels %s\n", t, p.cells, p.maxDist, p.spanX,
                    p.levelsOk ? "level==distance" : "WRONG");
        if (!p.levelsOk)
            std::printf("      at (%d,%d) level %d, expected %d\n",
                        p.badX, p.badZ, p.badLevel, p.wantLevel);
        CHECK(t >= 0);
        CHECK(p.maxDist == 7);
        CHECK(p.spanX == 15);
        CHECK(p.cells == 113);
        CHECK(p.levelsOk);
        CHECK(f.is_source(16, 1, 16));
        CHECK(f.level(17, 1, 16) == 1);
        CHECK(f.amount(17, 1, 16) == 7);      // the other convention, same cell
        CHECK(f.level(23, 1, 16) == 7);       // last block before it dies
        CHECK(w.at(24, 1, 16) == Air);
    }

    // LAVA, Overworld: "lava travels 3 blocks in any horizontal direction from a
    // source block". Levels 2,4,6 — dropOff 2. Diamond of radius 3 = 25 cells.
    {
        BlockWorld w(32, 32);
        flat_floor(w);
        Fluids f(w, Dimension::Overworld);
        f.place_source(16, 1, 16, Lava);
        const int t = f.settle();
        const Puddle p = measure_puddle(f, w, 16, 1, 16, Lava, kLavaOverworld.dropOff);
        std::printf("  lava OW settled in %d ticks: %d cells, reach %d, span %d, "
                    "levels %s (2,4,6)\n", t, p.cells, p.maxDist, p.spanX,
                    p.levelsOk ? "level==2*distance" : "WRONG");
        CHECK(t >= 0);
        CHECK(p.maxDist == 3);
        CHECK(p.spanX == 7);
        CHECK(p.cells == 25);
        CHECK(p.levelsOk);
        CHECK(f.level(17, 1, 16) == 2);
        CHECK(f.level(19, 1, 16) == 6);
        CHECK(w.at(20, 1, 16) == Air);
    }

    // LAVA, Nether: "In the Nether, lava travels 7 blocks horizontally" — same
    // code, dropOff 1, so it must land on water's diamond exactly.
    {
        BlockWorld w(32, 32);
        flat_floor(w);
        Fluids f(w, Dimension::Nether);
        f.place_source(16, 1, 16, Lava);
        const int t = f.settle();
        const Puddle p = measure_puddle(f, w, 16, 1, 16, Lava, kLavaNether.dropOff);
        std::printf("  lava NE settled in %d ticks: %d cells, reach %d, span %d\n",
                    t, p.cells, p.maxDist, p.spanX);
        CHECK(t >= 0);
        CHECK(p.maxDist == 7);
        CHECK(p.cells == 113);
        CHECK(p.levelsOk);
    }
}

// ── 3. falling: straight down, then spread from the bottom ─────────────────

// A room 8 blocks tall with a stone lid on it and one hole punched through the
// lid. A source placed on the lid beside the hole finds the hole (drop distance
// 0, unbeatable) and pours through it — the canonical waterfall, and the only
// way to get a genuine one-block-wide column. See the midair case below for
// what happens without the lid.
static void lidded_room(BlockWorld& w, int hx, int hz) {
    for (int z = 0; z < w.n(); ++z)
        for (int x = 0; x < w.n(); ++x) {
            w.set(x, 0, z, Stone);
            w.set(x, 9, z, Stone);
        }
    w.set(hx, 9, hz, Air);
    for (int z = 0; z < w.n(); ++z)
        for (int x = 0; x < w.n(); ++x) w.refresh_column(x, z);
}

static void test_falling_fluid() {
    std::printf("\n== 3. falling: straight down, then spread from the bottom ==\n");

    {
        BlockWorld w(32, 32);
        lidded_room(w, 17, 16);
        Fluids f(w);
        f.place_source(16, 10, 16, Water);    // on the lid, one block from the hole
        const int t = f.settle();

        int colWidths[9];
        for (int y = 2; y <= 8; ++y) colWidths[y] = count_at_y(w, y, Water);
        const Puddle p = measure_puddle(f, w, 17, 1, 16, Water, kWater.dropOff);

        std::printf("  settled in %d ticks. column width y=8..2:", t);
        for (int y = 8; y >= 2; --y) std::printf(" %d", colWidths[y]);
        std::printf("\n  foot of the fall at y=1: %d cells, reach %d, span %d, levels %s\n",
                    p.cells, p.maxDist, p.spanX, p.levelsOk ? "level==distance" : "WRONG");
        std::printf("  mid-column (17,5,16): falling=%d level=%d source=%d\n",
                    int(f.falling(17, 5, 16)), f.level(17, 5, 16), int(f.is_source(17, 5, 16)));
        std::printf("  on the lid: %d cells (the source plus one step toward the hole)\n",
                    count_at_y(w, 10, Water));

        CHECK(t >= 0);
        // Water falls STRAIGHT down: one cell per layer the whole way, no fan.
        for (int y = 2; y <= 8; ++y) CHECK(colWidths[y] == 1);
        // The falling blocks are full (level 0) but are NOT sources.
        CHECK(f.falling(17, 5, 16));
        CHECK(f.level(17, 5, 16) == 0);
        CHECK(!f.is_source(17, 5, 16));
        CHECK(f.is_source(16, 10, 16));
        // The source found the hole and went only that way.
        CHECK(count_at_y(w, 10, Water) == 2);
        CHECK(f.level(17, 10, 16) == 1);
        // ...and the foot spreads the full 7, exactly as a source would, because
        // a falling block is full-strength.
        CHECK(p.maxDist == 7);
        CHECK(p.cells == 113);
        CHECK(p.levelsOk);
    }

    // The foot of a LAVA fall. spreadToSides writes the first neighbour at
    // amount 7 (the falling override), which looks like it should buy a fourth
    // block of reach. It does not: that neighbour's own tick recomputes it from
    // its biggest neighbour — the full falling block, amount 8 — to 8-2 = 6,
    // before it has spread anything. Measured here rather than assumed; see
    // UNVERIFIED note 1 at the top of this file.
    {
        BlockWorld w(32, 32);
        lidded_room(w, 17, 16);
        Fluids f(w, Dimension::Overworld);
        f.place_source(16, 10, 16, Lava);

        // Catch the transient: the level-1 write, then the level-2 correction.
        int wroteLevel1At = -1, correctedTo2At = -1;
        for (int t = 1; t <= 600; ++t) {
            f.tick();
            if (wroteLevel1At < 0 && f.level(18, 1, 16) == 1) wroteLevel1At = t;
            if (wroteLevel1At >= 0 && correctedTo2At < 0 && f.level(18, 1, 16) == 2)
                correctedTo2At = t;
        }
        const int t = f.settle();
        const Puddle p = measure_puddle(f, w, 17, 1, 16, Lava, kLavaOverworld.dropOff);
        std::printf("  lava fall foot: falling override wrote level 1 at tick %d, "
                    "getNewLiquid corrected it to level 2 at tick %d (+%d)\n",
                    wroteLevel1At, correctedTo2At, correctedTo2At - wroteLevel1At);
        std::printf("      settled: %d cells, reach %d, levels %s — the SAME as a "
                    "lava source on flat ground (%d)\n",
                    p.cells, p.maxDist, p.levelsOk ? "2,4,6" : "WRONG",
                    fluid_reach(kLavaOverworld));
        CHECK(t >= 0);
        CHECK(wroteLevel1At > 0);                       // the override really fires
        CHECK(correctedTo2At == wroteLevel1At + kLavaOverworld.tickDelay);
        CHECK(p.maxDist == fluid_reach(kLavaOverworld));  // ...and buys nothing
        CHECK(p.maxDist == 3);
        CHECK(p.cells == 25);
        CHECK(p.levelsOk);
        CHECK(f.falling(17, 1, 16));                    // the foot IS falling
        CHECK(f.level(17, 10, 16) == 2);                // on the lid, plain dropOff 2
    }

    // The case that looks like a bug and is not. A source hanging in OPEN AIR
    // fills the cell below it, and from the next tick on canSpreadTo(DOWN) is
    // false — the below cell already holds this fluid — so vanilla's spread()
    // falls through to its `fluidState.isSource()` branch and the source spreads
    // sideways as well. The four arms each start their own column, so a floating
    // source is a five-column shower, not a single stream, and its pool is a
    // radius-8 diamond (145 cells) rather than radius 7.
    {
        BlockWorld w(32, 32);
        flat_floor(w);
        Fluids f(w);
        f.place_source(16, 9, 16, Water);
        const int t = f.settle();
        const Puddle p = measure_puddle(f, w, 16, 1, 16, Water, kWater.dropOff);
        std::printf("  midair source (no lid): %d columns wide, pool %d cells, "
                    "reach %d, %d ticks\n",
                    count_at_y(w, 5, Water), p.cells, p.maxDist, t);
        CHECK(t >= 0);
        CHECK(count_at_y(w, 5, Water) == 5);   // centre + four arms
        CHECK(p.maxDist == 8);
        CHECK(p.cells == 145);                 // 1 + 4*(1+..+8)
    }
}

// ── 4. the drop search: the 1000-weight algorithm ──────────────────────────

// Put a hole in the floor `dist` blocks along +x from the source and report how
// many of the four neighbours the fluid chose on its very first spread.
static int first_step_directions(std::uint8_t fluid, Dimension dim, int dist, int delay) {
    BlockWorld w(48, 32);
    flat_floor(w);
    if (dist > 0) { w.set(24 + dist, 0, 24, Air); w.refresh_column(24 + dist, 24); }
    Fluids f(w, dim);
    f.place_source(24, 1, 24, fluid);
    f.run(delay);                       // exactly one spread step
    int n = 0;
    if (w.at(25, 1, 24) == fluid) ++n;
    if (w.at(23, 1, 24) == fluid) ++n;
    if (w.at(24, 1, 25) == fluid) ++n;
    if (w.at(24, 1, 23) == fluid) ++n;
    // and the +x one specifically
    return (w.at(25, 1, 24) == fluid) ? n : -n;
}

static void test_drop_search() {
    std::printf("\n== 4. drop search: 'a way down reachable in four or fewer\n"
                "      blocks from the block it wants to flow to' ==\n");

    for (int d = 1; d <= 7; ++d) {
        const int r = first_step_directions(Water, Dimension::Overworld, d, kWater.tickDelay);
        const bool towardHole = (r > 0 && r == 1);
        std::printf("  water, hole %d blocks away: %d direction(s) chosen%s\n",
                    d, r < 0 ? -r : r, towardHole ? "  <- only toward the hole" : "");
        if (d <= drop_search_range(kWater)) {
            CHECK(r == 1);              // exactly one direction, and it is +x
        } else {
            CHECK(r == 4 || r == -4);   // no hole found: all four tie at 1000
        }
    }
    // No hole at all: the flat case must be a four-way tie.
    const int flat = first_step_directions(Water, Dimension::Overworld, 0, kWater.tickDelay);
    std::printf("  water, no hole anywhere: %d directions chosen\n", flat < 0 ? -flat : flat);
    CHECK(flat == 4);

    for (int d = 1; d <= 5; ++d) {
        const int r = first_step_directions(Lava, Dimension::Overworld, d,
                                            kLavaOverworld.tickDelay);
        std::printf("  lava OW, hole %d blocks away: %d direction(s) chosen\n",
                    d, r < 0 ? -r : r);
        if (d <= drop_search_range(kLavaOverworld)) CHECK(r == 1);
        else CHECK(r == 4 || r == -4);
    }
}

// ── 5. infinite source ─────────────────────────────────────────────────────

static void test_infinite_source() {
    std::printf("\n== 5. infinite source: two horizontal sources over a solid floor ==\n");

    {
        BlockWorld w(24, 32);
        flat_floor(w);
        Fluids f(w);
        f.place_source(10, 1, 12, Water);
        f.place_source(12, 1, 12, Water);
        const int t = f.settle();
        std::printf("  water gap cell (11,1,12): level=%d source=%d, "
                    "conversions=%llu, settled %d ticks\n",
                    f.level(11, 1, 12), int(f.is_source(11, 1, 12)),
                    (unsigned long long)f.stats.sourcesMade, t);
        CHECK(t >= 0);
        CHECK(f.is_source(11, 1, 12));
        CHECK(f.level(11, 1, 12) == 0);
        CHECK(f.stats.sourcesMade >= 1);
    }
    {
        // Game rule lavaSourceConversion defaults to false, so lava must NOT do
        // this. The gap cell is fed level 2 from both sides and stays flowing.
        BlockWorld w(24, 32);
        flat_floor(w);
        Fluids f(w, Dimension::Overworld);
        f.place_source(10, 1, 12, Lava);
        f.place_source(12, 1, 12, Lava);
        const int t = f.settle();
        std::printf("  lava  gap cell (11,1,12): level=%d source=%d, "
                    "conversions=%llu, settled %d ticks\n",
                    f.level(11, 1, 12), int(f.is_source(11, 1, 12)),
                    (unsigned long long)f.stats.sourcesMade, t);
        CHECK(t >= 0);
        CHECK(!f.is_source(11, 1, 12));
        CHECK(f.level(11, 1, 12) == 2);
        CHECK(f.stats.sourcesMade == 0);
    }
    {
        // And a source with no solid floor under it does not convert either —
        // the "block that liquids cannot flow into below itself" clause.
        BlockWorld w(24, 32);
        flat_floor(w);
        w.set(11, 0, 12, Air);
        w.refresh_column(11, 12);
        Fluids f(w);
        f.place_source(10, 1, 12, Water);
        f.place_source(12, 1, 12, Water);
        f.settle();
        std::printf("  water gap cell over a HOLE: source=%d (must be 0)\n",
                    int(f.is_source(11, 1, 12)));
        CHECK(!f.is_source(11, 1, 12));
    }
}

// ── 6. mixing ──────────────────────────────────────────────────────────────

static void test_mixing() {
    std::printf("\n== 6. mixing: obsidian, cobblestone, stone ==\n");

    // (a) water onto a lava SOURCE -> obsidian, and with no tick elapsing,
    //     because LiquidBlock::onPlace runs the contact check on placement.
    {
        BlockWorld w(24, 32);
        flat_floor(w);
        Fluids f(w, Dimension::Overworld);
        f.place_source(12, 1, 12, Lava);
        f.settle();
        const int lavaBefore = int(w.count(Lava));
        f.place_source(12, 2, 12, Water);          // straight onto the source
        const int instantly = w.at(12, 1, 12);
        const int t = f.settle();
        std::printf("  water above a lava source: %s the instant it was placed "
                    "(lava blocks before=%d after=%llu, obsidian=%llu, cobble=%llu, %d ticks)\n",
                    block_name(instantly), lavaBefore,
                    (unsigned long long)w.count(Lava),
                    (unsigned long long)f.stats.obsidian,
                    (unsigned long long)f.stats.cobble, t);
        CHECK(instantly == Obsidian);
        CHECK(f.stats.obsidian == 1);
        CHECK(t >= 0);
        CHECK(w.count(Lava) == 0);                 // the flow it fed dies with it
    }

    // (b) flowing water meeting FLOWING lava -> cobblestone, no obsidian.
    {
        BlockWorld w(32, 32);
        flat_floor(w);
        Fluids f(w, Dimension::Overworld);
        f.place_source(4, 1, 16, Lava);
        f.settle();
        CHECK(w.at(7, 1, 16) == Lava);             // the far end of the lava flow
        CHECK(!f.is_source(7, 1, 16));
        f.place_source(14, 1, 16, Water);          // 7 blocks of water, aimed at it
        const int t = f.settle();
        std::printf("  flowing water into flowing lava: (7,1,16)=%s, "
                    "cobble=%llu obsidian=%llu, source at (4,1,16)=%s, %d ticks\n",
                    block_name(w.at(7, 1, 16)),
                    (unsigned long long)f.stats.cobble,
                    (unsigned long long)f.stats.obsidian,
                    block_name(w.at(4, 1, 16)), t);
        CHECK(t >= 0);
        CHECK(w.at(7, 1, 16) == Cobble);
        CHECK(f.stats.cobble >= 1);
        CHECK(f.stats.obsidian == 0);
        CHECK(w.at(4, 1, 16) == Lava);             // the lava source survives
        CHECK(f.is_source(4, 1, 16));
    }

    // (c) lava flowing DOWN into water -> the WATER turns to stone.
    {
        BlockWorld w(16, 24);
        flat_floor(w);
        for (int z = 0; z < w.n(); ++z)
            for (int x = 0; x < w.n(); ++x) w.set(x, 1, z, Water);
        Fluids f(w, Dimension::Overworld);
        f.rebuild();                                // a sea of sources
        const std::uint64_t updates0 = f.stats.fluidUpdates;
        f.run(20);
        std::printf("  a settled sea of %llu source blocks costs %llu fluid "
                    "updates in 20 ticks\n",
                    (unsigned long long)w.count(Water),
                    (unsigned long long)(f.stats.fluidUpdates - updates0));
        CHECK(f.stats.fluidUpdates == updates0);    // settled water must not tick
        CHECK(f.pending() == 0);

        f.place_source(8, 3, 8, Lava);
        const int t = f.settle();
        std::printf("  lava falling into the sea: (8,1,8)=%s, stone=%llu, %d ticks\n",
                    block_name(w.at(8, 1, 8)), (unsigned long long)f.stats.stone, t);
        CHECK(t >= 0);
        CHECK(w.at(8, 1, 8) == Stone);
        CHECK(f.stats.stone >= 1);
    }
}

// ── 7. gravity blocks ──────────────────────────────────────────────────────

static void test_gravity() {
    std::printf("\n== 7. gravity blocks ==\n");

    // The headline number: minecraft.wiki, Sand — "Sand and gravel take about
    // 0.45 seconds to fall one meter". 0.45 s is 9 game ticks, and 9 is
    // kGravityDelay (2) plus the integration time for the first metre (7).
    {
        BlockWorld w(8, 16);
        flat_floor(w);
        w.set(4, 2, 4, Sand);
        w.refresh_column(4, 4);
        Fluids f(w);
        f.block_changed(4, 2, 4);
        int landed = -1;
        for (int i = 1; i <= 60 && landed < 0; ++i) {
            f.tick();
            if (w.at(4, 1, 4) == Sand) landed = i;
        }
        std::printf("  sand falls one metre in %d ticks = %.3f s "
                    "(wiki: 'about 0.45 seconds')\n", landed, landed / 20.0);
        std::printf("      = %d ticks of tile-tick delay + %d ticks of entity fall\n",
                    kGravityDelay, landed - kGravityDelay);
        CHECK(landed == 9);
        CHECK(landed / 20.0 == 0.45);
        CHECK(w.at(4, 2, 4) == Air);
        CHECK(f.stats.entitiesSpawned == 1);
        CHECK(f.stats.entitiesLanded == 1);
        // Landing is a block placement, so it schedules one more tile tick on
        // itself — which looks at the floor, finds it solid, and stops. The
        // queue is empty a couple of ticks later, not on the landing tick.
        const int after = f.settle();
        std::printf("      queue empties %d ticks after it lands\n", after);
        CHECK(after == kGravityDelay);
        CHECK(f.pending() == 0);
    }

    // The integration itself, checked independently of the block world: with
    // v -= 0.04; y += v; v *= 0.98 the entity covers 1.0 m during tick 7.
    {
        double y = 0.0, v = 0.0;
        int firstMetre = 0;
        for (int t = 1; t <= 20; ++t) {
            v -= kFallGravity;
            y += v;
            v *= kFallDrag;
            if (y <= -1.0) { firstMetre = t; break; }
        }
        std::printf("  raw integration (g=%.2f, drag=%.2f, order accel/pos/drag): "
                    "1 m at tick %d, y=%.4f\n", kFallGravity, kFallDrag, firstMetre, y);
        CHECK(firstMetre == 7);
    }

    // A column collapses into a stack, in order, and nothing is lost.
    {
        BlockWorld w(8, 24);
        flat_floor(w);
        for (int y = 8; y <= 10; ++y) w.set(4, y, 4, Sand);
        w.set(4, 12, 4, Gravel);
        w.refresh_column(4, 4);
        Fluids f(w);
        for (int y = 8; y <= 12; ++y) f.block_changed(4, y, 4);
        const int t = f.settle();
        std::printf("  a 3-sand column + 1 gravel above it lands as %s/%s/%s/%s "
                    "in %d ticks (spawned=%llu landed=%llu lost=%llu)\n",
                    block_name(w.at(4, 1, 4)), block_name(w.at(4, 2, 4)),
                    block_name(w.at(4, 3, 4)), block_name(w.at(4, 4, 4)), t,
                    (unsigned long long)f.stats.entitiesSpawned,
                    (unsigned long long)f.stats.entitiesLanded,
                    (unsigned long long)f.stats.entitiesLost);
        CHECK(t >= 0);
        CHECK(w.at(4, 1, 4) == Sand);
        CHECK(w.at(4, 2, 4) == Sand);
        CHECK(w.at(4, 3, 4) == Sand);
        CHECK(w.at(4, 4, 4) == Gravel);   // it started on top, it stays on top
        CHECK(w.count(Sand) == 3);
        CHECK(w.count(Gravel) == 1);
        CHECK(f.stats.entitiesLost == 0);
    }

    // Sand falls THROUGH water (FallingBlock::isFree counts liquids as free) and
    // displaces it on landing — which kills the source and dries up its flow.
    {
        BlockWorld w(24, 24);
        flat_floor(w);
        Fluids f(w);
        f.place_source(12, 1, 12, Water);
        f.settle();
        const std::size_t before = w.count(Water);
        w.set(12, 8, 12, Sand);
        w.refresh_column(12, 12);
        f.block_changed(12, 8, 12);
        const int t = f.settle();
        std::printf("  sand dropped into a %zu-block puddle: (12,1,12)=%s, "
                    "water left=%zu, %d ticks\n",
                    before, block_name(w.at(12, 1, 12)), w.count(Water), t);
        CHECK(t >= 0);
        CHECK(before == 113);
        CHECK(w.at(12, 1, 12) == Sand);
        CHECK(w.count(Water) == 0);       // the only source was buried
    }

    // Water flowing over sand undermines nothing, but breaking the support
    // does: the block update path has to reach a gravity block through a fluid
    // neighbour, which is the case that silently does nothing if notify is wrong.
    {
        BlockWorld w(16, 24);
        flat_floor(w);
        w.set(8, 1, 8, Stone);
        w.set(8, 2, 8, Sand);
        w.refresh_column(8, 8);
        Fluids f(w);
        f.settle();
        CHECK(w.at(8, 2, 8) == Sand);     // supported: it must not move
        f.break_block(8, 1, 8);           // pull the support out
        const int t = f.settle();
        std::printf("  sand on a pillar, support mined: sand now at y=%d after %d ticks\n",
                    w.at(8, 1, 8) == Sand ? 1 : -1, t);
        CHECK(t >= 0);
        CHECK(w.at(8, 1, 8) == Sand);
        CHECK(w.at(8, 2, 8) == Air);
    }
}

// ── 8. determinism ─────────────────────────────────────────────────────────

static void test_determinism() {
    std::printf("\n== 8. determinism ==\n");

    // The only randomness in the file is LavaFluid::getSpreadDelay, which
    // quadruples the delay 3 times in 4 when a lava block gets DEEPER. A plain
    // spreading flow never gets deeper — every cell is written once and only
    // thins — so a naive scenario exercises no randomness at all and proves
    // nothing. This one does: let a flow settle, then put a second source next
    // to its thin far end so those cells are refilled at a higher amount.
    auto lava_run = [](std::uint64_t seed, int& ticks) {
        BlockWorld w(32, 32);
        flat_floor(w);
        Fluids f(w, Dimension::Overworld, seed);
        f.place_source(10, 1, 16, Lava);
        f.settle();
        f.place_source(14, 1, 16, Lava);      // (13,1,16) goes from level 6 to level 2
        ticks = f.settle();
        return f.digest();
    };

    int ta = 0, tb = 0;
    const std::uint64_t a = lava_run(12345, ta);
    const std::uint64_t b = lava_run(12345, tb);
    std::printf("  seed 12345 -> digest %016llx, %d ticks\n", (unsigned long long)a, ta);
    std::printf("  seed 12345 -> digest %016llx, %d ticks   (must match exactly)\n",
                (unsigned long long)b, tb);
    CHECK(a == b);
    CHECK(ta == tb);
    CHECK(ta > 0);

    // Different seeds: the same settled shape, reached on different schedules.
    // If every seed gave the same tick count the randomness would be dead code.
    int distinctTimings = 0;
    int seen[8];
    for (int k = 0; k < 8; ++k) {
        int t = 0;
        const std::uint64_t d = lava_run(1000ull + std::uint64_t(k) * 7919ull, t);
        CHECK(d == a);                        // shape is seed-independent
        bool isNew = true;
        for (int j = 0; j < distinctTimings; ++j) if (seen[j] == t) isNew = false;
        if (isNew) seen[distinctTimings++] = t;
    }
    std::printf("  8 other seeds: same digest every time, %d distinct settle times (",
                distinctTimings);
    for (int j = 0; j < distinctTimings; ++j) std::printf("%s%d", j ? " " : "", seen[j]);
    std::printf(" ticks) — the 4x lava delay is timing only\n");
    CHECK(distinctTimings > 1);
}

// ── 9. cost ────────────────────────────────────────────────────────────────

static void test_cost() {
    std::printf("\n== 9. cost: per-tick work must not scale with the volume ==\n");

    // The same local scenario in a small world and a large one. If anything in
    // tick() scanned, these numbers would differ.
    std::uint64_t updates[2], probes[2];
    int ticks[2];
    double build[2], runMs[2];
    const int sizes[2] = {64, 256};
    for (int k = 0; k < 2; ++k) {
        const int n = sizes[k];
        auto t0 = std::chrono::steady_clock::now();
        BlockWorld w(n, 128);
        flat_floor(w);
        Fluids f(w);
        build[k] = ms_since(t0);
        f.place_source(n / 2, 9, n / 2, Water);
        t0 = std::chrono::steady_clock::now();
        ticks[k] = f.settle();
        runMs[k] = ms_since(t0);
        updates[k] = f.stats.fluidUpdates;
        probes[k] = f.stats.cellProbes;
        std::printf("  %3dx128x%-3d (%8zu cells): setup %6.1f ms | settled in %d ticks, "
                    "%llu fluid updates, %llu probes, %.2f ms\n",
                    n, n, w.volume(), build[k], ticks[k],
                    (unsigned long long)updates[k], (unsigned long long)probes[k], runMs[k]);
    }
    CHECK(ticks[0] == ticks[1]);
    CHECK(updates[0] == updates[1]);      // identical work in a 64x world and a 256x one
    CHECK(probes[0] == probes[1]);

    // Probes per fluid update, against the bound in the header: for water the
    // slope search cannot exceed 4*(3+9+27+81) = 480 probe pairs per call.
    // The slope search is the one non-trivial constant in a fluid update. Its
    // bound is 480 visited cells at up to two world reads each; anything above
    // that means the depth cap or the backtrack exclusion has come undone.
    const double perUpdate = double(probes[0]) / double(updates[0]);
    std::printf("  slope-search world reads per fluid update: %.1f (hard bound %d)\n",
                perUpdate, 2 * 4 * (3 + 9 + 27 + 81));
    CHECK(perUpdate <= 2.0 * 4.0 * (3 + 9 + 27 + 81));

    // A real world: generate terrain, adopt the ocean, then flood it.
    {
        auto t0 = std::chrono::steady_clock::now();
        BlockWorld w(96, 128);
        w.generate(20260814ull);
        const double genMs = ms_since(t0);
        t0 = std::chrono::steady_clock::now();
        Fluids f(w);
        f.rebuild();
        const double rebuildMs = ms_since(t0);
        std::printf("  generated 96x128x96: %zu cells, %zu water, gen %.0f ms, "
                    "rebuild %.0f ms\n", w.volume(), w.count(Water), genMs, rebuildMs);

        // A settled ocean must cost nothing at all.
        f.run(100);
        std::printf("  100 ticks over a settled generated ocean: %llu fluid updates\n",
                    (unsigned long long)f.stats.fluidUpdates);
        CHECK(f.stats.fluidUpdates == 0);
        CHECK(f.pending() == 0);

        // Now break it: carve a room under the sea bed and open a shaft into it.
        const int sea = w.sea_level();
        int sx = -1, sz = -1;
        for (int z = 8; z < 88 && sx < 0; ++z)
            for (int x = 8; x < 88; ++x)
                if (w.surface(x, z) < sea - 4 && w.at(x, sea, z) == Water) { sx = x; sz = z; break; }
        std::printf("  drain site (%d,%d): sea bed y=%d, sea level y=%d\n",
                    sx, sz, sx < 0 ? -1 : w.surface(sx, sz), sea);
        CHECK(sx >= 0);

        // The sea bed height has to be read BEFORE anything is carved — once the
        // shaft is cut, surface() for that column reports the room floor and the
        // shaft gets dug in the wrong place, which silently produces a drain
        // that drains nothing.
        const int bed = w.surface(sx, sz);
        const int roomTop = 30, roomBot = 26, half = 6;
        for (int y = roomBot; y <= roomTop; ++y)
            for (int z = sz - half; z <= sz + half; ++z)
                for (int x = sx - half; x <= sx + half; ++x)
                    if (w.inside(x, y, z) && w.at(x, y, z) != Water) w.set(x, y, z, Air);
        for (int y = roomTop; y <= bed; ++y) w.set(sx, y, sz, Air);      // the shaft
        for (int z = sz - half; z <= sz + half; ++z)
            for (int x = sx - half; x <= sx + half; ++x) w.refresh_column(x, z);
        CHECK(w.at(sx, bed + 1, sz) == Water);      // the ocean cell above the shaft

        t0 = std::chrono::steady_clock::now();
        f.block_changed(sx, bed + 1, sz);           // "a block just vanished below you"
        const std::size_t volume = w.volume();
        const int t = f.settle();
        const double floodMs = ms_since(t0);
        const double perTick = double(f.stats.fluidUpdates) / double(t > 0 ? t : 1);
        std::printf("  ocean drains %d blocks down a shaft into a %dx%dx%d room: "
                    "settled in %d ticks, %.1f ms\n",
                    bed - roomTop, 2 * half + 1, roomTop - roomBot + 1, 2 * half + 1,
                    t, floodMs);
        std::printf("      %llu fluid updates, %llu gravity updates, %llu spreads, "
                    "peak queue %zu\n",
                    (unsigned long long)f.stats.fluidUpdates,
                    (unsigned long long)f.stats.blockUpdates,
                    (unsigned long long)f.stats.spreads, f.stats.peakPending);
        std::printf("      average %.1f fluid updates per tick = %.5f%% of the "
                    "%zu-cell volume a scan would touch\n",
                    perTick, 100.0 * perTick / double(volume), volume);
        std::printf("      %.0f ticks/s on this machine\n", t / (floodMs / 1000.0));
        CHECK(t >= 0);
        CHECK(f.stats.fluidUpdates > 0);
        // The claim: per-tick work is a rounding error against the volume.
        CHECK(perTick * 1000.0 < double(volume));
        CHECK(f.stats.peakPending * 100 < volume);
    }

    // The heavy case, because a 3.9-updates-per-tick drain proves very little.
    // A 192-wide floor tiled with water sources 20 apart: every one of them
    // floods at once, so this is the widest simultaneously-active fluid front
    // the model can produce. The number that matters is the WORST tick, not the
    // average — an average hides the spike that would drop a frame.
    {
        const int n = 192, step = 20;
        BlockWorld w(n, 128);
        flat_floor(w);
        Fluids f(w);
        int sources = 0;
        for (int z = 10; z < n - 10; z += step)
            for (int x = 10; x < n - 10; x += step) { f.place_source(x, 1, z, Water); ++sources; }

        std::uint64_t prev = 0, worst = 0;
        int worstTick = 0, t = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (f.pending() != 0 && t < 20000) {
            f.tick();
            ++t;
            const std::uint64_t d = f.stats.fluidUpdates - prev;
            prev = f.stats.fluidUpdates;
            if (d > worst) { worst = d; worstTick = t; }
        }
        const double msec = ms_since(t0);
        std::printf("  %d simultaneous floods on a %dx%d floor: settled in %d ticks, "
                    "%.1f ms, %.0f ticks/s\n", sources, n, n, t, msec, t / (msec / 1000.0));
        std::printf("      %zu water cells, %llu fluid updates, peak queue %zu\n",
                    w.count(Water), (unsigned long long)f.stats.fluidUpdates,
                    f.stats.peakPending);
        std::printf("      WORST tick: %llu fluid updates (at tick %d) = %.4f%% of the "
                    "%zu-cell volume\n", (unsigned long long)worst, worstTick,
                    100.0 * double(worst) / double(w.volume()), w.volume());
        std::printf("      peak queue is %.2fx the number of live fluid cells — the "
                    "per-(cell,kind) dedupe holding\n",
                    double(f.stats.peakPending) / double(w.count(Water)));
        CHECK(f.pending() == 0);
        CHECK(sources == 81);
        CHECK(w.count(Water) == std::size_t(sources) * 113);   // the diamonds do not touch
        // Even the worst single tick touches well under 1% of the volume, and
        // the queue never holds more entries than there are fluid cells.
        CHECK(double(worst) < 0.01 * double(w.volume()));
        CHECK(f.stats.peakPending <= w.count(Water));
    }
}

// ── main ───────────────────────────────────────────────────────────────────

int main() {
    std::printf("fluids.hpp — Minecraft Java Edition fluid flow, mixing and gravity\n");
    std::printf("every number below is measured from the running simulation\n");

    test_constants();
    test_flat_reach();
    test_falling_fluid();
    test_drop_search();
    test_infinite_source();
    test_mixing();
    test_gravity();
    test_determinism();
    test_cost();

    std::printf("\nOK — %d assertions passed.\n", g_checks);
    return 0;
}
