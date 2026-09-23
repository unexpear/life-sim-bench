// craft.hpp — the crafting, smelting, gear and food tables, at the game's own
// numbers.
//
// SCOPE. Everything a survival player touches on the way to diamond gear: the
// five tool types in five materials (plus netherite, which is smithed rather
// than crafted), the four armour slots in six materials, the utility recipes
// (sticks, torches, chests, furnaces, tables, doors, beds, boats, buckets,
// shears, flint and steel, bread), the furnace — its 200-tick cycle, its fuel
// burn table and its recipe list — and the food table with hunger and
// saturation.
//
// SOURCES. Java Edition, minecraft.wiki, read August 2026. Every published
// number below carries the page it came from. Where the wiki gives a formula
// (armour damage reduction, block breaking, armour durability, saturation) the
// FORMULA is implemented and the published table is then asserted against it in
// test_craft.cpp, rather than the table being transcribed and the formula
// paraphrased. That ordering matters: it means a wrong constant shows up as a
// failing test instead of as a plausible-looking number.
//
// ── four things that are easy to get wrong, and did go wrong while writing ──
//
// 1. GOLD IS TWO DIFFERENT NUMBERS. A golden tool has mining SPEED 12, the
//    fastest in the game, and harvest LEVEL 0, the same as wood. A golden
//    pickaxe therefore mines stone faster than diamond does and cannot harvest
//    diamond ore at all — it is "the only pickaxe unable to harvest the
//    material it is made from" (minecraft.wiki, Golden Pickaxe). Any code that
//    stores one integer per material and calls it "tier" gets gold wrong.
//    bench::tool_speed() in blockworld.hpp is exactly that code: its argument
//    is a harvest tier and it returns 1/2/4/6/8, so it has no way to express
//    gold. This header keeps speed and harvest level as separate tables and
//    break_ticks_for() below deliberately disagrees with bench::break_ticks()
//    for gold. test_craft.cpp measures the size of that disagreement so nobody
//    later "fixes" it back.
//
// 2. THE WIKI'S SATURATION MODIFIER COLUMN IS TWICE THE GAME'S FIELD. The
//    saturation actually restored is hunger x modifier x 2 where `modifier` is
//    the FoodProperties field (bread 0.6). The wiki's food table lists a
//    "saturation modifier" of 1.2 for bread, i.e. already doubled, so that
//    hunger x column = saturation. Both conventions are in circulation. This
//    file stores the game's field and applies the x2, and asserts the published
//    saturation restored.
//
// 3. FOOD TABLES ARE SORTED BY SATURATION, NOT BY NAME, AND ROWS MERGE. A first
//    pass at this file had cooked mutton and cooked salmon at 8 hunger because
//    they share a merged 9.6-saturation row with the golden apples. They are 6.
//    Every food below was cross-checked against BOTH the hunger table and the
//    saturation table, and the two must agree through hunger x mod x 2 — which
//    is why that identity is asserted for every food rather than spot-checked.
//
// 4. BURN TIME IS NOT ITEMS SMELTED. Fuel burns continuously once lit: "the
//    fuel slot is decremented immediately, and that unit of fuel starts
//    burning... regardless of whether the upper slot has any items remaining"
//    (minecraft.wiki, Furnace). A stick is 100 ticks against a 200-tick cycle,
//    which is half an item, and the other half is thrown away if nothing
//    follows it. items_smelted() returns a float on purpose.
//
// COMPLEXITY. Every function here is O(1): a switch or an index into a
// fixed-size constexpr table, the largest of which is 6x5 = 30 entries. Nothing
// allocates, nothing touches the world, nothing scales with n. A sim that
// resolves one craft per agent per tick pays O(agents); the O(volume) budget
// the project cares about is untouched because this file never looks at volume.
//
// NAMESPACE. bench::craft, not bench, on purpose: blockworld.hpp already owns
// bench::Recipe, bench::Ingredient, bench::Item and bench::recipe() for its own
// much smaller tech tree, and those are different things with the same names.
// Scoped enums here mean craft::ItemId::Coal and bench::Coal can never be
// confused by the compiler or by a reader.

#pragma once
#include "blockworld.hpp"   // Block, block_rule() — for the bridge at the end
#include <array>
#include <cstdint>
#include <string>

namespace bench::craft {

// ── enums ───────────────────────────────────────────────────────────────────

enum class Material : std::uint8_t {
    Wood, Stone, Iron, Gold, Diamond, Netherite, Count
};
enum class ToolKind : std::uint8_t {
    Pickaxe, Axe, Shovel, Hoe, Sword, Count
};
enum class ArmourMaterial : std::uint8_t {
    Leather, Chainmail, Iron, Gold, Diamond, Netherite, Count
};
enum class ArmourSlot : std::uint8_t {
    Helmet, Chestplate, Leggings, Boots, Count
};
// Where a recipe can be resolved. Inventory means the 2x2 grid a player carries;
// CraftingTable means the recipe needs the 3x3. Which one a recipe needs is not
// decoration — it is the first gate in the tech tree, and it is decided purely
// by whether the shape fits in 2x2.
enum class Station : std::uint8_t {
    Inventory, CraftingTable, Furnace, SmithingTable
};

template <class E>
[[nodiscard]] constexpr int ix(E e) noexcept { return static_cast<int>(e); }

constexpr int kMaterials       = ix(Material::Count);        // 6
constexpr int kToolKinds       = ix(ToolKind::Count);        // 5
constexpr int kArmourMaterials = ix(ArmourMaterial::Count);  // 6
constexpr int kArmourSlots     = ix(ArmourSlot::Count);      // 4

// ── item ids ────────────────────────────────────────────────────────────────
//
// Plain items are enumerated. Tools and armour are NOT: 5 kinds x 6 materials
// plus 4 slots x 6 materials is 54 hand-written enumerators that would drift
// out of step with the stat tables the first time anyone edited one of them.
// They are packed into id space instead — tool_item(kind, mat) and
// armour_item(slot, mat) build the id, is_tool()/is_armour() and the accessors
// take it apart again. The packing is 8 materials per row so the arithmetic is
// a shift; 6 are used and 2 are slack.
enum class ItemId : std::uint16_t {
    None = 0,
    // wood chain
    OakLog, Planks, Stick, Sapling, Bamboo,
    // stone chain
    Cobblestone, Stone, Sand, Glass, Gravel, Flint, Obsidian,
    // ore chain. RawIron/RawGold are 1.17+; IronOre/GoldOre are the pre-1.17
    // ore BLOCKS, which smelted directly. Both are kept because this project's
    // BlockWorld generates ore blocks in the pre-1.18 distribution and would
    // otherwise have nothing to put in a furnace.
    IronOre, GoldOre, RawIron, RawGold, IronIngot, GoldIngot, Diamond,
    AncientDebris, NetheriteScrap, NetheriteIngot,
    Coal, Charcoal, BlockOfCoal,
    // other ingredients
    Leather, Wool, Wheat, String, Cactus, GreenDye, Kelp, DriedKelp,
    DriedKelpBlock, BlazeRod, ClayBall, Brick,
    // crafted utility
    CraftingTable, Furnace, Chest, Torch, WoodenDoor, Bed, Boat, Bucket,
    LavaBucket, WaterBucket, Shears, FlintAndSteel, Ladder, Bookshelf,
    WoodenSlab, Sign, Bow, FishingRod,
    // food
    Bread, Apple, GoldenApple, MelonSlice, Cookie, RottenFlesh,
    Carrot, Potato, BakedPotato,
    RawBeef, Steak, RawPorkchop, CookedPorkchop, RawChicken, CookedChicken,
    RawMutton, CookedMutton, RawRabbit, CookedRabbit,
    RawCod, CookedCod, RawSalmon, CookedSalmon,
    PlainCount
};

constexpr std::uint16_t kToolIdBase   = 512;
constexpr std::uint16_t kArmourIdBase = 640;
constexpr std::uint16_t kRowStride    = 8;   // materials per packed row

[[nodiscard]] constexpr ItemId tool_item(ToolKind k, Material m) noexcept {
    return ItemId(std::uint16_t(kToolIdBase + ix(k) * kRowStride + ix(m)));
}
[[nodiscard]] constexpr ItemId armour_item(ArmourSlot s, ArmourMaterial m) noexcept {
    return ItemId(std::uint16_t(kArmourIdBase + ix(s) * kRowStride + ix(m)));
}
[[nodiscard]] constexpr bool is_tool(ItemId i) noexcept {
    const std::uint16_t v = std::uint16_t(i);
    return v >= kToolIdBase && v < kToolIdBase + kToolKinds * kRowStride
        && (v - kToolIdBase) % kRowStride < kMaterials;
}
[[nodiscard]] constexpr bool is_armour(ItemId i) noexcept {
    const std::uint16_t v = std::uint16_t(i);
    return v >= kArmourIdBase && v < kArmourIdBase + kArmourSlots * kRowStride
        && (v - kArmourIdBase) % kRowStride < kArmourMaterials;
}
[[nodiscard]] constexpr ToolKind tool_kind_of(ItemId i) noexcept {
    return ToolKind((std::uint16_t(i) - kToolIdBase) / kRowStride);
}
[[nodiscard]] constexpr Material tool_material_of(ItemId i) noexcept {
    return Material((std::uint16_t(i) - kToolIdBase) % kRowStride);
}
[[nodiscard]] constexpr ArmourSlot armour_slot_of(ItemId i) noexcept {
    return ArmourSlot((std::uint16_t(i) - kArmourIdBase) / kRowStride);
}
[[nodiscard]] constexpr ArmourMaterial armour_material_of(ItemId i) noexcept {
    return ArmourMaterial((std::uint16_t(i) - kArmourIdBase) % kRowStride);
}

[[nodiscard]] inline const char* material_name(Material m) {
    switch (m) {
        case Material::Wood:      return "wooden";
        case Material::Stone:     return "stone";
        case Material::Iron:      return "iron";
        case Material::Gold:      return "golden";
        case Material::Diamond:   return "diamond";
        case Material::Netherite: return "netherite";
        default:                  return "?";
    }
}
[[nodiscard]] inline const char* tool_kind_name(ToolKind k) {
    switch (k) {
        case ToolKind::Pickaxe: return "pickaxe";
        case ToolKind::Axe:     return "axe";
        case ToolKind::Shovel:  return "shovel";
        case ToolKind::Hoe:     return "hoe";
        case ToolKind::Sword:   return "sword";
        default:                return "?";
    }
}
[[nodiscard]] inline const char* armour_material_name(ArmourMaterial m) {
    switch (m) {
        case ArmourMaterial::Leather:   return "leather";
        case ArmourMaterial::Chainmail: return "chainmail";
        case ArmourMaterial::Iron:      return "iron";
        case ArmourMaterial::Gold:      return "golden";
        case ArmourMaterial::Diamond:   return "diamond";
        case ArmourMaterial::Netherite: return "netherite";
        default:                        return "?";
    }
}
[[nodiscard]] inline const char* armour_slot_name(ArmourSlot s) {
    switch (s) {
        case ArmourSlot::Helmet:     return "helmet";
        case ArmourSlot::Chestplate: return "chestplate";
        case ArmourSlot::Leggings:   return "leggings";
        case ArmourSlot::Boots:      return "boots";
        default:                     return "?";
    }
}

[[nodiscard]] inline std::string item_name(ItemId i) {
    if (is_tool(i))
        return std::string(material_name(tool_material_of(i))) + " "
             + tool_kind_name(tool_kind_of(i));
    if (is_armour(i))
        return std::string(armour_material_name(armour_material_of(i))) + " "
             + armour_slot_name(armour_slot_of(i));
    switch (i) {
        case ItemId::None:           return "nothing";
        case ItemId::OakLog:         return "log";
        case ItemId::Planks:         return "planks";
        case ItemId::Stick:          return "stick";
        case ItemId::Sapling:        return "sapling";
        case ItemId::Bamboo:         return "bamboo";
        case ItemId::Cobblestone:    return "cobblestone";
        case ItemId::Stone:          return "stone";
        case ItemId::Sand:           return "sand";
        case ItemId::Glass:          return "glass";
        case ItemId::Gravel:         return "gravel";
        case ItemId::Flint:          return "flint";
        case ItemId::Obsidian:       return "obsidian";
        case ItemId::IronOre:        return "iron ore";
        case ItemId::GoldOre:        return "gold ore";
        case ItemId::RawIron:        return "raw iron";
        case ItemId::RawGold:        return "raw gold";
        case ItemId::IronIngot:      return "iron ingot";
        case ItemId::GoldIngot:      return "gold ingot";
        case ItemId::Diamond:        return "diamond";
        case ItemId::AncientDebris:  return "ancient debris";
        case ItemId::NetheriteScrap: return "netherite scrap";
        case ItemId::NetheriteIngot: return "netherite ingot";
        case ItemId::Coal:           return "coal";
        case ItemId::Charcoal:       return "charcoal";
        case ItemId::BlockOfCoal:    return "block of coal";
        case ItemId::Leather:        return "leather";
        case ItemId::Wool:           return "wool";
        case ItemId::Wheat:          return "wheat";
        case ItemId::String:         return "string";
        case ItemId::Cactus:         return "cactus";
        case ItemId::GreenDye:       return "green dye";
        case ItemId::Kelp:           return "kelp";
        case ItemId::DriedKelp:      return "dried kelp";
        case ItemId::DriedKelpBlock: return "dried kelp block";
        case ItemId::BlazeRod:       return "blaze rod";
        case ItemId::ClayBall:       return "clay ball";
        case ItemId::Brick:          return "brick";
        case ItemId::CraftingTable:  return "crafting table";
        case ItemId::Furnace:        return "furnace";
        case ItemId::Chest:          return "chest";
        case ItemId::Torch:          return "torch";
        case ItemId::WoodenDoor:     return "wooden door";
        case ItemId::Bed:            return "bed";
        case ItemId::Boat:           return "boat";
        case ItemId::Bucket:         return "bucket";
        case ItemId::LavaBucket:     return "lava bucket";
        case ItemId::WaterBucket:    return "water bucket";
        case ItemId::Shears:         return "shears";
        case ItemId::FlintAndSteel:  return "flint and steel";
        case ItemId::Ladder:         return "ladder";
        case ItemId::Bookshelf:      return "bookshelf";
        case ItemId::WoodenSlab:     return "wooden slab";
        case ItemId::Sign:           return "sign";
        case ItemId::Bow:            return "bow";
        case ItemId::FishingRod:     return "fishing rod";
        case ItemId::Bread:          return "bread";
        case ItemId::Apple:          return "apple";
        case ItemId::GoldenApple:    return "golden apple";
        case ItemId::MelonSlice:     return "melon slice";
        case ItemId::Cookie:         return "cookie";
        case ItemId::RottenFlesh:    return "rotten flesh";
        case ItemId::Carrot:         return "carrot";
        case ItemId::Potato:         return "potato";
        case ItemId::BakedPotato:    return "baked potato";
        case ItemId::RawBeef:        return "raw beef";
        case ItemId::Steak:          return "steak";
        case ItemId::RawPorkchop:    return "raw porkchop";
        case ItemId::CookedPorkchop: return "cooked porkchop";
        case ItemId::RawChicken:     return "raw chicken";
        case ItemId::CookedChicken:  return "cooked chicken";
        case ItemId::RawMutton:      return "raw mutton";
        case ItemId::CookedMutton:   return "cooked mutton";
        case ItemId::RawRabbit:      return "raw rabbit";
        case ItemId::CookedRabbit:   return "cooked rabbit";
        case ItemId::RawCod:         return "raw cod";
        case ItemId::CookedCod:      return "cooked cod";
        case ItemId::RawSalmon:      return "raw salmon";
        case ItemId::CookedSalmon:   return "cooked salmon";
        default:                     return "?";
    }
}

// ── tool statistics ─────────────────────────────────────────────────────────
//
// Durability, in uses. minecraft.wiki, Durability, Java Edition. Gold at 32 is
// the lowest in the game and sits below wood — that inversion is the point of
// gold, not a typo.
[[nodiscard]] constexpr int tool_durability(Material m) noexcept {
    constexpr int d[kMaterials] = { 59, 131, 250, 32, 1561, 2031 };
    return d[ix(m)];
}

// Mining speed multiplier. minecraft.wiki, Breaking: "no tool 1, wood 2,
// stone 4, copper 5, iron 6, diamond 8, netherite 9, gold 12". Copper is
// omitted here because this project has no copper.
[[nodiscard]] constexpr float tool_mining_speed(Material m) noexcept {
    constexpr float s[kMaterials] = { 2.0f, 4.0f, 6.0f, 12.0f, 8.0f, 9.0f };
    return s[ix(m)];
}
constexpr float kBareHandSpeed = 1.0f;

// Harvest level, in the game's own numbering: 0 wood/gold, 1 stone, 2 iron,
// 3 diamond, 4 netherite. Note gold == wood; see the header note.
[[nodiscard]] constexpr int tool_harvest_level(Material m) noexcept {
    constexpr int h[kMaterials] = { 0, 1, 2, 0, 3, 4 };
    return h[ix(m)];
}

// The same fact in blockworld.hpp's numbering, where 0 is the bare hand and 1
// is wood, so every level is shifted up by one. Provided because block_rule()
// .tier and drops_for() speak that dialect and a caller should not have to
// remember which of the two it is holding.
[[nodiscard]] constexpr int blockworld_tier(Material m) noexcept {
    return tool_harvest_level(m) + 1;
}

// Attack damage, Java Edition (minecraft.wiki, Damage). Implemented as the
// game builds it: a per-type base plus a per-material bonus. The axe is the
// exception — it does not follow the additive rule (wood and gold both 7,
// stone/iron/diamond all 9), so it gets its own row.
[[nodiscard]] constexpr float material_attack_bonus(Material m) noexcept {
    constexpr float b[kMaterials] = { 0.0f, 1.0f, 2.0f, 0.0f, 3.0f, 4.0f };
    return b[ix(m)];
}
[[nodiscard]] constexpr float tool_attack_damage(ToolKind k, Material m) noexcept {
    constexpr float axe[kMaterials] = { 7.0f, 9.0f, 9.0f, 7.0f, 9.0f, 10.0f };
    switch (k) {
        case ToolKind::Sword:   return 4.0f + material_attack_bonus(m);
        case ToolKind::Pickaxe: return 2.0f + material_attack_bonus(m);
        case ToolKind::Shovel:  return 2.5f + material_attack_bonus(m);
        case ToolKind::Axe:     return axe[ix(m)];
        case ToolKind::Hoe:     return 1.0f;   // flat across every material
        default:                return 1.0f;
    }
}

// Attack speed in attacks per second. Sword/pickaxe/shovel are flat; axe and
// hoe vary by material, in opposite directions.
[[nodiscard]] constexpr float tool_attack_speed(ToolKind k, Material m) noexcept {
    constexpr float axe[kMaterials] = { 0.8f, 0.8f, 0.9f, 1.0f, 1.0f, 1.0f };
    constexpr float hoe[kMaterials] = { 1.0f, 2.0f, 3.0f, 1.0f, 4.0f, 4.0f };
    switch (k) {
        case ToolKind::Sword:   return 1.6f;
        case ToolKind::Pickaxe: return 1.2f;
        case ToolKind::Shovel:  return 1.0f;
        case ToolKind::Axe:     return axe[ix(m)];
        case ToolKind::Hoe:     return hoe[ix(m)];
        default:                return 4.0f;
    }
}

// ── armour statistics ───────────────────────────────────────────────────────
//
// Armour points per piece. minecraft.wiki, Armor. Full sets: leather 7,
// chainmail 12, iron 15, gold 11, diamond 20, netherite 20. Note gold sits
// BELOW chainmail on leggings, which is why a gold set is 11 and a chain set
// is 12 despite gold being the rarer metal.
[[nodiscard]] constexpr int armour_points(ArmourMaterial m, ArmourSlot s) noexcept {
    constexpr int p[kArmourMaterials][kArmourSlots] = {
        // helmet, chestplate, leggings, boots
        { 1, 3, 2, 1 },   // leather   = 7
        { 2, 5, 4, 1 },   // chainmail = 12
        { 2, 6, 5, 2 },   // iron      = 15
        { 2, 5, 3, 1 },   // gold      = 11
        { 3, 8, 6, 3 },   // diamond   = 20
        { 3, 8, 6, 3 },   // netherite = 20
    };
    return p[ix(m)][ix(s)];
}

// Toughness is per PIECE and uniform across slots: only diamond (2) and
// netherite (3) have any, giving 8 and 12 for a full set.
[[nodiscard]] constexpr float armour_toughness(ArmourMaterial m, ArmourSlot s) noexcept {
    (void)s;   // uniform across slots in Java Edition; the parameter is kept so
               // callers can write the same loop they write for points
    switch (m) {
        case ArmourMaterial::Diamond:   return 2.0f;
        case ArmourMaterial::Netherite: return 3.0f;
        default:                        return 0.0f;
    }
}

// Knockback resistance, per piece, as the attribute value: netherite 0.1,
// everything else 0. A full netherite set sums to 0.4, the published 40%.
[[nodiscard]] constexpr float armour_knockback_resistance(ArmourMaterial m) noexcept {
    return m == ArmourMaterial::Netherite ? 0.1f : 0.0f;
}

// Armour durability is a product, not a table: a per-material factor times a
// per-slot multiplier. minecraft.wiki, Durability. Storing it as the product
// would be 24 numbers that can each be wrong independently; storing it as the
// formula makes the published table a test rather than an input.
[[nodiscard]] constexpr int armour_durability_factor(ArmourMaterial m) noexcept {
    constexpr int f[kArmourMaterials] = { 5, 15, 15, 7, 33, 37 };
    return f[ix(m)];
}
[[nodiscard]] constexpr int armour_slot_multiplier(ArmourSlot s) noexcept {
    constexpr int k[kArmourSlots] = { 11, 16, 15, 13 };
    return k[ix(s)];
}
[[nodiscard]] constexpr int armour_durability(ArmourMaterial m, ArmourSlot s) noexcept {
    return armour_durability_factor(m) * armour_slot_multiplier(s);
}

[[nodiscard]] constexpr int full_set_points(ArmourMaterial m) noexcept {
    int t = 0;
    for (int s = 0; s < kArmourSlots; ++s) t += armour_points(m, ArmourSlot(s));
    return t;
}
[[nodiscard]] constexpr float full_set_toughness(ArmourMaterial m) noexcept {
    float t = 0.0f;
    for (int s = 0; s < kArmourSlots; ++s) t += armour_toughness(m, ArmourSlot(s));
    return t;
}

// Damage after armour, Java Edition 1.9+ (minecraft.wiki, Armor):
//
//   reduction = min(20, max(points / 5, points - damage / (2 + toughness / 4)))
//   result    = damage * (1 - reduction / 25)
//
// The wiki also states it as a percentage,
//   reduction% = min(80, max(4/5 * points, 4 * points - 16 * damage / (toughness + 8)))
// which is the same expression multiplied through by 4; both forms are
// implemented and test_craft.cpp asserts they agree over a sweep, because a
// transcription error in either one would otherwise be invisible.
//
// The two arms are the whole design: points/5 is the floor that guarantees
// armour always does something, and the other arm is what makes a big hit tear
// through it. Toughness only enters the second arm — it delays the point at
// which large damage starts stripping the armour's value.
[[nodiscard]] inline float damage_after_armour(float damage, float points,
                                               float toughness) noexcept {
    if (damage <= 0.0f) return 0.0f;
    const float a = points / 5.0f;
    const float b = points - damage / (2.0f + toughness / 4.0f);
    float reduction = a > b ? a : b;
    if (reduction > 20.0f) reduction = 20.0f;
    if (reduction < 0.0f)  reduction = 0.0f;
    return damage * (1.0f - reduction / 25.0f);
}
// The percentage form, returned as a fraction in [0, 0.8].
[[nodiscard]] inline float armour_reduction_fraction(float damage, float points,
                                                     float toughness) noexcept {
    if (damage <= 0.0f) return 0.0f;
    const float a = 0.8f * points;
    const float b = 4.0f * points - 16.0f * damage / (toughness + 8.0f);
    float pct = a > b ? a : b;
    if (pct > 80.0f) pct = 80.0f;
    if (pct < 0.0f)  pct = 0.0f;
    return pct / 100.0f;
}

// ── block breaking ──────────────────────────────────────────────────────────
//
// minecraft.wiki, Breaking. Damage accumulates per tick as
//
//   damage = speed / hardness / 30     if the tool can harvest the block
//   damage = speed / hardness / 100    if it cannot
//
// and the block breaks when the accumulated damage reaches 1, i.e. after
// ceil(hardness * 30 / speed) ticks. `speed` is the tool's multiplier only when
// the tool is the right TYPE for the block; a pickaxe on wood is speed 1.
//
// The CEILING is not a rounding convenience, it is the mechanic: stone with a
// wooden pickaxe is 1.5 * 30 / 2 = 22.5 ticks, which the game runs as 23 ticks
// = 1.15 s, and 1.15 s is exactly what the wiki's stone row publishes. The
// whole published stone row and the obsidian row fall out of this ceiling; both
// are asserted in the test.
constexpr int kHarvestDivisor    = 30;    // ticks-scale for a harvestable block
constexpr int kNoHarvestDivisor  = 100;   // ...and for one the tool cannot drop

[[nodiscard]] inline int break_ticks(float hardness, float speed,
                                     bool canHarvest) noexcept {
    if (hardness < 0.0f) return -1;                 // bedrock and friends: never
    if (hardness == 0.0f) return 0;                 // instant
    if (speed <= 0.0f) speed = kBareHandSpeed;
    const float div = canHarvest ? float(kHarvestDivisor) : float(kNoHarvestDivisor);
    const float ticks = hardness * div / speed;
    int t = int(ticks);
    if (float(t) < ticks) ++t;                      // ceil, without <cmath>
    return t < 1 ? 1 : t;
}

// ── the recipe book ─────────────────────────────────────────────────────────

struct Stack { ItemId id = ItemId::None; int n = 0; };

// Four ingredient slots is enough for everything here; the widest real recipe
// is the netherite ingot at two distinct ingredients, and a bed at two.
struct Recipe {
    Stack out{};
    std::array<Stack, 4> in{};
    int ins = 0;
    Station station = Station::Inventory;
    [[nodiscard]] bool ok() const noexcept { return out.n > 0; }
};

// Built through a helper rather than braced directly so every one of the four
// slots is always initialised — partial aggregate init of a std::array inside a
// struct is exactly the shape -Wmissing-field-initializers complains about.
[[nodiscard]] constexpr std::array<Stack, 4> ing(Stack a = {}, Stack b = {},
                                                 Stack c = {}, Stack d = {}) noexcept {
    return std::array<Stack, 4>{{ a, b, c, d }};
}

// The material unit a tool or armour piece is built from.
[[nodiscard]] constexpr ItemId tool_material_item(Material m) noexcept {
    switch (m) {
        case Material::Wood:      return ItemId::Planks;
        case Material::Stone:     return ItemId::Cobblestone;
        case Material::Iron:      return ItemId::IronIngot;
        case Material::Gold:      return ItemId::GoldIngot;
        case Material::Diamond:   return ItemId::Diamond;
        case Material::Netherite: return ItemId::NetheriteIngot;
        default:                  return ItemId::None;
    }
}
[[nodiscard]] constexpr ItemId armour_material_item(ArmourMaterial m) noexcept {
    switch (m) {
        case ArmourMaterial::Leather:   return ItemId::Leather;
        case ArmourMaterial::Iron:      return ItemId::IronIngot;
        case ArmourMaterial::Gold:      return ItemId::GoldIngot;
        case ArmourMaterial::Diamond:   return ItemId::Diamond;
        case ArmourMaterial::Netherite: return ItemId::NetheriteIngot;
        // Chainmail has no crafting recipe in Java Edition at all — it is trade
        // and loot only. Returning None here is the honest answer, and
        // tool_recipe()/armour_recipe() turn it into an un-craftable Recipe.
        case ArmourMaterial::Chainmail: return ItemId::None;
        default:                        return ItemId::None;
    }
}

// Material units and sticks per tool. minecraft.wiki, Pickaxe/Axe/Shovel/Hoe/
// Sword. Every one of the five needs the 3x3 grid: even the shovel, whose
// recipe is only one column wide, is three ROWS tall.
[[nodiscard]] constexpr int tool_material_cost(ToolKind k) noexcept {
    constexpr int c[kToolKinds] = { 3, 3, 1, 2, 2 };  // pick, axe, shovel, hoe, sword
    return c[ix(k)];
}
[[nodiscard]] constexpr int tool_stick_cost(ToolKind k) noexcept {
    constexpr int c[kToolKinds] = { 2, 2, 2, 2, 1 };
    return c[ix(k)];
}
// Material units per armour piece. minecraft.wiki, Helmet/Chestplate/Leggings/
// Boots. 5 + 8 + 7 + 4 = 24 units for a full set.
[[nodiscard]] constexpr int armour_material_cost(ArmourSlot s) noexcept {
    constexpr int c[kArmourSlots] = { 5, 8, 7, 4 };
    return c[ix(s)];
}

[[nodiscard]] inline Recipe tool_recipe(ToolKind k, Material m) {
    // Netherite is not crafted. It is a smithing-table upgrade of the diamond
    // tool plus one netherite ingot. In 1.20+ the smithing recipe additionally
    // consumes a netherite upgrade smithing template, which is loot-only and so
    // has no recipe of its own; it is listed as an ingredient here.
    if (m == Material::Netherite) {
        Recipe r;
        r.out = { tool_item(k, Material::Netherite), 1 };
        r.in  = ing({ tool_item(k, Material::Diamond), 1 },
                    { ItemId::NetheriteIngot, 1 });
        r.ins = 2;
        r.station = Station::SmithingTable;
        return r;
    }
    Recipe r;
    r.out = { tool_item(k, m), 1 };
    r.in  = ing({ tool_material_item(m), tool_material_cost(k) },
                { ItemId::Stick, tool_stick_cost(k) });
    r.ins = 2;
    r.station = Station::CraftingTable;
    return r;
}

[[nodiscard]] inline Recipe armour_recipe(ArmourSlot s, ArmourMaterial m) {
    if (m == ArmourMaterial::Netherite) {
        Recipe r;
        r.out = { armour_item(s, ArmourMaterial::Netherite), 1 };
        r.in  = ing({ armour_item(s, ArmourMaterial::Diamond), 1 },
                    { ItemId::NetheriteIngot, 1 });
        r.ins = 2;
        r.station = Station::SmithingTable;
        return r;
    }
    const ItemId unit = armour_material_item(m);
    if (unit == ItemId::None) return Recipe{};      // chainmail: not craftable
    Recipe r;
    r.out = { armour_item(s, m), 1 };
    r.in  = ing({ unit, armour_material_cost(s) });
    r.ins = 1;
    r.station = Station::CraftingTable;
    return r;
}

// The non-tool, non-armour recipes. Station is derived from the real grid
// shape: anything that fits inside a 2x2 is Inventory. Shears look like they
// need a table and do not — the two ingots sit diagonally in a 2x2.
inline constexpr Recipe kBook[] = {
    // wood chain
    { { ItemId::Planks, 4 },        ing({ ItemId::OakLog, 1 }),        1, Station::Inventory },
    { { ItemId::Stick, 4 },         ing({ ItemId::Planks, 2 }),        1, Station::Inventory },
    { { ItemId::CraftingTable, 1 }, ing({ ItemId::Planks, 4 }),        1, Station::Inventory },
    { { ItemId::Torch, 4 },         ing({ ItemId::Coal, 1 },
                                        { ItemId::Stick, 1 }),         2, Station::Inventory },
    // 3x3 utility
    { { ItemId::Furnace, 1 },       ing({ ItemId::Cobblestone, 8 }),   1, Station::CraftingTable },
    { { ItemId::Chest, 1 },         ing({ ItemId::Planks, 8 }),        1, Station::CraftingTable },
    { { ItemId::WoodenDoor, 3 },    ing({ ItemId::Planks, 6 }),        1, Station::CraftingTable },
    { { ItemId::Bed, 1 },           ing({ ItemId::Wool, 3 },
                                        { ItemId::Planks, 3 }),        2, Station::CraftingTable },
    { { ItemId::Boat, 1 },          ing({ ItemId::Planks, 5 }),        1, Station::CraftingTable },
    { { ItemId::Bucket, 1 },        ing({ ItemId::IronIngot, 3 }),     1, Station::CraftingTable },
    { { ItemId::Shears, 1 },        ing({ ItemId::IronIngot, 2 }),     1, Station::Inventory },
    { { ItemId::FlintAndSteel, 1 }, ing({ ItemId::IronIngot, 1 },
                                        { ItemId::Flint, 1 }),         2, Station::Inventory },
    { { ItemId::Bread, 1 },         ing({ ItemId::Wheat, 3 }),         1, Station::CraftingTable },
    { { ItemId::BlockOfCoal, 1 },   ing({ ItemId::Coal, 9 }),          1, Station::CraftingTable },
    { { ItemId::GoldenApple, 1 },   ing({ ItemId::GoldIngot, 8 },
                                        { ItemId::Apple, 1 }),         2, Station::CraftingTable },
    { { ItemId::NetheriteIngot, 1 },ing({ ItemId::NetheriteScrap, 4 },
                                        { ItemId::GoldIngot, 4 }),     2, Station::CraftingTable },
};
constexpr int kBookSize = int(sizeof(kBook) / sizeof(kBook[0]));

// One lookup for every craftable thing in the file — plain, tool or armour.
// The point of this function is that a caller never writes its own switch: if a
// recipe changes it changes here and nowhere else.
[[nodiscard]] inline Recipe recipe_for(ItemId want) {
    if (is_tool(want))   return tool_recipe(tool_kind_of(want), tool_material_of(want));
    if (is_armour(want)) return armour_recipe(armour_slot_of(want), armour_material_of(want));
    for (int i = 0; i < kBookSize; ++i)
        if (kBook[i].out.id == want) return kBook[i];
    return Recipe{};
}

// Enumeration, so a test or a planner can walk the whole book without knowing
// how it is stored. Order: plain recipes, then tools, then armour.
constexpr int kToolRecipes   = kToolKinds * kMaterials;         // 30
constexpr int kArmourRecipes = kArmourSlots * kArmourMaterials; // 24
constexpr int kRecipeCount   = kBookSize + kToolRecipes + kArmourRecipes;

[[nodiscard]] inline Recipe recipe_at(int i) {
    if (i < 0 || i >= kRecipeCount) return Recipe{};
    if (i < kBookSize) return kBook[i];
    i -= kBookSize;
    if (i < kToolRecipes)
        return tool_recipe(ToolKind(i / kMaterials), Material(i % kMaterials));
    i -= kToolRecipes;
    return armour_recipe(ArmourSlot(i / kArmourMaterials),
                         ArmourMaterial(i % kArmourMaterials));
}

// ── smelting ────────────────────────────────────────────────────────────────
//
// minecraft.wiki, Smelting and Furnace. "A furnace runs at a speed of one item
// every 200 game tick". Blast furnace and smoker are half that, and each is
// restricted to a subset of the recipes — ores and food respectively.
constexpr int kSmeltTicks         = 200;
constexpr int kBlastFurnaceTicks  = 100;
constexpr int kSmokerTicks        = 100;

struct SmeltResult { ItemId out = ItemId::None; float xp = 0.0f; };

// Experience values are the wiki's per-item smelting XP. 0.35 is the food value;
// it was read directly for steak and baked potato and applied to the rest of the
// cooked-food recipes on the wiki's statement that food smelting yields 0.35 —
// the individual fish and mutton rows were not separately sourced, and that is
// recorded here rather than hidden.
[[nodiscard]] inline SmeltResult smelt(ItemId in) {
    switch (in) {
        // ore chain, 1.17+ raw ores
        case ItemId::RawIron:       return { ItemId::IronIngot,      0.7f };
        case ItemId::RawGold:       return { ItemId::GoldIngot,      1.0f };
        // ore chain, the pre-1.17 ore BLOCKS, which this project's BlockWorld
        // still generates. Same product. The XP is carried over from the raw-ore
        // rows above rather than separately sourced — 1.17 moved the recipe from
        // the block to the raw item without changing the reward, but the
        // pre-1.17 row itself was not read, so treat these two XP values as
        // inferred rather than cited.
        case ItemId::IronOre:       return { ItemId::IronIngot,      0.7f };
        case ItemId::GoldOre:       return { ItemId::GoldIngot,      1.0f };
        case ItemId::AncientDebris: return { ItemId::NetheriteScrap, 2.0f };
        // stone chain
        case ItemId::Sand:          return { ItemId::Glass,          0.1f };
        case ItemId::Cobblestone:   return { ItemId::Stone,          0.1f };
        case ItemId::ClayBall:      return { ItemId::Brick,          0.3f };  // wiki, Brick
        // wood chain
        case ItemId::OakLog:        return { ItemId::Charcoal,       0.15f };
        // misc
        case ItemId::Cactus:        return { ItemId::GreenDye,       1.0f };
        case ItemId::Kelp:          return { ItemId::DriedKelp,      0.1f };
        // food
        case ItemId::RawBeef:       return { ItemId::Steak,          0.35f };
        case ItemId::RawPorkchop:   return { ItemId::CookedPorkchop, 0.35f };
        case ItemId::RawChicken:    return { ItemId::CookedChicken,  0.35f };
        case ItemId::RawMutton:     return { ItemId::CookedMutton,   0.35f };
        case ItemId::RawRabbit:     return { ItemId::CookedRabbit,   0.35f };
        case ItemId::RawCod:        return { ItemId::CookedCod,      0.35f };
        case ItemId::RawSalmon:     return { ItemId::CookedSalmon,   0.35f };
        case ItemId::Potato:        return { ItemId::BakedPotato,    0.35f };
        default:                    return {};
    }
}
[[nodiscard]] inline bool smeltable(ItemId in) { return smelt(in).out != ItemId::None; }

// Fuel burn time in ticks. minecraft.wiki, Smelting (Fuel) and
// Template:Smelting table. Every value below was read off that table.
//
// The 0 return is "not a fuel", which is most items. Note WoodenSlab at 150 and
// Wool at 100 are Java-only; Bedrock differs and this file is Java.
[[nodiscard]] inline int fuel_burn_ticks(ItemId f) {
    if (is_tool(f)) {
        // Wooden tools burn for 200 ticks — exactly one smelt. Nothing else in
        // the tool grid is a fuel; a stone pickaxe is just a stone pickaxe.
        return tool_material_of(f) == Material::Wood ? 200 : 0;
    }
    switch (f) {
        case ItemId::LavaBucket:     return 20000;   // 1000 s, 100 items
        case ItemId::BlockOfCoal:    return 16000;   //  800 s,  80 items
        case ItemId::DriedKelpBlock: return  4000;   //  200 s,  20 items
        case ItemId::BlazeRod:       return  2400;   //  120 s,  12 items
        case ItemId::Coal:           return  1600;   //   80 s,   8 items
        case ItemId::Charcoal:       return  1600;   //   80 s,   8 items
        case ItemId::Boat:           return  1200;   //   60 s,   6 items
        case ItemId::OakLog:         return   300;   //   15 s, 1.5 items
        case ItemId::Planks:         return   300;
        case ItemId::CraftingTable:  return   300;
        case ItemId::Chest:          return   300;
        case ItemId::Bookshelf:      return   300;
        case ItemId::Ladder:         return   300;
        case ItemId::WoodenSlab:     return   150;   //  7.5 s, Java only
        case ItemId::WoodenDoor:     return   200;   //   10 s,   1 item
        case ItemId::Sign:           return   200;
        // 300, not 200. Both were entered as 200 and neither was covered by the
        // fuel assertions in the test — five of the twenty-one fuels were left
        // unasserted and the one wrong value was among them, which is the whole
        // argument for asserting a table exhaustively rather than
        // representatively.
        case ItemId::Bow:            return   300;   //   15 s, 1.5 items
        case ItemId::FishingRod:     return   300;   //   15 s, 1.5 items
        case ItemId::Stick:          return   100;   //    5 s, 0.5 items
        case ItemId::Sapling:        return   100;
        case ItemId::Wool:           return   100;   //    5 s, Java only
        case ItemId::Bamboo:         return    50;   //  2.5 s, 0.25 items
        default:                     return     0;
    }
}
[[nodiscard]] inline bool is_fuel(ItemId f) { return fuel_burn_ticks(f) > 0; }

// How many items one unit of this fuel can smelt in a regular furnace. Float,
// not int: a stick is 0.5 and the half is real — fuel burns down whether or not
// there is anything in the input slot.
[[nodiscard]] inline float items_smelted(ItemId f) {
    return float(fuel_burn_ticks(f)) / float(kSmeltTicks);
}
// Ticks to smelt n items, ignoring fuel. Linear, which is the whole model.
[[nodiscard]] inline int smelt_ticks_for(int n) { return n < 0 ? 0 : n * kSmeltTicks; }
// Whole units of a fuel needed to smelt n items, rounding up — you cannot feed
// a furnace half a piece of coal.
[[nodiscard]] inline int fuel_units_for(int n, ItemId f) {
    const int burn = fuel_burn_ticks(f);
    if (burn <= 0 || n <= 0) return 0;
    const int need = n * kSmeltTicks;
    return (need + burn - 1) / burn;
}

// ── food ────────────────────────────────────────────────────────────────────
//
// minecraft.wiki, Food. `hunger` is the hunger points restored (the bar is 20,
// drawn as 10 drumsticks). `mod` is the game's saturationModifier field, and
// the saturation actually restored is hunger * mod * 2 — see note 2 in the
// header for why that 2 is easy to lose.
struct FoodStats {
    int   hunger = 0;
    float mod    = 0.0f;
    [[nodiscard]] constexpr bool edible() const noexcept { return hunger > 0; }
    [[nodiscard]] constexpr float saturation() const noexcept {
        return float(hunger) * mod * 2.0f;
    }
};

[[nodiscard]] inline FoodStats food_stats(ItemId i) {
    switch (i) {
        // hunger, saturationModifier          -> published saturation restored
        case ItemId::Bread:          return { 5, 0.6f };   //  6.0
        case ItemId::Apple:          return { 4, 0.3f };   //  2.4
        case ItemId::GoldenApple:    return { 4, 1.2f };   //  9.6
        case ItemId::MelonSlice:     return { 2, 0.3f };   //  1.2
        case ItemId::Cookie:         return { 2, 0.1f };   //  0.4
        case ItemId::RottenFlesh:    return { 4, 0.1f };   //  0.8
        case ItemId::Carrot:         return { 3, 0.6f };   //  3.6
        case ItemId::Potato:         return { 1, 0.3f };   //  0.6
        case ItemId::BakedPotato:    return { 5, 0.6f };   //  6.0
        case ItemId::DriedKelp:      return { 1, 0.3f };   //  0.6  (Java)
        case ItemId::RawBeef:        return { 3, 0.3f };   //  1.8
        case ItemId::Steak:          return { 8, 0.8f };   // 12.8
        case ItemId::RawPorkchop:    return { 3, 0.3f };   //  1.8
        case ItemId::CookedPorkchop: return { 8, 0.8f };   // 12.8
        case ItemId::RawChicken:     return { 2, 0.3f };   //  1.2
        case ItemId::CookedChicken:  return { 6, 0.6f };   //  7.2
        case ItemId::RawMutton:      return { 2, 0.3f };   //  1.2
        case ItemId::CookedMutton:   return { 6, 0.8f };   //  9.6  (6, not 8)
        case ItemId::RawRabbit:      return { 3, 0.3f };   //  1.8
        case ItemId::CookedRabbit:   return { 5, 0.6f };   //  6.0
        case ItemId::RawCod:         return { 2, 0.1f };   //  0.4
        case ItemId::CookedCod:      return { 5, 0.6f };   //  6.0
        case ItemId::RawSalmon:      return { 2, 0.1f };   //  0.4
        case ItemId::CookedSalmon:   return { 6, 0.8f };   //  9.6  (6, not 8)
        default:                     return {};
    }
}
[[nodiscard]] inline bool edible(ItemId i) { return food_stats(i).edible(); }

// The hunger bar and the saturation ceiling. minecraft.wiki, Hunger: saturation
// can never exceed the current food level, which is what makes a big meal on a
// nearly-full bar wasteful.
constexpr int kMaxHunger = 20;

// Eating, as the game applies it: hunger is capped at 20, and saturation is
// then capped at the NEW hunger value. Returns the pair after eating.
struct Nourishment { int hunger = 0; float saturation = 0.0f; };
[[nodiscard]] inline Nourishment eat(Nourishment now, ItemId food) {
    const FoodStats f = food_stats(food);
    if (!f.edible()) return now;
    Nourishment out;
    out.hunger = now.hunger + f.hunger;
    if (out.hunger > kMaxHunger) out.hunger = kMaxHunger;
    out.saturation = now.saturation + f.saturation();
    if (out.saturation > float(out.hunger)) out.saturation = float(out.hunger);
    return out;
}

// ── bridge to bench::BlockWorld ─────────────────────────────────────────────
//
// blockworld.hpp's Block enum is the world's alphabet; ItemId is this file's.
// The bridge is here rather than in blockworld.hpp because blockworld.hpp must
// not know about crafting, and because a caller that has a Block in hand and
// wants to know what it smelts into should not have to write the mapping twice.

[[nodiscard]] inline ItemId item_from_block(int b) {
    switch (b) {
        case bench::Wood:        return ItemId::OakLog;
        case bench::Plank:       return ItemId::Planks;
        case bench::Cobble:      return ItemId::Cobblestone;
        case bench::Stone:       return ItemId::Cobblestone;  // stone drops cobble
        case bench::Sand:        return ItemId::Sand;
        case bench::Gravel:      return ItemId::Gravel;
        case bench::Coal:        return ItemId::Coal;         // ore drops coal
        case bench::IronOre:     return ItemId::IronOre;
        case bench::GoldOre:     return ItemId::GoldOre;
        case bench::DiamondOre:  return ItemId::Diamond;      // ore drops diamond
        case bench::Obsidian:    return ItemId::Obsidian;
        case bench::Table:       return ItemId::CraftingTable;
        case bench::Furnace:     return ItemId::Furnace;
        case bench::Chest:       return ItemId::Chest;
        case bench::Torch:       return ItemId::Torch;
        case bench::Wheat:       return ItemId::Wheat;
        default:                 return ItemId::None;
    }
}

// Is a tool of this material the right TYPE for this block, per blockworld's
// BlockRule.pickaxe flag? Only the pickaxe/non-pickaxe split exists over there,
// so that is all this can honestly answer.
[[nodiscard]] inline bool correct_tool_for(int block, ToolKind k) {
    return bench::block_rule(block).pickaxe ? (k == ToolKind::Pickaxe) : true;
}

// Ticks to break a world block with a given tool, using the game's speed and
// harvest tables rather than blockworld's single conflated tier.
//
// This DELIBERATELY disagrees with bench::break_ticks() for gold, and only for
// gold: over there the argument is a harvest tier and the speed is looked up
// from it, so a golden pickaxe cannot be expressed at all. Here a golden
// pickaxe is speed 12 and harvest level 0, which makes it the fastest tool in
// the game on stone and useless on diamond ore. test_craft.cpp measures both
// halves of that so the divergence is on the record instead of being rounded
// away by a later reader who assumes one of the two files is simply wrong.
[[nodiscard]] inline int break_ticks_for(int block, ToolKind k, Material m) {
    const bench::BlockRule r = bench::block_rule(block);
    const bool right = correct_tool_for(block, k);
    const float speed = right ? tool_mining_speed(m) : kBareHandSpeed;
    const bool canHarvest = right && blockworld_tier(m) >= r.tier;
    return break_ticks(r.hardness, speed, canHarvest);
}

// Does this material's tool get a drop from this block? blockworld's tiers.
[[nodiscard]] inline bool drops_for(int block, ToolKind k, Material m) {
    if (!correct_tool_for(block, k)) return bench::block_rule(block).tier == 0;
    return blockworld_tier(m) >= bench::block_rule(block).tier;
}

} // namespace bench::craft
