// fluids.hpp — fluid flow, fluid mixing, and gravity blocks over BlockWorld.
//
// This is Minecraft's fluid algorithm, not a fluid algorithm that looks like
// Minecraft's. Every number below is Java Edition's and is cited where it is
// declared. Where the wiki gives a rule as a procedure ("assign a weight of
// 1000 to every direction, then...") the procedure is implemented, not an
// approximation of the shape it produces.
//
// ── THE TWO NUMBERING CONVENTIONS, because they will trip you up ────────────
//
// The game has two, and they run in opposite directions:
//
//   LEVEL  0..7, what the block state and the wiki use. 0 is a full/source
//          block, 7 is the thinnest flowing block, 8 means "falling".
//   AMOUNT 1..8, what FluidState::getAmount() uses inside the fluid code.
//          8 is full, 1 is thinnest.  amount = 8 - level.
//
// minecraft.wiki, Water § Block states: "Values from 1 to 7 are reversed
// compared to fluidstate level values." This file STORES level (the wiki's
// convention, so that level(x,y,z) means what a reader expects) and converts to
// amount where the algorithm needs it, because the arithmetic that decides how
// far a fluid reaches is written in amounts.
//
// A FALLING block is level 0 (amount 8, full) with a separate falling flag. It
// is full but it is not a source: it does not seed infinite water and it
// disappears the moment the fluid above it does. That is why water poured off a
// cliff spreads a full seven blocks from the foot of the fall — the bottom of
// the column is a full-strength block, so its neighbours start at level 1
// exactly as a source's would.
//
// ── HOW FAR THINGS GO, AND WHY ──────────────────────────────────────────────
//
// Horizontal reach is not a constant in the game; it falls out of the level
// increase per block ("dropOff"). A block spreads sideways at amount
// (own amount - dropOff) and stops when that hits 0:
//
//     reach = (8 - 1) / dropOff        blocks of flowing fluid from a source
//
//   water         dropOff 1 -> amounts 7,6,5,4,3,2,1 -> 7 blocks
//   lava (OW/End) dropOff 2 -> amounts 6,4,2         -> 3 blocks
//   lava (Nether) dropOff 1 -> amounts 7..1          -> 7 blocks
//
// which is exactly what minecraft.wiki publishes for each: "7 blocks
// horizontally from a source block on a flat surface" (Water); "In the
// Overworld and the End, lava travels 3 blocks in any horizontal direction from
// a source block" and "In the Nether, lava travels 7 blocks horizontally"
// (Lava). The reach is DERIVED here rather than hard-coded, and the test
// asserts the derivation reproduces all three published numbers.
//
// ── PERFORMANCE, which is the whole reason this file has a queue ────────────
//
// The world is 96x128x96 (1.18M cells) up to 256x128x256 (8.4M cells). A scan
// per tick is dead on arrival. Minecraft does not scan either: a fluid cell is
// only visited when something scheduled it, and only a block update schedules
// one. So this file carries the same two schedulers the game has — block tile
// ticks, then fluid tile ticks — as delay-bucketed ring queues, deduplicated
// per (cell, kind).
//
//   per tick    O(due updates) + O(live falling entities). Nothing scans.
//   per fluid update   O(1) block writes plus the slope search, which visits at
//       most 4 * (3 + 3^2 + 3^3 + 3^4) = 480 cells for water (branching 3 after
//       the backtrack is excluded, depth capped at slopeFind) and 4 * (3 + 3^2)
//       = 48 for Overworld lava, at up to two world reads each. A constant, but
//       NOT a small one: the measured figure on an open plain is ~635 reads per
//       fluid update, so the test prints it rather than trusting the bound.
//   memory      one byte per cell, allocated once: 1.2 MB at 96 wide, 8.4 MB at
//       256, the same order as the block array itself.
//
// The only O(volume) call in the file is rebuild(), which is setup, not a tick.
// tick() never touches a cell nobody asked about.

#pragma once

#include "blockworld.hpp"
#include "../rng.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bench {

// ── published constants, Java Edition ───────────────────────────────────────

enum class Dimension { Overworld, Nether };

// The four numbers that define a fluid's behaviour in FlowingFluid.
struct FluidRule {
    int  tickDelay;    // scheduled-tick delay between spread steps (getTickDelay)
    int  dropOff;      // level increase per horizontal block       (getDropOff)
    int  slopeFind;    // how far ahead the drop search may look    (getSlopeFindDistance),
                       //   counted from the cell the fluid wants to flow INTO
    bool sourceConv;   // can two adjacent sources create a third   (canConvertToSource)
};

// WATER, Java Edition.
//   tickDelay 5   minecraft.wiki, Water: "Water spreads at a rate of 1 block
//                 every 5 game ticks, or 4 blocks per second."
//   dropOff   1   minecraft.wiki, Fluid: "Each flowing block increases the
//                 level by 1"; gives the published 7-block reach.
//   slopeFind 4   minecraft.wiki, Water: "for every adjacent block it can flow
//                 into it tries to find a way down that is reachable in four or
//                 fewer blocks from the block it wants to flow to." The same
//                 fact stated from the source block's point of view on
//                 minecraft.wiki, Fluid: "The area checked is up to 5 blocks
//                 away for water" — one step to reach the neighbour, four more
//                 from there. Both numbers are asserted in the test.
//   sourceConv    true. Game rule waterSourceConversion, default true.
inline constexpr FluidRule kWater{5, 1, 4, true};

// LAVA in the Overworld and the End, Java Edition.
//   tickDelay 30  minecraft.wiki, Lava: "lava flows far more slowly than water
//                 (1 block every 30 game ticks, or 1.5 seconds)".
//   dropOff   2   minecraft.wiki, Fluid: "Overworld/End lava stops at level 6",
//                 i.e. levels 2,4,6 — a level increase of 2 per block. Gives
//                 the published 3-block reach.
//   slopeFind 2   minecraft.wiki, Fluid: the drop search covers "up to 3 blocks
//                 away for lava elsewhere" — one step plus two more.
//   sourceConv    false. Game rule lavaSourceConversion, default false.
inline constexpr FluidRule kLavaOverworld{30, 2, 2, false};

// LAVA in the Nether, Java Edition. Carried because it is the clean control for
// the reach derivation: same code, dropOff 1, and 7 must come out the far end.
//   tickDelay 10  minecraft.wiki, Lava: "spreads 1 block every 10 game ticks,
//                 or 2 blocks per second".
//   dropOff   1   gives the published "In the Nether, lava travels 7 blocks".
//   slopeFind 4   minecraft.wiki, Fluid: "up to 5 blocks away for water or lava
//                 in the Nether".
inline constexpr FluidRule kLavaNether{10, 1, 4, false};

// Horizontal reach in blocks of flowing fluid, from the level arithmetic
// itself. See the header comment: a block spreads at (amount - dropOff) and
// stops at 0, so the last non-empty step is (8-1)/dropOff.
[[nodiscard]] inline constexpr int fluid_reach(const FluidRule& r) {
    return (8 - 1) / r.dropOff;
}

// The furthest a fluid's drop search can see, measured from the SOURCE block —
// one step into the neighbour, then slopeFind more. This is the "up to 5 blocks
// away" / "up to 3 blocks away" phrasing on minecraft.wiki, Fluid.
[[nodiscard]] inline constexpr int drop_search_range(const FluidRule& r) {
    return r.slopeFind + 1;
}

// GRAVITY BLOCKS, Java Edition.
//
// kGravityDelay 2 — FallingBlock schedules a tile tick and only then spawns the
//   entity. Technical Minecraft Wiki (techmcdocs.github.io, Tile Ticks) delay
//   table: "Sand, Anvil, Concrete powder" have a delay of 2 game ticks.
//   Cross-checked against minecraft.wiki, Sand: "Sand and gravel take about
//   0.45 seconds to fall one meter" = 9 game ticks, and the entity integration
//   below takes exactly 7 ticks to cover the first metre. 2 + 7 = 9. The test
//   measures the total and asserts 9, which is the only independent check
//   available on the delay.
inline constexpr int kGravityDelay = 2;

// Falling-block entity motion, Java Edition. minecraft.wiki, Falling Block:
// gravity "-0.04 m/tick2", vertical drag "0.98". minecraft.wiki, Entity gives
// the per-tick order for falling blocks and TNT as "Acceleration, Position,
// Drag", so each tick is:  v -= 0.04 ;  y += v ;  v *= 0.98.
inline constexpr double kFallGravity = 0.04;
inline constexpr double kFallDrag    = 0.98;
// minecraft.wiki, Falling Block: "a falling block that has existed for more
// than 600 ticks (30 seconds) destroys itself and drops as an item."
inline constexpr int kFallDespawn = 600;

// ── block predicates ───────────────────────────────────────────────────────

[[nodiscard]] inline bool is_fluid(int b) { return b == Water || b == Lava; }

// FlowingFluid::canHoldFluid is, once the waterloggable special cases are
// stripped, "!state.blocksMotion()". This world has no waterlogging and no
// doors or signs, so the non-colliding blocks are exactly air, torches and
// crops — and a fluid destroys the latter two on the way through, which is the
// behaviour anyone who has flooded a wheat farm expects.
[[nodiscard]] inline bool fluid_replaceable(int b) {
    return b == Air || b == Torch || b == Wheat;
}

// FallingBlock::isFree — "state.isAir() || state.is(BlockTags.FIRE) ||
// state.liquid() || state.canBeReplaced()". There is no fire in this block set,
// and nothing here is canBeReplaced (torches and crops are not), so: air and
// the two fluids. This is why sand dropped in the sea keeps going.
[[nodiscard]] inline bool gravity_free(int b) {
    return b == Air || b == Water || b == Lava;
}

[[nodiscard]] inline bool is_gravity_block(int b) { return b == Sand || b == Gravel; }

// ── the simulator ──────────────────────────────────────────────────────────

class Fluids {
public:
    // A falling block in flight. Real entity, real physics — not a "teleport
    // the block down" fake, because the published 0.45 s per metre is a
    // statement about the integration above and there is no way to assert it
    // without doing the integration.
    struct FallingBlockEntity {
        int          x = 0, z = 0;
        double       y = 0.0;      // continuous; the block occupies [y, y+1)
        double       vy = 0.0;
        std::uint8_t block = Air;
        int          age = 0;
    };

    struct Stats {
        std::uint64_t fluidUpdates    = 0;  // fluid tile ticks executed
        std::uint64_t blockUpdates    = 0;  // gravity tile ticks executed
        std::uint64_t cellProbes      = 0;  // world reads made by the slope search
        std::uint64_t spreads         = 0;  // fluid blocks actually written
        std::uint64_t sourcesMade     = 0;  // infinite-source conversions
        std::uint64_t obsidian        = 0;
        std::uint64_t cobble          = 0;
        std::uint64_t stone           = 0;
        std::uint64_t entitiesSpawned = 0;
        std::uint64_t entitiesLanded  = 0;
        std::uint64_t entitiesLost    = 0;  // despawned, blocked, or off the bottom
        std::size_t   peakPending     = 0;
    };

    Fluids(BlockWorld& w, Dimension dim = Dimension::Overworld,
           std::uint64_t seed = 0x5EEDF10D1DULL)
        : world_(&w), dim_(dim), rng_(seed) {
        state_.assign(w.volume(), 0);
    }

    // ── setup ──────────────────────────────────────────────────────────────

    // Adopt whatever fluid the world generator left behind. Every existing
    // fluid block becomes a SOURCE, which is what world generation produces —
    // an ocean is sources all the way down, and a settled ocean must not tick.
    // O(volume), called once. This is the only whole-volume pass in the file.
    void rebuild() {
        state_.assign(world_->volume(), 0);
        clear_queues();
        entities_.clear();
        const int n = world_->n(), h = world_->height();
        for (int y = 0; y < h; ++y)
            for (int z = 0; z < n; ++z)
                for (int x = 0; x < n; ++x)
                    if (is_fluid(world_->at(x, y, z)))
                        state_[world_->idx(x, y, z)] = kHas;   // level 0, source
    }

    // Put a source block down and give it the update it would get from being
    // placed. LiquidBlock::onPlace runs the mixing check first, which is why a
    // bucket of water above a lava source makes obsidian with no tick elapsing.
    void place_source(int x, int y, int z, std::uint8_t fluid) {
        if (!world_->inside(x, y, z) || !is_fluid(fluid)) return;
        set_block(x, y, z, fluid);
        state_[world_->idx(x, y, z)] =
            std::uint8_t((state_[world_->idx(x, y, z)] & (kSchedF | kSchedB)) | kHas);
        on_place(x, y, z);
        notify_neighbours(x, y, z);
    }

    // Put a flowing block down at an explicit level. For scenario setup; normal
    // play only ever makes these by spreading.
    void place_flowing(int x, int y, int z, std::uint8_t fluid, int level, bool falling) {
        if (!world_->inside(x, y, z) || !is_fluid(fluid)) return;
        set_block(x, y, z, fluid);
        state_[world_->idx(x, y, z)] = std::uint8_t(
            (state_[world_->idx(x, y, z)] & (kSchedF | kSchedB)) | kHas
            | (falling ? kFalling : 0) | std::uint8_t(level & kLevelMask));
        on_place(x, y, z);
        notify_neighbours(x, y, z);
    }

    // Remove whatever is at a cell, as if mined, and let the world react.
    void break_block(int x, int y, int z) {
        if (!world_->inside(x, y, z)) return;
        clear_fluid(world_->idx(x, y, z));
        set_block(x, y, z, Air);
        notify_neighbours(x, y, z);
    }

    // Call after any edit made behind this class's back, so the cell and its
    // neighbours get the block update the game would have sent them.
    void block_changed(int x, int y, int z) {
        if (!world_->inside(x, y, z)) return;
        on_place(x, y, z);
        notify_neighbours(x, y, z);
    }

    // ── running ────────────────────────────────────────────────────────────

    // One game tick. Order follows the server loop: block tile ticks, then
    // fluid tile ticks, then entities. The two-scheduler ordering is not
    // cosmetic — sand that lands this tick must be a block before the water
    // next to it decides where to go. Technical Minecraft Wiki, Tile Ticks:
    // "There are two different schedulers that get processed after one another:
    // block tile ticks, then fluid tile ticks."
    void tick() {
        ++now_;
        // Entities that already existed when this tick began. Anything spawned
        // by the tile ticks below waits until next tick, as vanilla queues
        // newly added entities rather than ticking them mid-frame. Getting this
        // wrong shifts every fall time by one tick, which is exactly the
        // resolution the 0.45 s figure has.
        const std::size_t live = entities_.size();

        const std::size_t slot = std::size_t(now_ % kRing);

        work_.clear();
        work_.swap(ringBlock_[slot]);
        queued_ -= work_.size();
        for (const std::uint32_t c : work_) {
            state_[c] &= std::uint8_t(~kSchedB);
            tick_gravity(c);
        }

        work_.clear();
        work_.swap(ringFluid_[slot]);
        queued_ -= work_.size();
        for (const std::uint32_t c : work_) {
            state_[c] &= std::uint8_t(~kSchedF);
            tick_fluid(c);
        }

        step_entities(live);

        if (pending() > stats.peakPending) stats.peakPending = pending();
    }

    void run(int ticks) { for (int i = 0; i < ticks; ++i) tick(); }

    // Run until nothing is scheduled and nothing is in the air. Returns the
    // number of ticks it took, or -1 if it never settled — a fluid sim that
    // cannot come to rest is a bug, so the test asserts on this everywhere.
    int settle(int maxTicks = 200000) {
        for (int i = 0; i < maxTicks; ++i) {
            if (pending() == 0) return i;
            tick();
        }
        return pending() == 0 ? maxTicks : -1;
    }

    // ── reading ────────────────────────────────────────────────────────────

    [[nodiscard]] bool has_fluid(int x, int y, int z) const {
        if (!world_->inside(x, y, z)) return false;
        return (state_[world_->idx(x, y, z)] & kHas) != 0;
    }
    // 0 (full/source) .. 7 (thinnest). -1 if there is no fluid here.
    [[nodiscard]] int level(int x, int y, int z) const {
        if (!has_fluid(x, y, z)) return -1;
        return state_[world_->idx(x, y, z)] & kLevelMask;
    }
    // 1 (thinnest) .. 8 (full), the FluidState::getAmount() convention.
    [[nodiscard]] int amount(int x, int y, int z) const {
        const int l = level(x, y, z);
        return l < 0 ? 0 : 8 - l;
    }
    [[nodiscard]] bool falling(int x, int y, int z) const {
        if (!has_fluid(x, y, z)) return false;
        return (state_[world_->idx(x, y, z)] & kFalling) != 0;
    }
    [[nodiscard]] bool is_source(int x, int y, int z) const {
        if (!has_fluid(x, y, z)) return false;
        return (state_[world_->idx(x, y, z)] & (kLevelMask | kFalling)) == 0;
    }

    [[nodiscard]] std::size_t pending() const { return queued_ + entities_.size(); }
    [[nodiscard]] std::size_t queued() const { return queued_; }
    [[nodiscard]] const std::vector<FallingBlockEntity>& entities() const { return entities_; }
    [[nodiscard]] std::uint64_t now() const { return now_; }
    [[nodiscard]] Dimension dimension() const { return dim_; }

    [[nodiscard]] const FluidRule& rule(int fluid) const {
        if (fluid == Water) return kWater;
        return dim_ == Dimension::Nether ? kLavaNether : kLavaOverworld;
    }

    // Order-sensitive digest of everything this class owns, for the determinism
    // test. Only fluid-bearing cells and the entities are folded in, so the
    // number is not dominated by untouched stone.
    [[nodiscard]] std::uint64_t digest() const {
        std::uint64_t h = 0xCBF29CE484222325ull;
        auto mix = [&h](std::uint64_t v) { h ^= v; h *= 0x100000001B3ull; };
        for (std::size_t i = 0; i < state_.size(); ++i)
            if (state_[i] & kHas) { mix(i); mix(fluid_state_of(state_[i])); }
        for (const auto& e : entities_) {
            mix(std::uint64_t(e.x));
            mix(std::uint64_t(e.z));
            mix(std::uint64_t(std::int64_t(e.y * 4096.0)));
            mix(std::uint64_t(std::int64_t(e.vy * 4096.0)));
            mix(e.block);
        }
        return h;
    }

    Stats stats;

private:
    // ── state byte layout ──────────────────────────────────────────────────
    // bit 7    a fluid occupies this cell
    // bit 6    a block tile tick is scheduled here
    // bit 5    a fluid tile tick is scheduled here
    // bit 4    falling
    // bits 0-3 level, 0..7
    //
    // The two scheduled bits are the deduplication that keeps the queue finite:
    // the game's schedulers also hold at most one pending tick per (position,
    // type). Without it, every one of a cell's six neighbours enqueues it again
    // on every write, so the queue would grow with the SURFACE AREA of the
    // flood times the number of writes touching it rather than with the number
    // of distinct cells that have work to do. The test prints peak queue depth
    // next to the number of fluid cells so that ratio stays visible.
    static constexpr std::uint8_t kHas       = 0x80;
    static constexpr std::uint8_t kSchedB    = 0x40;
    static constexpr std::uint8_t kSchedF    = 0x20;
    static constexpr std::uint8_t kFalling   = 0x10;
    static constexpr std::uint8_t kLevelMask = 0x0F;

    // Longest delay anything can ask for is lava's 30 * 4 = 120 (spread_delay),
    // so 256 buckets can never alias onto the slot being drained.
    static constexpr std::size_t kRing = 256;

    static constexpr int kDx[4] = { 1, -1,  0,  0 };
    static constexpr int kDz[4] = { 0,  0,  1, -1 };
    static constexpr int opposite(int d) { return d ^ 1; }

    // ── queue ──────────────────────────────────────────────────────────────

    void clear_queues() {
        for (auto& s : ringFluid_) s.clear();
        for (auto& s : ringBlock_) s.clear();
        work_.clear();
        queued_ = 0;
    }

    void schedule_fluid(int x, int y, int z, int delay) {
        const std::size_t i = world_->idx(x, y, z);
        if (state_[i] & kSchedF) return;                 // already pending
        state_[i] |= kSchedF;
        ringFluid_[std::size_t((now_ + std::uint64_t(delay)) % kRing)]
            .push_back(std::uint32_t(i));
        ++queued_;
    }
    void schedule_block(int x, int y, int z, int delay) {
        const std::size_t i = world_->idx(x, y, z);
        if (state_[i] & kSchedB) return;
        state_[i] |= kSchedB;
        ringBlock_[std::size_t((now_ + std::uint64_t(delay)) % kRing)]
            .push_back(std::uint32_t(i));
        ++queued_;
    }

    void decode(std::uint32_t i, int& x, int& y, int& z) const {
        const std::uint32_t n = std::uint32_t(world_->n());
        x = int(i % n);
        const std::uint32_t t = i / n;
        z = int(t % n);
        y = int(t / n);
    }

    // ── world writes ───────────────────────────────────────────────────────

    // BlockWorld caches a surface height per column, and it is only stale if
    // solidity changed. Water and air are both non-solid, so the common case
    // (fluid moving) costs nothing; cobblestone forming in a river does pay the
    // O(height) refresh, which is the right trade at the rate that happens.
    void set_block(int x, int y, int z, std::uint8_t b) {
        const int old = world_->at(x, y, z);
        if (old == b) return;
        world_->set(x, y, z, b);
        if (block_solid(old) != block_solid(b)) world_->refresh_column(x, z);
    }

    void clear_fluid(std::size_t i) { state_[i] &= std::uint8_t(kSchedF | kSchedB); }

    // ── block updates ──────────────────────────────────────────────────────

    // LiquidBlock::onPlace / FallingBlock::onPlace / neighborChanged, for one
    // cell. All three do the same work at this level of detail.
    void on_place(int x, int y, int z) {
        const int b = world_->at(x, y, z);
        if (is_fluid(b)) {
            if (lava_contact_check(x, y, z))          // may convert the lava instead
                schedule_fluid(x, y, z, rule(b).tickDelay);
        } else if (is_gravity_block(b)) {
            schedule_block(x, y, z, kGravityDelay);
        }
    }

    // Level::updateNeighborsAt — the six face neighbours.
    //
    // This can recurse, through lava_contact_check: a lava block that converts
    // notifies its neighbours, one of which may itself be lava touching water.
    // The chain is bounded by the length of a water-adjacent lava run, because
    // a lava block with no water against it converts nothing and just schedules
    // — so this is a handful of frames deep, not a walk of the lava lake.
    void notify_neighbours(int x, int y, int z) {
        static constexpr int dx[6] = { 1, -1, 0,  0, 0,  0 };
        static constexpr int dy[6] = { 0,  0, 1, -1, 0,  0 };
        static constexpr int dz[6] = { 0,  0, 0,  0, 1, -1 };
        for (int i = 0; i < 6; ++i) {
            const int nx = x + dx[i], ny = y + dy[i], nz = z + dz[i];
            if (world_->inside(nx, ny, nz)) on_place(nx, ny, nz);
        }
    }

    // ── mixing ─────────────────────────────────────────────────────────────
    //
    // LiquidBlock::shouldSpreadLiquid. Returns false when the lava at this cell
    // converted and must therefore not go on to schedule a fluid tick.
    //
    // The vanilla loop walks POSSIBLE_FLOW_DIRECTIONS (down plus the four
    // horizontals) and probes the OPPOSITE of each, which is: the block ABOVE,
    // and the four horizontal neighbours. Down is deliberately not probed —
    // lava sitting on water is handled by the downward-spread path instead, and
    // that is the whole difference between obsidian and stone.
    //
    // minecraft.wiki, Fluid § Mixing (Java Edition):
    //   lava contacting water except downward -> "the lava turns into cobblestone"
    //   water contacting a lava source        -> "the lava turns into obsidian"
    bool lava_contact_check(int x, int y, int z) {
        if (world_->at(x, y, z) != Lava) return true;
        static constexpr int dx[5] = { 0, 1, -1, 0,  0 };
        static constexpr int dy[5] = { 1, 0,  0, 0,  0 };
        static constexpr int dz[5] = { 0, 0,  0, 1, -1 };
        for (int i = 0; i < 5; ++i) {
            if (world_->at(x + dx[i], y + dy[i], z + dz[i]) != Water) continue;
            const bool src = is_source(x, y, z);
            clear_fluid(world_->idx(x, y, z));
            set_block(x, y, z, std::uint8_t(src ? Obsidian : Cobble));
            if (src) ++stats.obsidian; else ++stats.cobble;
            notify_neighbours(x, y, z);
            return false;
        }
        return true;
    }

    // ── the fluid tick ─────────────────────────────────────────────────────

    // FlowingFluid::tick. A source never recomputes itself; everything else
    // recomputes, republishes if it changed, and then spreads.
    void tick_fluid(std::uint32_t cell) {
        int x, y, z;
        decode(cell, x, y, z);
        const int b = world_->at(x, y, z);
        if (!is_fluid(b)) return;                       // the block changed under us
        const std::uint8_t fluid = std::uint8_t(b);
        ++stats.fluidUpdates;

        std::uint8_t st = state_[cell];
        if (!state_is_source(st)) {
            const std::uint8_t next = new_liquid(x, y, z, fluid);
            if (!(next & kHas)) {
                // Dried up. setBlock(AIR, flag 3), which notifies neighbours.
                clear_fluid(cell);
                set_block(x, y, z, Air);
                notify_neighbours(x, y, z);
                return;                                 // spread() of nothing is nothing
            }
            if (fluid_state_of(next) != fluid_state_of(st)) {
                const int delay = spread_delay(fluid, st, next);
                // An infinite source is born HERE, in getNewLiquid, not in
                // spreadTo — nothing ever spreads a source block into a cell.
                if (state_is_source(next)) ++stats.sourcesMade;
                state_[cell] = std::uint8_t((state_[cell] & (kSchedF | kSchedB))
                                            | fluid_state_of(next));
                st = state_[cell];
                schedule_fluid(x, y, z, delay);
                notify_neighbours(x, y, z);
            }
        }
        spread(x, y, z, st, fluid);
    }

    // FlowingFluid::getNewLiquid — what this cell SHOULD be, given its
    // neighbours. Returns a state byte; kHas clear means "empty".
    [[nodiscard]] std::uint8_t new_liquid(int x, int y, int z, std::uint8_t fluid) const {
        int maxAmount = 0;
        int sources = 0;
        for (int d = 0; d < 4; ++d) {
            const int nx = x + kDx[d], nz = z + kDz[d];
            if (world_->at(nx, y, nz) != fluid) continue;
            const std::uint8_t s = state_[world_->idx(nx, y, nz)];
            if (!(s & kHas)) continue;
            if (state_is_source(s)) ++sources;
            maxAmount = std::max(maxAmount, 8 - (s & kLevelMask));
        }

        // Infinite source. minecraft.wiki, Fluid: a source is created when a
        // block "has at least two water sources next to any of its horizontal
        // faces and has a block that liquids cannot flow into below itself".
        // The code also accepts a source of the same fluid below, which is what
        // lets an ocean heal itself rather than only a stone-floored pool.
        if (rule(fluid).sourceConv && sources >= 2) {
            const int below = world_->at(x, y - 1, z);
            if (block_solid(below) || (below == fluid && is_source(x, y - 1, z)))
                return kHas;                             // level 0, not falling
        }

        // Anything with the same fluid directly above is a FULL FALLING block,
        // whatever its horizontal neighbours say. This single rule is what makes
        // a waterfall a one-block column that spreads seven blocks when it
        // lands, instead of a widening cone.
        if (world_->at(x, y + 1, z) == fluid && has_fluid(x, y + 1, z))
            return std::uint8_t(kHas | kFalling);        // level 0, falling

        const int a = maxAmount - rule(fluid).dropOff;
        if (a <= 0) return 0;                            // empty
        return std::uint8_t(kHas | std::uint8_t(8 - a)); // level = 8 - amount
    }

    // LavaFluid::getSpreadDelay. Lava that is getting DEEPER waits four times as
    // long, three times out of four — which is why a lava flow filling a
    // depression looks like it is stalling. This is the only randomness in the
    // file and it comes from the Rng handed to the constructor, so a seed
    // reproduces a flow exactly.
    [[nodiscard]] int spread_delay(std::uint8_t fluid, std::uint8_t cur, std::uint8_t next) {
        const int base = rule(fluid).tickDelay;
        if (fluid != Lava) return base;
        if (!(cur & kHas) || !(next & kHas)) return base;
        if ((cur & kFalling) || (next & kFalling)) return base;
        const int curA = 8 - (cur & kLevelMask), nextA = 8 - (next & kLevelMask);
        if (nextA > curA && (rng_.next() % 4u) != 0u) return base * 4;
        return base;
    }

    // FlowingFluid::spread. Down first, and down EXCLUDES sideways — that is
    // the rule that makes water fall straight rather than fan out on the way.
    void spread(int x, int y, int z, std::uint8_t st, std::uint8_t fluid) {
        if (!(st & kHas)) return;
        const int by = y - 1;
        if (can_spread_to(x, by, z, fluid, /*down=*/true)) {
            // getNewLiquid of the cell below would see this fluid above it, so
            // what lands there is a full falling block. Passing it explicitly
            // rather than recomputing keeps the two paths from drifting apart.
            spread_to(x, by, z, fluid, std::uint8_t(kHas | kFalling), /*down=*/true);
            // A cell with three or more source neighbours spreads sideways as
            // well as down, so a wide pool with a hole in it does not drain
            // through the hole and leave its own surface unchanged.
            if (source_neighbours(x, y, z, fluid) >= 3) spread_sides(x, y, z, st, fluid);
        } else if (state_is_source(st) || !is_fluid_hole(x, by, z, fluid)) {
            spread_sides(x, y, z, st, fluid);
        }
    }

    [[nodiscard]] int source_neighbours(int x, int y, int z, std::uint8_t fluid) const {
        int k = 0;
        for (int d = 0; d < 4; ++d) {
            const int nx = x + kDx[d], nz = z + kDz[d];
            if (world_->at(nx, y, nz) == fluid && is_source(nx, y, nz)) ++k;
        }
        return k;
    }

    // FlowingFluid::isWaterHole — can the fluid keep going down here, or is the
    // cell below already carrying it? Either way a flowing block above one does
    // not spread sideways, which is why a column falling through open water
    // stays a column instead of dyeing the whole ocean.
    [[nodiscard]] bool is_fluid_hole(int x, int y, int z, std::uint8_t fluid) const {
        const int b = world_->at(x, y, z);
        return b == fluid || fluid_replaceable(b);
    }

    // FlowingFluid::spreadToSides plus getSpread — the 1000-weight search.
    //
    // minecraft.wiki, Water: "When spreading horizontally, a weight is assigned
    // to every direction water can flow. For each direction, this weight is
    // initially set to 1000. Then, for every adjacent block it can flow into it
    // tries to find a way down that is reachable in four or fewer blocks from
    // the block it wants to flow to. When found, the flow weight for that
    // direction is set to the shortest path distance to the way down. Finally,
    // water spreads in the directions with the lowest flow weight."
    void spread_sides(int x, int y, int z, std::uint8_t st, std::uint8_t fluid) {
        int i = (8 - (st & kLevelMask)) - rule(fluid).dropOff;
        // A falling block spreads at amount 7 whatever the fluid's dropOff is
        // (FlowingFluid::spreadToSides). For water this is invisible: 8-1 is
        // already 7.
        //
        // For Overworld lava it looks like it should matter — 7 instead of 6,
        // so the foot of a lava fall would reach four blocks where a source on
        // flat ground reaches three. It does NOT, and this is worth knowing
        // before you go looking for the bug. The receiving cell is written at
        // amount 7, but the very next thing that happens to it is its own tick,
        // which runs getNewLiquid FIRST: its biggest neighbour is the full
        // falling block at amount 8, so it recomputes to 8-2 = 6 before it ever
        // spreads. The override is erased before it can propagate, and the
        // settled reach is 3 either way. Measured in test_fluids.cpp §3, where
        // the level-1 write and the level-2 correction are 30 ticks apart.
        if (st & kFalling) i = 7;
        if (i <= 0) return;

        int best = 1000;
        int chosen[4] = {0, 0, 0, 0};
        int nChosen = 0;
        for (int d = 0; d < 4; ++d) {
            const int tx = x + kDx[d], tz = z + kDz[d];
            if (!can_pass(tx, y, tz, fluid)) continue;
            int j;
            if (can_pass(tx, y - 1, tz, fluid)) j = 0;   // drops immediately: unbeatable
            else j = slope_distance(tx, y, tz, 1, opposite(d), fluid);
            if (j < best) { nChosen = 0; best = j; }
            if (j <= best) chosen[nChosen++] = d;
        }

        const std::uint8_t out = std::uint8_t(kHas | std::uint8_t(8 - i));  // level = 8 - amount
        for (int k = 0; k < nChosen; ++k) {
            const int tx = x + kDx[chosen[k]], tz = z + kDz[chosen[k]];
            if (can_spread_to(tx, y, tz, fluid, /*down=*/false))
                spread_to(tx, y, tz, fluid, out, /*down=*/false);
        }
    }

    // FlowingFluid::getSlopeDistance. `depth` is the number of horizontal steps
    // already taken FROM the cell the fluid wants to move into, so a hole
    // directly beyond that cell returns 1 and the deepest a water search can
    // return is slopeFind == 4 — a hole five blocks from the source block.
    // `exclude` blocks the immediate backtrack only, exactly as vanilla does;
    // longer loops are permitted and are terminated by the depth limit.
    [[nodiscard]] int slope_distance(int x, int y, int z, int depth, int exclude,
                                     std::uint8_t fluid) {
        int best = 1000;
        for (int d = 0; d < 4; ++d) {
            if (d == exclude) continue;
            const int tx = x + kDx[d], tz = z + kDz[d];
            ++stats.cellProbes;                          // counts world reads, one per can_pass
            if (!can_pass(tx, y, tz, fluid)) continue;
            ++stats.cellProbes;
            if (can_pass(tx, y - 1, tz, fluid)) return depth;
            if (depth < rule(fluid).slopeFind) {
                const int j = slope_distance(tx, y, tz, depth + 1, opposite(d), fluid);
                if (j < best) best = j;
            }
        }
        return best;
    }

    // FlowingFluid::canPassThrough, minus the waterlogging and door cases this
    // world does not have: the cell must be able to hold fluid and must not
    // already be a source of this fluid (a source is a wall to the search —
    // water does not path through its own pool looking for a cliff).
    //
    // DIVERGENCE, deliberate: vanilla's canHoldFluid is "!blocksMotion", which
    // is also true of the OTHER fluid, so vanilla will route a water search
    // through a lava block. Here the other fluid is opaque to the search,
    // because in this world contact converts it to rock in the same tick and a
    // path through it is not a path.
    [[nodiscard]] bool can_pass(int x, int y, int z, std::uint8_t fluid) const {
        const int b = world_->at(x, y, z);
        if (b == fluid) return !is_source(x, y, z);
        return fluid_replaceable(b);
    }

    // FlowingFluid::canSpreadTo, whose real gate is FluidState::canBeReplacedWith.
    // WaterFluid's is "direction == DOWN && !fluid.is(WATER)": water is never
    // replaced by water, and is only ever replaced downward by lava. So a fluid
    // never overwrites its own kind — upgrades happen in getNewLiquid when the
    // target itself ticks — and the only cross-fluid write is lava falling in.
    [[nodiscard]] bool can_spread_to(int x, int y, int z, std::uint8_t fluid, bool down) const {
        const int b = world_->at(x, y, z);
        if (b == fluid) return false;
        if (down && fluid == Lava && b == Water) return true;   // -> stone
        if (is_fluid(b)) return false;                          // contact rules handle it
        return fluid_replaceable(b);
    }

    // FlowingFluid::spreadTo, with LavaFluid's override on the downward case.
    //
    // minecraft.wiki, Fluid § Mixing: "lava flowing into water" -> "the water
    // turns into stone". Note which block changes: the WATER does, and the lava
    // does not advance into the cell.
    void spread_to(int x, int y, int z, std::uint8_t fluid, std::uint8_t newState, bool down) {
        if (down && fluid == Lava && world_->at(x, y, z) == Water) {
            clear_fluid(world_->idx(x, y, z));
            set_block(x, y, z, Stone);
            ++stats.stone;
            notify_neighbours(x, y, z);
            return;
        }
        const std::size_t i = world_->idx(x, y, z);
        set_block(x, y, z, fluid);
        state_[i] = std::uint8_t((state_[i] & (kSchedF | kSchedB)) | fluid_state_of(newState));
        ++stats.spreads;
        on_place(x, y, z);
        notify_neighbours(x, y, z);
    }

    // ── gravity ────────────────────────────────────────────────────────────

    // FallingBlock::tick — if the cell below is free, the block stops being a
    // block and becomes an entity.
    void tick_gravity(std::uint32_t cell) {
        int x, y, z;
        decode(cell, x, y, z);
        const int b = world_->at(x, y, z);
        if (!is_gravity_block(b)) return;
        ++stats.blockUpdates;
        if (y <= 0) return;                                  // bottom of the world
        if (!gravity_free(world_->at(x, y - 1, z))) return;

        clear_fluid(cell);
        set_block(x, y, z, Air);
        notify_neighbours(x, y, z);
        FallingBlockEntity e;
        e.x = x; e.z = z; e.y = double(y); e.block = std::uint8_t(b);
        entities_.push_back(e);
        ++stats.entitiesSpawned;
    }

    // Advance the `live` entities that existed before this tick's tile ticks.
    // Newcomers sit at the back of the vector and wait for the next tick.
    void step_entities(std::size_t live) {
        if (live == 0) return;
        std::size_t out = 0;
        for (std::size_t k = 0; k < entities_.size(); ++k) {
            FallingBlockEntity e = entities_[k];
            if (k < live && !advance_entity(e)) continue;    // landed, lost, or despawned
            entities_[out++] = e;
        }
        entities_.resize(out);
    }

    // One tick of falling-block physics. Returns false if the entity is gone.
    bool advance_entity(FallingBlockEntity& e) {
        if (++e.age > kFallDespawn) { ++stats.entitiesLost; return false; }

        e.vy -= kFallGravity;                 // Acceleration
        const double ny = e.y + e.vy;         // Position
        // The block occupies [y, y+1) and rests on the cell below its feet, so
        // it stops at the highest integer floor L it crosses this tick whose
        // supporting cell L-1 is not free.
        const int hi = int(std::floor(e.y));
        const int lo = int(std::ceil(ny));
        for (int L = hi; L >= lo; --L) {
            if (gravity_free(world_->at(e.x, L - 1, e.z))) continue;
            land(e, L);
            return false;
        }
        e.y = ny;
        e.vy *= kFallDrag;                    // Drag
        if (e.y < 0.0) { ++stats.entitiesLost; return false; }
        return true;
    }

    void land(FallingBlockEntity& e, int y) {
        // Vanilla drops the block as an item if the landing cell cannot take it.
        // Here the cell is normally free by construction (the entity fell
        // through it), but another entity can land in it first within the same
        // tick, so the branch is real rather than decorative.
        if (!gravity_free(world_->at(e.x, y, e.z))) { ++stats.entitiesLost; return; }
        clear_fluid(world_->idx(e.x, y, e.z));
        set_block(e.x, y, e.z, e.block);       // displaces any fluid: gravel fills a pond
        ++stats.entitiesLanded;
        on_place(e.x, y, e.z);
        notify_neighbours(e.x, y, e.z);
    }

    // ── small helpers ──────────────────────────────────────────────────────

    static bool state_is_source(std::uint8_t s) {
        return (s & kHas) != 0 && (s & (kLevelMask | kFalling)) == 0;
    }
    static std::uint8_t fluid_state_of(std::uint8_t s) {
        return std::uint8_t(s & (kHas | kFalling | kLevelMask));
    }

    BlockWorld*   world_;
    Dimension     dim_;
    Rng           rng_;
    std::uint64_t now_ = 0;

    std::vector<std::uint8_t>  state_;
    std::vector<std::uint32_t> ringFluid_[kRing];
    std::vector<std::uint32_t> ringBlock_[kRing];
    std::vector<std::uint32_t> work_;
    std::size_t                queued_ = 0;

    std::vector<FallingBlockEntity> entities_;
};

} // namespace bench
