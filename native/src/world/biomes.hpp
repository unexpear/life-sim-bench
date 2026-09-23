// biomes.hpp — biome assignment and the per-biome generation parameters that
// actually change what a world looks like.
//
// ── WHAT THIS IS A MODEL OF ────────────────────────────────────────────────
//
// Minecraft Java Edition 1.18 replaced the old "biome zoom" layer stack with a
// MULTI-NOISE biome source: six independent noise fields — temperature,
// humidity (called `vegetation` in the data files), continentalness, erosion,
// depth and weirdness — quantised into levels, and a lookup that picks the
// biome whose parameter box the point falls in.
//
// This file implements FOUR of those six: temperature, humidity,
// continentalness and erosion. Depth and weirdness are NOT modelled, and that
// is the single biggest simplification here. Consequences, stated plainly so
// nobody is surprised later:
//   · no "variant" biomes — vanilla uses weirdness to split Snowy Plains from
//     Ice Spikes, Jungle from Sparse Jungle, Birch Forest from Old Growth
//     Birch Forest. This file has one biome per (temperature, humidity) cell.
//   · no depth-driven cave biomes (Lush Caves, Dripstone Caves).
//   · Windswept Hills stands in for the whole mountain family; vanilla reaches
//     Jagged/Frozen/Stony Peaks through weirdness and depth.
// The task brief allowed a 2D noise pair; this is a 4D subset of the real
// thing, which is closer, but it is still a SUBSET and not a reconstruction.
//
// ── WHERE THE NUMBERS COME FROM ────────────────────────────────────────────
//
// Every climate value, colour and tree count below was read out of the vanilla
// data files (snapshot 23w31a, mirrored at mcasset.cloud) or off minecraft.wiki,
// and the source is named at the point of use. Nothing here is from memory.
// Two things I could NOT source are flagged with UNVERIFIED and are the
// bench's own choices, not claims about Minecraft.
//
// ── THE DISTRIBUTION PROBLEM, AND THE ONE DELIBERATE DIVERGENCE ────────────
//
// Vanilla's parameter thresholds (temperature level 0 is [-1.0, -0.45), and so
// on) are quoted against Mojang's `NormalNoise`, which is an octave stack of
// improved-Perlin noise over their own permutation tables. I cannot reproduce
// that distribution without those tables, and guessing a standard deviation for
// it would be exactly the kind of half-remembered number this project bans.
//
// So instead of guessing the shape, this file REMOVES the shape: each noise
// field is histogram-equalised over the actual world footprint, so it is
// uniform on [-1, 1] by construction. The published band edges then become
// exact AREA FRACTIONS, which is a property a test can assert rather than a
// property I have to take on faith. The cost is that the resulting biome mix is
// flatter than vanilla's — vanilla's bell-shaped noise spends most of its time
// near zero, so vanilla has proportionally more temperate biomes and less
// desert and snow than this file produces. That is a divergence, it is
// measured in test_biomes.cpp, and it is the price of not inventing a sigma.
//
// ── COST ───────────────────────────────────────────────────────────────────
//
// Build is ONE pass over the n x n column footprint: ~17 lattice evaluations
// per column for the four octave stacks, then four sorts for the equalisers.
// At n=256 that is 65,536 columns — 128x cheaper than a single pass over the
// n x 128 x n volume, and it happens once at world build, never per tick.
// After the bake, biome_at(x, z) is a single byte load out of an n*n array:
// O(1), no noise, no branches worth counting.

#pragma once
#include "../rng.hpp"
#include "blockworld.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace bench {

// ── the biomes ──────────────────────────────────────────────────────────────
//
// enum class, not a plain enum, on purpose: `Plains`, `Forest`, `Ocean` and
// `Beach` are ordinary English words and this header shares namespace bench
// with several sibling subsystems. Scoping them costs a cast at the table
// lookups and buys immunity from a name collision nobody would enjoy debugging.
enum class Biome : std::uint8_t {
    Plains = 0, Forest, Taiga, Desert, Savanna, SnowyPlains,
    Swamp, Jungle, Beach, Ocean, Mountains,
    kCount
};
inline constexpr int kBiomeCount = static_cast<int>(Biome::kCount);
[[nodiscard]] inline constexpr int bidx(Biome b) { return static_cast<int>(b); }

// The dominant tree of a biome. Vanilla puts a `random_selector` inside each
// configured feature so a biome grows a MIX; this records the mix in the name
// and the weights it does not model are called out in the table's comments.
enum class TreeKind : std::uint8_t {
    None = 0,
    Oak,          // trees_plains -> oak_bees_0002, 1/3 of them fancy (large) oak
    BirchAndOak,  // trees_birch_and_oak -> birch + oak + fancy oak
    Spruce,       // trees_taiga / trees_snowy -> spruce (+ pine in taiga)
    Acacia,       // trees_savanna -> 80% acacia, 20% oak
    JungleTree,   // trees_jungle -> mega jungle, jungle, jungle bush
    SwampOak,     // trees_swamp -> swamp_oak (oak with vines, tolerates water)
    SpruceAndOak  // trees_windswept_hills -> spruce + oak + fancy oak
};

// A vanilla `minecraft:count` placement modifier whose count is a
// `minecraft:weighted_list` of exactly two entries. Every overworld tree
// placement in 23w31a has this shape, which is why it is worth a struct:
// the whole of "how many trees per chunk" is four integers.
struct TreeCount {
    int lo = 0, lo_weight = 1;
    int hi = 0, hi_weight = 0;

    // Expected trees per chunk. This is the number people quote when they say
    // "plains has 0.05 trees a chunk" — it is not a count the game ever rolls.
    [[nodiscard]] constexpr double mean() const {
        const int w = lo_weight + hi_weight;
        return w > 0 ? (static_cast<double>(lo) * lo_weight
                      + static_cast<double>(hi) * hi_weight) / static_cast<double>(w)
                     : 0.0;
    }
};

// Roll one chunk's tree count, the way the weighted_list does.
[[nodiscard]] inline int roll_tree_count(const TreeCount& tc, Rng& r) {
    const int w = tc.lo_weight + tc.hi_weight;
    if (w <= 0) return 0;
    return static_cast<int>(r.below(static_cast<std::uint32_t>(w))) < tc.lo_weight
         ? tc.lo : tc.hi;
}

// Everything the generator needs to know about one biome.
//
// Colours are 0xRRGGBB. water/sky are the biome JSON's `effects` integers
// verbatim; grass/foliage are the wiki's published samples of the colormap
// (see grass_colormap_index below for why they are not computed here).
struct BiomeDef {
    const char*   name;
    float         temperature;          // biome JSON "temperature"
    float         downfall;             // biome JSON "downfall"
    bool          has_precipitation;    // biome JSON "has_precipitation"
    std::uint8_t  surface;              // top block
    std::uint8_t  filler;               // the few blocks under the top block
    TreeKind      tree;
    TreeCount     trees;                // placed_feature count, per chunk
    std::uint32_t grass;                // 0xRRGGBB
    std::uint32_t foliage;              // 0xRRGGBB
    std::uint32_t water;                // 0xRRGGBB
    std::uint32_t sky;                  // 0xRRGGBB
};

// How deep the filler runs under the surface block. Vanilla's surface rules use
// a noise-driven depth; this is flat. UNVERIFIED as a Minecraft constant — 4 is
// what blockworld.hpp's own generator already uses (`y > gh - 4`), and matching
// it keeps the two files from disagreeing about the same column.
inline constexpr int kFillerDepth = 4;

// ── the table ───────────────────────────────────────────────────────────────
//
// SOURCES, per column.
//
// temperature / downfall / has_precipitation
//   data/minecraft/worldgen/biome/<name>.json, Java Edition snapshot 23w31a.
//   Read directly, not from the wiki prose — the wiki's Biome page summary
//   lists Swamp's temperature as 0.9, and swamp.json says 0.8. The JSON wins.
//
// water / sky
//   the same JSONs' `effects` block. water_color is 4159204 = 0x3F76E4 for
//   every biome here except Swamp (6388580 = 0x617B64).
//
// grass / foliage
//   minecraft.wiki "Block colors", which publishes both the colormap
//   coordinate and the resulting hex for each biome. These are LOOKED UP, not
//   computed: computing them needs the 256x256 grass.png/foliage.png, and this
//   project may not download assets. grass_colormap_index() below reproduces
//   the index arithmetic exactly, and the test asserts it lands on the
//   published coordinate — so the lookup is at least verified to be the right
//   pixel of the right image.
//
// trees
//   data/minecraft/worldgen/placed_feature/<feature>.json, 23w31a, the
//   `minecraft:count` modifier's weighted_list.
//
// surface / filler
//   minecraft.wiki biome pages (prose, not a JSON constant — vanilla's real
//   surface rules are a noise-conditioned rule tree in
//   worldgen/noise_settings/overworld.json, far past what this bench needs).
inline constexpr std::array<BiomeDef, kBiomeCount> kBiomeTable = {{
    // Plains — trees_plains {0:19, 1:1} = 0.05 trees/chunk. The wiki's Plains
    // page says the same thing in words: "Plains now have some trees (5% of
    // chunks)". This is the number people get wrong by a factor of twenty.
    { "plains",       0.8f, 0.4f, true,  Grass,  Dirt,  TreeKind::Oak,
      { 0, 19,  1, 1 }, 0x91BD59, 0x77AB2F, 0x3F76E4, 0x78A7FF },

    // Forest — trees_birch_and_oak {10:9, 11:1} = 10.1 trees/chunk.
    { "forest",       0.7f, 0.8f, true,  Grass,  Dirt,  TreeKind::BirchAndOak,
      {10,  9, 11, 1 }, 0x79C05A, 0x59AE30, 0x3F76E4, 0x79A6FF },

    // Taiga — trees_taiga {10:9, 11:1} = 10.1 trees/chunk, same as forest.
    { "taiga",        0.25f, 0.8f, true, Grass,  Dirt,  TreeKind::Spruce,
      {10,  9, 11, 1 }, 0x86B783, 0x68A464, 0x3F76E4, 0x7DA3FF },

    // Desert — desert.json contains NO tree feature at all in any generation
    // step. Zero is a checked zero, not an assumption. has_precipitation is
    // false: it neither rains nor snows.
    { "desert",       2.0f, 0.0f, false, Sand,   Sand,  TreeKind::None,
      { 0,  1,  0, 0 }, 0xBFB755, 0xAEA42A, 0x3F76E4, 0x6EB1FF },

    // Savanna — trees_savanna {1:9, 2:1} = 1.1 trees/chunk. The configured
    // feature is a random_selector: 0.8 chance acacia, else oak. Same climate
    // as desert (2.0 / 0.0), hence the identical grass colour.
    { "savanna",      2.0f, 0.0f, false, Grass,  Dirt,  TreeKind::Acacia,
      { 1,  9,  2, 1 }, 0xBFB755, 0xAEA42A, 0x3F76E4, 0x6EB1FF },

    // Snowy Plains — trees_snowy {0:9, 1:1} = 0.1 trees/chunk, and the feature
    // is a bare `minecraft:spruce`. Note downfall 0.5 with temperature 0.0:
    // it precipitates, and precipitation below 0.15 falls as snow.
    // The surface is grass with a snow layer on top; BlockWorld has no snow
    // block, so `snows` is data the caller can act on, not a block here.
    { "snowy_plains", 0.0f, 0.5f, true,  Grass,  Dirt,  TreeKind::Spruce,
      { 0,  9,  1, 1 }, 0x80B497, 0x60A17B, 0x3F76E4, 0x7FA1FF },

    // Swamp — trees_swamp {2:9, 3:1} = 2.1 trees/chunk of `swamp_oak`.
    // swamp.json has no `grass_color`; it has `grass_color_modifier: "swamp"`,
    // which picks between two colours by noise (see swamp_grass_color below).
    // foliage_color is an explicit override: 6975545 = 0x6A7039.
    { "swamp",        0.8f, 0.9f, true,  Grass,  Dirt,  TreeKind::SwampOak,
      { 2,  9,  3, 1 }, 0x6A7039, 0x6A7039, 0x617B64, 0x78A7FF },

    // Jungle — trees_jungle {50:9, 51:1} = 50.1 trees/chunk. Five times a
    // forest; this is why jungles look solid from the air.
    { "jungle",       0.95f, 0.9f, true, Grass,  Dirt,  TreeKind::JungleTree,
      {50,  9, 51, 1 }, 0x59C93C, 0x30BB0B, 0x3F76E4, 0x77A8FF },

    // Beach — same climate as plains (0.8 / 0.4), hence the same grass colour
    // on the rare grass block. No tree feature.
    { "beach",        0.8f, 0.4f, true,  Sand,   Sand,  TreeKind::None,
      { 0,  1,  0, 0 }, 0x91BD59, 0x77AB2F, 0x3F76E4, 0x78A7FF },

    // Ocean — "the ocean floor, which is mostly covered by a one-block layer of
    // gravel" (minecraft.wiki, Ocean). No tree feature.
    { "ocean",        0.5f, 0.5f, true,  Gravel, Dirt,  TreeKind::None,
      { 0,  1,  0, 0 }, 0x8EB971, 0x71A74D, 0x3F76E4, 0x7BA4FF },

    // Mountains — vanilla's Windswept Hills, standing in for the whole
    // mountain family. trees_windswept_hills {0:9, 1:1} = 0.1 trees/chunk:
    // twice plains, and still effectively bare. Surface is stone with grass
    // patches; this file takes the stone, because the thing that matters to an
    // agent is that a mountain is not farmable.
    { "mountains",    0.2f, 0.3f, true,  Stone,  Stone, TreeKind::SpruceAndOak,
      { 0,  9,  1, 1 }, 0x8AB689, 0x6DA36B, 0x3F76E4, 0x7DA2FF },
}};

[[nodiscard]] inline const BiomeDef& biome_def(Biome b) {
    const int i = bidx(b);
    return kBiomeTable[static_cast<std::size_t>(i >= 0 && i < kBiomeCount ? i : 0)];
}
[[nodiscard]] inline const char* biome_name(Biome b) { return biome_def(b).name; }

// ── snow, and the altitude lapse rate ───────────────────────────────────────
//
// Java Edition's Biome.getTemperature(BlockPos):
//
//     if (pos.getY() > 80) {
//         float f1 = (float)(TEMPERATURE_NOISE.getValue(x / 8.0F, z / 8.0F, false) * 8.0);
//         return base - (f1 + (float)pos.getY() - 80.0F) * 0.05F / 40.0F;
//     }
//     return base;
//
// 0.05/40 is exactly 1/800 = 0.00125 per block, which is the figure
// minecraft.wiki quotes ("the actual temperature decreases by 0.00125 (1/800)
// every block up"). The formula is implemented, not approximated.
//
// The wobble term f1 uses Mojang's PerlinSimplexNoise seeded 1234. I cannot
// reproduce that stream, so `wobble` is a caller-supplied value in [-8, 8]
// (the range that noise's *8 scaling produces). Pass 0 and you get the exact
// deterministic lapse rate; pass biome_temperature_wobble() to get a jittered
// snow line of the right amplitude but the wrong phase. The divergence is in
// the phase only — the lapse rate itself is exact.
inline constexpr float kLapseRatePerBlock = 0.05f / 40.0f;   // = 0.00125
inline constexpr int   kLapseStartY       = 80;

[[nodiscard]] inline float temperature_at(Biome b, int y, float wobble = 0.0f) {
    const float base = biome_def(b).temperature;
    if (y <= kLapseStartY) return base;
    return base - (wobble + static_cast<float>(y) - static_cast<float>(kLapseStartY))
                * kLapseRatePerBlock;
}

// Java Edition: Biome.warmEnoughToRain(pos) is `getTemperature(pos) >= 0.15F`,
// and coldEnoughToSnow is its negation. So precipitation below 0.15 is snow.
inline constexpr float kRainThreshold = 0.15f;

[[nodiscard]] inline bool rains_at(Biome b, int y, float wobble = 0.0f) {
    return biome_def(b).has_precipitation && temperature_at(b, y, wobble) >= kRainThreshold;
}
[[nodiscard]] inline bool snows_at(Biome b, int y, float wobble = 0.0f) {
    return biome_def(b).has_precipitation && temperature_at(b, y, wobble) < kRainThreshold;
}
// At sea level, i.e. the flag people mean when they say "does it snow there".
[[nodiscard]] inline bool snows(Biome b) { return snows_at(b, 0, 0.0f); }

// ── the colormap index ──────────────────────────────────────────────────────
//
// Java Edition GrassColor.get(double temperature, double humidity):
//
//     humidity *= temperature;
//     int i = (int)((1.0 - temperature) * 255.0);
//     int j = (int)((1.0 - humidity) * 255.0);
//     int k = j << 8 | i;
//
// with the arguments produced by Biome.getGrassColorFromTexture():
//
//     double d0 = (double)Mth.clamp(temperature, 0.0F, 1.0F);
//     double d1 = (double)Mth.clamp(downfall,    0.0F, 1.0F);
//
// The clamp happens in FLOAT and is then widened, and that detail is not
// cosmetic: taiga is temperature 0.25, downfall 0.8, and 0.25 * 0.8 in exact
// arithmetic gives j = 204, but 0.8f widened to double is 0.80000001192...,
// which drags the product just over and gives j = 203 — which is the
// coordinate minecraft.wiki publishes. Doing this in double throughout gets
// the wrong pixel. The test asserts all eleven coordinates against the wiki.
//
// Only the index is computed. The RGB behind it lives in grass.png, a
// downloaded asset, so the colours themselves are table entries above.
struct ColormapIndex {
    int x;       // horizontal, from temperature
    int y;       // vertical, from adjusted downfall
    int pixel;   // (y << 8) | x, the linear index Java actually uses
};

[[nodiscard]] inline ColormapIndex grass_colormap_index(float temperature, float downfall) {
    const float ct = std::clamp(temperature, 0.0f, 1.0f);
    const float cd = std::clamp(downfall,    0.0f, 1.0f);
    const double t = static_cast<double>(ct);
    double d = static_cast<double>(cd);
    d *= t;                                            // humidity *= temperature
    const int i = static_cast<int>((1.0 - t) * 255.0);
    const int j = static_cast<int>((1.0 - d) * 255.0);
    return { i, j, (j << 8) | i };
}
[[nodiscard]] inline ColormapIndex grass_colormap_index(Biome b) {
    const BiomeDef& d = biome_def(b);
    return grass_colormap_index(d.temperature, d.downfall);
}

// Swamp's `grass_color_modifier`. The two colours are sourced: 0x6A7039 is
// swamp.json's foliage_color (6975545) and 0x4C763C (5011004) is the second
// value minecraft.wiki lists for swamp grass. The RULE that chooses between
// them is a noise threshold in Java's GrassColorModifier.SWAMP, and I could not
// find the noise's scale or its cutoff in any citable source.
//
// UNVERIFIED: `scale` and `cutoff` below are the bench's own numbers, picked so
// the patches are a few blocks across at BlockWorld's scale. They are NOT a
// claim about Minecraft. The colours are; the threshold is not.
inline constexpr std::uint32_t kSwampGrassA = 0x6A7039;
inline constexpr std::uint32_t kSwampGrassB = 0x4C763C;
inline constexpr double kSwampGrassNoiseScale  = 0.0225;   // UNVERIFIED
inline constexpr double kSwampGrassNoiseCutoff = -0.1;     // UNVERIFIED

// ── noise ───────────────────────────────────────────────────────────────────
//
// Value noise on an integer-hashed lattice with smoothstep interpolation, the
// same construction blockworld.hpp uses. It is NOT Mojang's improved-Perlin
// NormalNoise; what is copied from vanilla is the OCTAVE LAYOUT — which
// octaves are present and with what amplitude — taken from the worldgen noise
// JSONs, because that is what sets the size of the features.
//
//   noise/temperature.json     firstOctave -10, amplitudes [1.5, 0, 1, 0, 0, 0]
//   noise/vegetation.json      firstOctave  -8, amplitudes [1, 1, 0, 0, 0, 0]
//   noise/continentalness.json firstOctave  -9, amplitudes [1,1,2,2,2,1,1,1,1]
//   noise/erosion.json         firstOctave  -9, amplitudes [1, 1, 0, 1, 1]
//
// firstOctave f means the first octave has period 2^-f blocks, so the base
// periods are 1024, 256, 512 and 512 blocks. Octave o has period base / 2^o.
// Zero amplitudes are dropped entirely — they cost a hash and contribute
// nothing, and vanilla's temperature stack is two live octaves out of six.
class OctaveNoise {
public:
    OctaveNoise() = default;

    OctaveNoise(std::uint64_t seed, std::uint32_t salt, double basePeriod,
                std::initializer_list<double> amplitudes)
        : seed_(seed) {
        double norm = 0.0;
        int o = 0;
        for (double a : amplitudes) {
            if (a != 0.0) {
                Octave t;
                t.freq = std::exp2(static_cast<double>(o)) / basePeriod;
                t.amp  = a;
                t.salt = salt + static_cast<std::uint32_t>(o) * 7919u;
                oct_.push_back(t);
                norm += a;
            }
            ++o;
        }
        norm_ = norm > 0.0 ? norm : 1.0;
    }

    // Raw field value, normalised to [-1, 1] by construction.
    [[nodiscard]] float raw(double x, double z) const {
        double sum = 0.0;
        for (const Octave& t : oct_)
            sum += t.amp * (2.0 * lattice(x * t.freq, z * t.freq, t.salt) - 1.0);
        return static_cast<float>(sum / norm_);
    }

    [[nodiscard]] int live_octaves() const { return static_cast<int>(oct_.size()); }

private:
    struct Octave { double freq; double amp; std::uint32_t salt; };

    [[nodiscard]] std::uint32_t hash(std::uint32_t a) const {
        a ^= static_cast<std::uint32_t>(seed_) + 0x9E3779B9u + (a << 6) + (a >> 2);
        a ^= static_cast<std::uint32_t>(seed_ >> 32);
        a ^= a >> 16; a *= 0x7FEB352Du;
        a ^= a >> 15; a *= 0x846CA68Bu;
        a ^= a >> 16;
        return a;
    }
    [[nodiscard]] float corner(int xi, int zi, std::uint32_t salt) const {
        return static_cast<float>(hash(static_cast<std::uint32_t>(xi) * 374761393u
                                     + static_cast<std::uint32_t>(zi) * 668265263u
                                     + salt) & 0xFFFFFFu) / static_cast<float>(0xFFFFFF);
    }
    static double smooth(double t) { return t * t * (3.0 - 2.0 * t); }

    // Bilinear value noise, smoothstepped. Linear interpolation would leave
    // creases along the lattice and biome borders would come out as rectangles.
    [[nodiscard]] double lattice(double x, double z, std::uint32_t salt) const {
        const double fxi = std::floor(x), fzi = std::floor(z);
        const int xi = static_cast<int>(fxi), zi = static_cast<int>(fzi);
        const double tx = smooth(x - fxi), tz = smooth(z - fzi);
        const double a = corner(xi,     zi,     salt), b = corner(xi + 1, zi,     salt);
        const double c = corner(xi,     zi + 1, salt), d = corner(xi + 1, zi + 1, salt);
        return (a + (b - a) * tx) * (1.0 - tz) + (c + (d - c) * tx) * tz;
    }

    std::uint64_t seed_ = 1;
    double norm_ = 1.0;
    std::vector<Octave> oct_;
};

// ── histogram equalisation ──────────────────────────────────────────────────
//
// Turns whatever distribution the octave stack happens to have into a uniform
// one on [-1, 1], by building a quantile table from the field's own values over
// the world footprint and interpolating between the knots.
//
// This is the piece that makes the cited band edges assertable: after
// equalisation, "temperature level 0 is [-1.0, -0.45)" means level 0 covers
// exactly 27.5% of the map, and a test can check that number instead of
// trusting it.
class Equalizer {
public:
    // `values` is consumed by sorting a copy; knots must be >= 1.
    void build(const std::vector<float>& values, int knots) {
        q_.clear();
        if (values.empty() || knots < 1) return;
        std::vector<float> v = values;
        std::sort(v.begin(), v.end());
        const std::size_t m = v.size();
        q_.resize(static_cast<std::size_t>(knots) + 1);
        for (int k = 0; k <= knots; ++k) {
            const double p = static_cast<double>(k) / static_cast<double>(knots);
            const double t = p * static_cast<double>(m - 1);
            const std::size_t i0 = static_cast<std::size_t>(t);
            const std::size_t i1 = std::min(i0 + 1, m - 1);
            const double f = t - static_cast<double>(i0);
            q_[static_cast<std::size_t>(k)] =
                static_cast<float>(static_cast<double>(v[i0]) * (1.0 - f)
                                 + static_cast<double>(v[i1]) * f);
        }
        // A flat field would make every knot equal and the inverse lookup
        // meaningless. Never happens with live octaves, but a degenerate
        // amplitude list would produce it, and silently returning garbage is
        // worse than returning a constant.
        if (q_.front() >= q_.back()) q_.clear();
    }

    [[nodiscard]] bool ready() const { return q_.size() >= 2; }

    // Raw field value -> uniform [-1, 1].
    [[nodiscard]] float map(float raw) const {
        if (!ready()) return 0.0f;
        if (raw <= q_.front()) return -1.0f;
        if (raw >= q_.back())  return  1.0f;
        const auto it = std::upper_bound(q_.begin(), q_.end(), raw);
        const std::size_t hi = static_cast<std::size_t>(it - q_.begin());
        const std::size_t lo = hi - 1;
        const float a = q_[lo], b = q_[hi];
        const double f = (b > a) ? static_cast<double>(raw - a) / static_cast<double>(b - a)
                                 : 0.0;
        const double p = (static_cast<double>(lo) + f)
                       / static_cast<double>(q_.size() - 1);
        return static_cast<float>(p * 2.0 - 1.0);
    }

private:
    std::vector<float> q_;
};

// ── the parameter bands ─────────────────────────────────────────────────────
//
// minecraft.wiki, World generation (Java Edition 1.18+). Quoted verbatim:
//
//   temperature      5 levels: -1.0~-0.45, -0.45~-0.15, -0.15~0.2, 0.2~0.55,
//                              0.55~1.0
//   humidity         5 levels: -1.0~-0.35, -0.35~-0.1, -0.1~0.1, 0.1~0.3,
//                              0.3~1.0
//   erosion          7 levels: -1.0~-0.78, -0.78~-0.375, -0.375~-0.2225,
//                              -0.2225~0.05, 0.05~0.45, 0.45~0.55, 0.55~1.0
//   continentalness  7 bands:  mushroom fields -1.2~-1.05, deep ocean
//                              -1.05~-0.455, ocean -0.455~-0.19, coast
//                              -0.19~-0.11, near-inland -0.11~0.03,
//                              mid-inland 0.03~0.3, far-inland 0.3~1.0
//
// Note the continentalness range starts at -1.2, not -1.0. The equalised field
// here is [-1, 1], so band 0 (mushroom fields) is unreachable by construction —
// which is correct for this file, since it has no mushroom fields biome.
inline constexpr std::array<float, 4> kTemperatureEdges = { -0.45f, -0.15f, 0.20f, 0.55f };
inline constexpr std::array<float, 4> kHumidityEdges    = { -0.35f, -0.10f, 0.10f, 0.30f };
inline constexpr std::array<float, 6> kErosionEdges     = { -0.78f, -0.375f, -0.2225f,
                                                             0.05f,  0.45f,   0.55f };
inline constexpr std::array<float, 6> kContinentalEdges = { -1.05f, -0.455f, -0.19f,
                                                            -0.11f,  0.03f,   0.30f };

// Which band does v fall in? Returns 0..N for N edges.
template <std::size_t N>
[[nodiscard]] inline int band_of(float v, const std::array<float, N>& edges) {
    int level = 0;
    for (std::size_t i = 0; i < N; ++i) if (v >= edges[i]) level = static_cast<int>(i) + 1;
    return level;
}

// Continentalness band names, for readability at the call site.
inline constexpr int kContOcean      = 2;   // <= this band is open water
inline constexpr int kContCoast      = 3;   // exactly this band is shore
// Erosion band 0 is the least-eroded, most rugged terrain; band 6 is the
// flattest. Vanilla reaches Windswept Hills through erosion 0-1 plus weirdness
// and puts Swamp at erosion 6; without weirdness this file uses band 0 alone
// for mountains so they stay rare (11% of the erosion range, and vanilla's
// mountains are rarer still because its noise is not uniform).
inline constexpr int kErosionMountain = 0;
inline constexpr int kErosionSwamp    = 6;

// The 5x5 middle-biome table, temperature level (row) by humidity level
// (column). Vanilla's OverworldBiomeBuilder.MIDDLE_BIOMES as documented in
// minecraft.wiki's World generation biome table, with every biome this file
// does not have substituted by its nearest neighbour. Substitutions are marked;
// unmarked cells are vanilla's own choice.
//
//   T0 H3 vanilla Snowy Taiga        -> Taiga        (no snowy taiga here)
//   T1 H4 vanilla Old Growth Spruce  -> Taiga
//   T2 H0 vanilla Flower Forest      -> Forest
//   T2 H3 vanilla Birch Forest       -> Forest
//   T2 H4 vanilla Dark Forest        -> Forest
//   T3 H3/H4 vanilla Jungle          -> Jungle       (exact)
//
// Swamp, Beach, Ocean and Mountains are NOT in this table: vanilla places them
// off continentalness and erosion, and so does biome_from_parameters below.
inline constexpr std::array<std::array<Biome, 5>, 5> kMiddleBiomes = {{
    // H0                 H1                  H2                  H3                H4
    { Biome::SnowyPlains, Biome::SnowyPlains, Biome::SnowyPlains, Biome::Taiga,     Biome::Taiga  },
    { Biome::Plains,      Biome::Plains,      Biome::Forest,      Biome::Taiga,     Biome::Taiga  },
    { Biome::Forest,      Biome::Plains,      Biome::Forest,      Biome::Forest,    Biome::Forest },
    { Biome::Savanna,     Biome::Savanna,     Biome::Forest,      Biome::Jungle,    Biome::Jungle },
    { Biome::Desert,      Biome::Desert,      Biome::Desert,      Biome::Desert,    Biome::Desert },
}};

// The classifier itself, exposed separately from the noise so a test (or a
// caller with its own fields) can drive it directly. All four inputs are the
// EQUALISED parameters, uniform on [-1, 1].
[[nodiscard]] inline Biome biome_from_parameters(float temperature, float humidity,
                                                 float continentalness, float erosion) {
    const int c = band_of(continentalness, kContinentalEdges);
    if (c <= kContOcean) return Biome::Ocean;
    if (c == kContCoast) return Biome::Beach;

    const int e = band_of(erosion, kErosionEdges);
    if (e == kErosionMountain) return Biome::Mountains;

    const int t = band_of(temperature, kTemperatureEdges);
    // Vanilla puts Swamp in flat valleys at erosion 6, over the middle of the
    // temperature range — it is neither a frozen nor a desert biome.
    if (e == kErosionSwamp && t >= 1 && t <= 3) return Biome::Swamp;

    const int h = band_of(humidity, kHumidityEdges);
    return kMiddleBiomes[static_cast<std::size_t>(t)][static_cast<std::size_t>(h)];
}

// ── configuration ───────────────────────────────────────────────────────────

struct BiomeParams {
    // Vanilla base periods in blocks, from firstOctave in the noise JSONs.
    static constexpr double kTemperaturePeriod = 1024.0;   // firstOctave -10
    static constexpr double kHumidityPeriod    =  256.0;   // firstOctave  -8
    static constexpr double kContinentalPeriod =  512.0;   // firstOctave  -9
    static constexpr double kErosionPeriod     =  512.0;   // firstOctave  -9

    // Multiplies every period, so the 1024:256:512:512 RATIO is preserved
    // exactly and only the absolute scale moves. zoom = 1 is Minecraft's own
    // scale. fitted() below picks a zoom from the world's width.
    double zoom = 1.0;

    // Added to continentalness before banding. At bias 0 the cited edges put
    // 40.5% of an equalised map under water, because uniform noise reaches the
    // ocean band far more often than vanilla's bell-shaped noise does. Positive
    // bias pushes land up; the effect is exactly linear and the test measures
    // it. Default 0 = the cited edges, unmodified.
    float continental_bias = 0.0f;

    // Quantile knots for the equalisers. 256 gives sub-1% placement error on
    // the band edges at n=96 and costs 257 floats per field.
    int calibration_knots = 256;

    // fitted() is scaled off the HUMIDITY period, not the temperature period,
    // and that is a correction rather than a preference. The first version here
    // put 2 temperature periods across the map; since humidity's period is a
    // quarter of temperature's, that put 8 humidity periods across a 96-block
    // world — a humidity cell 12 blocks wide, finer than a chunk. The measured
    // result was a mean biome run of 2.3 blocks: not biomes, speckle. Scaling
    // off the finest field is the only way to control patch size, because the
    // finest field is what breaks the patches up.
    //
    // Equalisation is what makes this safe. A field is stretched to fill
    // [-1, 1] over the map's own footprint, so a single period across the map
    // still visits every band — there is no need to cram several periods in to
    // "get variety", and cramming them in only costs patch size.
    //
    // At n=256 with the default, zoom lands on exactly 1.0: a 256-block world
    // is one vanilla humidity period wide, at Minecraft's own scale.
    static constexpr double kDefaultHumidityPeriods = 1.0;

    [[nodiscard]] static BiomeParams fitted(int n, double humidity_periods
                                                       = kDefaultHumidityPeriods) {
        BiomeParams p;
        const double hp = humidity_periods > 0.0 ? humidity_periods
                                                 : kDefaultHumidityPeriods;
        const double ratio = kTemperaturePeriod / kHumidityPeriod;   // 4.0
        p.zoom = (ratio * static_cast<double>(n) / hp) / kTemperaturePeriod;
        return p;
    }
};

// ── the map ─────────────────────────────────────────────────────────────────
//
// Build once, then query. The four parameter fields are kept after the bake
// because they are cheap (4 floats a column, 1 MB at n=256) and a caller that
// wants to drive terrain height off continentalness should not have to
// recompute the noise to get it.
class BiomeMap {
public:
    BiomeMap() = default;
    BiomeMap(std::uint64_t seed, int n, const BiomeParams& p = BiomeParams{}) {
        build(seed, n, p);
    }

    void build(std::uint64_t seed, int n, const BiomeParams& p = BiomeParams{}) {
        n_ = std::max(1, n);
        params_ = p;
        seed_ = seed;
        const std::size_t cells = static_cast<std::size_t>(n_) * static_cast<std::size_t>(n_);

        const double z = params_.zoom > 0.0 ? params_.zoom : 1.0;
        // Salts are arbitrary but fixed, and distinct, so the four fields are
        // independent streams of the same hash rather than the same field
        // sampled at four scales.
        const OctaveNoise nT(seed, 0xB10E0001u, BiomeParams::kTemperaturePeriod * z,
                             {1.5, 0.0, 1.0, 0.0, 0.0, 0.0});
        const OctaveNoise nH(seed, 0xB10E0002u, BiomeParams::kHumidityPeriod * z,
                             {1.0, 1.0, 0.0, 0.0, 0.0, 0.0});
        const OctaveNoise nC(seed, 0xB10E0003u, BiomeParams::kContinentalPeriod * z,
                             {1.0, 1.0, 2.0, 2.0, 2.0, 1.0, 1.0, 1.0, 1.0});
        const OctaveNoise nE(seed, 0xB10E0004u, BiomeParams::kErosionPeriod * z,
                             {1.0, 1.0, 0.0, 1.0, 1.0});
        octaves_ = nT.live_octaves() + nH.live_octaves()
                 + nC.live_octaves() + nE.live_octaves();

        temp_.resize(cells); humid_.resize(cells); cont_.resize(cells); ero_.resize(cells);

        // One pass over the footprint, four raw fields. This is the only time
        // the noise is evaluated: the equalisers are built from these same
        // values and then applied in place, so nothing is sampled twice.
        for (int zz = 0; zz < n_; ++zz)
            for (int xx = 0; xx < n_; ++xx) {
                const std::size_t k = cell(xx, zz);
                const double dx = static_cast<double>(xx), dz = static_cast<double>(zz);
                temp_[k]  = nT.raw(dx, dz);
                humid_[k] = nH.raw(dx, dz);
                cont_[k]  = nC.raw(dx, dz);
                ero_[k]   = nE.raw(dx, dz);
            }

        // Equalise against the world's own footprint, not against some larger
        // notional area. Calibrating elsewhere would leave the map's actual
        // band fractions off by however much this particular patch of noise
        // differs from the field at large — and the whole point of equalising
        // is that the fractions are exact HERE.
        Equalizer eqT, eqH, eqC, eqE;
        eqT.build(temp_,  params_.calibration_knots);
        eqH.build(humid_, params_.calibration_knots);
        eqC.build(cont_,  params_.calibration_knots);
        eqE.build(ero_,   params_.calibration_knots);

        biome_.assign(cells, static_cast<std::uint8_t>(Biome::Plains));
        for (std::size_t k = 0; k < cells; ++k) {
            temp_[k]  = eqT.map(temp_[k]);
            humid_[k] = eqH.map(humid_[k]);
            cont_[k]  = std::clamp(eqC.map(cont_[k]) + params_.continental_bias, -1.0f, 1.0f);
            ero_[k]   = eqE.map(ero_[k]);
            biome_[k] = static_cast<std::uint8_t>(
                bidx(biome_from_parameters(temp_[k], humid_[k], cont_[k], ero_[k])));
        }

        // Built once, not per query: swamp_grass_color() used to construct this
        // on every call, which turned a colour lookup into a heap allocation.
        swamp_ = OctaveNoise(seed ^ 0x5A11Fu, 0xB10E0005u,
                             1.0 / kSwampGrassNoiseScale, {1.0});
    }

    [[nodiscard]] int  n() const { return n_; }
    [[nodiscard]] std::uint64_t seed() const { return seed_; }
    [[nodiscard]] const BiomeParams& params() const { return params_; }
    // Live octave evaluations per column during build — the cost number.
    [[nodiscard]] int  octaves_per_column() const { return octaves_; }

    // THE query. One byte load. Out-of-range clamps to the edge column rather
    // than returning a sentinel: a caller sampling one block past the wall
    // wants the wall's biome, not a crash or an "unknown" it has to handle.
    [[nodiscard]] Biome biome_at(int x, int z) const {
        if (biome_.empty()) return Biome::Plains;
        return static_cast<Biome>(biome_[cell(clampc(x), clampc(z))]);
    }

    [[nodiscard]] float temperature_param(int x, int z) const {
        return temp_.empty() ? 0.0f : temp_[cell(clampc(x), clampc(z))];
    }
    [[nodiscard]] float humidity_param(int x, int z) const {
        return humid_.empty() ? 0.0f : humid_[cell(clampc(x), clampc(z))];
    }
    [[nodiscard]] float continentalness_param(int x, int z) const {
        return cont_.empty() ? 0.0f : cont_[cell(clampc(x), clampc(z))];
    }
    [[nodiscard]] float erosion_param(int x, int z) const {
        return ero_.empty() ? 0.0f : ero_[cell(clampc(x), clampc(z))];
    }

    // How many columns carry this biome.
    [[nodiscard]] std::size_t count(Biome b) const {
        const std::uint8_t v = static_cast<std::uint8_t>(bidx(b));
        std::size_t c = 0;
        for (std::uint8_t x : biome_) if (x == v) ++c;
        return c;
    }
    [[nodiscard]] double fraction(Biome b) const {
        return biome_.empty() ? 0.0
             : static_cast<double>(count(b)) / static_cast<double>(biome_.size());
    }

    // Swamp's noise-picked grass colour. See kSwampGrassA/B: the colours are
    // sourced, the threshold is not.
    [[nodiscard]] std::uint32_t swamp_grass_color(int x, int z) const {
        return static_cast<double>(swamp_.raw(static_cast<double>(x),
                                              static_cast<double>(z)))
                   < kSwampGrassNoiseCutoff ? kSwampGrassB : kSwampGrassA;
    }

    // What block belongs at this y in this column, by biome. Pure query — it
    // does not touch BlockWorld, which this file does not own. `surfaceY` is
    // the top solid block of the column; `seaLevel` lets the caller flood.
    [[nodiscard]] std::uint8_t column_block(Biome b, int y, int surfaceY, int seaLevel) const {
        const BiomeDef& d = biome_def(b);
        if (y > surfaceY) return (y <= seaLevel) ? static_cast<std::uint8_t>(Water)
                                                 : static_cast<std::uint8_t>(Air);
        if (y == surfaceY) return d.surface;
        if (y > surfaceY - kFillerDepth) return d.filler;
        return static_cast<std::uint8_t>(Stone);
    }

private:
    [[nodiscard]] int clampc(int v) const { return std::clamp(v, 0, n_ - 1); }
    [[nodiscard]] std::size_t cell(int x, int z) const {
        return static_cast<std::size_t>(z) * static_cast<std::size_t>(n_)
             + static_cast<std::size_t>(x);
    }

    int n_ = 0;
    int octaves_ = 0;
    std::uint64_t seed_ = 1;
    BiomeParams params_{};
    OctaveNoise swamp_;
    std::vector<float> temp_, humid_, cont_, ero_;
    std::vector<std::uint8_t> biome_;
};

// ── predicted area fractions ────────────────────────────────────────────────
//
// Because the parameter fields are uniform on [-1, 1], the width of a band IS
// its share of the map. These helpers compute the prediction from the published
// edges so a test can compare it against a measured map — the assertion is then
// "my classifier agrees with my constants", which is worth something, rather
// than "my classifier agrees with a number I typed twice".
template <std::size_t N>
[[nodiscard]] inline double band_fraction(int level, const std::array<float, N>& edges,
                                          float bias = 0.0f) {
    const double lo = (level == 0) ? -1.0
                    : std::clamp(static_cast<double>(edges[static_cast<std::size_t>(level) - 1])
                                 - static_cast<double>(bias), -1.0, 1.0);
    const double hi = (level == static_cast<int>(N)) ? 1.0
                    : std::clamp(static_cast<double>(edges[static_cast<std::size_t>(level)])
                                 - static_cast<double>(bias), -1.0, 1.0);
    return std::max(0.0, hi - lo) / 2.0;
}

} // namespace bench
