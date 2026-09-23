// test_biomes.cpp — self-test for src/world/biomes.hpp.
//
// Everything printed here is MEASURED at run time. Where a number came out of a
// Minecraft data file the assertion names the file; where a number is a
// consequence of this file's own construction (the equalised band fractions)
// the assertion recomputes it from the published edges rather than restating a
// constant, so it is a real check of the classifier and not a tautology.
//
// build:
//   g++ -std=c++20 -O2 -Wall -Wextra -Isrc test_biomes.cpp -o test_biomes.exe
//       -static -static-libgcc -static-libstdc++
// (delete the exe first — a stale binary has produced false passes here before)

#include "world/biomes.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace bench;

static int g_checks = 0;

#define CHECK(cond, ...)                                                          \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(cond)) {                                                            \
            std::printf("\nFAIL %s:%d  %s\n     ", __FILE__, __LINE__, #cond);    \
            std::printf(__VA_ARGS__);                                             \
            std::printf("\n");                                                    \
            std::exit(1);                                                         \
        }                                                                         \
    } while (0)

static void rule(const char* title) {
    std::printf("\n== %s ==\n", title);
}

// ────────────────────────────────────────────────────────────────────────────
// 1. the climate table, against the vanilla biome JSONs (23w31a)
// ────────────────────────────────────────────────────────────────────────────
static void test_climate() {
    rule("climate: worldgen/biome/<name>.json, Java Edition 23w31a");

    struct Row { Biome b; float t; float d; bool precip; };
    const Row rows[] = {
        { Biome::Plains,      0.8f,  0.4f, true  },
        { Biome::Forest,      0.7f,  0.8f, true  },
        { Biome::Taiga,       0.25f, 0.8f, true  },
        { Biome::Desert,      2.0f,  0.0f, false },
        { Biome::Savanna,     2.0f,  0.0f, false },
        { Biome::SnowyPlains, 0.0f,  0.5f, true  },
        { Biome::Swamp,       0.8f,  0.9f, true  },   // JSON says 0.8, wiki prose says 0.9
        { Biome::Jungle,      0.95f, 0.9f, true  },
        { Biome::Beach,       0.8f,  0.4f, true  },
        { Biome::Ocean,       0.5f,  0.5f, true  },
        { Biome::Mountains,   0.2f,  0.3f, true  },   // windswept_hills.json
    };

    std::printf("%-14s %6s %8s %7s %6s\n", "biome", "temp", "downfall", "precip", "snows");
    for (const Row& r : rows) {
        const BiomeDef& d = biome_def(r.b);
        std::printf("%-14s %6.2f %8.2f %7s %6s\n", d.name,
                    double(d.temperature), double(d.downfall),
                    d.has_precipitation ? "yes" : "no",
                    snows(r.b) ? "yes" : "no");
        CHECK(d.temperature == r.t, "%s temperature %.4f, expected %.4f",
              d.name, double(d.temperature), double(r.t));
        CHECK(d.downfall == r.d, "%s downfall %.4f, expected %.4f",
              d.name, double(d.downfall), double(r.d));
        CHECK(d.has_precipitation == r.precip, "%s has_precipitation wrong", d.name);
    }

    // Java: warmEnoughToRain is temperature >= 0.15F. Only Snowy Plains is
    // below that at sea level; Taiga at 0.25 rains, which is why plain taiga
    // is not white and snowy taiga is a separate biome.
    CHECK(snows(Biome::SnowyPlains), "snowy plains must snow");
    CHECK(!snows(Biome::Taiga), "taiga at temperature 0.25 must NOT snow at sea level");
    CHECK(!snows(Biome::Desert), "desert has no precipitation at all");
    CHECK(!snows(Biome::Savanna), "savanna has no precipitation at all");
    std::printf("  rain threshold %.2f: snowy_plains snows, taiga(0.25) rains\n",
                double(kRainThreshold));
}

// ────────────────────────────────────────────────────────────────────────────
// 2. the altitude lapse rate, Biome.getTemperature
// ────────────────────────────────────────────────────────────────────────────
static void test_lapse_rate() {
    rule("altitude: base - (wobble + y - 80) * 0.05/40   [Biome.getTemperature]");

    CHECK(std::fabs(kLapseRatePerBlock - 0.00125f) < 1e-7f,
          "lapse rate %.8f, expected 0.00125 (= 1/800)", double(kLapseRatePerBlock));
    std::printf("  0.05/40 = %.5f per block = 1/%.0f\n",
                double(kLapseRatePerBlock), 1.0 / double(kLapseRatePerBlock));

    // Below and at y=80 the base temperature applies unchanged.
    for (int y : {0, 40, 63, 80}) {
        const float t = temperature_at(Biome::Mountains, y);
        CHECK(t == biome_def(Biome::Mountains).temperature,
              "y=%d changed the base temperature to %.5f", y, double(t));
    }
    std::printf("  y<=80 leaves temperature at base (mountains %.2f)\n",
                double(biome_def(Biome::Mountains).temperature));

    // Windswept Hills is 0.2 at sea level. Snow needs it under 0.15, i.e. a
    // drop of more than 0.05, i.e. more than 40 blocks above y=80.
    const int predicted = kLapseStartY + 40;
    int first_snow = -1;
    for (int y = 81; y < 320; ++y)
        if (snows_at(Biome::Mountains, y)) { first_snow = y; break; }
    std::printf("  mountains(0.20) first snows at y=%d, predicted %d\n",
                first_snow, predicted + 1);
    CHECK(first_snow == predicted + 1,
          "snow line y=%d, expected %d", first_snow, predicted + 1);

    const float t121 = temperature_at(Biome::Mountains, 121);
    std::printf("  temperature_at(mountains, y=121) = %.5f  (0.20 - 41*0.00125)\n",
                double(t121));
    CHECK(std::fabs(t121 - (0.2f - 41.0f * 0.00125f)) < 1e-6f,
          "y=121 temperature %.6f", double(t121));

    // The wobble term is caller-supplied because Mojang's noise stream is not
    // reproducible here; it must still move the answer in the right direction.
    CHECK(temperature_at(Biome::Mountains, 200, +8.0f)
        < temperature_at(Biome::Mountains, 200, -8.0f),
          "wobble sign is inverted");
    std::printf("  wobble +-8 spans %.5f of temperature (= 16 * 0.00125)\n",
                double(temperature_at(Biome::Mountains, 200, -8.0f)
                     - temperature_at(Biome::Mountains, 200, +8.0f)));
}

// ────────────────────────────────────────────────────────────────────────────
// 3. the grass colormap index, GrassColor.get
// ────────────────────────────────────────────────────────────────────────────
static void test_colormap_index() {
    rule("colormap index: GrassColor.get, vs minecraft.wiki 'Block colors'");

    // Published (x, y) coordinates into the 256x256 grass.png.
    struct Row { Biome b; int x; int y; };
    const Row rows[] = {
        { Biome::Plains,       50, 173 },
        { Biome::Forest,       76, 112 },
        { Biome::Taiga,       191, 203 },
        { Biome::Desert,        0, 255 },
        { Biome::Savanna,       0, 255 },
        { Biome::SnowyPlains, 255, 255 },
        { Biome::Jungle,       12,  36 },
        { Biome::Beach,        50, 173 },
        { Biome::Ocean,       127, 191 },
        { Biome::Mountains,   203, 239 },
    };

    std::printf("%-14s %5s %5s %8s   %-8s %-8s\n",
                "biome", "cmapX", "cmapY", "pixel", "grass", "foliage");
    for (const Row& r : rows) {
        const BiomeDef& d = biome_def(r.b);
        const ColormapIndex c = grass_colormap_index(r.b);
        std::printf("%-14s %5d %5d %8d   #%06X #%06X\n",
                    d.name, c.x, c.y, c.pixel, d.grass, d.foliage);
        CHECK(c.x == r.x && c.y == r.y,
              "%s colormap index (%d,%d), wiki publishes (%d,%d)",
              d.name, c.x, c.y, r.x, r.y);
        CHECK(c.pixel == ((r.y << 8) | r.x), "%s linear pixel wrong", d.name);
    }

    // Taiga is the case that only comes out right in float. 0.25 * 0.8 in exact
    // arithmetic gives y = 204; 0.8f widened to double gives 203, which is what
    // the game and the wiki both have.
    {
        const double exact_j = (1.0 - 0.25 * 0.8) * 255.0;
        std::printf("  taiga: exact arithmetic gives y=%d, float-widened gives y=%d "
                    "(wiki: 203)\n",
                    int(exact_j), grass_colormap_index(Biome::Taiga).y);
        CHECK(int(exact_j) == 204, "sanity: exact arithmetic should give 204");
        CHECK(grass_colormap_index(Biome::Taiga).y == 203, "float path lost");
    }

    // Desert temperature is 2.0 and must clamp to 1.0 before the lookup, or the
    // index goes negative.
    const ColormapIndex des = grass_colormap_index(Biome::Desert);
    CHECK(des.x == 0 && des.y == 255, "desert clamp failed: (%d,%d)", des.x, des.y);
    std::printf("  desert temperature 2.0 clamps to 1.0 -> corner (0,255)\n");

    // Swamp has no colormap colour: swamp.json carries grass_color_modifier.
    CHECK(biome_def(Biome::Swamp).foliage == 0x6A7039,
          "swamp foliage_color should be 6975545 = 0x6A7039");
    CHECK(biome_def(Biome::Swamp).water == 0x617B64,
          "swamp water_color should be 6388580 = 0x617B64");
    std::printf("  swamp overrides: foliage #%06X (6975545), water #%06X (6388580)\n",
                biome_def(Biome::Swamp).foliage, biome_def(Biome::Swamp).water);

    // Every other biome here uses the default water colour 4159204.
    int default_water = 0;
    for (int i = 0; i < kBiomeCount; ++i)
        if (kBiomeTable[std::size_t(i)].water == 0x3F76E4) ++default_water;
    CHECK(default_water == kBiomeCount - 1,
          "%d of %d biomes use water 0x3F76E4, expected all but swamp",
          default_water, kBiomeCount);
    std::printf("  %d of %d biomes use water_color 4159204 = #3F76E4\n",
                default_water, kBiomeCount);
}

// ────────────────────────────────────────────────────────────────────────────
// 4. tree density, against worldgen/placed_feature/*.json (23w31a)
// ────────────────────────────────────────────────────────────────────────────
static void test_tree_density() {
    rule("trees/chunk: worldgen/placed_feature count weighted_list, 23w31a");

    struct Row { Biome b; const char* feature; double mean; };
    const Row rows[] = {
        { Biome::Plains,      "trees_plains          {0:19, 1:1}", 0.05 },
        { Biome::Forest,      "trees_birch_and_oak   {10:9,11:1}", 10.1 },
        { Biome::Taiga,       "trees_taiga           {10:9,11:1}", 10.1 },
        { Biome::Desert,      "(no tree feature)                ", 0.00 },
        { Biome::Savanna,     "trees_savanna         {1:9,  2:1}", 1.10 },
        { Biome::SnowyPlains, "trees_snowy           {0:9,  1:1}", 0.10 },
        { Biome::Swamp,       "trees_swamp           {2:9,  3:1}", 2.10 },
        { Biome::Jungle,      "trees_jungle          {50:9,51:1}", 50.1 },
        { Biome::Beach,       "(no tree feature)                ", 0.00 },
        { Biome::Ocean,       "(no tree feature)                ", 0.00 },
        { Biome::Mountains,   "trees_windswept_hills {0:9,  1:1}", 0.10 },
    };

    // Roll the weighted list a lot and check the sample mean lands on the
    // published expectation. This is the only way the count constants are
    // actually exercised rather than merely typed.
    const int kRolls = 400000;
    std::printf("%-14s %-34s %9s %11s\n", "biome", "placed_feature", "published", "measured");
    for (const Row& r : rows) {
        const BiomeDef& d = biome_def(r.b);
        CHECK(std::fabs(d.trees.mean() - r.mean) < 1e-9,
              "%s mean() = %.6f, expected %.6f", d.name, d.trees.mean(), r.mean);

        Rng rng(0xB10E5EEDull ^ (std::uint64_t(bidx(r.b)) << 32));
        double sum = 0.0;
        for (int i = 0; i < kRolls; ++i) sum += roll_tree_count(d.trees, rng);
        const double measured = sum / double(kRolls);
        std::printf("%-14s %-34s %9.2f %11.4f\n", d.name, r.feature, r.mean, measured);
        // Rolling a two-point distribution: the standard error is tiny, but
        // the tolerance is on the WEIGHT ratio, which is 1-in-10 or 1-in-20.
        CHECK(std::fabs(measured - r.mean) < 0.02,
              "%s measured %.4f trees/chunk, published %.4f", d.name, measured, r.mean);
    }

    // Desert's zero is a checked zero: desert.json has no tree feature in any
    // generation step. Beach and Ocean likewise.
    CHECK(biome_def(Biome::Desert).tree == TreeKind::None, "desert must have no trees");
    CHECK(biome_def(Biome::Ocean).tree  == TreeKind::None, "ocean must have no trees");
    CHECK(biome_def(Biome::Beach).tree  == TreeKind::None, "beach must have no trees");

    // The ordering that matters for a wood-gated tech tree.
    CHECK(biome_def(Biome::Jungle).trees.mean() > biome_def(Biome::Forest).trees.mean(),
          "jungle must out-tree forest");
    CHECK(biome_def(Biome::Forest).trees.mean() > biome_def(Biome::Savanna).trees.mean(),
          "forest must out-tree savanna");
    CHECK(biome_def(Biome::Savanna).trees.mean() > biome_def(Biome::Mountains).trees.mean(),
          "savanna must out-tree mountains");
    CHECK(biome_def(Biome::Mountains).trees.mean() > biome_def(Biome::Plains).trees.mean(),
          "windswept hills must out-tree plains");
    std::printf("  jungle %.1f > forest %.1f > savanna %.1f > mountains %.2f "
                "> plains %.2f > desert %.0f\n",
                biome_def(Biome::Jungle).trees.mean(), biome_def(Biome::Forest).trees.mean(),
                biome_def(Biome::Savanna).trees.mean(), biome_def(Biome::Mountains).trees.mean(),
                biome_def(Biome::Plains).trees.mean(), biome_def(Biome::Desert).trees.mean());
    std::printf("  forest and taiga are the same feature shape: %.2f each\n",
                biome_def(Biome::Taiga).trees.mean());
}

// ────────────────────────────────────────────────────────────────────────────
// 5. the parameter bands, and that band_of agrees with the published edges
// ────────────────────────────────────────────────────────────────────────────
static void test_bands() {
    rule("parameter bands: minecraft.wiki 'World generation' 1.18+");

    // Temperature: -1.0~-0.45, -0.45~-0.15, -0.15~0.2, 0.2~0.55, 0.55~1.0
    CHECK(band_of(-1.00f, kTemperatureEdges) == 0, "T(-1.00)");
    CHECK(band_of(-0.46f, kTemperatureEdges) == 0, "T(-0.46)");
    CHECK(band_of(-0.45f, kTemperatureEdges) == 1, "T(-0.45) is the level-1 edge");
    CHECK(band_of(-0.15f, kTemperatureEdges) == 2, "T(-0.15)");
    CHECK(band_of( 0.20f, kTemperatureEdges) == 3, "T(0.20)");
    CHECK(band_of( 0.55f, kTemperatureEdges) == 4, "T(0.55)");
    CHECK(band_of( 1.00f, kTemperatureEdges) == 4, "T(1.00)");

    // Humidity: -1.0~-0.35, -0.35~-0.1, -0.1~0.1, 0.1~0.3, 0.3~1.0
    CHECK(band_of(-0.36f, kHumidityEdges) == 0, "H(-0.36)");
    CHECK(band_of(-0.35f, kHumidityEdges) == 1, "H(-0.35)");
    CHECK(band_of(-0.10f, kHumidityEdges) == 2, "H(-0.10)");
    CHECK(band_of( 0.10f, kHumidityEdges) == 3, "H(0.10)");
    CHECK(band_of( 0.30f, kHumidityEdges) == 4, "H(0.30)");

    // Erosion: 7 levels, edges -0.78, -0.375, -0.2225, 0.05, 0.45, 0.55
    CHECK(band_of(-1.00f, kErosionEdges) == 0, "E(-1.00)");
    CHECK(band_of(-0.78f, kErosionEdges) == 1, "E(-0.78)");
    CHECK(band_of( 0.55f, kErosionEdges) == 6, "E(0.55)");
    CHECK(band_of( 1.00f, kErosionEdges) == 6, "E(1.00)");

    // Continentalness: ocean ends at -0.19, coast at -0.11.
    CHECK(band_of(-0.20f, kContinentalEdges) == 2, "C(-0.20) is ocean");
    CHECK(band_of(-0.19f, kContinentalEdges) == 3, "C(-0.19) is coast");
    CHECK(band_of(-0.11f, kContinentalEdges) == 4, "C(-0.11) is near-inland");

    // Band widths, as fractions of an equalised [-1,1] field. These are pure
    // arithmetic on the published edges, printed so the later map measurement
    // has something to be compared against.
    const char* tn[5] = { "T0", "T1", "T2", "T3", "T4" };
    double tsum = 0.0;
    std::printf("  temperature band shares:");
    for (int i = 0; i < 5; ++i) {
        const double f = band_fraction(i, kTemperatureEdges);
        tsum += f;
        std::printf(" %s=%.3f", tn[i], f);
    }
    std::printf("  (sum %.6f)\n", tsum);
    CHECK(std::fabs(tsum - 1.0) < 1e-9, "temperature bands sum to %.9f", tsum);

    double hsum = 0.0;
    std::printf("  humidity    band shares:");
    for (int i = 0; i < 5; ++i) {
        const double f = band_fraction(i, kHumidityEdges);
        hsum += f;
        std::printf(" H%d=%.3f", i, f);
    }
    std::printf("  (sum %.6f)\n", hsum);
    CHECK(std::fabs(hsum - 1.0) < 1e-9, "humidity bands sum to %.9f", hsum);

    // Continentalness band 0 is -1.2..-1.05, outside an equalised [-1,1] field,
    // so mushroom fields are unreachable here by construction. Say so out loud.
    CHECK(band_fraction(0, kContinentalEdges) == 0.0,
          "continentalness band 0 should be unreachable in [-1,1]");
    std::printf("  continentalness band 0 (mushroom fields, -1.2..-1.05) is "
                "unreachable in [-1,1]: share %.3f\n",
                band_fraction(0, kContinentalEdges));
}

// ────────────────────────────────────────────────────────────────────────────
// 6. the classifier, driven directly
// ────────────────────────────────────────────────────────────────────────────
static void test_classifier() {
    rule("biome_from_parameters: continentalness > erosion > temperature x humidity");

    struct Row { float t, h, c, e; Biome want; const char* why; };
    const Row rows[] = {
        { 0.0f,  0.0f, -0.90f,  0.0f, Biome::Ocean,       "deep ocean band" },
        { 0.9f,  0.9f, -0.30f,  0.0f, Biome::Ocean,       "hot and wet, still ocean" },
        { 0.0f,  0.0f, -0.15f,  0.0f, Biome::Beach,       "coast band" },
        { 0.9f,  0.0f,  0.50f, -0.90f, Biome::Mountains,  "erosion 0 beats climate" },
        { 0.0f,  0.0f,  0.50f, -0.90f, Biome::Mountains,  "cold mountains too" },
        {-0.90f,-0.90f, 0.50f,  0.80f, Biome::SnowyPlains,"erosion 6 but T0: no swamp" },
        { 0.0f,  0.0f,  0.50f,  0.80f, Biome::Swamp,      "erosion 6, T2" },
        {-0.30f, 0.0f,  0.50f,  0.80f, Biome::Swamp,      "erosion 6, T1" },
        { 0.40f, 0.0f,  0.50f,  0.80f, Biome::Swamp,      "erosion 6, T3" },
        { 0.90f, 0.0f,  0.50f,  0.80f, Biome::Desert,     "erosion 6 but T4: no swamp" },
        {-0.90f,-0.90f, 0.50f,  0.0f,  Biome::SnowyPlains,"T0 H0" },
        {-0.90f, 0.90f, 0.50f,  0.0f,  Biome::Taiga,      "T0 H4" },
        {-0.30f,-0.90f, 0.50f,  0.0f,  Biome::Plains,     "T1 H0" },
        {-0.30f, 0.00f, 0.50f,  0.0f,  Biome::Forest,     "T1 H2" },
        { 0.00f, 0.00f, 0.50f,  0.0f,  Biome::Forest,     "T2 H2" },
        { 0.00f,-0.20f, 0.50f,  0.0f,  Biome::Plains,     "T2 H1" },
        { 0.40f,-0.90f, 0.50f,  0.0f,  Biome::Savanna,    "T3 H0" },
        { 0.40f, 0.90f, 0.50f,  0.0f,  Biome::Jungle,     "T3 H4" },
        { 0.90f, 0.90f, 0.50f,  0.0f,  Biome::Desert,     "T4 is desert at every humidity" },
    };

    for (const Row& r : rows) {
        const Biome got = biome_from_parameters(r.t, r.h, r.c, r.e);
        std::printf("  T%+.2f H%+.2f C%+.2f E%+.2f -> %-12s  (%s)\n",
                    double(r.t), double(r.h), double(r.c), double(r.e),
                    biome_name(got), r.why);
        CHECK(got == r.want, "got %s, wanted %s", biome_name(got), biome_name(r.want));
    }

    // The whole T4 row is desert in vanilla's MIDDLE_BIOMES. Check all five.
    for (int h = 0; h < 5; ++h)
        CHECK(kMiddleBiomes[4][std::size_t(h)] == Biome::Desert,
              "T4 H%d is not desert", h);
    std::printf("  T4 row is desert at all five humidity levels (vanilla MIDDLE_BIOMES)\n");
}

// ────────────────────────────────────────────────────────────────────────────
// 7. a baked map: does the measured area match the arithmetic from the edges?
// ────────────────────────────────────────────────────────────────────────────
static double predicted_fraction(Biome b, float bias) {
    // Sum the continentalness bands that mean "open water" / "coast" / "inland".
    double ocean = 0.0, coast = 0.0;
    for (int c = 0; c <= kContOcean; ++c) ocean += band_fraction(c, kContinentalEdges, bias);
    coast = band_fraction(kContCoast, kContinentalEdges, bias);
    const double inland = std::max(0.0, 1.0 - ocean - coast);

    if (b == Biome::Ocean) return ocean;
    if (b == Biome::Beach) return coast;

    const double mountain = band_fraction(kErosionMountain, kErosionEdges);
    if (b == Biome::Mountains) return inland * mountain;

    const double flat = band_fraction(kErosionSwamp, kErosionEdges);
    double swamp_t = 0.0;
    for (int t = 1; t <= 3; ++t) swamp_t += band_fraction(t, kTemperatureEdges);
    if (b == Biome::Swamp) return inland * flat * swamp_t;

    // Everything else falls through the 5x5 table on the inland area that is
    // neither mountain nor swamp.
    double share = 0.0;
    for (int t = 0; t < 5; ++t) {
        const bool swamped = (t >= 1 && t <= 3);
        const double t_area = band_fraction(t, kTemperatureEdges)
                            * (1.0 - mountain - (swamped ? flat : 0.0));
        for (int h = 0; h < 5; ++h)
            if (kMiddleBiomes[std::size_t(t)][std::size_t(h)] == b)
                share += t_area * band_fraction(h, kHumidityEdges);
    }
    return inland * share;
}

// A representative parameter value inside a band — the classifier is constant
// across a band, so any interior point stands for the whole of it.
template <std::size_t N>
static float band_midpoint(int level, const std::array<float, N>& edges) {
    const double lo = (level == 0) ? -1.0
                    : std::clamp(double(edges[std::size_t(level) - 1]), -1.0, 1.0);
    const double hi = (level == int(N)) ? 1.0
                    : std::clamp(double(edges[std::size_t(level)]), -1.0, 1.0);
    return float(0.5 * (lo + hi));
}

// Exact area integration: the classifier is piecewise constant over the 5x5x7x7
// grid of band tuples, so summing the product of the band widths over every
// tuple integrates it exactly. No noise, no sampling, no statistics — this
// checks the closed form in predicted_fraction() against the classifier itself.
static void test_area_exact() {
    rule("exact area integration: 5x5x7x7 band tuples vs the closed form");

    std::array<double, kBiomeCount> integrated{};
    integrated.fill(0.0);
    int tuples = 0;
    for (int t = 0; t < 5; ++t)
        for (int h = 0; h < 5; ++h)
            for (int c = 0; c < 7; ++c)
                for (int e = 0; e < 7; ++e) {
                    const double w = band_fraction(t, kTemperatureEdges)
                                   * band_fraction(h, kHumidityEdges)
                                   * band_fraction(c, kContinentalEdges)
                                   * band_fraction(e, kErosionEdges);
                    ++tuples;
                    if (w <= 0.0) continue;
                    const Biome b = biome_from_parameters(
                        band_midpoint(t, kTemperatureEdges),
                        band_midpoint(h, kHumidityEdges),
                        band_midpoint(c, kContinentalEdges),
                        band_midpoint(e, kErosionEdges));
                    integrated[std::size_t(bidx(b))] += w;
                }

    std::printf("  %d band tuples enumerated\n", tuples);
    std::printf("%-14s %12s %12s\n", "biome", "closed form", "integrated");
    double total = 0.0;
    for (int i = 0; i < kBiomeCount; ++i) {
        const Biome b = static_cast<Biome>(i);
        const double closed = predicted_fraction(b, 0.0f);
        const double integ  = integrated[std::size_t(i)];
        total += integ;
        std::printf("%-14s %12.6f %12.6f\n", biome_name(b), closed, integ);
        CHECK(std::fabs(closed - integ) < 1e-9,
              "%s: closed form %.9f, integrated %.9f", biome_name(b), closed, integ);
    }
    std::printf("  integrated total %.9f\n", total);
    CHECK(std::fabs(total - 1.0) < 1e-9, "integrated areas sum to %.9f", total);
}

static void test_map_areas() {
    rule("baked map at n=256: measured area vs the integrated prediction");

    const int n = 256;
    const BiomeParams p = BiomeParams::fitted(n);
    const BiomeMap map(0xC0FFEEull, n, p);

    std::printf("  zoom %.6f  (temperature period %.1f blocks, humidity %.1f, "
                "continentalness %.1f, erosion %.1f)\n",
                p.zoom,
                BiomeParams::kTemperaturePeriod * p.zoom,
                BiomeParams::kHumidityPeriod * p.zoom,
                BiomeParams::kContinentalPeriod * p.zoom,
                BiomeParams::kErosionPeriod * p.zoom);
    std::printf("  %d columns, %d live octave evaluations per column\n",
                n * n, map.octaves_per_column());

    // The ratios must be vanilla's 1024:256:512:512 exactly, whatever the zoom.
    CHECK(std::fabs(BiomeParams::kTemperaturePeriod / BiomeParams::kHumidityPeriod - 4.0) < 1e-12,
          "temperature:humidity period ratio is not 4:1");
    CHECK(std::fabs(BiomeParams::kTemperaturePeriod / BiomeParams::kContinentalPeriod - 2.0) < 1e-12,
          "temperature:continentalness period ratio is not 2:1");

    std::printf("\n%-14s %10s %10s %8s\n", "biome", "predicted", "measured", "delta");
    double total = 0.0, worst = 0.0;
    for (int i = 0; i < kBiomeCount; ++i) {
        const Biome b = static_cast<Biome>(i);
        const double pred = predicted_fraction(b, p.continental_bias);
        const double meas = map.fraction(b);
        const double d = meas - pred;
        worst = std::max(worst, std::fabs(d));
        total += meas;
        std::printf("%-14s %10.4f %10.4f %+8.4f\n", biome_name(b), pred, meas, d);
    }
    std::printf("  total measured %.6f, worst absolute error %.4f\n", total, worst);

    CHECK(std::fabs(total - 1.0) < 1e-9, "biome fractions sum to %.9f", total);
    // The prediction assumes the four fields are INDEPENDENT. Equalisation
    // makes each marginal exactly uniform, but at the default fit there is
    // about one humidity period across the whole map, so the map is a sample of
    // roughly one effective cell per field and the joint is nowhere near the
    // product of the marginals. This is not a defect to be tuned away: big
    // patches and good joint statistics are the same knob pulled in opposite
    // directions. The bound here is loose on purpose, and the convergence
    // measurement below shows the error going away as the periods multiply.
    CHECK(worst < 0.10, "worst area error %.4f exceeds 0.10", worst);

    // Ocean depends on continentalness alone, so no independence assumption is
    // involved and equalisation should nail it at any scale.
    const double ocean_err = std::fabs(map.fraction(Biome::Ocean)
                                     - predicted_fraction(Biome::Ocean, 0.0f));
    std::printf("  ocean (a one-field biome, so no independence assumed): "
                "error %.5f\n", ocean_err);
    CHECK(ocean_err < 0.002, "ocean fraction is off by %.5f", ocean_err);

    // Every biome in the table must actually be reachable on a real map, or the
    // table is decoration. (Mushroom fields is the one documented exception and
    // it is not in the table.)
    for (int i = 0; i < kBiomeCount; ++i) {
        const Biome b = static_cast<Biome>(i);
        CHECK(map.count(b) > 0, "%s never generated on a %dx%d map", biome_name(b), n, n);
    }
    std::printf("  all %d biomes present on one %dx%d map\n", kBiomeCount, n, n);

    // The equalisation claim: each parameter field is uniform on [-1,1].
    // Measure it as decile occupancy.
    {
        int dec[10] = {0};
        for (int z = 0; z < n; ++z)
            for (int x = 0; x < n; ++x) {
                int d = int((map.temperature_param(x, z) + 1.0f) * 5.0f);
                dec[std::clamp(d, 0, 9)]++;
            }
        std::printf("  temperature deciles (expect %.3f each):", 0.1);
        double worst_dec = 0.0;
        for (int d = 0; d < 10; ++d) {
            const double f = double(dec[d]) / double(n * n);
            worst_dec = std::max(worst_dec, std::fabs(f - 0.1));
            std::printf(" %.3f", f);
        }
        std::printf("\n  worst decile error %.4f\n", worst_dec);
        CHECK(worst_dec < 0.005, "equalisation is off: worst decile error %.4f", worst_dec);
    }
}

// ────────────────────────────────────────────────────────────────────────────
// 7b. the trade-off, measured: patch size against joint-statistics error
// ────────────────────────────────────────────────────────────────────────────
static void test_scale_tradeoff() {
    rule("scale trade-off: humidity periods vs patch size vs area error");

    const int n = 256;
    std::printf("%10s %9s %11s %11s\n",
                "hum.per.", "zoom", "mean run", "worst err");
    double first_run = 0.0, last_err = 1.0;
    for (double hp : { 1.0, 2.0, 4.0, 16.0, 64.0 }) {
        const BiomeParams p = BiomeParams::fitted(n, hp);
        const BiomeMap m(0xD15CEull, n, p);

        int borders = 0, pairs = 0;
        for (int z = 0; z < n; ++z)
            for (int x = 0; x + 1 < n; ++x, ++pairs)
                if (m.biome_at(x, z) != m.biome_at(x + 1, z)) ++borders;
        const double run = double(pairs) / double(std::max(1, borders));

        double worst = 0.0;
        for (int i = 0; i < kBiomeCount; ++i) {
            const Biome b = static_cast<Biome>(i);
            worst = std::max(worst, std::fabs(m.fraction(b)
                                            - predicted_fraction(b, 0.0f)));
        }
        std::printf("%10.0f %9.4f %11.1f %11.4f\n", hp, p.zoom, run, worst);
        if (hp == 1.0) { first_run = run; }
        last_err = worst;
    }
    // At many periods the map really is the product of its marginals, which is
    // what the closed form assumes. This is the evidence for the loose bound
    // used at the default fit.
    CHECK(last_err < 0.02, "at 64 humidity periods the area error is still %.4f", last_err);
    CHECK(first_run > 8.0, "default fit gives %.1f-block runs, expected big patches",
          first_run);
    std::printf("  patch size and joint accuracy trade against each other; the\n"
                "  default (1 humidity period) buys patches and pays in statistics.\n");
}

// ────────────────────────────────────────────────────────────────────────────
// 8. continental_bias actually moves the coastline, by the predicted amount
// ────────────────────────────────────────────────────────────────────────────
static void test_continental_bias() {
    rule("continental_bias: ocean share should track (edge - bias)");

    const int n = 192;
    std::printf("%8s %10s %10s\n", "bias", "predicted", "measured");
    for (float bias : { 0.0f, 0.2f, 0.4f, 0.6f }) {
        BiomeParams p = BiomeParams::fitted(n);
        p.continental_bias = bias;
        const BiomeMap map(0x51DEull, n, p);
        const double pred = predicted_fraction(Biome::Ocean, bias);
        const double meas = map.fraction(Biome::Ocean);
        std::printf("%8.2f %10.4f %10.4f\n", double(bias), pred, meas);
        CHECK(std::fabs(meas - pred) < 0.02,
              "bias %.2f: ocean %.4f, predicted %.4f", double(bias), meas, pred);
    }
    std::printf("  at bias 0 the cited edges put %.1f%% of an equalised map under water;\n"
                "  vanilla's bell-shaped noise reaches that band far less often.\n",
                100.0 * predicted_fraction(Biome::Ocean, 0.0f));
}

// ────────────────────────────────────────────────────────────────────────────
// 9. determinism, clamping, and the small-world case
// ────────────────────────────────────────────────────────────────────────────
static void test_determinism_and_edges() {
    rule("determinism, bounds, and n=96");

    const int n = 96;
    const BiomeParams p = BiomeParams::fitted(n);
    const BiomeMap a(12345ull, n, p);
    const BiomeMap b(12345ull, n, p);
    const BiomeMap c(12346ull, n, p);

    int same_ab = 0, same_ac = 0;
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            if (a.biome_at(x, z) == b.biome_at(x, z)) ++same_ab;
            if (a.biome_at(x, z) == c.biome_at(x, z)) ++same_ac;
        }
    std::printf("  seed 12345 twice: %d/%d columns identical\n", same_ab, n * n);
    std::printf("  seed 12345 vs 12346: %d/%d columns identical (%.1f%%)\n",
                same_ac, n * n, 100.0 * same_ac / (n * n));
    CHECK(same_ab == n * n, "same seed gave a different map");
    // A different seed must give a genuinely different map, not the same map
    // shifted. Chance agreement for this biome mix is roughly 15%.
    CHECK(same_ac < n * n / 2, "seed 12346 agrees with 12345 on %d/%d columns",
          same_ac, n * n);

    // Out-of-range queries clamp to the edge column rather than exploding.
    CHECK(a.biome_at(-5, -5)     == a.biome_at(0, 0),        "negative clamp");
    CHECK(a.biome_at(n + 9, 3)   == a.biome_at(n - 1, 3),    "high-x clamp");
    CHECK(a.biome_at(3, n + 9)   == a.biome_at(3, n - 1),    "high-z clamp");
    std::printf("  out-of-range queries clamp to the edge column\n");

    // Parameters stay inside [-1, 1] everywhere.
    float lo = 2.0f, hi = -2.0f;
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const float v[4] = { a.temperature_param(x, z), a.humidity_param(x, z),
                                 a.continentalness_param(x, z), a.erosion_param(x, z) };
            for (float t : v) { lo = std::min(lo, t); hi = std::max(hi, t); }
        }
    std::printf("  parameter range over the map: [%.4f, %.4f]\n", double(lo), double(hi));
    CHECK(lo >= -1.0f && hi <= 1.0f, "parameter escaped [-1,1]: [%.4f,%.4f]",
          double(lo), double(hi));

    // A default (unbuilt) map must answer rather than crash.
    const BiomeMap empty;
    CHECK(empty.biome_at(0, 0) == Biome::Plains, "default-constructed map misbehaved");
    std::printf("  default-constructed BiomeMap answers %s\n",
                biome_name(empty.biome_at(0, 0)));

    // Biome patch size: the number that decides whether the map reads as
    // regions or as speckle. Count 4-neighbour boundaries.
    int borders = 0, pairs = 0;
    for (int z = 0; z < n; ++z)
        for (int x = 0; x + 1 < n; ++x, ++pairs)
            if (a.biome_at(x, z) != a.biome_at(x + 1, z)) ++borders;
    const double mean_run = double(pairs) / double(std::max(1, borders));
    std::printf("  mean horizontal run at n=%d: %.1f blocks (%d borders in %d pairs)\n",
                n, mean_run, borders, pairs);
    CHECK(mean_run > 4.0, "biome patches are speckle: mean run %.2f blocks", mean_run);
}

// ────────────────────────────────────────────────────────────────────────────
// 10. surface / filler, and the column query
// ────────────────────────────────────────────────────────────────────────────
static void test_surface_blocks() {
    rule("surface and filler blocks");

    struct Row { Biome b; Block surface; Block filler; };
    const Row rows[] = {
        { Biome::Plains,      Grass,  Dirt  },
        { Biome::Forest,      Grass,  Dirt  },
        { Biome::Taiga,       Grass,  Dirt  },
        { Biome::Desert,      Sand,   Sand  },
        { Biome::Savanna,     Grass,  Dirt  },
        { Biome::SnowyPlains, Grass,  Dirt  },
        { Biome::Swamp,       Grass,  Dirt  },
        { Biome::Jungle,      Grass,  Dirt  },
        { Biome::Beach,       Sand,   Sand  },
        { Biome::Ocean,       Gravel, Dirt  },
        { Biome::Mountains,   Stone,  Stone },
    };
    std::printf("%-14s %-10s %-10s %-14s\n", "biome", "surface", "filler", "tree");
    for (const Row& r : rows) {
        const BiomeDef& d = biome_def(r.b);
        std::printf("%-14s %-10s %-10s %-14s\n", d.name,
                    block_name(d.surface), block_name(d.filler),
                    d.tree == TreeKind::None ? "none" : "yes");
        CHECK(d.surface == r.surface, "%s surface is %s", d.name, block_name(d.surface));
        CHECK(d.filler  == r.filler,  "%s filler is %s",  d.name, block_name(d.filler));
    }

    // column_block: air above, water up to sea level, then surface, filler,
    // stone. Checked against BlockWorld's own sea level of 63.
    const BiomeMap m(7ull, 64, BiomeParams::fitted(64));
    const int sea = 63, surf = 60;   // an underwater column
    CHECK(m.column_block(Biome::Ocean, 70, surf, sea) == Air,   "above sea should be air");
    CHECK(m.column_block(Biome::Ocean, 62, surf, sea) == Water, "below sea should be water");
    CHECK(m.column_block(Biome::Ocean, 60, surf, sea) == Gravel,"ocean floor is gravel");
    CHECK(m.column_block(Biome::Ocean, 58, surf, sea) == Dirt,  "ocean filler is dirt");
    CHECK(m.column_block(Biome::Ocean, 56, surf, sea) == Stone, "deep is stone");
    std::printf("  ocean column at sea=%d surf=%d: y70=%s y62=%s y60=%s y58=%s y56=%s\n",
                sea, surf,
                block_name(m.column_block(Biome::Ocean, 70, surf, sea)),
                block_name(m.column_block(Biome::Ocean, 62, surf, sea)),
                block_name(m.column_block(Biome::Ocean, 60, surf, sea)),
                block_name(m.column_block(Biome::Ocean, 58, surf, sea)),
                block_name(m.column_block(Biome::Ocean, 56, surf, sea)));

    const int hi = 80;
    CHECK(m.column_block(Biome::Desert, hi,     hi, sea) == Sand, "desert surface");
    CHECK(m.column_block(Biome::Desert, hi - 3, hi, sea) == Sand, "desert filler");
    CHECK(m.column_block(Biome::Desert, hi - 4, hi, sea) == Stone,
          "filler is %d deep", kFillerDepth);
    std::printf("  filler depth %d blocks under the surface block\n", kFillerDepth);
}

// ────────────────────────────────────────────────────────────────────────────
// 11. swamp grass, and the honest label on it
// ────────────────────────────────────────────────────────────────────────────
static void test_swamp_grass() {
    rule("swamp grass_color_modifier (colours sourced, threshold NOT)");

    const int n = 128;
    const BiomeMap m(0xBEEFull, n, BiomeParams::fitted(n));
    int a = 0, b = 0;
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const std::uint32_t c = m.swamp_grass_color(x, z);
            CHECK(c == kSwampGrassA || c == kSwampGrassB,
                  "swamp grass produced #%06X, which is neither published colour", c);
            (c == kSwampGrassA) ? ++a : ++b;
        }
    std::printf("  #%06X (6975545): %d columns   #%06X (5011004): %d columns\n",
                kSwampGrassA, a, kSwampGrassB, b);
    std::printf("  both colours are from minecraft.wiki; the noise scale %.4f and\n"
                "  cutoff %.2f that choose between them are UNVERIFIED bench values.\n",
                kSwampGrassNoiseScale, kSwampGrassNoiseCutoff);
    CHECK(a > 0 && b > 0, "the swamp modifier only ever produced one colour");
    // Determinism: the same coordinate must give the same colour every time.
    CHECK(m.swamp_grass_color(31, 47) == m.swamp_grass_color(31, 47), "not deterministic");
}

// ────────────────────────────────────────────────────────────────────────────
// 12. cost
// ────────────────────────────────────────────────────────────────────────────
static void test_cost() {
    rule("cost: build is O(n^2), query is O(1)");

    for (int n : { 96, 128, 192, 256 }) {
        const BiomeMap m(0xA11CEull, n, BiomeParams::fitted(n));
        const std::size_t columns = std::size_t(n) * std::size_t(n);
        const std::size_t volume  = columns * 128;
        // 4 parameter floats + 1 biome byte per column.
        const std::size_t bytes = columns * (4 * sizeof(float) + 1);
        std::printf("  n=%3d  %7zu columns  (volume would be %9zu, %.0fx more)  "
                    "%6.0f KB resident\n",
                    n, columns, volume, double(volume) / double(columns),
                    double(bytes) / 1024.0);
        CHECK(m.biome_at(n / 2, n / 2) < Biome::kCount, "query returned nonsense");
    }

    // Hammer the query so the "O(1), one byte load" claim is not just prose.
    const int n = 256;
    const BiomeMap m(0xA11CEull, n, BiomeParams::fitted(n));
    Rng r(99ull);
    long long acc = 0;
    const int kQueries = 4000000;
    for (int i = 0; i < kQueries; ++i)
        acc += bidx(m.biome_at(int(r.unit() * float(n)) % n,
                               int(r.unit() * float(n)) % n));
    std::printf("  %d random biome_at queries completed (checksum %lld)\n", kQueries, acc);
    CHECK(acc > 0, "query loop produced nothing");
}

int main() {
    std::printf("test_biomes — src/world/biomes.hpp\n");
    std::printf("constants from Java Edition 23w31a data files and minecraft.wiki\n");

    test_climate();
    test_lapse_rate();
    test_colormap_index();
    test_tree_density();
    test_bands();
    test_classifier();
    test_area_exact();
    test_map_areas();
    test_scale_tradeoff();
    test_continental_bias();
    test_determinism_and_edges();
    test_surface_blocks();
    test_swamp_grass();
    test_cost();

    std::printf("\nOK — %d assertions passed\n", g_checks);
    return 0;
}
