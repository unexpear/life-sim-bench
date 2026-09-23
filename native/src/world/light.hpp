// light.hpp — the light engine: sky light and block light, 0..15, per block,
// with incremental propagation AND incremental removal.
//
// ─────────────────────────────────────────────────────────────────────────────
// WHY THIS FILE EXISTS AT ALL
//
// Light is the only global field in this world. Everything else an agent cares
// about is local — is there stone here, is there water there — but "how dark is
// this square" is the answer to a flood fill that may have started fifteen
// blocks away, and it changes when a block fifteen blocks away changes. Get it
// wrong and the hostile-spawn predicate lies, which means the torch an agent
// crafted did nothing, which means the whole reason to craft a torch evaporates.
//
// ─────────────────────────────────────────────────────────────────────────────
// THE MODEL, AND WHERE EVERY NUMBER COMES FROM
//
// Minecraft Java Edition. Sources, all read (August 2026):
//   [W-LIGHT] https://minecraft.wiki/w/Light
//   [W-OPAC]  https://minecraft.wiki/w/Opacity
//   [W-SPAWN] https://minecraft.wiki/w/Mob_spawning
//   [SOA]     Ben Arnold, "Fast Flood Fill Lighting in a Blocky Voxel Game",
//             Seed of Andromeda, parts 1-2 — the canonical write-up of the
//             REMOVAL queue. (seedofandromeda.com; its TLS chain no longer
//             validates here, so the mirror at
//             notverymoe.github.io/md-gamedev-gems/voxel/lighting/soa/ was read
//             instead. Same text, same pseudocode.)
//
// 1. SIXTEEN LEVELS. [W-LIGHT]: "16 light levels, specified by an integer from
//    0 (the minimum) through 15 (the maximum)." Two independent channels, sky
//    and block, exactly as the game stores them (one byte per block, a nibble
//    each — 0fps.net/2018/02/21/voxel-lighting/ notes Minecraft's own layout is
//    "4 bits for torch light + 4 bits for sky light"). That is why this file
//    packs two nibbles rather than using two byte arrays: it is what the game
//    does, and at 256x128x256 it is 8 MB instead of 16 MB.
//
// 2. BLOCK LIGHT. [W-LIGHT]: "The block light level decreases by one for each
//    meter (block) of taxicab distance from the light source." The general form,
//    which is what is actually implemented below, is Java's
//    LayerLightEngine.computeLevelFromNeighbor:
//
//        arriving = source_level - max(1, opacity(DESTINATION block))
//
//    Note *destination*: the block being entered pays its own opacity. An
//    opaque block (opacity 15) therefore ends at 0 and passes nothing on.
//    max(1, ·) is why [W-LIGHT] can say the opacity-1 filter blocks "do not
//    affect block light" — max(1,1) == 1, which is the ordinary step cost.
//
// 3. SKY LIGHT. [W-LIGHT], quoted exactly: "When sky light of a level of 15
//    spreads down through a transparent block, the level remains unchanged.
//    When it spreads horizontally or upwards, it reduces its level by 1.
//    However, when it spreads through a light-filtering block, it does not
//    follow the above two rules and it attenuates by a certain number of light
//    levels. Sky light with a level less than 15 spreads in a similar way as
//    block light does."
//
//    That is a formula, not a description, and it is implemented as the formula:
//
//        if (direction == DOWN && source_level == 15 && opacity(dest) == 0)
//             arriving = 15;
//        else arriving = source_level - max(1, opacity(dest));
//
//    Two consequences worth stating because they surprise people. A sky light
//    of 15 can only ever be the *direct* column — sideways costs a level, so
//    nothing off-column reaches 15. And light does not recover after passing a
//    filter: under a single water block the air is 13, not 15, because the water
//    is at 14 and 14 is not 15 so the free-fall rule no longer applies.
//
// 4. OPACITY, per block. [W-LIGHT] lists the Java light-filtering blocks:
//    "all of the following light-filtering blocks decrease sky light by 1 level
//    (but do not affect block light)" — water, waterlogged blocks, ice, cobweb,
//    leaves, slime, honey, lava, and others. [W-OPAC] adds that glass and
//    carpets have "no additional modifier", and that chests are transparent
//    (made so in Beta 1.8 pre-release 1). Opaque full cubes block everything.
//    See light_opacity() for the table and for the two values I could not source.
//
// 5. HOSTILE SPAWNING. [W-SPAWN], quoted: "if the internal sky light level is 7
//    or less (which always occurs inside a cave) and the block level is 0, all
//    Overworld monsters can spawn". The block-light-must-be-0 rule is the 1.18
//    (Caves & Cliffs part II) change; before 1.18 the test was light <= 7.
//    [W-LIGHT] on the internal value: "the game uses sky light, time, and
//    weather to calculate an internal sky light value", and gives the worked
//    case that at midnight in clear weather a sky light of 15 is an internal sky
//    light of 4. 15 - 4 = 11 is therefore the midnight darkening, and that
//    subtraction is exposed as a parameter rather than hardcoded because the
//    wiki quotes the endpoint, not the curve.
//
// ─────────────────────────────────────────────────────────────────────────────
// REMOVAL, WHICH IS THE ENTIRE POINT
//
// Adding light is a breadth-first flood fill and nobody gets it wrong. Removing
// it is where naive engines break, and it is why Minecraft carries a second
// queue. The failure mode: break one of two nearby torches, walk the removal
// outward zeroing everything, and you have just erased half of the *other*
// torch's field, which no longer has any reason to re-propagate.
//
// The fix [SOA] is a removal BFS that carries the level it is erasing:
//
//     if (neighbour != 0 && neighbour < current)   -> zero it, queue for removal
//     else if (neighbour >= current)               -> queue it for PROPAGATION
//
// The second branch is the whole trick. A neighbour at least as bright as the
// wave you are erasing cannot have been lit by the source you are killing, so it
// is a survivor, and survivors are exactly the seeds that refill the hole. The
// pass may over-erase; the refill pass repairs it. Both are bounded by 15 steps.
//
// SKY LIGHT NEEDS ONE MORE RULE, and this is the bug that cost me the most time.
// Roof over an open column: the cell you just darkened held 15 and the cell
// below it also holds 15. `neighbour < current` is false (15 < 15), so the naive
// rule files the entire shaft below as a *survivor* and the column stays lit
// forever under a solid roof. Because the propagation rule lets 15 fall for free,
// the removal rule must let 15 fall for free too:
//
//     if (direction == DOWN && current == 15 && neighbour == 15) -> also remove
//
// It is sound precisely because sky 15 is unreachable sideways (rule 3): a 15
// under a 15 that just died can only have come from the same column.
//
// ─────────────────────────────────────────────────────────────────────────────
// COMPLEXITY
//
//   rebuild()        O(volume). Startup only. 96x128x96 = 1.18 M cells.
//   block_changed()  O(h) for one column rescan, plus a flood bounded by the
//                    light radius. Independent of n. Light dies after 15 steps,
//                    so the fill can never leave a taxicab ball of radius 15
//                    (~5000 cells) except in the one case where sky light falls
//                    down a shaft — and that is bounded by h * 15^2, ~29k cells,
//                    for a change that opens a 128-deep column to the sky.
//                    Measured in test_light.cpp: the mean is ~2 orders of
//                    magnitude below even that. Nothing here scales with n^2,
//                    which is the property that makes 256x128x256 affordable.

#pragma once
#include "blockworld.hpp"
#include "../rng.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace bench {

// ── published constants ─────────────────────────────────────────────────────

// [W-LIGHT] "16 light levels ... from 0 (the minimum) through 15 (the maximum)".
inline constexpr int kLightMax = 15;

// Luminance of the emitters this world has, Java Edition.
// [W-LIGHT] light-emitting-blocks table.
inline constexpr int kTorchLight     = 14;   // torch
inline constexpr int kLavaLight      = 15;   // lava (source and flowing alike)
inline constexpr int kGlowstoneLight = 15;   // glowstone — no such block here,
                                             // published because the light API
                                             // is specified in terms of it
inline constexpr int kLitFurnaceLight = 13;  // furnace with lit=true; blast
                                             // furnace and smoker are also 13

// [W-LIGHT]: at midnight in clear weather a sky light of 15 reads as an
// internal sky light of 4. The darkening is therefore 15 - 4 = 11. DERIVED from
// the wiki's worked example, not quoted from it — the wiki gives the endpoint,
// not the day-length curve, so intermediate times of day are not modelled here
// and daytime is simply 0.
inline constexpr int kSkyDarkenMidnight = 11;
inline constexpr int kSkyDarkenNoon     = 0;

// [W-SPAWN]: "if the internal sky light level is 7 or less ... and the block
// level is 0, all Overworld monsters can spawn". Java Edition 1.18+.
inline constexpr int kHostileMaxInternalSky = 7;
inline constexpr int kHostileMaxBlockLight  = 0;

// [W-SPAWN]: "a spawn only occurs if the light level is less than or equal to a
// random number between 0 and 7", i.e. eight outcomes.
inline constexpr int kSpawnLightRollSides = 8;

// ── per-block tables ────────────────────────────────────────────────────────

// How much light a block emits. Only two blocks in this world emit anything.
//
// Furnace is 0 on purpose and it is not an oversight: a lit furnace is 13 in
// Java, but this world's Furnace block has no lit state — it is a crafting
// station that is either there or not — so giving it 13 would light every
// kitchen permanently and would be a claim about a block state that does not
// exist here. kLitFurnaceLight is published above for anyone who adds one.
[[nodiscard]] inline int light_emission(int block) {
    switch (block) {
        case Torch: return kTorchLight;   // 14, [W-LIGHT]
        case Lava:  return kLavaLight;    // 15, [W-LIGHT]
        default:    return 0;
    }
}

// How much light a block subtracts from anything entering it.
//
//   0  — free passage. Sky light 15 falls through at 15.
//   1  — a light-filtering block: costs sky light its free fall, costs block
//        light nothing extra because the step already costs 1 (max(1,opacity)).
//   15 — opaque. Arrives at 0, propagates nothing.
//
// CITED: water, leaves and lava are on [W-LIGHT]'s Java light-filtering list
// (opacity 1). Torch, wheat/crops and chest are on [W-OPAC]'s transparent list
// — non-solids and non-full-block solids — so 0. Full opaque cubes are 15.
//
// A CONTRADICTION I DID NOT PAPER OVER: [W-OPAC] says "Lava is set to
// completely block light propagation", while [W-LIGHT]'s Java table lists lava
// among the blocks that decrease sky light by exactly 1. They cannot both hold.
// I follow [W-LIGHT], because it is the page that is explicitly scoped to Java
// Edition and because Java's BlockBehaviour.getLightBlock returns 1 for any
// non-solid-render block that does not propagate skylight down, which is what a
// fluid is. If [W-OPAC] is describing Bedrock, both are right and this comment
// is the reason the two agree.
//
// NOT VERIFIED, and flagged rather than guessed: Farmland is 15 here. Farmland
// is a 15/16-high block in Java, and I could not source its light value; it is
// treated as the dirt it is made of, which may be wrong by one level for sky
// light directly above a field. Road is a bench-invented block with no
// Minecraft counterpart at all, so its 15 is a design choice, not a citation.
[[nodiscard]] inline int light_opacity(int block) {
    switch (block) {
        case Air:                  return 0;   // [W-OPAC] air
        case Torch:                return 0;   // [W-OPAC] non-solids
        case Wheat:                return 0;   // [W-OPAC] non-solids (crops)
        case Chest:                return 0;   // [W-OPAC] non-full-block solids
        case Water:                return 1;   // [W-LIGHT] light-filtering
        case Leaves:               return 1;   // [W-LIGHT] light-filtering
        case Lava:                 return 1;   // [W-LIGHT] light-filtering
        default:                   return kLightMax;  // opaque full cube
    }
}

// ── the engine ──────────────────────────────────────────────────────────────

class LightEngine {
public:
    // The engine reads the world and never writes it. The caller mutates the
    // world through BlockWorld::set and then tells the engine which cell moved.
    explicit LightEngine(const BlockWorld& world) : w_(&world) { resize(); }

    // Drops all light and re-reads the world's dimensions. Call after the world
    // is resized or regenerated; rebuild() calls it for you.
    void resize() {
        n_ = w_->n();
        h_ = w_->height();
        light_.assign(std::size_t(n_) * std::size_t(h_) * std::size_t(n_), 0);
        skyTop_.assign(std::size_t(n_) * std::size_t(n_), std::int16_t(-1));
        add_.clear();  addHead_ = 0;
        rem_.clear();  remHead_ = 0;
        nodes_ = 0; lastNodes_ = 0; totalNodes_ = 0;
    }

    // ── queries ─────────────────────────────────────────────────────────────

    // Out of bounds is dark, matching BlockWorld's "outside is bedrock".
    [[nodiscard]] int sky(int x, int y, int z) const {
        if (!w_->inside(x, y, z)) return 0;
        return light_[w_->idx(x, y, z)] >> 4;
    }
    [[nodiscard]] int block_light(int x, int y, int z) const {
        if (!w_->inside(x, y, z)) return 0;
        return light_[w_->idx(x, y, z)] & 0x0F;
    }
    // What a torch-carrying observer sees. [W-LIGHT]: the displayed light is
    // max(sky light, block light).
    [[nodiscard]] int light(int x, int y, int z) const {
        return std::max(sky(x, y, z), block_light(x, y, z));
    }
    // [W-LIGHT] "the game uses sky light, time, and weather to calculate an
    // internal sky light value". skyDarken is that reduction: 0 at noon,
    // kSkyDarkenMidnight at midnight.
    [[nodiscard]] int internal_sky(int x, int y, int z, int skyDarken) const {
        return std::max(0, sky(x, y, z) - skyDarken);
    }
    // [W-LIGHT] "the internal light level is calculated as the maximum level of
    // the block light and the internal sky light". This is the number that
    // drives spawning, crop growth and daylight sensors — not the displayed one.
    [[nodiscard]] int internal_light(int x, int y, int z, int skyDarken) const {
        return std::max(block_light(x, y, z), internal_sky(x, y, z, skyDarken));
    }

    // The light half of the spawn test, and nothing else. [W-SPAWN], Java 1.18+:
    // internal sky light <= 7 AND block light == 0.
    [[nodiscard]] bool light_allows_hostile_spawn(int x, int y, int z, int skyDarken) const {
        return block_light(x, y, z) <= kHostileMaxBlockLight
            && internal_sky(x, y, z, skyDarken) <= kHostileMaxInternalSky;
    }

    // The geometry half. [W-SPAWN]: "the block directly below it must have a
    // solid, complete, top surface", and the spawn space "must not be a block
    // with a full 1x1x1 collision box". Two blocks of clearance, because the
    // mobs this predicate is about are two blocks tall. Fluids are excluded:
    // drowned and squid are the water spawners and this world has neither.
    [[nodiscard]] bool spawnable_footing(int x, int y, int z) const {
        if (!w_->inside(x, y, z)) return false;
        if (!block_solid(w_->at(x, y - 1, z))) return false;
        const int here  = w_->at(x, y,     z);
        const int above = w_->at(x, y + 1, z);
        if (block_solid(here)  || here  == Water || here  == Lava) return false;
        if (block_solid(above) || above == Water || above == Lava) return false;
        return true;
    }

    // "Can a hostile mob spawn here" — light and footing together.
    [[nodiscard]] bool hostile_can_spawn(int x, int y, int z, int skyDarken) const {
        return spawnable_footing(x, y, z)
            && light_allows_hostile_spawn(x, y, z, skyDarken);
    }

    // The randomised form. [W-SPAWN]: "a spawn only occurs if the light level is
    // less than or equal to a random number between 0 and 7". Randomness comes
    // from the caller's Rng so a seed reproduces a run exactly; there is no
    // hidden global and no clock.
    [[nodiscard]] bool roll_hostile_spawn(int x, int y, int z, int skyDarken, Rng& rng) const {
        if (!hostile_can_spawn(x, y, z, skyDarken)) return false;
        const int roll = int(rng.below(kSpawnLightRollSides));
        return internal_light(x, y, z, skyDarken) <= roll;
    }

    // y of the highest light-blocking (opacity > 0) block in the column, or -1.
    // Everything strictly above it is sky light 15 by definition.
    [[nodiscard]] int sky_top(int x, int z) const {
        if (x < 0 || z < 0 || x >= n_ || z >= n_) return -1;
        return skyTop_[std::size_t(z) * std::size_t(n_) + std::size_t(x)];
    }

    // Cells written by the most recent block_changed(), and since construction.
    // The test uses these to prove the update is not O(volume).
    [[nodiscard]] std::uint64_t last_nodes()  const { return lastNodes_; }
    [[nodiscard]] std::uint64_t total_nodes() const { return totalNodes_; }

    // Packed nibbles, for byte-for-byte comparison against a fresh rebuild.
    [[nodiscard]] const std::vector<std::uint8_t>& raw() const { return light_; }

    // ── the two entry points ────────────────────────────────────────────────

    // Full recompute. O(volume); use at startup, or after generate().
    void rebuild() {
        resize();
        nodes_ = 0;

        // Sky: every column's ceiling first, because the seeding below needs the
        // neighbours' ceilings and not just its own.
        for (int z = 0; z < n_; ++z)
            for (int x = 0; x < n_; ++x) refresh_sky_top(x, z);

        for (int z = 0; z < n_; ++z)
            for (int x = 0; x < n_; ++x) {
                const int top = sky_top(x, z);
                for (int y = h_ - 1; y > top; --y) put(CSky, w_->idx(x, y, z), kLightMax);

                // Seed the flood only where the free-falling column can actually
                // do something. A 15 whose four horizontal neighbours are also
                // 15, and whose downstairs neighbour is also 15, cannot raise
                // anything — so only the stretch from this column's floor up to
                // one above the tallest neighbouring ceiling is worth queueing.
                // Without this the queue for a 96^2 map is ~600k entries of
                // which ~99% are no-ops.
                const int lo = top + 1;
                if (lo > h_ - 1) continue;
                int hi = lo;
                for (int d = 0; d < 4; ++d) {
                    const int nx = x + kDX[d], nz = z + kDZ[d];
                    if (nx < 0 || nz < 0 || nx >= n_ || nz >= n_) continue;
                    hi = std::max(hi, sky_top(nx, nz) + 1);
                }
                hi = std::min(hi, h_ - 1);
                for (int y = lo; y <= hi; ++y) add_.push_back(pack(x, y, z));
            }
        propagate(CSky);

        // Block light: every emitter is a seed.
        for (int y = 0; y < h_; ++y)
            for (int z = 0; z < n_; ++z)
                for (int x = 0; x < n_; ++x) {
                    const int e = light_emission(w_->at(x, y, z));
                    if (e <= 0) continue;
                    put(CBlock, w_->idx(x, y, z), e);
                    add_.push_back(pack(x, y, z));
                }
        propagate(CBlock);

        lastNodes_ = nodes_;
        totalNodes_ += nodes_;
    }

    // One block changed. The world must ALREADY hold the new block; the engine
    // needs no memory of the old one, because the old *light* is still in its
    // own array and that is the only history the algorithm uses.
    void block_changed(int x, int y, int z) {
        if (!w_->inside(x, y, z)) return;
        nodes_ = 0;
        const std::size_t i = w_->idx(x, y, z);
        const int b = w_->at(x, y, z);

        // ── block light ──
        {
            const int cur = get(CBlock, i);
            if (cur > 0) {
                // Unconditional: whether the cell got dimmer (source destroyed),
                // opaquer (light must stop here) or brighter, erasing first and
                // refilling after is always right, and the erase is bounded by
                // 15 steps. Trying to skip it on "the new emitter is at least as
                // bright" is a correctness trap, because a cell can also lose
                // light by becoming opaque while its emission goes up.
                put(CBlock, i, 0);
                rem_.push_back(RemNode{pack(x, y, z), std::uint8_t(cur)});
                remove(CBlock);
            }
            const int emit = light_emission(b);
            if (emit > get(CBlock, i)) {
                put(CBlock, i, emit);
                add_.push_back(pack(x, y, z));
                ++nodes_;
            }
            // If the cell just became transparent, its neighbours' light has
            // somewhere new to go. Six seeds is cheaper than deciding whether
            // it was needed.
            seed_neighbours(CBlock, x, y, z);
            propagate(CBlock);
        }

        // ── sky light ──
        {
            refresh_sky_top(x, z);

            const int cur = get(CSky, i);
            if (cur > 0) {
                put(CSky, i, 0);
                rem_.push_back(RemNode{pack(x, y, z), std::uint8_t(cur)});
                remove(CSky);   // carries the DOWN/15 column rule
            }

            // Restore the free-falling column. Only cells at or below the change
            // can have moved — a block placed at y cannot alter the sky above
            // itself — so this walk is bounded by the depth opened up, not by h.
            const int top = sky_top(x, z);
            for (int yy = std::min(y, h_ - 1); yy > top; --yy) {
                const std::size_t ii = w_->idx(x, yy, z);
                if (get(CSky, ii) == kLightMax) continue;
                put(CSky, ii, kLightMax);
                add_.push_back(pack(x, yy, z));
                ++nodes_;
            }

            seed_neighbours(CSky, x, y, z);
            propagate(CSky);
        }

        lastNodes_ = nodes_;
        totalNodes_ += nodes_;
    }

private:
    enum Chan { CBlock = 0, CSky = 1 };

    // 0..3 are the horizontal steps, 4 is up, 5 is down. kDown must stay last:
    // both the sky free-fall rule and the sky removal rule test for it by index.
    static constexpr int kDX[6] = { 1, -1,  0,  0,  0,  0 };
    static constexpr int kDY[6] = { 0,  0,  0,  0,  1, -1 };
    static constexpr int kDZ[6] = { 0,  0,  1, -1,  0,  0 };
    static constexpr int kDown  = 5;

    struct RemNode { std::uint32_t p; std::uint8_t v; };

    // Coordinates packed into 30 bits, 10 each. The alternative — carrying the
    // linear index — costs two integer divisions per dequeue to get x,y,z back,
    // and the queues are the hot loop. Good for worlds up to 1024 on a side and
    // 1024 tall; this project's largest is 256x128x256.
    [[nodiscard]] static std::uint32_t pack(int x, int y, int z) {
        return std::uint32_t(x) | (std::uint32_t(z) << 10) | (std::uint32_t(y) << 20);
    }

    [[nodiscard]] int get(Chan c, std::size_t i) const {
        return c == CSky ? (light_[i] >> 4) : (light_[i] & 0x0F);
    }
    void put(Chan c, std::size_t i, int v) {
        light_[i] = c == CSky ? std::uint8_t((light_[i] & 0x0F) | (v << 4))
                              : std::uint8_t((light_[i] & 0xF0) |  v);
    }

    void refresh_sky_top(int x, int z) {
        if (x < 0 || z < 0 || x >= n_ || z >= n_) return;
        int top = -1;
        for (int y = h_ - 1; y >= 0; --y)
            if (light_opacity(w_->at(x, y, z)) > 0) { top = y; break; }
        skyTop_[std::size_t(z) * std::size_t(n_) + std::size_t(x)] = std::int16_t(top);
    }

    void seed_neighbours(Chan c, int x, int y, int z) {
        for (int d = 0; d < 6; ++d) {
            const int nx = x + kDX[d], ny = y + kDY[d], nz = z + kDZ[d];
            if (!w_->inside(nx, ny, nz)) continue;
            if (get(c, w_->idx(nx, ny, nz)) > 0) add_.push_back(pack(nx, ny, nz));
        }
    }

    // The increase queue. Standard BFS: a cell is enqueued when its value rises,
    // and a cell whose value is already at least what we offer is skipped, which
    // is what terminates it.
    void propagate(Chan c) {
        while (addHead_ < add_.size()) {
            const std::uint32_t p = add_[addHead_++];
            const int x = int(p & 1023u), z = int((p >> 10) & 1023u), y = int((p >> 20) & 1023u);
            const int lv = get(c, w_->idx(x, y, z));
            if (lv <= 1) continue;      // 1 - max(1,opacity) is 0 in every case
            for (int d = 0; d < 6; ++d) {
                const int nx = x + kDX[d], ny = y + kDY[d], nz = z + kDZ[d];
                if (!w_->inside(nx, ny, nz)) continue;
                const int op = light_opacity(w_->at(nx, ny, nz));
                // [W-LIGHT], rule 3 in the header, verbatim as a formula.
                const int nv = (c == CSky && d == kDown && lv == kLightMax && op == 0)
                             ? kLightMax
                             : lv - std::max(1, op);
                if (nv <= 0) continue;
                const std::size_t ni = w_->idx(nx, ny, nz);
                if (get(c, ni) >= nv) continue;
                put(c, ni, nv);
                add_.push_back(pack(nx, ny, nz));
                ++nodes_;
            }
        }
        add_.clear();
        addHead_ = 0;
    }

    // The decrease queue [SOA]. Every node carries the level it is erasing.
    void remove(Chan c) {
        while (remHead_ < rem_.size()) {
            const RemNode rn = rem_[remHead_++];
            const int x = int(rn.p & 1023u), z = int((rn.p >> 10) & 1023u),
                      y = int((rn.p >> 20) & 1023u);
            const int lv = int(rn.v);
            for (int d = 0; d < 6; ++d) {
                const int nx = x + kDX[d], ny = y + kDY[d], nz = z + kDZ[d];
                if (!w_->inside(nx, ny, nz)) continue;
                const std::size_t ni = w_->idx(nx, ny, nz);
                const int nl = get(c, ni);
                if (nl == 0) continue;
                // The column rule. Without the second clause a roof laid over an
                // open shaft leaves the whole shaft at 15 forever, because
                // 15 < 15 is false and the shaft is misfiled as a survivor.
                const bool dimmer = nl < lv;
                const bool freefall = (c == CSky && d == kDown
                                    && lv == kLightMax && nl == kLightMax);
                if (dimmer || freefall) {
                    put(c, ni, 0);
                    rem_.push_back(RemNode{pack(nx, ny, nz), std::uint8_t(nl)});
                    ++nodes_;
                    // A cell the wave darkens may be a LIGHT SOURCE itself, and
                    // zeroing it without re-seeding puts the source out.
                    //
                    // Reachable, not theoretical: this world generates lava at
                    // Y1-10 and lava is in the edit palette, so a torch beside
                    // lava that is then mined went dark and STAYED dark, with
                    // the torch still standing there. The test never caught it
                    // because its soak did 600 random edits over 1.2 million
                    // cells and compared once at the end — the odds of a torch
                    // landing next to a lava and that lava then being edited are
                    // effectively nil. A random soak is not coverage; it is a
                    // lottery ticket against the bug you did not think of.
                    if (c == CBlock) {
                        const int em = light_emission(w_->at(nx, ny, nz));
                        if (em > 0) {
                            put(c, ni, std::uint8_t(em));
                            add_.push_back(pack(nx, ny, nz));
                        }
                    }
                } else {
                    // At least as bright as the wave being erased, so it was lit
                    // by something else: a survivor, and therefore a refill seed.
                    add_.push_back(pack(nx, ny, nz));
                }
            }
        }
        rem_.clear();
        remHead_ = 0;
    }

    const BlockWorld* w_ = nullptr;
    int n_ = 0, h_ = 0;

    std::vector<std::uint8_t> light_;    // hi nibble sky, lo nibble block
    std::vector<std::int16_t> skyTop_;   // per column, highest opacity>0 block

    // Kept as members, not locals: an update allocates nothing after warm-up.
    std::vector<std::uint32_t> add_;
    std::vector<RemNode>       rem_;
    std::size_t addHead_ = 0, remHead_ = 0;

    std::uint64_t nodes_ = 0, lastNodes_ = 0, totalNodes_ = 0;
};

} // namespace bench
