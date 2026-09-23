// mobs.hpp — the mob table, the spawning rules, and a four-state brain.
//
// WHAT THIS IS. Eight mobs — zombie, skeleton, creeper, spider, cow, pig,
// sheep, chicken — with the numbers Minecraft actually publishes for them, the
// conditions under which the game is allowed to put one on the ground, the
// conditions under which it takes one away again, and enough of a brain that a
// mob will walk at you and hit you.
//
// EVERY CONSTANT BELOW CARRIES ITS SOURCE. Java Edition throughout. Where a
// number could not be sourced it says so at the point of use and is labelled a
// BENCH constant — a decision this file made, not a fact about the game. That
// distinction is the whole point: a table of half-remembered numbers would make
// every measurement taken against this file a statement about my memory.
//
// WHAT IS DELIBERATELY NOT HERE.
//
//   · PATHFINDING. Vanilla runs A* over a node graph in which each block type
//     carries a "malus" — water, lava, fire, open doors, rails and leaves all
//     cost differently, and a negative malus means "impassable". None of that
//     is here. `greedy_step` walks one step down the straight line to the
//     target, and when that step is blocked it slides along one axis and
//     remembers which way it went so it commits to a side instead of jittering
//     in front of the wall. It will get around a pillar and through a doorway.
//     It will NOT solve a maze, and it is not trying to. Said plainly rather
//     than left for a reader to assume the mobs navigate.
//
//   · A LIGHT ENGINE. This file does not compute light. It asks a `LightView`
//     two questions — "how bright is this cell" and "may a hostile spawn here"
//     — and whoever owns the light answers them. A caller that has a real
//     flood-filled engine (light.hpp's LightEngine) wraps it in a LightView and
//     this file is none the wiser; a caller that has none uses `LightField`,
//     the standalone approximation below.
//
//     `LightField` takes the maximum of (source level - taxicab distance) over
//     the sources near the query point, where vanilla floods breadth-first and
//     loses one level per step through a non-opaque block. In open air those
//     are the same number; a torch behind a wall lights through the wall here
//     and does not there. The error is one-sided — LightField sees MORE light
//     than the game does, so it spawns FEWER hostiles than the game would,
//     never more. It also has no way to LOSE a source: mine a torch out and it
//     still reports the torch. That is survivable for a test fixture and fatal
//     for a live world, which is exactly why the live world passes its engine
//     through LightView instead of keeping a second copy of the light.
//
//   · SKY LIGHT PROPAGATION — again, in the LightField path only. Sky light
//     there is 15 in a column with nothing solid above it and 0 otherwise, read
//     straight off BlockWorld's cached heightmap. Vanilla spills sky light
//     sideways under an overhang; that does not. One-sided in the same
//     direction for hostile spawning. A LightView over a real engine has none
//     of these caveats, and the sim uses one.
//
// COMPLEXITY, because the world runs to 256x128x256 and per-tick volume work is
// not allowed.
//
//   · MobWorld::tick        O(M), M = live mobs, and M is bounded by the mob
//                           cap (70 monsters + 10 animals per 289 spawnable
//                           chunks). Each mob does about ten O(1) block reads.
//                           Nothing in the tick touches the world's volume.
//   · raw_sky_light         O(1) — BlockWorld::surface() is a cached heightmap.
//   · LightView queries     whatever the implementation costs. LightField's are
//                           O(sources in a 48x48 column) — sources are bucketed
//                           into 16-wide cells in X and Z, and a query whose
//                           range is at most 15 only ever reads 3x3 of them.
//                           LightEngine's are an array read.
//   · MobWorld::spawn_cycle O(positions * (M + 3*packSize)) — the M is one cap
//                           recount per position. Independent of world size.
//   · LightField::rebuild   O(volume). The ONLY volume pass in the file. It is
//                           a setup call — after generation, or after a bulk
//                           edit — and must not be called per tick.
//                           `add_source` exists so incremental torch placement
//                           never needs the rescan.

#pragma once
#include "blockworld.hpp"
#include "../rng.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace bench {

// ── the mobs ────────────────────────────────────────────────────────────────

enum MobKind : std::uint8_t {
    MobZombie = 0, MobSkeleton, MobCreeper, MobSpider,   // hostile
    MobCow, MobPig, MobSheep, MobChicken,                // passive
    kMobKinds
};

// Vanilla's MobCategory. Only the two the bench needs are modelled; the game
// also has ambient, water_creature, water_ambient, underground_water_creature,
// axolotls and misc, and their caps are quoted in the mob_cap() comment.
enum MobCat : std::uint8_t { CatMonster = 0, CatCreature, kMobCats };

enum Difficulty : std::uint8_t { DiffPeaceful = 0, DiffEasy, DiffNormal, DiffHard };

// Items only mobs drop. Deliberately a separate enum from Block/Item in
// blockworld.hpp: rotten flesh and feathers are not blocks and not in that
// file's crafting graph, and widening someone else's enum to hold them would
// renumber their recipes.
enum MobItem : std::uint8_t {
    MiRottenFlesh = 0, MiBone, MiArrow, MiGunpowder, MiString, MiSpiderEye,
    MiBeef, MiLeather, MiPorkchop, MiMutton, MiWool, MiChickenRaw, MiFeather,
    kMobItems
};

[[nodiscard]] inline const char* mob_item_name(int i) {
    switch (i) {
        case MiRottenFlesh: return "rotten flesh";  case MiBone:      return "bone";
        case MiArrow:       return "arrow";         case MiGunpowder: return "gunpowder";
        case MiString:      return "string";        case MiSpiderEye: return "spider eye";
        case MiBeef:        return "raw beef";      case MiLeather:   return "leather";
        case MiPorkchop:    return "raw porkchop";  case MiMutton:    return "raw mutton";
        case MiWool:        return "wool";          case MiChickenRaw:return "raw chicken";
        case MiFeather:     return "feather";       default:          return "?";
    }
}

// A loot-table line. Vanilla writes these as a uniform count over a range; the
// percentages the wiki quotes alongside them (e.g. "0-2, 66.67%") are not a
// second roll, they are P(count > 0) for that same uniform. So there is exactly
// one random number per line here and the 66.67% falls out — test_mobs.cpp
// MEASURES it rather than asserting it as an input.
struct MobDropRule {
    MobItem      item;
    std::uint8_t lo, hi;      // inclusive, Looting 0
    bool         playerKillOnly;   // vanilla's `killed_by_player` loot condition
    // An INDEPENDENT chance that the drop happens at all, for the entries where
    // the published percentage is not simply P(count > 0) of the uniform range.
    // The spider's eye is the case that forced this: "0-1, 33.3%" cannot be a
    // uniform roll over {0,1}, because that is 50%. Encoding it as one was a
    // real error in this table and the test caught nothing, because it asserted
    // only that some eyes fell rather than at what rate.
    double       chance = 1.0;
};

struct MobType {
    const char* name;
    MobCat      cat;
    float       health;        // max_health attribute
    float       attack;        // melee attack_damage on Normal; 0 = no melee
    float       speed;         // movement_speed attribute
    int         xpLo, xpHi;    // XP orbs on a player kill, inclusive
    bool        burnsInDaylight;
    float       followRange;   // blocks at which it notices a player
    int         dropCount;
    std::array<MobDropRule, 3> drops;
};

// ── THE PUBLISHED TABLE ─────────────────────────────────────────────────────
//
// Sources, all minecraft.wiki, Java Edition, read 2026-08-14. Each mob's own
// article unless noted.
//
//   Zombie   20 HP, attack 3 on Normal ("Easy 2.5, Normal 3, Hard 4.5"),
//            speed 0.23, 5 XP, rotten flesh 0-2, burns in sunlight.
//   Skeleton 20 HP, 5 XP, bone 0-2 and arrow 0-2, burns in sunlight. Ranged —
//            see kSkeletonShootRange and skeleton_arrow_range below.
//   Creeper  20 HP, 5 XP, gunpowder 0-2, explosion "power of 3".
//   Spider   16 HP, attack "Easy 2, Normal 2, Hard 3", speed 0.3, 5 XP,
//            string 0-2 and spider eye 0-1 (player kill only).
//   Cow      10 HP, speed 0.2, 1-3 XP, raw beef 1-3 and leather 0-2.
//   Pig      10 HP, speed 0.25, 1-3 XP, raw porkchop 1-3.
//   Sheep     8 HP, speed 0.23, 1-3 XP, raw mutton 1-2 and wool 1.
//   Chicken   4 HP, speed 0.25, 1-3 XP, raw chicken 1 and feather 0-2.
//
// FOLLOW RANGE. Zombie 35 — the zombie article, "pursue the player on sight
// from 35 blocks away", "follow_range ... base value is 35". Skeleton 16 —
// "chase players ... they see within 16 blocks". Creeper and spider are given
// the generic 16 here because I could NOT find a per-mob figure for either;
// treat those two as UNVERIFIED.
//
// BURNS IN DAYLIGHT. Only undead. The Undead article: "The majority of undead
// mobs catch fire under sunlight, a property not found in any other mobs", and
// it lists zombie and skeleton as undead while stating explicitly that creepers
// and spiders are not.
//
// WHY THE SPIDER'S SPEED DISAGREES WITH ONE SOURCE. The spider article says
// 0.3. The Attribute article's movement-speed table read back as 0.25 on one
// fetch and 0.35 on another, and that same table gave skeleton max_health as 8,
// which is flatly wrong. That table does not survive extraction, so the
// per-mob articles win wherever they state a number.
//
// SKELETON AND CREEPER SPEED ARE THE WEAKEST NUMBERS IN THIS FILE. Neither
// article states one. The Attribute table gave skeleton 0.25 on all three reads
// and creeper 0.25 on two of three (0.3 on the third). 0.25 is used for both
// and both are UNVERIFIED. If you are about to publish a measurement that turns
// on creeper speed, source it first.
[[nodiscard]] inline const MobType& mob_type(int k) {
    auto D = [](MobItem it, int lo, int hi, bool pk = false, double chance = 1.0) {
        return MobDropRule{it, std::uint8_t(lo), std::uint8_t(hi), pk, chance};
    };
    static const MobDropRule none = D(MiRottenFlesh, 0, 0);
    static const std::array<MobType, kMobKinds + 1> table = {{
        {"zombie",   CatMonster,  20.f, 3.f, 0.23f, 5, 5, true,  35.f, 1,
            {D(MiRottenFlesh,0,2), none, none}},
        {"skeleton", CatMonster,  20.f, 0.f, 0.25f, 5, 5, true,  16.f, 2,
            {D(MiBone,0,2), D(MiArrow,0,2), none}},
        {"creeper",  CatMonster,  20.f, 0.f, 0.25f, 5, 5, false, 16.f, 1,
            {D(MiGunpowder,0,2), none, none}},
        {"spider",   CatMonster,  16.f, 2.f, 0.30f, 5, 5, false, 16.f, 2,
            // Spider eye is "0-1, 33.3%", which a uniform roll over {0,1}
            // cannot express — that is 50%. It is ONE eye at a one-in-three
            // chance, so the range is 1-1 and the third lives in the
            // probability. Writing it as 0-1 WITH the probability compounds the
            // two and gives 16.6%, which is how it read on the first attempt to
            // fix this: half the error, in the other direction.
            {D(MiString,0,2), D(MiSpiderEye,1,1,true,1.0/3.0), none}},
        {"cow",      CatCreature, 10.f, 0.f, 0.20f, 1, 3, false, 16.f, 2,
            {D(MiBeef,1,3), D(MiLeather,0,2), none}},
        {"pig",      CatCreature, 10.f, 0.f, 0.25f, 1, 3, false, 16.f, 1,
            {D(MiPorkchop,1,3), none, none}},
        {"sheep",    CatCreature,  8.f, 0.f, 0.23f, 1, 3, false, 16.f, 2,
            {D(MiMutton,1,2), D(MiWool,1,1), none}},
        {"chicken",  CatCreature,  4.f, 0.f, 0.25f, 1, 3, false, 16.f, 2,
            {D(MiChickenRaw,1,1), D(MiFeather,0,2), none}},
        {"?",        CatMonster,   1.f, 0.f, 0.10f, 0, 0, false, 16.f, 0,
            {none, none, none}},
    }};
    if (k < 0 || k >= kMobKinds) return table[kMobKinds];
    return table[std::size_t(k)];
}

[[nodiscard]] inline bool mob_hostile(int k) { return mob_type(k).cat == CatMonster; }
[[nodiscard]] inline const char* mob_name(int k) { return mob_type(k).name; }

// ── damage ──────────────────────────────────────────────────────────────────
//
// THE DIFFICULTY RULE IS A FORMULA, NOT A TABLE, so it is implemented as one.
// minecraft.wiki, Difficulty: "if a mob deals D damage on Normal difficulty,
// then it deals 1.5D on Hard difficulty and min(D, 0.5D+1) on Easy difficulty",
// and "Mobs deal no damage to players on Peaceful difficulty". It lives in the
// player's damage path, which is why it applies to ANY incoming damage —
// explosions included — and why the per-mob damage tables on the wiki are
// derivable from the Normal column alone. Zombie base 3 gives Easy min(3, 2.5)
// = 2.5 and Hard 4.5, exactly what the zombie article lists; spider base 2
// gives Easy min(2, 2) = 2 and Hard 3, again exactly the article. Those two
// agreements are the test that this is the right formula and not a coincidence.
[[nodiscard]] inline float damage_to_player(float d, Difficulty diff) {
    switch (diff) {
        case DiffPeaceful: return 0.0f;
        case DiffEasy:     return std::min(d, 0.5f * d + 1.0f);
        case DiffHard:     return d * 1.5f;
        case DiffNormal:
        default:           return d;
    }
}

// Creeper explosion. minecraft.wiki, Explosion:
//   "impact = (1 - distance/(2*power)) * exposure;
//    damage = (float)((impact*impact + impact)/2 * 7*(2*power) + 1)"
// The truncation to int is the game's own cast and is reproduced; dropping it
// would shift damage by up to a whole heart. Creeper power is 3 (charged is 6),
// giving a maximum of 43 at point blank with full exposure. 43, not 49 — the
// bigger creeper numbers quoted around the web are the Hard-difficulty figures,
// which are this value put through damage_to_player (43 * 1.5 = 64.5, and the
// creeper article does list 64.5 for Hard).
inline constexpr float kCreeperPower        = 3.0f;
inline constexpr float kChargedCreeperPower = 6.0f;

[[nodiscard]] inline float explosion_damage(float power, float distance, float exposure) {
    const float reach = 2.0f * power;
    if (distance >= reach || exposure <= 0.0f) return 0.0f;
    const float impact = (1.0f - distance / reach) * exposure;
    return float(int((impact * impact + impact) * 0.5f * 7.0f * reach + 1.0f));
}

// Creeper fuse, from the creeper article: "When within 3 blocks of a player, a
// creeper stops moving, hisses, flashes and expands", "1.5 seconds (30 ticks)"
// before detonation, and the player must move "7 blocks" away to cancel it.
inline constexpr int   kCreeperFuseTicks   = 30;
inline constexpr float kCreeperIgniteRange = 3.0f;
inline constexpr float kCreeperCancelRange = 7.0f;

// Skeleton ranged attack, from the skeleton article: "When within 15 blocks of
// a target, with a clear line of sight, a skeleton starts shooting arrows, once
// every 3 seconds on Easy and Normal difficulties or once every 2 seconds on
// Hard difficulty", with arrow damage Easy 2-4, Normal 3-5, Hard 4-8.
//
// NOTE those ranges ALREADY contain the difficulty effect — they come from the
// arrow's velocity and crit roll, not from the player's damage scaling. Putting
// them through damage_to_player would apply difficulty twice, so skeleton()
// does not. This is the one place in the file where the difficulty formula is
// deliberately skipped, and it is skipped for a reason.
inline constexpr float kSkeletonShootRange = 15.0f;

[[nodiscard]] inline int skeleton_shoot_interval(Difficulty d) {
    return d == DiffHard ? 40 : 60;                    // 2 s vs 3 s at 20 tps
}
inline void skeleton_arrow_range(Difficulty d, float& lo, float& hi) {
    switch (d) {
        case DiffPeaceful: lo = 0.f; hi = 0.f; break;
        case DiffEasy:     lo = 2.f; hi = 4.f; break;
        case DiffHard:     lo = 4.f; hi = 8.f; break;
        case DiffNormal:
        default:           lo = 3.f; hi = 5.f; break;
    }
}

// BENCH constants — NOT sourced, do not cite them as Minecraft.
//   kMeleeCooldown: vanilla's melee goal counts down a fixed interval between
//     swings. 20 ticks is a round second and is in the right place, but I could
//     not source the exact figure.
//   kMeleeReach: vanilla derives reach from the attacker's hitbox width. Not
//     sourced; 2.0 blocks puts a zombie in range when it is standing on you.
inline constexpr int   kMeleeCooldown = 20;
inline constexpr float kMeleeReach    = 2.0f;

// ── how fast a mob actually moves ───────────────────────────────────────────
//
// READ THIS BEFORE TRUSTING A SPEED MEASUREMENT. The wiki publishes
// movement_speed as an ATTRIBUTE, and the attribute-to-blocks-per-second
// conversion is not the same for a mob as for a player. The Walking article
// pairs the player's attribute of 0.1 with "4.317 blocks per second", a factor
// of 43.17; applying that factor to a spider's 0.3 gives 13 blocks per second,
// two and a half times a sprinting player, which is plainly not what a spider
// does. Mob navigation multiplies the attribute by a per-goal speed modifier
// before it reaches the movement code and I could not source that chain. The
// honest position: the attribute is a RELATIVE speed here and nothing more.
//
// The bench therefore reads the attribute as blocks per TICK. Zombie 0.23 gives
// 4.6 blocks/s, spider 0.3 gives 6.0, cow 0.2 gives 4.0, against a walking
// player's published 4.317. The ORDERING is the game's; the SCALE is this
// file's, and a claim like "a spider crosses 10 blocks in N ticks" is a claim
// about kSpeedToBlocksPerTick, not about Minecraft.
inline constexpr float kSpeedToBlocksPerTick = 1.0f;

[[nodiscard]] inline float mob_step(int kind) {
    return mob_type(kind).speed * kSpeedToBlocksPerTick;
}

// ── time of day and daylight burning ────────────────────────────────────────
//
// minecraft.wiki, Daylight cycle: a day is "24,000" ticks; daytime starts at
// "0 ticks (06:00:00.0)" and ends at "12000 ticks (18:00:00.0)", sunset runs to
// 13000, night to 23000, sunrise to 24000. The same table gives the burn window
// directly: undead "begin to burn" at tick 23460 and "no longer burn" at tick
// 12542, in clear weather.
inline constexpr int kDayLength      = 24000;
inline constexpr int kSunsetStart    = 12000;
inline constexpr int kNightStart     = 13000;
inline constexpr int kSunriseStart   = 23000;
inline constexpr int kUndeadBurnFrom = 23460;
inline constexpr int kUndeadBurnTo   = 12542;

[[nodiscard]] inline int wrap_time(int t) {
    t %= kDayLength;
    return t < 0 ? t + kDayLength : t;
}
[[nodiscard]] inline bool undead_burn_window(int timeOfDay) {
    const int t = wrap_time(timeOfDay);
    return t >= kUndeadBurnFrom || t < kUndeadBurnTo;
}

// Fire, from minecraft.wiki Fire: burning outside a fire block deals 1 HP "per
// second", and a mob leaving fire "will always burn for exactly 160 ticks".
inline constexpr int   kFireTicks       = 160;
inline constexpr int   kFireDamageEvery = 20;
inline constexpr float kFireDamage      = 1.0f;

// How much the game dims sky light at this time of day. Vanilla computes it
// from a cosine of the celestial angle; that formula is NOT reproduced here
// because I did not source it, so this is a LINEAR stand-in pinned to the four
// published boundaries above — 0 through the day, 11 through the night, ramping
// across sunset and sunrise. It is a divergence, and it moves the exact tick at
// which hostiles start spawning at the surface by a few hundred ticks. It moves
// nothing underground, where raw sky light is 0 and the darkening is irrelevant
// — which is where the great majority of hostile spawns happen anyway.
inline constexpr int kNightSkyDarken = 11;

[[nodiscard]] inline int sky_darken(int timeOfDay) {
    const int t = wrap_time(timeOfDay);
    if (t < kSunsetStart)  return 0;
    if (t < kNightStart)   return (kNightSkyDarken * (t - kSunsetStart))
                                / (kNightStart - kSunsetStart);
    if (t < kSunriseStart) return kNightSkyDarken;
    return kNightSkyDarken - (kNightSkyDarken * (t - kSunriseStart))
                           / (kDayLength - kSunriseStart);
}

// ── light ───────────────────────────────────────────────────────────────────

// Raw sky light: 15 where nothing solid stands above, 0 otherwise. O(1) —
// BlockWorld caches the heightmap. NOTE it is only correct where the caller has
// kept that cache honest: BlockWorld::set() does not refresh it, so code that
// builds a roof must call refresh_column() afterwards, exactly as generate()
// does for the whole map. Forgetting that is the single most likely way to get
// a wrong spawning answer out of this file.
[[nodiscard]] inline int raw_sky_light(const BlockWorld& w, int x, int y, int z) {
    if (!w.inside(x, y, z)) return 0;
    return (y > w.surface(x, z)) ? 15 : 0;
}
[[nodiscard]] inline int internal_sky_light(const BlockWorld& w, int x, int y, int z,
                                            int timeOfDay) {
    return std::max(0, raw_sky_light(w, x, y, z) - sky_darken(timeOfDay));
}

// The light thresholds, up here rather than down with the rest of the spawning
// rules because they are the CONTRACT of the interface below — an
// implementation of LightView is promising to answer against these numbers.
// minecraft.wiki, Mob spawning, Java Edition:
//   · Hostiles, 1.18 and later: "Block light level now must be 0 for many
//     hostile mobs to spawn (the sky light can still prevent these mobs from
//     spawning like before)", the sky condition being internal sky light 7 or
//     less. Before 1.18 the rule was a combined light level of 7 or less, so a
//     torch used to have to raise the level to 8 to stop spawns and now ANY
//     block light at all does. That change is the reason this file cares which
//     version it is modelling.
//   · Animals: "The spawning block must have a light level of 9 or greater".
inline constexpr int kMonsterMaxBlockLight = 0;    // 1.18+, overworld
inline constexpr int kMonsterMaxSkyLight   = 7;
inline constexpr int kAnimalMinLight       = 9;

// ── the only thing this file knows about light ──────────────────────────────
//
// TWO QUESTIONS. Everything mobs.hpp does with light is one of these: how
// bright is this cell (spider hostility, the animal spawn floor) and may a
// hostile spawn here (the monster spawn rule). Anything that can answer both is
// a light source as far as this file is concerned.
//
// WHY AN INTERFACE RATHER THAN A CONCRETE CLASS. This used to take `const
// LightField&`, so a caller that already owned a real light engine had to keep
// a SECOND light structure alongside it just to call in here — and the second
// one went stale, because only one of them was on the end of the world's
// mutation path. Two structures that are supposed to describe the same thing
// and do not is a bug that no amount of care at the call sites fixes. With the
// question asked through this interface there is one field, owned by whoever
// owns the world, and this file still does not include light.hpp.
//
// THE WORLD IS A PARAMETER, not a member, because half of the answer — the sky
// half — is a property of the column and not of the light source, and because
// LightField is usable before any world is bound to it (test fixtures do
// exactly that: reset(n), add a torch, ask).
class LightView {
public:
    virtual ~LightView() = default;

    // [W-LIGHT]'s internal light level: max(block light, internal sky light).
    // The number vanilla's spawn and mob-behaviour rules read, not the
    // displayed one.
    [[nodiscard]] virtual int internal_light(const BlockWorld& w, int x, int y, int z,
                                             int timeOfDay) const = 0;

    // The light half of the hostile spawn rule, Java 1.18+: block light 0 AND
    // internal sky light <= 7. Asked as one question rather than composed out
    // of two getters so an implementation that has a cheaper combined test (a
    // real engine does) can give it.
    [[nodiscard]] virtual bool hostile_light_ok(const BlockWorld& w, int x, int y, int z,
                                                int timeOfDay) const = 0;

protected:
    // Constructible and copyable only as a base — a `LightView` is always some
    // implementation, and copying one by value through this type would slice it.
    LightView() = default;
    LightView(const LightView&) = default;
    LightView& operator=(const LightView&) = default;
};

// Block light from point sources, bucketed 16 wide in X and Z. A source of
// level L reaches L blocks and L <= 15 < 16, so a query inside a cell can only
// be reached by sources in that cell or the eight around it.
//
// THE FALLBACK IMPLEMENTATION of LightView: what this file uses when nobody
// hands it a better one. Sources only ever go in — see the header note — so it
// is right for a fixture that builds a world and then asks about it, and wrong
// for a world that is still being dug.
class LightField final : public LightView {
public:
    // minecraft.wiki, Torch: "Torches give off a light level of 14."
    static constexpr int kTorchLight = 14;
    // Lava is the other emitter this world contains. 15 is the standard figure
    // and is NOT sourced here — flagged rather than silently trusted.
    static constexpr int kLavaLight = 15;
    static constexpr int kCell      = 16;

    void reset(int n) {
        cw_ = std::max(1, (n + kCell - 1) / kCell);
        cells_.assign(std::size_t(cw_) * std::size_t(cw_), {});
        count_ = 0;
    }

    void add_source(int x, int y, int z, int level) {
        if (cw_ <= 0 || level <= 0) return;
        const int cx = std::clamp(x / kCell, 0, cw_ - 1);
        const int cz = std::clamp(z / kCell, 0, cw_ - 1);
        cells_[std::size_t(cz) * std::size_t(cw_) + std::size_t(cx)].push_back(
            Src{std::int16_t(x), std::int16_t(y), std::int16_t(z), std::uint8_t(level)});
        ++count_;
    }

    // O(volume). Setup only — never call this from a tick. Incremental torch
    // placement should call add_source instead.
    void rebuild(const BlockWorld& w) {
        reset(w.n());
        for (int y = 0; y < w.height(); ++y)
            for (int z = 0; z < w.n(); ++z)
                for (int x = 0; x < w.n(); ++x) {
                    const std::uint8_t b = w.at(x, y, z);
                    if (b == Torch)     add_source(x, y, z, kTorchLight);
                    else if (b == Lava) add_source(x, y, z, kLavaLight);
                }
    }

    [[nodiscard]] int sources() const { return count_; }

    // Taxicab falloff, no occlusion. See the header note: this over-lights, so
    // it under-spawns hostiles, never over-spawns them.
    [[nodiscard]] int block_light(int x, int y, int z) const {
        if (cw_ <= 0) return 0;
        int best = 0;
        const int cx = std::clamp(x / kCell, 0, cw_ - 1);
        const int cz = std::clamp(z / kCell, 0, cw_ - 1);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const int ax = cx + dx, az = cz + dz;
                if (ax < 0 || az < 0 || ax >= cw_ || az >= cw_) continue;
                for (const Src& s : cells_[std::size_t(az) * std::size_t(cw_) + std::size_t(ax)]) {
                    const int d = std::abs(int(s.x) - x) + std::abs(int(s.y) - y)
                                + std::abs(int(s.z) - z);
                    best = std::max(best, int(s.level) - d);
                }
            }
        return std::max(0, best);
    }

    // ── LightView ───────────────────────────────────────────────────────────
    [[nodiscard]] int internal_light(const BlockWorld& w, int x, int y, int z,
                                     int timeOfDay) const override {
        return std::max(block_light(x, y, z), internal_sky_light(w, x, y, z, timeOfDay));
    }
    [[nodiscard]] bool hostile_light_ok(const BlockWorld& w, int x, int y, int z,
                                        int timeOfDay) const override {
        if (block_light(x, y, z) > kMonsterMaxBlockLight) return false;
        return internal_sky_light(w, x, y, z, timeOfDay) <= kMonsterMaxSkyLight;
    }

private:
    struct Src { std::int16_t x, y, z; std::uint8_t level; };
    std::vector<std::vector<Src>> cells_;
    int cw_ = 0;
    int count_ = 0;
};

// ── spawning rules ──────────────────────────────────────────────────────────
//
// minecraft.wiki, Mob spawning, Java Edition:
//   · "The mob's collision box must not intersect with a solid block", and the
//     block below "must have a solid, complete, top surface" — hence two free
//     blocks and a solid floor.
//   · "The mob's collision box must not collide with any liquid."
//   · Animals: on a grass block, with "at least two blocks of space above".
// The light halves of both rules, and the thresholds they quote, are up with
// LightView — that is the interface's contract and this section only composes
// it with the geometry.
//
// Two free blocks. `forSpawn` also rejects liquid, which movement does not: a
// mob may walk into water, it may not be spawned into it.
[[nodiscard]] inline bool body_clear(const BlockWorld& w, int x, int y, int z, bool forSpawn) {
    const std::uint8_t a = w.at(x, y, z), b = w.at(x, y + 1, z);
    if (block_solid(a) || block_solid(b)) return false;
    if (forSpawn && (a == Water || a == Lava || b == Water || b == Lava)) return false;
    return true;
}

// The floor. Solid with a complete top face — leaves are excluded because
// vanilla's leaf block admits only ocelots and parrots as valid spawns, and
// none of the eight mobs here is either.
[[nodiscard]] inline bool spawn_floor_ok(const BlockWorld& w, int x, int y, int z) {
    if (y <= 0) return false;
    const std::uint8_t f = w.at(x, y - 1, z);
    return block_solid(f) && f != Leaves;
}

[[nodiscard]] inline bool monster_light_ok(const BlockWorld& w, const LightView& lv,
                                           int x, int y, int z, int timeOfDay) {
    return lv.hostile_light_ok(w, x, y, z, timeOfDay);
}

[[nodiscard]] inline bool animal_light_ok(const BlockWorld& w, const LightView& lv,
                                          int x, int y, int z, int timeOfDay) {
    return lv.internal_light(w, x, y, z, timeOfDay) >= kAnimalMinLight;
}

// The whole rule for a category, at one block position.
[[nodiscard]] inline bool spawn_allowed(MobCat cat, const BlockWorld& w, const LightView& lv,
                                        int x, int y, int z, int timeOfDay) {
    if (!w.inside(x, y + 1, z))        return false;
    if (!spawn_floor_ok(w, x, y, z))   return false;
    if (!body_clear(w, x, y, z, true)) return false;
    if (cat == CatCreature) {
        // Animals want grass specifically, not merely any solid floor.
        if (w.at(x, y - 1, z) != Grass) return false;
        return animal_light_ok(w, lv, x, y, z, timeOfDay);
    }
    return monster_light_ok(w, lv, x, y, z, timeOfDay);
}

// ── the mob cap ─────────────────────────────────────────────────────────────
//
// minecraft.wiki, Mob spawning: "globalCap = mobCap x chunks / 289", where the
// chunks are "the total number of chunks within a 17x17 chunk square around any
// player" — and 17x17 IS 289, so one player at simulation distance 8 gets
// exactly the base cap and no arithmetic is visible. Base caps: monster 70,
// creature 10, ambient 15, water creature 5, water ambient 20. The division is
// integer, as in the game, so two players' worth of chunks does not give
// exactly twice the cap when the chunk count is not a multiple of 289.
inline constexpr int kSpawnChunkSpan  = 17;
inline constexpr int kMagicChunkCount = kSpawnChunkSpan * kSpawnChunkSpan;   // 289
inline constexpr int kMonsterCap      = 70;
inline constexpr int kCreatureCap     = 10;

[[nodiscard]] inline int mob_cap_base(MobCat c) {
    return c == CatMonster ? kMonsterCap : kCreatureCap;
}
[[nodiscard]] inline int mob_cap(MobCat c, int spawnableChunks) {
    return mob_cap_base(c) * spawnableChunks / kMagicChunkCount;
}

// Distances, minecraft.wiki Mob spawning and Mob:
//   · "There must be no players or the world spawn point within a 24 block
//     radius (spherical) of the spawning block."
//   · "spawns fail unless within a 128 radius block sphere around the player."
//   · "a mob will despawn immediately if there are no players within a distance
//     of 128 blocks", and "If it's not within 32 blocks of a player for more
//     than 30 seconds, there's a 1/800 chance each game tick it will despawn".
//     30 seconds is 600 ticks at 20 tps.
inline constexpr float kMinSpawnDistance  = 24.0f;
inline constexpr float kSpawnRadius       = 128.0f;
inline constexpr float kDespawnDistance   = 128.0f;
inline constexpr float kNoDespawnDistance = 32.0f;
inline constexpr int   kDespawnIdleTicks  = 600;
inline constexpr int   kDespawnOneIn      = 800;

// Pack spawning, from the vanilla loop the wiki describes: "There are a maximum
// of 3 pack spawn attempts per mob category" per chosen position, and members
// are displaced "by +-5 (triangular distribution) on the X and Z axes" — that
// is nextInt(6) - nextInt(6), applied CUMULATIVELY so a pack trails away from
// its first member rather than clustering on it. The initial pack size is
// ceil(random * 4); once a mob type has been chosen it is REPLACED by
// minCount + random(1 + maxCount - minCount) from the biome's spawn entry,
// which is why the ceil() value almost never survives.
//
// A pack size of 0 is reachable when random() returns exactly 0, in vanilla and
// here. It costs one wasted attempt. It is left in rather than clamped away,
// because clamping it would be a silent divergence from the game.
inline constexpr int kPackAttempts = 3;
inline constexpr int kPackSpread   = 6;    // the 6 in nextInt(6) - nextInt(6)

// Biome spawn entries, plains, Java Edition, from minecraft.wiki Plains. The
// real denominators are 520 for monsters and 46 for creatures; the difference
// is the mobs this file does not model (zombie villager, slime, enderman,
// witch; horse, donkey). Group size is 4 for all eight, min and max alike.
struct MobSpawnEntry { MobKind kind; int weight; int minCount, maxCount; };

[[nodiscard]] inline const std::array<MobSpawnEntry, 4>& plains_spawns(MobCat c) {
    static const std::array<MobSpawnEntry, 4> monsters = {{
        {MobCreeper, 100, 4, 4}, {MobSkeleton, 100, 4, 4},
        {MobSpider,  100, 4, 4}, {MobZombie,    95, 4, 4},
    }};
    static const std::array<MobSpawnEntry, 4> creatures = {{
        {MobSheep,    12, 4, 4}, {MobChicken,   10, 4, 4},
        {MobPig,      10, 4, 4}, {MobCow,        8, 4, 4},
    }};
    return c == CatMonster ? monsters : creatures;
}

[[nodiscard]] inline MobSpawnEntry pick_spawn_entry(MobCat c, Rng& rng) {
    const std::array<MobSpawnEntry, 4>& t = plains_spawns(c);
    int total = 0;
    for (const MobSpawnEntry& e : t) total += e.weight;
    int roll = int(rng.below(std::uint32_t(total)));
    for (const MobSpawnEntry& e : t) { roll -= e.weight; if (roll < 0) return e; }
    return t[0];
}

// ── a mob ───────────────────────────────────────────────────────────────────

enum MobState : std::uint8_t { StIdle = 0, StWander, StChase, StAttack, StFuse, kMobStates };

[[nodiscard]] inline const char* mob_state_name(int s) {
    switch (s) {
        case StIdle:   return "idle";    case StWander: return "wander";
        case StChase:  return "chase";   case StAttack: return "attack";
        case StFuse:   return "fuse";    default:       return "?";
    }
}

struct Mob {
    std::uint8_t kind  = MobZombie;
    std::uint8_t state = StIdle;
    bool  alive = true;
    float x = 0.f, y = 0.f, z = 0.f;
    float health = 0.f;
    int   stateTicks   = 0;
    int   idleTicks    = 0;    // vanilla's noActionTime
    int   attackCd     = 0;
    int   fuse         = -1;   // creeper only; -1 = not lit
    int   fireTicks    = 0;
    int   fireDamageCd = 0;    // see fire_tick: separate from fireTicks on purpose
    int   wanderTicks  = 0;
    float wanderX = 0.f, wanderZ = 0.f;
    std::int8_t slide = 1;     // which way it goes round an obstacle
};

struct SpawnReport {
    int positions = 0, packs = 0, spawned = 0;
    int rejectCap = 0, rejectDistance = 0, rejectRules = 0;
    int packSizeSum = 0;   // for the mean pack size
    int maxStepOffset = 0; // largest single-member displacement, must be <= 5
};

struct TickStats {
    float playerDamage = 0.f;
    int explosions = 0, despawned = 0, burning = 0, deaths = 0, attacks = 0;
};

// ── movement ────────────────────────────────────────────────────────────────
//
// GREEDY ONLY. See the header. Nothing below searches; every decision is local.

// Ground within four blocks below, so a mob will not stroll off a cliff. The
// four is a BENCH number: vanilla's node evaluator assigns a cost to drops
// rather than forbidding them, and I did not source its threshold.
inline constexpr int kMaxDropLookahead = 4;

[[nodiscard]] inline bool stand_ok(const BlockWorld& w, int x, int y, int z) {
    if (!w.inside(x, y, z) || !w.inside(x, y + 1, z)) return false;
    if (!body_clear(w, x, y, z, false)) return false;
    for (int d = 1; d <= kMaxDropLookahead; ++d)
        if (block_solid(w.at(x, y - d, z))) return true;
    return false;
}

// One attempt, with a one-block step up. The step up is what makes a hillside
// or a staircase walkable without a pathfinder; leaving it out is the easiest
// way to make these mobs look broken while every constant is still correct.
[[nodiscard]] inline bool try_move(const BlockWorld& w, Mob& m, float dx, float dz) {
    if (dx == 0.0f && dz == 0.0f) return false;
    const float nx = m.x + dx, nz = m.z + dz;
    const int bx = int(std::floor(nx)), bz = int(std::floor(nz));
    const int by = int(std::floor(m.y));
    if (stand_ok(w, bx, by, bz)) { m.x = nx; m.z = nz; return true; }
    if (stand_ok(w, bx, by + 1, bz) && !block_solid(w.at(bx, by + 2, bz))) {
        m.x = nx; m.z = nz; m.y = float(by + 1);
        return true;
    }
    return false;
}

inline void apply_gravity(const BlockWorld& w, Mob& m) {
    int by = int(std::floor(m.y));
    const int bx = int(std::floor(m.x)), bz = int(std::floor(m.z));
    int guard = kMaxDropLookahead + 1;
    while (guard-- > 0 && by > 1 && !block_solid(w.at(bx, by - 1, bz))) --by;
    m.y = float(by);
}

inline void greedy_step(const BlockWorld& w, Mob& m, float tx, float tz, float step) {
    float dx = tx - m.x, dz = tz - m.z;
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1e-4f) return;
    dx = dx / len * step;
    dz = dz / len * step;
    if (try_move(w, m, dx, dz)) { apply_gravity(w, m); return; }

    const bool xDominant = std::fabs(dx) >= std::fabs(dz);
    if (xDominant) { if (try_move(w, m, dx, 0.0f)) { apply_gravity(w, m); return; } }
    else           { if (try_move(w, m, 0.0f, dz)) { apply_gravity(w, m); return; } }

    // Head on into a wall. Slide along the perpendicular, and keep sliding the
    // same way until that fails too — a mob that re-picks a side every tick
    // stands in front of the wall vibrating instead of walking round it. That
    // was the actual first-draft failure here, and the sticky `slide` sign is
    // the fix.
    const float perp = (m.slide > 0) ? step : -step;
    const float px = xDominant ? 0.0f : perp;
    const float pz = xDominant ? perp  : 0.0f;
    if (try_move(w, m, px, pz)) { apply_gravity(w, m); return; }
    m.slide = std::int8_t(-m.slide);
}

// ── the population ──────────────────────────────────────────────────────────

class MobWorld {
public:
    explicit MobWorld(std::uint64_t seed) : rng_(seed) {}

    void reseed(std::uint64_t s) { rng_.reseed(s); }
    void set_player(float x, float y, float z) { px_ = x; py_ = y; pz_ = z; }
    void set_world_spawn(float x, float y, float z) { sx_ = x; sy_ = y; sz_ = z; }

    void clear() { mobs_.clear(); }
    [[nodiscard]] const std::vector<Mob>& mobs() const { return mobs_; }
    [[nodiscard]] std::vector<Mob>& mobs() { return mobs_; }
    [[nodiscard]] Rng& rng() { return rng_; }

    [[nodiscard]] int alive() const {
        int n = 0;
        for (const Mob& m : mobs_) if (m.alive) ++n;
        return n;
    }
    [[nodiscard]] int alive(MobCat c) const {
        int n = 0;
        for (const Mob& m : mobs_) if (m.alive && mob_type(m.kind).cat == c) ++n;
        return n;
    }

    int add(int kind, float x, float y, float z) {
        Mob m;
        m.kind   = std::uint8_t(kind);
        m.x = x; m.y = y; m.z = z;
        m.health = mob_type(kind).health;
        m.state  = StIdle;
        mobs_.push_back(m);
        return int(mobs_.size()) - 1;
    }

    // Compact away the dead. Not called automatically, because index stability
    // across a tick is worth more to a caller than a tight vector.
    void collect() {
        mobs_.erase(std::remove_if(mobs_.begin(), mobs_.end(),
                                   [](const Mob& m) { return !m.alive; }),
                    mobs_.end());
    }

    // One spawn cycle for one category over `positions` candidate positions.
    // The per-position work is the vanilla pack loop; nothing here scans the
    // world's volume.
    SpawnReport spawn_cycle(const BlockWorld& w, const LightView& lv, MobCat cat,
                            int timeOfDay, int spawnableChunks, int positions) {
        SpawnReport rep;
        const int cap = mob_cap(cat, spawnableChunks);
        int have = alive(cat);

        for (int p = 0; p < positions; ++p) {
            ++rep.positions;
            if (have >= cap) { ++rep.rejectCap; continue; }

            // Vanilla picks a random column and then a y uniformly up to the
            // surface heightmap, which is why caves take the bulk of hostile
            // spawns rather than the surface. I could not find that stated
            // verbatim on the wiki, so the uniform-in-column choice is FLAGGED
            // as reconstruction, not citation.
            const int cx = int(rng_.below(std::uint32_t(w.n())));
            const int cz = int(rng_.below(std::uint32_t(w.n())));
            const int top = w.surface(cx, cz);
            if (top < 1) continue;
            const int cy = 1 + int(rng_.below(std::uint32_t(top + 1)));

            for (int a = 0; a < kPackAttempts; ++a) {
                int mx = cx, mz = cz;
                int packSize = int(std::ceil(rng_.unit() * 4.0f));   // replaced below
                bool haveEntry = false;
                MobSpawnEntry entry{MobZombie, 0, 0, 0};
                int spawnedHere = 0;

                for (int i = 0; i < packSize; ++i) {
                    if (!haveEntry) {
                        entry = pick_spawn_entry(cat, rng_);
                        haveEntry = true;
                        packSize = entry.minCount
                                 + int(rng_.below(std::uint32_t(1 + entry.maxCount
                                                                  - entry.minCount)));
                    }
                    const int ox = int(rng_.below(kPackSpread)) - int(rng_.below(kPackSpread));
                    const int oz = int(rng_.below(kPackSpread)) - int(rng_.below(kPackSpread));
                    rep.maxStepOffset = std::max(rep.maxStepOffset,
                                                 std::max(std::abs(ox), std::abs(oz)));
                    mx += ox;
                    mz += oz;

                    if (have >= cap) { ++rep.rejectCap; break; }
                    if (!distance_ok(float(mx) + 0.5f, float(cy), float(mz) + 0.5f)) {
                        ++rep.rejectDistance; continue;
                    }
                    if (!spawn_allowed(cat, w, lv, mx, cy, mz, timeOfDay)) {
                        ++rep.rejectRules; continue;
                    }
                    add(entry.kind, float(mx) + 0.5f, float(cy), float(mz) + 0.5f);
                    ++have; ++rep.spawned; ++spawnedHere;
                }
                if (spawnedHere > 0) { ++rep.packs; rep.packSizeSum += spawnedHere; }
            }
        }
        return rep;
    }

    TickStats tick(const BlockWorld& w, const LightView& lv, int timeOfDay, Difficulty diff) {
        TickStats st;
        for (Mob& m : mobs_) {
            if (!m.alive) continue;
            const MobType& t = mob_type(m.kind);

            if (despawn_check(m, t))                { ++st.despawned; continue; }
            if (sunlight_check(w, m, t, timeOfDay)) { ++st.burning; }
            if (fire_tick(m))                       { ++st.deaths; continue; }

            think(w, lv, m, t, timeOfDay, diff, st);
            ++m.stateTicks;
            if (m.attackCd > 0) --m.attackCd;
        }
        return st;
    }

    // Loot. Uniform over the published inclusive range, one roll per line.
    [[nodiscard]] std::array<int, kMobItems> roll_drops(int kind, bool killedByPlayer) {
        std::array<int, kMobItems> out{};
        const MobType& t = mob_type(kind);
        for (int i = 0; i < t.dropCount; ++i) {
            const MobDropRule& d = t.drops[std::size_t(i)];
            if (d.playerKillOnly && !killedByPlayer) continue;
            if (d.chance < 1.0 && double(rng_.unit()) >= d.chance) continue;
            const int span = int(d.hi) - int(d.lo) + 1;
            out[std::size_t(d.item)] += int(d.lo) + int(rng_.below(std::uint32_t(span)));
        }
        return out;
    }

    // XP orbs, on a player kill only — vanilla drops none otherwise.
    [[nodiscard]] int roll_xp(int kind, bool killedByPlayer) {
        if (!killedByPlayer) return 0;
        const MobType& t = mob_type(kind);
        return t.xpLo + int(rng_.below(std::uint32_t(t.xpHi - t.xpLo + 1)));
    }

private:
    [[nodiscard]] float dist_to_player(const Mob& m) const {
        const float dx = m.x - px_, dy = m.y - py_, dz = m.z - pz_;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    [[nodiscard]] bool distance_ok(float x, float y, float z) const {
        const float dx = x - px_, dy = y - py_, dz = z - pz_;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d <= kMinSpawnDistance || d > kSpawnRadius) return false;
        const float ex = x - sx_, ey = y - sy_, ez = z - sz_;
        return std::sqrt(ex * ex + ey * ey + ez * ez) > kMinSpawnDistance;
    }

    // Vanilla despawns only categories whose "persistent" flag is false, which
    // is why "most passive mobs do not despawn, while most monsters do" (the
    // Mob article). CREATURE is persistent, MONSTER is not.
    bool despawn_check(Mob& m, const MobType& t) {
        if (t.cat != CatMonster) return false;
        const float d = dist_to_player(m);
        if (d > kDespawnDistance)   { m.alive = false; return true; }
        if (d < kNoDespawnDistance) { m.idleTicks = 0; return false; }
        ++m.idleTicks;
        if (m.idleTicks > kDespawnIdleTicks && rng_.below(kDespawnOneIn) == 0) {
            m.alive = false;
            return true;
        }
        return false;
    }

    // Sky-exposed undead catch fire inside the published burn window. Vanilla
    // makes ignition probabilistic (a random roll against the local light) and
    // tests the EYE position; this tests the head block deterministically.
    // Water suppresses it, as it does in the game. Returns "is burning".
    bool sunlight_check(const BlockWorld& w, Mob& m, const MobType& t, int timeOfDay) {
        if (!t.burnsInDaylight || !undead_burn_window(timeOfDay)) return m.fireTicks > 0;
        const int bx = int(std::floor(m.x)), by = int(std::floor(m.y)),
                  bz = int(std::floor(m.z));
        if (w.at(bx, by, bz) == Water || w.at(bx, by + 1, bz) == Water) return false;
        if (raw_sky_light(w, bx, by + 1, bz) < 15) return m.fireTicks > 0;
        m.fireTicks = kFireTicks;
        return true;
    }

    // WHY fireDamageCd EXISTS, since it looks redundant next to fireTicks. The
    // first draft dealt fire damage when fireTicks hit a multiple of 20. A mob
    // standing in the sun has fireTicks reset to 160 every single tick, so it
    // never reached a multiple of 20 and a zombie stood in full daylight
    // forever taking nothing. The damage clock has to be independent of the
    // burn clock. Returns true if the mob died of it.
    bool fire_tick(Mob& m) {
        if (m.fireTicks <= 0) return false;
        --m.fireTicks;
        if (--m.fireDamageCd <= 0) {
            m.fireDamageCd = kFireDamageEvery;
            m.health -= kFireDamage;
            if (m.health <= 0.0f) { m.alive = false; return true; }
        }
        return false;
    }

    void think(const BlockWorld& w, const LightView& lv, Mob& m, const MobType& t,
               int timeOfDay, Difficulty diff, TickStats& st) {
        const float d = dist_to_player(m);
        const bool hostile = (t.cat == CatMonster) && hostile_now(w, lv, m, timeOfDay);
        const bool sees = hostile && d <= t.followRange && diff != DiffPeaceful;

        if (!sees) {
            if (m.state == StFuse) m.fuse = -1;   // lost you: the fuse goes out
            wander(w, m);
            return;
        }
        if (m.kind == MobCreeper)  { creeper(w, m, d, diff, st);  return; }
        if (m.kind == MobSkeleton) { skeleton(w, m, d, diff, st); return; }

        if (d <= kMeleeReach) {
            set_state(m, StAttack);
            if (m.attackCd == 0 && t.attack > 0.0f) {
                st.playerDamage += damage_to_player(t.attack, diff);
                ++st.attacks;
                m.attackCd = kMeleeCooldown;
            }
        } else {
            set_state(m, StChase);
            greedy_step(w, m, px_, pz_, mob_step(m.kind));
        }
    }

    // Spiders are the one mob here whose hostility depends on light: the spider
    // article, "A spider stays hostile toward the player or an iron golem as
    // long as the light level immediately around the spider is 11 or less;
    // otherwise, it does not attack unless attacked first."
    [[nodiscard]] bool hostile_now(const BlockWorld& w, const LightView& lv, const Mob& m,
                                   int timeOfDay) const {
        if (m.kind != MobSpider) return true;
        const int bx = int(std::floor(m.x)), by = int(std::floor(m.y)),
                  bz = int(std::floor(m.z));
        return lv.internal_light(w, bx, by, bz, timeOfDay) <= 11;
    }

    void creeper(const BlockWorld& w, Mob& m, float d, Difficulty diff, TickStats& st) {
        if (m.state == StFuse) {
            if (d > kCreeperCancelRange) { m.fuse = -1; set_state(m, StChase); return; }
            if (--m.fuse <= 0) {
                st.playerDamage += damage_to_player(
                    explosion_damage(kCreeperPower, d, 1.0f), diff);
                ++st.explosions;
                ++st.deaths;
                m.alive = false;              // consumed by its own blast
            }
            return;                            // it does not move once lit
        }
        if (d <= kCreeperIgniteRange) {
            set_state(m, StFuse);
            m.fuse = kCreeperFuseTicks;
            return;
        }
        set_state(m, StChase);
        greedy_step(w, m, px_, pz_, mob_step(m.kind));
    }

    void skeleton(const BlockWorld& w, Mob& m, float d, Difficulty diff, TickStats& st) {
        if (d <= kSkeletonShootRange) {
            set_state(m, StAttack);
            if (m.attackCd == 0) {
                float lo = 0.f, hi = 0.f;
                skeleton_arrow_range(diff, lo, hi);
                st.playerDamage += lo + rng_.unit() * (hi - lo);  // NOT re-scaled
                ++st.attacks;
                m.attackCd = skeleton_shoot_interval(diff);
            }
            return;
        }
        set_state(m, StChase);
        greedy_step(w, m, px_, pz_, mob_step(m.kind));
    }

    void wander(const BlockWorld& w, Mob& m) {
        if (m.wanderTicks <= 0) {
            // Idle for a spell, then pick a heading and hold it. Both dwell
            // times are BENCH numbers; vanilla's stroll goal re-rolls on its
            // own schedule and I did not source it.
            if (m.state == StWander) {
                set_state(m, StIdle);
            } else {
                set_state(m, StWander);
                const float ang = rng_.unit() * 6.2831853f;
                m.wanderX = std::cos(ang);
                m.wanderZ = std::sin(ang);
            }
            m.wanderTicks = 20 + int(rng_.below(60));
        }
        --m.wanderTicks;
        if (m.state == StWander)
            greedy_step(w, m, m.x + m.wanderX * 8.0f, m.z + m.wanderZ * 8.0f,
                        mob_step(m.kind));
    }

    static void set_state(Mob& m, MobState s) {
        if (m.state != s) { m.state = std::uint8_t(s); m.stateTicks = 0; }
    }

    std::vector<Mob> mobs_;
    Rng   rng_;
    float px_ = 0.f, py_ = 0.f, pz_ = 0.f;
    float sx_ = 0.f, sy_ = 0.f, sz_ = 0.f;
};

} // namespace bench
