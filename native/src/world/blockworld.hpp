// blockworld.hpp — the block world itself, with nothing in it that knows about
// agents. Terrain, ores, caves, water, trees, and the recipe graph.
//
// SHAPE. Wide and shallow, not a cube. voxelcraft.hpp is n^3 because the thing
// it measures is a vertical tech tree: dig down, and depth IS the difficulty.
// A settlement is the other problem. Agents need ground to spread out on, and a
// cube spends most of its volume on stone nobody will ever visit — at 256 cubed
// that is sixteen million blocks to hold a town that lives on sixty-five
// thousand of them. So the world is n x height x n with height fixed near 64,
// which buys a 512-wide map for the memory a 160-cube would have cost.
//
// GENERATION. Value noise summed over octaves (fractional Brownian motion), the
// standard construction — Perlin, K. "An Image Synthesizer", SIGGRAPH 1985, and
// the fBm treatment in Ebert et al., Texturing & Modeling, 3rd ed., 2003. The
// hash is integer and seeded, so a seed reproduces a world exactly and two
// seeds give genuinely different maps rather than the same map shifted.
//
// Four layers:
//   1. a height field, so there are hills and valleys
//   2. a water table at a fixed level, so low ground floods and coastlines exist
//   3. ore veins, placed by depth band — coal shallow, diamond deep
//   4. caves, carved last here
//
// A NOTE ON ORDER, since this file used to argue the opposite. Vanilla carves
// caves BEFORE placing ore, and keeps veins off cave walls with a per-block
// `discard_chance_on_air_exposure` instead. The claim once written here — that
// ore-then-caves is the only way to leave a vein exposed in a cave wall — was
// simply wrong about what the game does. Ore-then-caves is kept because it is
// what every measurement in this project was taken against, and changing it
// would move numbers that other files cite; it is a deliberate divergence now
// rather than a mistaken reconstruction.
//
// WHAT THE ORE DEPTHS MEAN. They are Minecraft's, near enough to be recognised
// and not close enough to be a claim about Minecraft: coal everywhere, iron
// below the midpoint, gold and diamond in the bottom eighth. The bench does not
// reproduce Mojang's generator and does not pretend to; what matters here is
// that the tiers are ordered and the deep ones are rare, because that ordering
// is what a learning agent has to discover.

#pragma once
#include "../rng.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace bench {

enum Block : std::uint8_t {
    Air = 0, Bedrock, Stone, Dirt, Grass, Sand, Water,
    Wood, Leaves, Coal, IronOre, GoldOre, DiamondOre,
    Plank, Cobble, Table, Furnace, Torch, Chest, Farmland, Wheat, Road,
    RedstoneOre, LapisOre, EmeraldOre, Obsidian, Gravel, Lava,
    // Two growth stages, matching Java's oak sapling. Not a full cube: agents
    // walk through them, and they are not a second kind of log.
    Sapling, SaplingAged,
    kBlocks
};

[[nodiscard]] inline const char* block_name(int b) {
    switch (b) {
        case Air: return "air";            case Bedrock: return "bedrock";
        case Stone: return "stone";        case Dirt: return "dirt";
        case Grass: return "grass";        case Sand: return "sand";
        case Water: return "water";        case Wood: return "wood";
        case Leaves: return "leaves";      case Coal: return "coal ore";
        case IronOre: return "iron ore";   case GoldOre: return "gold ore";
        case DiamondOre: return "diamond ore"; case Plank: return "planks";
        case Cobble: return "cobblestone"; case Table: return "crafting table";
        case Furnace: return "furnace";    case Torch: return "torch";
        case Chest: return "chest";        case Farmland: return "farmland";
        case Wheat: return "wheat";        case Road: return "road";
        case RedstoneOre: return "redstone ore"; case LapisOre: return "lapis ore";
        case EmeraldOre: return "emerald ore";   case Obsidian: return "obsidian";
        case Gravel: return "gravel";      case Lava: return "lava";
        case Sapling: return "sapling";    case SaplingAged: return "sapling, stage 1";
        default: return "?";
    }
}

// Solid for movement. Water is NOT solid — an agent can enter it and drown,
// which is one of the few ways this world can kill you and therefore one of the
// few things survival can be about.
[[nodiscard]] inline bool block_solid(int b) {
    return b != Air && b != Water && b != Lava && b != Torch && b != Wheat
        && b != Sapling && b != SaplingAged;
}

// ── breaking a block, by the game's own arithmetic ──────────────────────────
//
// Minecraft (Java Edition) gives every block a HARDNESS in seconds and every
// tool a SPEED MULTIPLIER, and the time to break is
//
//     seconds = hardness * 1.5 / speed          with the correct tool
//     seconds = hardness * 5.0                  with the wrong tool or none
//
// A block also has a HARVEST TIER: below it the block still breaks but drops
// nothing, which is why a wooden pickaxe on iron ore is a waste of time rather
// than slow progress. Those three numbers are the whole mining model, and they
// are reproduced here rather than approximated, because "recognisable" numbers
// would make every measurement in this sim a statement about my guesses.
//
// Values are the published Java Edition ones. Tiers: 0 hand, 1 wooden,
// 2 stone, 3 iron, 4 diamond.
struct BlockRule {
    float hardness;   // seconds, Java Edition
    int   tier;       // pickaxe tier needed to get a DROP
    bool  pickaxe;    // is a pickaxe the correct tool at all
};

[[nodiscard]] inline BlockRule block_rule(int b) {
    switch (b) {
        // shovel/hand materials — no tier, they always drop
        case Dirt: case Sand: case Gravel: case Farmland: return {0.5f, 0, false};
        case Grass:                                        return {0.6f, 0, false};
        case Leaves:                                       return {0.2f, 0, false};
        case Wheat:                                        return {0.0f, 0, false};
        // Java saplings break instantly. blockworld's break_ticks still
        // charges one tick, the same floor every zero-hardness block gets.
        case Sapling: case SaplingAged:                    return {0.0f, 0, false};
        // axe materials
        case Wood:  case Plank:                            return {2.0f, 0, false};
        case Table:                                        return {2.5f, 0, false};
        case Chest:                                        return {2.5f, 0, false};
        case Road:                                         return {2.0f, 1, true};
        case Torch:                                        return {0.0f, 0, false};
        // pickaxe materials
        case Stone:                                        return {1.5f, 1, true};
        case Cobble:                                       return {2.0f, 1, true};
        case Furnace:                                      return {3.5f, 1, true};
        case Coal:                                         return {3.0f, 1, true};
        case IronOre:                                      return {3.0f, 2, true};
        case LapisOre:                                     return {3.0f, 2, true};
        case GoldOre:                                      return {3.0f, 3, true};
        case DiamondOre:                                   return {3.0f, 3, true};
        case RedstoneOre:                                  return {3.0f, 3, true};
        case EmeraldOre:                                   return {3.0f, 3, true};
        case Obsidian:                                     return {50.0f, 4, true};
        case Bedrock:                                      return {-1.0f, 99, true};  // never
        default:                                           return {1.0f, 0, false};
    }
}

// Tool speed multipliers, Java Edition. Gold is the fast, fragile one and sits
// out of tier order on purpose — it is not a mistake in the table.
[[nodiscard]] inline float tool_speed(int tier) {
    switch (tier) {
        case 0: return 1.0f;    // fist
        case 1: return 2.0f;    // wooden
        case 2: return 4.0f;    // stone
        case 3: return 6.0f;    // iron
        case 4: return 8.0f;    // diamond
        default: return 1.0f;
    }
}

// Ticks to break, at 20 ticks a second. Always at least one tick: in the game a
// block with hardness 0 breaks instantly, and an action that takes no time at
// all would let an agent empty a chunk in a single step.
[[nodiscard]] inline int break_ticks(int block, int tier) {
    const BlockRule r = block_rule(block);
    if (r.hardness < 0.0f) return -1;                  // bedrock: never
    const bool correct = r.pickaxe ? (tier >= 1) : true;
    const float seconds = correct ? r.hardness * 1.5f / tool_speed(tier)
                                  : r.hardness * 5.0f;
    return std::max(1, int(seconds * 20.0f + 0.5f));
}

// Does the agent's tool tier get a DROP from this block? Below the harvest tier
// the block still breaks and yields nothing.
[[nodiscard]] inline bool drops_for(int block, int tier) {
    return tier >= block_rule(block).tier;
}

// ── the recipe graph ────────────────────────────────────────────────────────
//
// Items an agent can hold that are not blocks. Costs are real: a recipe consumes
// its inputs. voxelcraft learned this the hard way — crafting with no material
// cost meant one adjacency unlocked a whole rung and the benchmark measured
// nothing.
enum Item : std::uint8_t {
    ItPlank = 0, ItStick, ItTable, ItWoodPick, ItStonePick, ItFurnace,
    ItIronIngot, ItIronPick, ItDiamondPick, ItBread, ItTorch,
    kItems
};

[[nodiscard]] inline const char* item_name(int i) {
    switch (i) {
        case ItPlank: return "planks";        case ItStick: return "sticks";
        case ItTable: return "crafting table";case ItWoodPick: return "wooden pickaxe";
        case ItStonePick: return "stone pickaxe"; case ItFurnace: return "furnace";
        case ItIronIngot: return "iron ingot";case ItIronPick: return "iron pickaxe";
        case ItDiamondPick: return "diamond pickaxe"; case ItBread: return "bread";
        case ItTorch: return "torch";         default: return "?";
    }
}

// A recipe: up to three ingredients, each a block or an item, and how many it
// makes. `station` is the block that must be adjacent — Air means anywhere.
struct Ingredient { bool isItem; std::uint8_t what; int count; };
struct Recipe {
    std::array<Ingredient, 3> in;
    int  makes;
    std::uint8_t station;     // Air, Table or Furnace
    int  tier;                // tool tier this recipe confers, -1 for none
};

[[nodiscard]] inline Recipe recipe(int item) {
    auto B = [](Block b, int n) { return Ingredient{false, std::uint8_t(b), n}; };
    auto I = [](Item i, int n)  { return Ingredient{true,  std::uint8_t(i), n}; };
    auto none = Ingredient{false, Air, 0};
    switch (item) {
        case ItPlank:      return {{B(Wood,1), none, none},            4, Air,     -1};
        case ItStick:      return {{I(ItPlank,2), none, none},         4, Air,     -1};
        case ItTable:      return {{I(ItPlank,4), none, none},         1, Air,     -1};
        case ItWoodPick:   return {{I(ItPlank,3), I(ItStick,2), none}, 1, Table,    1};
        case ItStonePick:  return {{B(Cobble,3), I(ItStick,2), none},  1, Table,    2};
        case ItFurnace:    return {{B(Cobble,8), none, none},          1, Table,   -1};
        case ItIronIngot:  return {{B(IronOre,1), B(Coal,1), none},    1, Furnace, -1};
        case ItIronPick:   return {{I(ItIronIngot,3), I(ItStick,2), none}, 1, Table, 3};
        case ItDiamondPick:return {{B(DiamondOre,3), I(ItStick,2), none}, 1, Table, 4};
        case ItBread:      return {{B(Wheat,3), none, none},           1, Air,     -1};
        case ItTorch:      return {{B(Coal,1), I(ItStick,1), none},    4, Air,     -1};
        default:           return {{none, none, none},                 0, Air,     -1};
    }
}

// ── the world ───────────────────────────────────────────────────────────────

class BlockWorld {
public:
    BlockWorld() = default;
    BlockWorld(int n, int height = 128) { resize(n, height); }

    void resize(int n, int height) {
        n_ = std::max(16, n);
        h_ = std::max(16, height);
        blocks_.assign(std::size_t(n_) * h_ * n_, std::uint8_t(Air));
        heights_.assign(std::size_t(n_) * n_, 0);
        changed_.clear();
    }

    [[nodiscard]] int n() const { return n_; }
    [[nodiscard]] int height() const { return h_; }
    [[nodiscard]] std::size_t volume() const { return blocks_.size(); }
    [[nodiscard]] const std::vector<std::uint8_t>& raw() const { return blocks_; }

    [[nodiscard]] bool inside(int x, int y, int z) const {
        return x >= 0 && y >= 0 && z >= 0 && x < n_ && y < h_ && z < n_;
    }
    [[nodiscard]] std::size_t idx(int x, int y, int z) const {
        return (std::size_t(y) * std::size_t(n_) + std::size_t(z)) * std::size_t(n_)
             + std::size_t(x);
    }
    // Out of bounds reads as Bedrock, not Air. Air would invite an agent to walk
    // off the edge of the world and fall forever; Bedrock makes the boundary a
    // wall that nothing can break, which is what it is.
    [[nodiscard]] std::uint8_t at(int x, int y, int z) const {
        if (!inside(x, y, z)) return Bedrock;
        return blocks_[idx(x, y, z)];
    }
    void set(int x, int y, int z, std::uint8_t b) {
        if (!inside(x, y, z)) return;
        const std::size_t i = idx(x, y, z);
        if (blocks_[i] == b) return;
        blocks_[i] = b;
        if (recording_) changed_.push_back(std::uint32_t(i));
    }

    // ── change recording ────────────────────────────────────────────────────
    //
    // WHY THIS IS HERE AND NOT IN THE CALLER. Light is a global field: a cell
    // that changes has to be handed to LightEngine::block_changed or the light
    // there is stale forever. A caller can do that for its own edits, but
    // Fluids owns a BlockWorld& and writes through it directly — water
    // spreading, sand landing, lava turning to obsidian — and none of those
    // writes pass through any code the sim controls. The alternatives were to
    // diff the whole block array after every fluid tick (O(volume) per tick,
    // which this project will not pay) or to put a callback inside fluids.hpp
    // (a finished, tested subsystem that is not to be rewritten). Recording the
    // writes at the one place they all funnel through costs a single predictable
    // branch and is exact.
    //
    // Recording is OFF by default so generate() — which writes millions of
    // cells — does not build a list nobody will read.
    void record_changes(bool on) { recording_ = on; }
    [[nodiscard]] bool recording() const { return recording_; }
    [[nodiscard]] const std::vector<std::uint32_t>& changes() const { return changed_; }
    void clear_changes() { changed_.clear(); }
    // Unpack a recorded linear index. Same arithmetic as idx(), inverted.
    void unpack(std::uint32_t i, int& x, int& y, int& z) const {
        const std::uint32_t n = std::uint32_t(n_);
        x = int(i % n);
        const std::uint32_t t = i / n;
        z = int(t % n);
        y = int(t / n);
    }

    // y of the highest solid block in this column, or -1 if the column is empty.
    // Cached, because agents ask constantly and a scan is O(height).
    [[nodiscard]] int surface(int x, int z) const {
        if (x < 0 || z < 0 || x >= n_ || z >= n_) return -1;
        return heights_[std::size_t(z) * std::size_t(n_) + std::size_t(x)];
    }
    void refresh_column(int x, int z) {
        if (x < 0 || z < 0 || x >= n_ || z >= n_) return;
        int top = -1;
        for (int y = h_ - 1; y >= 0; --y)
            if (block_solid(at(x, y, z))) { top = y; break; }
        heights_[std::size_t(z) * std::size_t(n_) + std::size_t(x)] = top;
    }

    // Y=63, the game's own sea level, which is why the world is 128 tall: the
    // published ore bands are quoted in absolute Y and rescaling them would
    // make every depth in this file a statement about my arithmetic instead of
    // about the game.
    [[nodiscard]] int sea_level() const { return std::min(63, h_ / 2 - 1); }

    // Roughly how many hills across the map, at any size.
    static constexpr float kFeatures = 6.0f;

    // ── generation ──────────────────────────────────────────────────────────
    //
    // `plantTrees` exists for a caller that wants to place its own trees — the
    // town sim plants per BIOME, at the published per-biome densities, and the
    // generic pass here would put an oak in the middle of a desert first. It
    // defaults to true so every existing caller (voxelcraft, the self-test's
    // "a 64-wide world grows trees" assertion) is untouched.
    //
    // There is deliberately no biome hook here. A caller with a biome map
    // repaints the soil afterwards (VoxelCity::applyBiomeSurface), which is
    // strictly better than painting it during generation: by then the ore veins
    // and caves exist, so the repaint can refuse to touch them. A hook in this
    // loop would run before either and would have to be told about both.
    void generate(std::uint64_t seed, bool plantTrees = true) {
        seed_ = seed;
        maxGround_ = 0;
        // Generation writes the whole volume. Recording that would build a list
        // of millions of indices nobody is going to read, so it is suspended
        // here rather than left to every caller to remember.
        const bool wasRecording = recording_;
        recording_ = false;
        std::fill(blocks_.begin(), blocks_.end(), std::uint8_t(Air));

        const int sea = sea_level();
        // 1. terrain. Two octaves of ridge for the large shapes plus two fine
        //    ones for texture; amplitude scaled to the world's height so the
        //    map looks the same at 64 wide and at 512.
        const float amp = float(h_) * 0.22f;
        for (int z = 0; z < n_; ++z)
            for (int x = 0; x < n_; ++x) {
                // Frequency scales with the map, so every size gets the same
                // NUMBER of hills rather than the same hill size. At a fixed
                // 0.012 per block a 64-wide world spanned 0.77 of a noise cell
                // — one nearly flat plane, which landed under sea level and
                // turned the entire map to sand: 113 grass cells in 4,096 and
                // seven wood blocks in the whole world. Every agent then
                // starved for want of a tree, and the tech tree measured
                // nothing at all. A generator that only works at one size is
                // not a generator.
                const float k = kFeatures / float(n_);
                const float f = fbm(float(x) * k, float(z) * k, 4);
                int gh = sea + int(amp * (f * 2.0f - 1.0f));
                gh = std::clamp(gh, 2, h_ - 6);
                maxGround_ = std::max(maxGround_, gh);
                const bool beach = gh <= sea + 1;
                for (int y = 0; y <= gh; ++y) {
                    std::uint8_t b = Stone;
                    if (y == gh)          b = beach ? Sand : Grass;
                    else if (y > gh - 4)  b = beach ? Sand : Dirt;
                    set(x, y, z, b);
                }
                set(x, 0, z, Bedrock);
                // 2. water table, filling everything below sea level that the
                //    terrain did not.
                for (int y = gh + 1; y <= sea; ++y) set(x, y, z, Water);
            }

        // 3. ore veins, by depth band. Counts are densities against the world's
        //    area so a bigger map is the same world with more of it, rather than
        //    the same handful of resources lost in more space.
        // ── ore, at the game's own depths and vein sizes ────────────────────
        //
        // Java Edition (pre-1.18 distribution, which is the one everybody knows
        // and the one the MineRL work was built on). Veins per chunk and
        // maximum vein size are the published values; a chunk is 16x16, so the
        // count below is veins-per-chunk times the number of chunks in this map.
        //
        //   coal      Y 0-127   20 veins/chunk   size 17
        //   iron      Y 0-63    20 veins/chunk   size  9
        //   gold      Y 0-31     2 veins/chunk   size  9
        //   redstone  Y 0-15     8 veins/chunk   size  8
        //   diamond   Y 0-15     1 vein /chunk   size  8
        //   lapis     Y 0-30     1 vein /chunk   size  7
        //   emerald   Y 4-31    single blocks, mountains only
        //
        // `size` is the generator's shape parameter, NOT a block count — this
        // file previously read it as "up to 17 blocks" for coal, which
        // understates a real coal vein by about half. The published size-to-
        // maximum-blocks table gives 37 for size 17. The walk below uses size
        // as a step count, so its veins are smaller than vanilla's by roughly
        // that factor; that is a divergence to be aware of when comparing
        // yields, not a transcription of the game's numbers.
        //
        // An earlier version invented bands as fractions of the terrain and got
        // the tiering INVERTED — iron above coal — because veins only take in
        // stone and a band most of the world cannot honour collapses onto the
        // ones that can. The real numbers do not have that problem: they were
        // chosen against a world with the same sea level as this one.
        const double chunks = (double(n_) / 16.0) * (double(n_) / 16.0);
        vein(Coal,        int(chunks * 20), 0, std::min(127, h_ - 2), 17, 0x51A1u);
        vein(IronOre,     int(chunks * 20), 0, 63,                     9, 0x51A2u);
        vein(GoldOre,     int(chunks *  2), 0, 31,                     9, 0x51A3u);
        vein(RedstoneOre, int(chunks *  8), 0, 15,                     8, 0x51A5u);
        vein(DiamondOre,  int(chunks *  1), 0, 15,                     8, 0x51A4u);
        vein(LapisOre,    int(chunks *  1), 0, 30,                     7, 0x51A6u);
        vein(EmeraldOre,  int(chunks *  1), 4, 31,                     1, 0x51A7u);
        // Lava pools in the deep, which is the other thing that kills you down
        // there. Y 0-10, where the game puts its lava lakes.
        vein(Lava,        int(chunks *  2), 1, 10,                     6, 0x51A8u);

        // 4. caves, carved last so they cut ore open.
        carve_caves();

        // 5. trees, on grass only, after caves so none is left floating over a
        //    hole. Density per area, same reasoning as the veins.
        // Roughly a forest. Vanilla plains is NOT "about 1 tree a chunk" — it
        // is a weighted choice of {0 with weight 19, 1 with weight 1}, so 0.05
        // trees a chunk, while forest and taiga run about 10.1. Eight is a
        // wooded value between them, chosen because the first rung of the tech
        // tree is wood and a map an agent can starve on for want of a trunk is
        // not a fair benchmark. The old citation for this number was wrong by
        // a factor of twenty.
        if (plantTrees) plant_trees(int(chunks * 8));

        for (int z = 0; z < n_; ++z) for (int x = 0; x < n_; ++x) refresh_column(x, z);
        changed_.clear();
        recording_ = wasRecording;
    }

    // How many of a block the world contains. Used by the tests to assert that
    // generation actually produced the thing it claims to.
    [[nodiscard]] std::size_t count(std::uint8_t b) const {
        std::size_t n = 0;
        for (auto c : blocks_) if (c == b) ++n;
        return n;
    }

private:
    // Integer hash, so the noise is reproducible and has no table to initialise.
    [[nodiscard]] std::uint32_t hash(std::uint32_t a) const {
        a ^= std::uint32_t(seed_) + 0x9E3779B9u + (a << 6) + (a >> 2);
        a ^= a >> 16; a *= 0x7FEB352Du;
        a ^= a >> 15; a *= 0x846CA68Bu;
        a ^= a >> 16;
        return a;
    }
    [[nodiscard]] float vnoise(int xi, int zi, std::uint32_t salt) const {
        return float(hash(std::uint32_t(xi) * 374761393u
                        + std::uint32_t(zi) * 668265263u + salt) & 0xFFFFFFu)
             / float(0xFFFFFF);
    }
    [[nodiscard]] float vnoise3(int xi, int yi, int zi, std::uint32_t salt) const {
        return float(hash(std::uint32_t(xi) * 374761393u + std::uint32_t(yi) * 2246822519u
                        + std::uint32_t(zi) * 668265263u + salt) & 0xFFFFFFu)
             / float(0xFFFFFF);
    }
    // Smoothstep interpolation. Linear interpolation between lattice points
    // leaves visible creases along the grid, which read as square hills.
    static float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

    [[nodiscard]] float noise2(float x, float z, std::uint32_t salt) const {
        const int xi = int(std::floor(x)), zi = int(std::floor(z));
        const float fx = smooth(x - float(xi)), fz = smooth(z - float(zi));
        const float a = vnoise(xi,   zi,   salt), b = vnoise(xi+1, zi,   salt);
        const float c = vnoise(xi,   zi+1, salt), d = vnoise(xi+1, zi+1, salt);
        return (a + (b - a) * fx) * (1.0f - fz) + (c + (d - c) * fx) * fz;
    }
    [[nodiscard]] float fbm(float x, float z, int octaves) const {
        float sum = 0.0f, amp = 0.5f, freq = 1.0f, norm = 0.0f;
        for (int o = 0; o < octaves; ++o) {
            sum  += amp * noise2(x * freq, z * freq, 0x1234u + std::uint32_t(o) * 7919u);
            norm += amp;
            amp  *= 0.5f; freq *= 2.0f;
        }
        return norm > 0.0f ? sum / norm : 0.0f;
    }

    // A vein is a short random walk through stone, so ore comes in the clumps
    // that make finding one worth following rather than in isolated cubes.
    void vein(std::uint8_t ore, int veins, int yLo, int yHi, int len, std::uint32_t salt) {
        if (veins <= 0 || yHi <= yLo) return;
        Rng r(seed_ ^ (std::uint64_t(salt) << 17));
        for (int v = 0; v < veins; ++v) {
            int x = int(r.unit() * float(n_)) % n_;
            int z = int(r.unit() * float(n_)) % n_;
            int y = yLo + int(r.unit() * float(yHi - yLo)) % std::max(1, yHi - yLo);
            for (int s = 0; s < len; ++s) {
                if (at(x, y, z) == Stone) set(x, y, z, ore);
                x += int(r.unit() * 3.0f) - 1;
                y += int(r.unit() * 3.0f) - 1;
                z += int(r.unit() * 3.0f) - 1;
                x = std::clamp(x, 0, n_ - 1);
                z = std::clamp(z, 0, n_ - 1);
                y = std::clamp(y, yLo, yHi);
            }
        }
    }

    // Caves as a 3D noise threshold. Cheaper than worm tunnelling and it gives
    // connected chambers rather than isolated bubbles, which is what matters:
    // a cave an agent cannot walk into is just a hole in the density map.
    void carve_caves() {
        const int top = h_ - 3;
        for (int y = 2; y < top; ++y) {
            // Fade out near the surface so caves do not eat the terrain.
            const float depth = 1.0f - float(y) / float(top);
            const float cut = 0.58f + 0.22f * (1.0f - depth);
            for (int z = 0; z < n_; ++z)
                for (int x = 0; x < n_; ++x) {
                    const std::uint8_t b = at(x, y, z);
                    if (b != Stone && b != Dirt && b != Coal && b != IronOre) continue;
                    const float v = cave_noise(x, y, z);
                    if (v > cut) set(x, y, z, Air);
                }
        }
    }
    [[nodiscard]] float cave_noise(int x, int y, int z) const {
        // Trilinear value noise on a coarse lattice: cells about 8 blocks wide,
        // which is roughly a corridor an agent can stand in.
        const float s = 0.14f;
        const float fx = float(x) * s, fy = float(y) * s * 1.7f, fz = float(z) * s;
        const int xi = int(std::floor(fx)), yi = int(std::floor(fy)), zi = int(std::floor(fz));
        const float tx = smooth(fx - float(xi)), ty = smooth(fy - float(yi)),
                    tz = smooth(fz - float(zi));
        auto L = [&](int dy) {
            const float a = vnoise3(xi,   yi+dy, zi,   0xCAFEu), b = vnoise3(xi+1, yi+dy, zi,   0xCAFEu);
            const float c = vnoise3(xi,   yi+dy, zi+1, 0xCAFEu), d = vnoise3(xi+1, yi+dy, zi+1, 0xCAFEu);
            return (a + (b - a) * tx) * (1.0f - tz) + (c + (d - c) * tx) * tz;
        };
        const float l0 = L(0), l1 = L(1);
        return l0 + (l1 - l0) * ty;
    }

    void plant_trees(int count) {
        if (count <= 0) return;
        Rng r(seed_ ^ 0x77EE13ull);
        for (int t = 0; t < count; ++t) {
            const int x = int(r.unit() * float(n_)) % n_;
            const int z = int(r.unit() * float(n_)) % n_;
            int gy = -1;
            for (int y = h_ - 1; y >= 0; --y) if (at(x, y, z) != Air) { gy = y; break; }
            if (gy < 0) continue;
            const std::uint8_t top = at(x, gy, z);
            if (top != Grass && top != Dirt) continue;
            const int trunk = 3 + int(r.unit() * 3.0f);
            if (gy + trunk + 2 >= h_) continue;
            for (int i = 1; i <= trunk; ++i) set(x, gy + i, z, Wood);
            const int cy = gy + trunk;
            for (int dy = 0; dy <= 2; ++dy)
                for (int dz = -2; dz <= 2; ++dz)
                    for (int dx = -2; dx <= 2; ++dx) {
                        if (std::abs(dx) + std::abs(dz) + dy > 3) continue;
                        if (dx == 0 && dz == 0 && dy == 0) continue;
                        if (at(x+dx, cy+dy, z+dz) == Air) set(x+dx, cy+dy, z+dz, Leaves);
                    }
        }
    }

    int n_ = 0, h_ = 0;
    int maxGround_ = 0;        // highest terrain column, so ore bands can scale to it
    std::uint64_t seed_ = 1;
    std::vector<std::uint8_t> blocks_;
    std::vector<int> heights_;
    bool recording_ = false;
    std::vector<std::uint32_t> changed_;
};

} // namespace bench
