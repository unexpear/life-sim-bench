// voxelcity.hpp — a town of agents in a block world, each learning its own.
//
// WHAT THIS IS, AND WHAT voxelcraft.hpp IS. They are deliberately separate.
// voxelcraft measures ONE thing and measures it carefully: how much a tech tree
// costs an agent that does not know the order, against one that does. Its three
// policies are scripted, scripted-with-noise and random — there is no network in
// it at all — and the published table (115 steps to wood against 631, 227 to
// diamond against 11,437) is only meaningful because nothing about that setup
// has moved. Bolting a learning population onto it would have destroyed the
// measurement it exists to make. So this is a second sim that shares the world
// and the recipes and asks the harder question:
//
//   Given many agents, each with its own network and its own experience, what
//   does a population learn that a single agent does not?
//
// THE LEARNER. One Dqn per agent — Mnih et al., Nature 518 (2015) for replay and
// the target network, van Hasselt et al., AAAI 2016 for the double update. Every
// agent owns its network, its replay buffer and its own RNG stream. That last
// part is not a detail: agents sharing a stream explore in lockstep, and a
// "population result" from correlated explorers is a measurement of the sampler.
//
// WHY A POPULATION IS NOT JUST N TIMES ONE AGENT. Three things here are genuinely
// collective, and each is a knob so it can be switched off and measured:
//   · shared world     — one agent mining a vein means the next one cannot
//   · shared buffer    — optional pooling of transitions across agents
//   · placed structure — roads and buildings persist, so one agent's work is
//                        another's terrain
//
// HONEST LIMITS, up front:
//   · This is not Minecraft. No redstone, no nether, no enchanting, no combat
//     an agent can fight back in — a mob hits an agent and the agent can only
//     leave. The tech tree, the ore depths and the block hardness are
//     recognisable rather than accurate.
//     (This clause used to read "No mobs, no day/night, no physics beyond
//     gravity and drowning". All three are now false; see WHAT THE WORLD DOES
//     ON ITS OWN below. Left visible rather than quietly deleted, because a
//     stale limitation is worse than none: a reader who believes it will
//     misread every plot in the file.)
//   · "City" here means placed blocks that persist and are counted as structure.
//     No agent has a plan for a building. Whether anything worth calling a town
//     emerges is the question, not the premise — and the metrics are written so
//     the answer is allowed to be no.
//   · A learned policy is compared against scripted and random baselines on the
//     same worlds. Any claim that learning helps has to beat those, and the
//     baselines are in the same sim so they cannot drift apart.

// ─────────────────────────────────────────────────────────────────────────────
// WHAT THE WORLD DOES ON ITS OWN, added after the five world subsystems landed.
//
// The "HONEST LIMITS" note above used to read "No mobs, no day/night, no
// physics beyond gravity and drowning". Five of those are now false, and the
// five subsystems that made them false are owned here and driven once per world
// tick:
//
//   light.hpp   sky and block light, 0..15, incremental. Every block this file
//               mutates is handed to LightEngine::block_changed. Light darkens
//               the render and is part of the observation.
//   fluids.hpp  Minecraft's fluid scheduler. Dig into the sea and it floods;
//               dig under sand and it falls.
//   biomes.hpp  multi-noise biome assignment, driving surface block, filler and
//               tree density at generation.
//   craft.hpp   the recipe, smelting, fuel and durability tables. Iron is now a
//               200-tick smelt that burns fuel, and pickaxes wear out.
//   mobs.hpp    1.18+ spawn rules, the published caps, and a state machine that
//               chases and hits agents.
//
// THE ONE INVARIANT THAT MATTERS. Light is the only global field: a cell that
// changes without LightEngine hearing about it is stale forever, and stale light
// is a lying spawn predicate. So this file has exactly ONE way to write a block
// — setBlock() — and everything else (fluids, falling sand, lava mixing) writes
// through BlockWorld's change recorder, which setBlock drains. There is no
// second path. That is deliberate: "which of the fourteen mutation sites did I
// forget" is not a question anyone should have to answer twice.
//
// COST. Nothing added here scales with world volume per tick. Light is an
// incremental flood bounded by 15 steps, fluids are queue-driven, mobs are a
// capped population, biomes are baked once over the n x n footprint. Measured
// end to end: 0.047 ms a tick at 96x128x96 and 0.120 ms at 256x128x256 — 7.1x
// the volume for 2.6x the time.
//
// The one per-tick term that DOES scale with volume is randomTicks(), which
// predates all of this and is Minecraft's own rule (three blocks per 16x16x16
// section per tick, so 864 draws at n=96 and 6,144 at n=256). It is inherent to
// the model rather than a cost this work introduced, and it is the reason the
// two figures above are not flat. The only O(volume) passes are at generation:
// LightEngine::rebuild and Fluids::rebuild, both once per world.

#pragma once
#include "../sim.hpp"
#include "../render/voxel.hpp"
#include "../world/blockworld.hpp"
#include "../world/light.hpp"
#include "../world/fluids.hpp"
#include "../world/biomes.hpp"
#include "../world/craft.hpp"
#include "../world/mobs.hpp"
#include "../learn/dqn.hpp"
#include "../learn/dynscript.hpp"
#include "../rng.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

namespace bench {

// ── the one light field, in the shape mobs.hpp asks for ─────────────────────
//
// mobs.hpp deliberately does not depend on light.hpp — it is built and tested
// on its own — so it asks its two light questions through `LightView` and lets
// whoever owns the world answer them. This is that answer, and it forwards
// straight to the LightEngine that every block mutation in this file already
// feeds. One pointer, no copy of anything, nothing to keep in step.
//
// WHAT THIS REPLACED, because the bug is worth naming. There used to be a
// second field here: a `LightField mobLight_`, mobs.hpp's own taxicab
// approximation, rebuilt once after generate() and thereafter told only about
// torches. Mine out a cave and it still believed the cave was solid rock — for
// the rest of the run, because that structure has no way to lose or re-derive a
// source. Two fields describing the same world, one of them on the end of the
// mutation path and one of them not, is a lie that gets worse the longer the
// sim runs. There is now exactly one.
//
// COST. An indirect call and a byte out of an array, against a scan of up to
// nine source buckets before — the interface is the cheaper of the two, and
// test_mobs' 200k-tick benchmark did not move (15.2 -> 14.8 ns per mob-tick,
// which is noise either way).
class EngineLightView final : public LightView {
public:
    explicit EngineLightView(const LightEngine& e) : e_(&e) {}

    // The world argument is unused: LightEngine already holds the BlockWorld it
    // was built over, and its sky light is flood-filled rather than read off the
    // heightmap. Named-but-unused rather than dropped, so the signature still
    // reads as the interface's.
    [[nodiscard]] int internal_light(const BlockWorld&, int x, int y, int z,
                                     int timeOfDay) const override {
        return e_->internal_light(x, y, z, sky_darken(timeOfDay));
    }
    [[nodiscard]] bool hostile_light_ok(const BlockWorld&, int x, int y, int z,
                                        int timeOfDay) const override {
        return e_->light_allows_hostile_spawn(x, y, z, sky_darken(timeOfDay));
    }

private:
    const LightEngine* e_;
};

class VoxelCity final : public Sim {
public:
    // Actions. Six moves, then the things that change the world or the agent.
    // ONE action space, used by all three policies, and it is a PLAYER'S.
    //
    // It used to be six primitive moves plus mine/craft/place, which made the
    // comparison unwinnable rather than hard: the scripted policy searched a
    // radius of 20 for what it needed and walked there mining through rock,
    // while the learner saw six adjacent blocks and shuffled one step at a
    // time. It was not out-thought, it was blindfolded, and no amount of
    // training fixes an agent that cannot perceive its goal or travel to it.
    //
    // These are OPTIONS in the sense of Sutton, Precup & Singh (AIJ 112, 1999):
    // each Go* runs the same turn-walk-and-dig behaviour a person uses, so both
    // policies now have identical means and differ only in the decision that is
    // actually in dispute — WHICH resource to pursue, and when to stop
    // gathering and craft. That is the tech tree, and it is worth learning.
    enum Action {
        GoWood = 0, GoStone, GoCoal, GoIron, GoDiamond,
        Mine, Craft, PlaceBlock, Eat, Explore, Ascend,
        kActions
    };
    static constexpr std::uint8_t kGoTarget[5] = { Wood, Stone, Coal, IronOre, DiamondOre };

    // ── possession: the human as a policy ───────────────────────────────────
    //
    // A possessed agent is a HUMAN BASELINE. This sim has spent its whole life
    // comparing scripted (mean rung ~5) against a deep Q-network (~1) against
    // random, with nobody having any idea what a PERSON reaches — so every
    // claim about the learner has been made against a bar of unknown height.
    // Possession is therefore built as a policy and not as a toy: the possessed
    // agent goes through agentStep like every other, earns the same shaped
    // reward, is committed to the same multi-tick mining options, and lands in
    // the same metrics(). Swapping the policy knob to `possessed` changes who
    // chooses, and nothing else.
    //
    // A possession CODE is a superset of the shared action space:
    //
    //   0 .. kActions-1     the eleven actions every other policy draws from
    //   kMoveBase+0..5      a direct movement primitive, one of the six faces
    //   kStand              hold position for a tick
    //
    // The six movement codes are the honest part of the asymmetry and the
    // reason they are counted separately rather than folded into the histogram:
    // a person wants to steer, and Explore (which picks its own heading) is not
    // steering. They sit BELOW the shared action space — applyPrimitive is what
    // Explore and every Go* option are themselves built out of — so a human is
    // not given a power the machines lack, only a finer grain of the same one.
    // possessed_moves() reports how much of a run was spent down there, which
    // is what stops that difference being invisible.
    static constexpr int kMoveBase = kActions;
    static constexpr int kStand    = kActions + 6;
    static constexpr int kPossCodes = kActions + 7;
    [[nodiscard]] static bool poss_code_valid(int c) { return c >= 0 && c < kPossCodes; }
    static const char* poss_code_name(int c) {
        static const char* act[kActions] = {
            "go wood", "go stone", "go coal", "go iron", "go diamond",
            "mine", "craft", "place", "eat", "explore", "ascend" };
        static const char* mv[6] = { "move -x", "move +x", "move -z", "move +z",
                                     "move up", "move down" };
        if (c >= 0 && c < kActions)  return act[c];
        if (c >= kMoveBase && c < kMoveBase + 6) return mv[c - kMoveBase];
        if (c == kStand) return "stand";
        return "?";
    }

    // ── the recording ───────────────────────────────────────────────────────
    //
    // Without this a human run is an anecdote. One entry per DECISION POINT of
    // the possessed agent — not per tick, because a committed mine or smelt is
    // one decision that runs for up to 300 of them, and not per keystroke,
    // because the standing order repeats between keystrokes and what has to be
    // reproducible is what the agent DID.
    //
    // The tick is stored next to the code, and it is the part that earns its
    // keep. Replaying the codes alone would reproduce a score whether or not
    // the world underneath was reproducible; storing WHEN each decision
    // happened means a replay that reaches decision k on a different tick is
    // caught at the moment it diverges instead of being quietly rescored. That
    // check has a name in the suite and it is meant to fail loudly.
    struct TapeEntry {
        long long tick = 0;
        int  code = kStand;
        bool human = false;      // false = the idle timeout handed it back
    };
    struct PossessionTape {
        // Everything needed to rebuild the world this was recorded in. A tape
        // that does not carry its own seed is a tape you can only replay by
        // remembering how you were sitting at the time.
        int  seed = 1, size = 0, agents = 8, idle = 2400, index = 0;
        // roles and hunger were NOT carried, and both measurably change a run:
        // roles rebuilds the whole town's objectives, hunger scales every
        // exhaustion cost. A tape omitting them replays identically only when
        // those knobs happen to already match — a guarantee that holds in
        // testing and fails in use. The value of a recorded human baseline is
        // that it reproduces; a header recording only some of its world is not
        // a header, it is a hope.
        int   roles  = 1;
        float hunger = 1.f;
        long long ticks = 0;              // how long the recorded run ran
        std::vector<TapeEntry> entries;
        [[nodiscard]] bool empty() const { return entries.empty(); }
    };

    // ── the rulebase that dynamic scripting learns weights over ─────────────
    //
    // Each rule is a CONDITION and the action it fires. A generated script is a
    // subset of these in authored priority order; the agent runs the first rule
    // whose condition holds. This is exactly the shape of the scripted policy
    // that already reaches the top of the tech tree — the difference is that
    // which rules are in the list, and therefore which get a chance to fire, is
    // learned rather than fixed.
    //
    // The conditions are deliberately the same predicates a person would write.
    // Nothing here is clever: the learning is in the selection, and the whole
    // point of the method is that hand-authored rules are the part that already
    // works.
    enum Cond {
        CAlways = 0, CHungry, CCanCraft, CSurplus, CBuried, CNoWood, CNeedStone,
        CNeedCoal, CNeedIron, CCanDiamond, CHurt, CSeeWood, CSeeIron, CNearTable,
        kConds
    };
    struct Rule { std::uint8_t cond; std::uint8_t action; int priority; const char* text; };

    // Priority orders a script once its rules are drawn. Survival first, then
    // conversion of what is already carried, then acquisition, then filler.
    static const std::vector<Rule>& rulebase() {
        static const std::vector<Rule> rb = {
            { CHungry,    Eat,        0, "if starving and carrying bread, eat" },
            { CHurt,      Ascend,     1, "if badly hurt, get back to the surface" },
            { CBuried,    Ascend,     2, "if below the surface, climb out" },
            { CCanCraft,  Craft,      3, "if anything can be crafted, craft it" },
            { CNearTable, Craft,      3, "if standing at a table, craft" },
            { CSurplus,   PlaceBlock, 4, "if carrying spare stone or dirt, build with it" },
            { CNoWood,    GoWood,     5, "if short of wood, go and cut some" },
            { CNeedStone, GoStone,    5, "if short of cobble and able to mine it, go and get it" },
            { CNeedCoal,  GoCoal,     6, "if short of coal and able to mine it, go and get it" },
            { CNeedIron,  GoIron,     6, "if short of iron and able to mine it, go and get it" },
            { CCanDiamond,GoDiamond,  7, "if holding an iron pickaxe, go for diamond" },
            { CSeeWood,   GoWood,     8, "if wood is in range, go to it" },
            { CSeeIron,   GoIron,     8, "if iron is in range, go to it" },
            { CAlways,    GoWood,     9, "cut wood" },
            { CAlways,    GoStone,    9, "mine stone" },
            { CAlways,    GoCoal,     9, "mine coal" },
            { CAlways,    GoIron,     9, "mine iron" },
            { CAlways,    Mine,      10, "break whatever is in front" },
            { CAlways,    PlaceBlock,10, "put a block down" },
            { CAlways,    Craft,     10, "try to craft" },
            { CAlways,    Explore,   11, "wander" },
        };
        return rb;
    }

    // ── who each agent is ───────────────────────────────────────────────────
    //
    // Eight identical maximisers each trying to solve an eleven-rung chain
    // alone is not a town, and it is also the reason the learning was hopeless:
    // every agent faced the same very long credit-assignment problem and none
    // of them faced a short one. A settlement is a DIVISION OF LABOUR, and that
    // is the point here twice over — it is what was actually wanted, and it
    // shortens each agent's horizon to something a network can learn.
    //
    // Every role still sees the whole town's progress and is still paid for it,
    // so this is specialisation rather than four separate simulations sharing a
    // map: the forester is rewarded for wood, but also for the town reaching a
    // rung its wood made possible.
    enum Role { Forester = 0, Miner, Builder, Provisioner, kRoles };
    static const char* role_name(int r) {
        switch (r) {
            case Forester:    return "forester";
            case Miner:       return "miner";
            case Builder:     return "builder";
            case Provisioner: return "provisioner";
            default:          return "?";
        }
    }

    // ── the agent is a PLAYER, not an optimiser with six arms ───────────────
    //
    // A person faces one way and breaks the block they are looking at. The
    // first version let an agent survey all six neighbours and take whichever
    // was worth most, which is not a player — it is a machine with a hand on
    // every side. Facing costs a tick to turn and makes "walk to the tree, look
    // at it, cut it" the actual sequence, which is also why the Go* options
    // exist rather than raw movement.
    //
    // Reach is one block here against the game's 4.5. That is the honest
    // simplification in this model and the only one in the mining rules: it
    // turns "look at that ore across the cavern and mine it" into "walk over
    // and mine it", which changes how long things take and not what is
    // possible.
    static constexpr int kStack     = 64;   // items to a stack, as published
    static constexpr int kSlots     = 36;   // inventory slots, as published
    static constexpr int kInv = kBlocks;      // block counts an agent can carry

    struct Agent {
        int x = 0, y = 0, z = 0;
        float health = 20.f, food = 20.f;   // 10 hearts, 10 drumsticks
        int   tier = 0;                        // best pickaxe held
        int   rung = 0;                        // items crafted, in tree order
        // The HIGH-WATER MARK, which death does not take away.
        //
        // `rung` and `tier` reset when an agent respawns, because a corpse does
        // not keep its pickaxe. Reporting the mean of those measures how far the
        // average agent has got IN ITS CURRENT LIFE, so a longer run scores
        // LOWER — more agents are caught in infancy. Every cross-policy
        // comparison in this project taken at different tick counts was
        // measuring that artefact: the identical scripted policy read 4.50 at
        // 8,000 ticks and 0.00 at 40,000.
        int   bestRung = 0, bestTier = 0;
        // The RATE, which is what actually separates policies.
        //
        // `bestRung` fixed a real defect and introduced the opposite one. A
        // lifetime high-water mark is a MAXIMUM OVER LIVES, so a policy that
        // flails, dies, respawns and eventually stumbles one rung up the tree
        // records that rung forever — it is scored on its luckiest attempt, and
        // more attempts is strictly better. Measured over eight seeds that took
        // random from 1.00 to 4.72 against scripted's 4.56: the benchmark had
        // stopped measuring whether knowing the tech tree is worth anything.
        //
        // Summing the rung each life ENDED on and dividing by lives asks the
        // discriminating question instead: how far does a typical life get?
        // Luck no longer accumulates, and survival is still rewarded, because a
        // long life at rung 5 is one life scoring 5 rather than six lives
        // averaging 0.8.
        int   rungLifeSum = 0;      // rung reached by each life that has ended
        int   livesEnded  = 0;
        int   epTicks = 0;                     // ticks in the current script's episode
        bool  alive = true;
        int   mined = 0, placed = 0, crafted = 0, deaths = 0;
        int   stepsAlive = 0;
        std::array<int, kInv>   blocks{};
        std::array<int, kItems> items{};
        Dqn   brain;
        Rng   rng{1};
        std::vector<float> lastObs;
        int   lastAction = -1;
        float episodeReward = 0.f;
        // A remembered destination. Searching for the nearest resource every
        // tick is both wasteful and indecisive: the shell scan is O(r^3) and
        // its WORST case is when nothing is found, which is exactly the case
        // that repeats. Committing to a target made the informed policy ~600x
        // cheaper per tick and, less obviously, better — an agent that re-picks
        // its destination every tick dithers between two equidistant trees.
        int   tgtX = -1, tgtY = -1, tgtZ = -1;
        std::uint8_t tgtWhat = 0;
        int   tgtAge = 0;
        int   searchCooldown = 0;   // do not re-run a search that just failed
        int   facing = 0;           // 0 -x, 1 +x, 2 -z, 3 +z, 4 up, 5 down
        int   role = 0;             // what this one does for a living
        DynamicScript book;         // its rulebase, and what it has learned about it
        float lifeStartScore = 0.f; // for the per-life fitness
        // Where the agent stood when the CURRENT script was drawn. Fitness is
        // scored on the difference, or a script inherits its predecessor's
        // ladder. See lifeFitness().
        int   scriptStartRung = 0, scriptStartTier = 0;
        float scriptStartPot = 0.f;   // techPotential() when the script was drawn
        int   scriptStartPlaced = 0;
        bool  building = false;     // inside the build band; see scriptedAction
        std::uint64_t lastSig = 0;  // everything a working agent changes
        int   idleFor = 0;          // consecutive ticks that changed none of it
        // A bearing to the nearest of each tracked resource — the same thing
        // the scripted policy gets from its search, refreshed on a stagger
        // because a shell scan is O(r^3) and its worst case is failure, which
        // is the case that repeats.
        struct Sense { int x = -1, y = -1, z = -1; bool found = false; };
        std::array<Sense, 5> sense{};
        int   senseAge = 0;
        int   miningX = -1, miningY = -1, miningZ = -1, miningLeft = 0;
        int   airTicks = 0;         // ticks submerged, for drowning
        int   hurtCooldown = 0;     // the game's 10-tick invulnerability window
        float lastHurt = 0.f;       // the hit the window is currently holding
        float exhaustion = 0.f;     // fills to 4.0, then spends saturation or food
        float saturation = 5.f;     // the hidden buffer that drains before food does
        // A committed action in progress, and what it has earned so far.
        int   busy = 0;
        int   busyAction = -1;
        int   busyTicks = 0;        // how long the committed action ran, for gamma^k
        float busyDiscount = 1.f;
        float busyReward = 0.f;
        std::vector<float> busyObs;
        int   roamDir = 0;          // persistent heading, so roaming travels
        // A distant place to walk to when nothing is in sense range.
        int   wanderX = -1, wanderZ = -1, wanderAge = 0;
        // Uses left in each tool this agent holds, from craft.hpp's published
        // durability table. Indexed like `items` so there is no second mapping
        // to keep in step; only the four pickaxes are ever non-zero.
        std::array<int, kItems> durability{};
        // A smelt in progress. The furnace is a BLOCK, so the burn time lives
        // with the block (see Smelter) and only the agent's own cook clock is
        // here — two agents at one furnace share its fuel, as they would.
        int         smeltLeft = 0;
        std::size_t smeltCell = 0;
    };

    explicit VoxelCity(int n = 96) : n_(n) {
        about_ = Provenance{
            "Block world: a town", "—",
            "World and tech tree after Guss et al.; learners after Mnih et al. and van Hasselt et al.",
            "Guss, W. H. et al. \"MineRL\", IJCAI 2019; Mnih, V. et al., Nature 518 (2015) "
            "529-533; van Hasselt, H., Guez, A., Silver, D., AAAI 2016",
            Replication::No,
            "No. Agents do not copy themselves — they are a fixed population that learns. "
            "Replication in this bench means a pattern that builds another pattern, and "
            "nothing here does that. What it studies instead is whether many learners in "
            "one shared world reach further than one learner alone.",
            "Every agent carries its own deep Q-network, its own replay buffer and its own "
            "random stream, and they all share one world: the vein you mine is gone for "
            "everyone. Colour is the block; the white cursors are the agents. "
            "A cut tree drops saplings as its leaves rot, and those saplings grow "
            "into new trees, so the town is not limited to the trunks it started with."
        };
        buildPalette();
        buildKnobs();
        vox_.resize(kRender, kRender);
        rebuild();
        // Frame the world. camera_home() only runs when somebody presses the
        // button, so without this the sim shipped with the renderer's own
        // default camera — which for a volume 128 tall sits inside the rock and
        // renders a featureless black slab. Two changes to camera_home() made
        // no difference to the picture at all, which is the tell: the code
        // being edited was not the code being run.
        camera_home();
    }

    // ── Sim contract ────────────────────────────────────────────────────────
    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field&               field()   const override { return view_; }
    const Surface*             surface() const override { return &vox_.surface(); }
    std::uint64_t              generation() const override { return gen_; }
    std::vector<Knob>&         knobs()   override { return knobs_; }

    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        // Size and population both rebuild, because both change how much there
        // is and how many are competing for it. Doing it here rather than in
        // reset() is the rule this project keeps relearning: reset() refills
        // what exists, it does not resize it.
        // "roles" belongs here too. It was left out, so the knob moved and
        // nothing happened: roles were assigned once at construction from the
        // default and never reassigned. Both arms of the specialists-versus-
        // all-rounders comparison ran the SAME configuration and returned
        // identical numbers to the digit, which is what an inert control looks
        // like when you are lucky enough to be running an A/B against it.
        // "seed" belongs here and was missing, which made it the third inert
        // control this file has shipped. Its own help text says "Which world is
        // generated ... Same seed, same run, every time" and moving it did
        // nothing at all: worldSeed() is only read by rebuild() and reset(),
        // and neither was called. MEASURED, not suspected — three "different"
        // seeds produced biome histograms and block counts identical to the
        // digit, and seed 7 against seed 8 differed in 0 of 1,179,648 cells.
        // Every multi-seed result this sim has ever reported was one seed run
        // three times, which is the same failure the `roles` comment above
        // records and the reason both are now in one list.
        if (k == "size" || k == "agents" || k == "roles" || k == "seed") rebuild();
        // Turning the knob to `possessed` has to actually hand somebody over,
        // or the setting is the inert control this project keeps finding: the
        // dispatch would route every agent to scriptedAction() and the mode
        // would be a relabelled copy of the mode next to it. Agent 0 is the
        // town's world spawn (see reset()), so it is the one a person is
        // already looking at.
        if (k == "policy") {
            const int pol = int(v + 0.5f);
            if (pol == PolicyPossessed) { if (possessed_ < 0) possess(0); }
            else { possessed_ = -1; possReplay_ = false; prePossess_ = pol; }
        }
    }

    [[nodiscard]] bool has_camera() const override { return true; }
    bool camera_orbit(float dx, float dy) override {
        cam_.yaw -= dx * 0.010f; cam_.pitch += dy * 0.010f; cam_.clampPitch();
        publish(); return true;
    }
    bool camera_pan(float dx, float dy) override {
        float r[3], u[3]; cam_.basis(r, u);
        const float k = cam_.distance * 0.0016f;
        cam_.tx += (-dx*r[0] + dy*u[0]) * k;
        cam_.ty += (-dx*r[1] + dy*u[1]) * k;
        cam_.tz += (-dx*r[2] + dy*u[2]) * k;
        publish(); return true;
    }
    bool camera_dolly(float steps, float, float) override {
        cam_.distance = std::clamp(cam_.distance * std::pow(0.88f, steps), 0.25f, 12.0f);
        publish(); return true;
    }
    void camera_home() override {
        // Looking DOWN at the land from outside, not out from inside it. The
        // world is 128 tall against 64-256 wide, so the renderer normalises a
        // cube big enough for the height and a camera framed for a cube sits
        // inside the rock: the first render after the world grew taller was a
        // featureless black slab, which looks exactly like a broken renderer
        // and was a camera default.
        cam_.tx = 0.f; cam_.ty = 0.f; cam_.tz = 0.f;
        cam_.yaw = 0.7f; cam_.pitch = 0.55f;
        cam_.distance = 2.6f;
        publish();
    }
    bool camera_view(StdView v) override {
        switch (v) {
            case StdView::Front:  cam_.yaw = 0.f;        cam_.pitch = 0.f;   break;
            case StdView::Back:   cam_.yaw = 3.14159f;   cam_.pitch = 0.f;   break;
            case StdView::Right:  cam_.yaw = 1.5708f;    cam_.pitch = 0.f;   break;
            case StdView::Left:   cam_.yaw = -1.5708f;   cam_.pitch = 0.f;   break;
            case StdView::Top:    cam_.yaw = 0.f;        cam_.pitch = 1.5f;  break;
            case StdView::Bottom: cam_.yaw = 0.f;        cam_.pitch = -1.5f; break;
            case StdView::Iso:    cam_.yaw = 0.7f;       cam_.pitch = 0.6f;  break;
        }
        publish(); return true;
    }
    bool camera_fit() override { camera_home(); return true; }
    bool camera_ortho(bool on) override { cam_.ortho = on; publish(); return true; }

    void reset() override {
        generateWorld();
        gen_ = 0; ticks_ = 0; episode_ = 0; epochs_ = 0; deaths_ = 0; rescues_ = 0;
        structures_ = 0;
        smelters_.clear();
        mobs_.clear();
        mobSpawned_ = mobKills_ = mobHits_ = 0; mobDamage_ = 0.0;
        for (std::size_t i = 0; i < agents_.size(); ++i) resetAgent(agents_[i], int(i), true);
        // World spawn is the town's first agent, which is what the no-spawn
        // bubble in mobs.hpp measures from.
        if (!agents_.empty())
            mobs_.set_world_spawn(float(agents_[0].x), float(agents_[0].y), float(agents_[0].z));
        // A tape describes ONE run from tick 0. Carrying entries across a reset
        // would produce a recording whose tick stamps no longer match anything,
        // and the replay check would then fail on a bookkeeping error rather
        // than on the defect it is there to find.
        possQueue_.clear();
        possStanding_ = kStand; possIdle_ = 0; possAuto_ = false;
        possMoves_ = 0; possCommands_ = 0;
        replayAt_ = 0; replayFault_ = -1;
        if (!possReplay_) {
            tape_ = PossessionTape{};
            if (possessed_ >= 0) captureTapeHeader();
        }
        publish();
    }

    // ── the one and only way this file writes a block ───────────────────────
    //
    // Every mutation site goes through here: mining, placing, pillaring, the
    // station a craft puts down, poke(), and all four random ticks. Missing one
    // leaves light stale at that cell forever, which is why there is exactly one
    // function rather than a convention.
    //
    // Order is load-bearing. LightEngine::block_changed reads the world to find
    // out what is there NOW — it keeps no memory of the old block — so the write
    // has to land before the light is told. And the light is told LAST, after
    // Fluids has had its say, because a fluid reaction can turn this very cell
    // into something else (lava meeting water becomes obsidian on the spot) and
    // lighting it twice for one edit is pure waste. Both the cell and whatever
    // the reaction produced come out of the same change list.
    void setBlock(int x, int y, int z, std::uint8_t b) {
        if (!world_.inside(x, y, z) || world_.at(x, y, z) == b) return;
        world_.set(x, y, z, b);                 // recorded, if recording is on
        world_.refresh_column(x, z);
        // Fluids may write more cells than this one — water finding a new way
        // down, sand scheduling a fall, lava meeting water and turning to
        // obsidian on the spot. Those land in the same change list, so the drain
        // below covers this cell AND everything the reaction produced.
        fluids_.block_changed(x, y, z);
        if (world_.recording()) drainWorldWrites();
        else                    light_.block_changed(x, y, z);   // generation path
    }

    // Hand every block another system wrote to the light engine.
    //
    // Fluids owns a BlockWorld& and writes through it directly — spreading
    // water, landing sand, lava turning to obsidian — so there is no call site
    // here to hook. BlockWorld records the writes instead; this drains them.
    // The loop re-reads changes() by index rather than iterating, because
    // light_.block_changed cannot append but a future writer might, and an
    // invalidated iterator is a silent memory bug rather than a loud one.
    void drainWorldWrites() {
        std::size_t i = 0;
        while (i < world_.changes().size()) {
            const std::uint32_t c = world_.changes()[i++];
            int x, y, z;
            world_.unpack(c, x, y, z);
            light_.block_changed(x, y, z);
            ++lightUpdates_;
        }
        world_.clear_changes();
    }

    // ── random ticks ────────────────────────────────────────────────────────
    //
    // Java Edition picks 3 blocks at random from every 16x16x16 section of every
    // loaded chunk, every tick. It is what drives grass spreading onto bare
    // dirt, leaves rotting once their tree is cut, and crops growing — the slow
    // background processes that make the world feel like it is running rather
    // than waiting. A given block gets a random tick about once every 68
    // seconds on average.
    //
    // This is the cheapest thing in the file that makes the world feel alive:
    // cut a tree and the canopy rots over the next minute instead of hanging in
    // the air forever, which is the single most recognisable "this is
    // Minecraft" behaviour the sim was missing.
    //
    // Rotting leaves are also how wood comes back. Java oak leaves drop a
    // sapling with probability 0.05 (minecraft.wiki, Oak Leaves). This world
    // has no item entities, so a successful drop plants itself on the dirt or
    // grass directly below. Growth is AbstractSaplingBlock.randomTick: the
    // cell above needs internal light of at least 9, the same threshold as
    // crops, and one random tick in seven advances a stage. The second stage
    // grows a tree. Rolls use flora_, not rng_, so grass, mobs and fluids keep
    // the streams the rest of the file already pinned.
    static constexpr int kRandomTicksPerSection = 3;
    static constexpr int kSaplingLight = 9;             // same threshold as kCropMinLight
    static constexpr int kSaplingTries = 7;             // nextInt(7) == 0
    static constexpr int kOakSaplingDenom = 20;         // 0.05

    [[nodiscard]] bool one_in(int n) {
        return n > 0 && int(flora_.below(std::uint32_t(n))) == 0;
    }

    // A leaf is gone. With the oak drop chance, a sapling lands on the first
    // dirt or grass beneath it. Anywhere else the drop is lost, as an item
    // that fell onto stone would be if nobody picked it up.
    void releaseLeaf(int x, int y, int z) {
        if (world_.at(x, y, z) != Leaves) return;
        setBlock(x, y, z, Air);
        if (!one_in(kOakSaplingDenom)) return;
        int gy = y - 1;
        while (gy > 0 && world_.at(x, gy, z) == Air) --gy;
        if (gy <= 0) return;
        const std::uint8_t ground = world_.at(x, gy, z);
        if (ground != Dirt && ground != Grass) return;
        if (world_.at(x, gy + 1, z) != Air) return;
        setBlock(x, gy + 1, z, Sapling);
    }

    // The trunk and crown must be air or leaves before anything is written.
    // A failed attempt leaves the sapling where it is; the next successful
    // roll tries again. Generation's plantTree is a different path and still
    // writes through world_.set, so a world seed stays the map it always was.
    bool growSapling(int x, int y, int z) {
        const int gy = y - 1;
        if (gy < 1) return false;
        const std::uint8_t ground = world_.at(x, gy, z);
        if (ground != Dirt && ground != Grass) return false;
        TreeKind kind = biome_def(effectiveBiome(x, z)).tree;
        if (kind == TreeKind::None) kind = TreeKind::Oak;
        Rng r(flora_.next() | 1ull);
        int trunk = 4, radius = 2, layers = 3;
        switch (kind) {
            case TreeKind::Spruce:
            case TreeKind::SpruceAndOak: trunk = 6 + int(r.below(4)); radius = 2; layers = 4; break;
            case TreeKind::JungleTree:   trunk = 8 + int(r.below(5)); radius = 2; layers = 3; break;
            case TreeKind::Acacia:       trunk = 3 + int(r.below(3)); radius = 3; layers = 1; break;
            case TreeKind::SwampOak:     trunk = 4 + int(r.below(2)); radius = 3; layers = 2; break;
            default:                     trunk = 4 + int(r.below(3)); radius = 2; layers = 3; break;
        }
        if (gy + trunk + layers + 1 >= kHeight) return false;
        auto open = [&](int px, int py, int pz, bool base) {
            const std::uint8_t c = world_.at(px, py, pz);
            if (base) return c == Sapling || c == SaplingAged || c == Air;
            return c == Air || c == Leaves;
        };
        for (int i = 1; i <= trunk; ++i)
            if (!open(x, gy + i, z, i == 1)) return false;
        const int cy = gy + trunk;
        for (int dy = 0; dy < layers; ++dy) {
            const int rr = std::max(1, radius - dy / 2);
            for (int dz = -rr; dz <= rr; ++dz)
                for (int dx = -rr; dx <= rr; ++dx) {
                    if (std::abs(dx) + std::abs(dz) + dy > radius + layers - 1) continue;
                    if (dx == 0 && dz == 0 && dy == 0) continue;
                    if (!open(x + dx, cy + dy, z + dz, false)) return false;
                }
        }
        for (int i = 1; i <= trunk; ++i) setBlock(x, gy + i, z, Wood);
        for (int dy = 0; dy < layers; ++dy) {
            const int rr = std::max(1, radius - dy / 2);
            for (int dz = -rr; dz <= rr; ++dz)
                for (int dx = -rr; dx <= rr; ++dx) {
                    if (std::abs(dx) + std::abs(dz) + dy > radius + layers - 1) continue;
                    if (dx == 0 && dz == 0 && dy == 0) continue;
                    if (world_.at(x + dx, cy + dy, z + dz) == Air)
                        setBlock(x + dx, cy + dy, z + dz, Leaves);
                }
        }
        return true;
    }

    void randomTicks() {
        const int sx = std::max(1, n_ / 16), sy = std::max(1, kHeight / 16);
        const int shots = sx * sx * sy * kRandomTicksPerSection;
        for (int i = 0; i < shots; ++i) {
            const int x = int(rng_.unit() * float(n_)) % n_;
            const int z = int(rng_.unit() * float(n_)) % n_;
            const int y = int(rng_.unit() * float(kHeight)) % kHeight;
            randomTick(x, y, z);
        }
    }

    void randomTick(int x, int y, int z) {
        const std::uint8_t b = world_.at(x, y, z);
        switch (b) {
            case Leaves: {
                // Leaves persist only near a log. Cut the trunk and the canopy
                // rots away over the following minute, which is exactly what a
                // player sees and what this world never did. The drop, when it
                // hits, is what replants the stand.
                for (int dy = -4; dy <= 4; ++dy)
                    for (int dz = -4; dz <= 4; ++dz)
                        for (int dx = -4; dx <= 4; ++dx)
                            if (world_.at(x+dx, y+dy, z+dz) == Wood) return;
                releaseLeaf(x, y, z);
                break;
            }
            case Sapling:
            case SaplingAged: {
                if (y + 1 >= kHeight) return;
                if (light_.internal_light(x, y + 1, z, skyDarken()) < kSaplingLight) return;
                if (!one_in(kSaplingTries)) return;
                if (b == Sapling) { setBlock(x, y, z, SaplingAged); return; }
                growSapling(x, y, z);
                break;
            }
            case Dirt: {
                // Grass spreads onto bare dirt that has sky above it, from any
                // grass block within one step.
                if (world_.at(x, y + 1, z) != Air) return;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dx = -1; dx <= 1; ++dx)
                            if (world_.at(x+dx, y+dy, z+dz) == Grass) {
                                setBlock(x, y, z, Grass);
                                return;
                            }
                break;
            }
            case Farmland: {
                // Farmland grows wheat, and now only where there is light to
                // grow it by: [W-LIGHT] gives crops a minimum internal light of
                // 9, which is the rule that makes a torch worth putting over a
                // field and makes a field in a cave pointless.
                if (world_.at(x, y + 1, z) == Air
                    && light_.internal_light(x, y + 1, z, skyDarken()) >= kCropMinLight) {
                    setBlock(x, y + 1, z, Wheat);
                }
                break;
            }
            case Grass: {
                // Grass smothers under anything solid, becoming dirt again.
                if (block_solid(world_.at(x, y + 1, z))) setBlock(x, y, z, Dirt);
                break;
            }
            default: break;
        }
    }

    // ── one world tick ──────────────────────────────────────────────────────
    //
    // ONE function, called from step(), run_quiet() and advance_epoch(). It used
    // to be three copies of the same two lines, and advance_epoch's copy had
    // already drifted — it did not run the random ticks, so grass stopped
    // spreading and leaves stopped rotting for anyone driving the sim by epoch
    // rather than by frame. Adding four more world processes to three separate
    // loops would have guaranteed that happening again.
    //
    // ORDER follows the server's: agents act, then the world's own processes,
    // then the clock. Fluids before mobs, because a mob must not be spawned into
    // water that arrived this tick.
    void worldTick() {
        for (auto& a : agents_) agentStep(a);
        randomTicks();
        tickSmelters();
        fluids_.tick();
        drainWorldWrites();          // everything the fluids just wrote
        tickMobs();
        ++ticks_;
        // Counted in TICKS, because the knob is in ticks and the agent's
        // decisions are not: a committed mine is one decision spread over up to
        // 300 of them, so counting decisions would make the timeout fire after
        // wildly different amounts of hands-off time depending on what the agent
        // happened to be doing when you stopped typing.
        if (possession_live() && !possReplay_) {
            if (possQueue_.empty()) ++possIdle_; else possIdle_ = 0;
            tape_.ticks = ticks_;
        }
    }

    void step() override {
        const int batch = std::max(1, int(knob("speed") + 0.5f));
        for (int b = 0; b < batch; ++b) worldTick();
        ++gen_;
        publish();
    }

    // An epoch is a life. Measuring a learner per FRAME says nothing — the
    // quantity that has to improve is what an agent achieves between spawning
    // and dying, and that is what the plots and the sweep should index by.
    [[nodiscard]] const char* epoch_name() const override { return "life"; }
    [[nodiscard]] int epoch_count() const override { return epochs_; }
    bool advance_epoch() override {
        // ONE tick at a time, and deliberately not through step(): step() runs
        // `speed` ticks per call, so going through it would make the epoch
        // outcome depend on the display rate — which is exactly what a
        // display-only knob must never do. It also has to advance the counter by
        // exactly one however many agents happen to die on the same tick, so the
        // counter is its own and not a count of deaths.
        const long long start = deaths_;
        for (int guard = 0; guard < 40000 && deaths_ == start; ++guard) worldTick();
        ++epochs_;
        publish();
        return true;
    }

    // Drop a crafting table where you click. The one intervention worth having:
    // a table is the gate between rung 3 and everything above it, so putting one
    // down in front of a town and watching whether anybody uses it is a real
    // experiment rather than a decoration.
    bool poke(float nx, float ny) override {
        const int x = std::clamp(int(nx * float(n_)), 0, n_ - 1);
        const int z = std::clamp(int(ny * float(n_)), 0, n_ - 1);
        const int s2 = world_.surface(x, z);
        if (s2 < 0 || s2 + 1 >= kHeight) return false;
        setBlock(x, s2 + 1, z, std::uint8_t(Table));
        ++structures_;
        publish();
        return true;
    }

    std::vector<Metric> metrics() const override {
        std::vector<Metric> m;
        const double n = double(std::max<std::size_t>(1, agents_.size()));
        double rung = 0, alive = 0, placed = 0, mined = 0, tier = 0, food = 0;
        double now = 0;
        int best = 0;
        for (const auto& a : agents_) {
            rung += a.bestRung; alive += a.alive ? 1 : 0; placed += a.placed;
            mined += a.mined; tier += a.bestTier; food += a.food;
            now += a.rung;
            best = std::max(best, a.bestRung);
        }
        // Mean rung is the headline: how far up the tree the TOWN is, not how
        // far its luckiest member got. Best is reported next to it because a
        // population that carries one expert and nineteen novices and one where
        // everybody is competent have the same mean and are not the same town.
        m.push_back(Metric{"mean rung",   rung / n,      double(kItems), Metric::Higher});
        m.push_back(Metric{"best rung",   double(best),  double(kItems), Metric::Higher});
        // The one to read when comparing two policies. `mean rung` above is a
        // max over lives and saturates; this is a rate and does not.
        m.push_back(Metric{"rung per life", mean_rung_per_life(), double(kItems),
                           Metric::Higher});
        // Continuous where the rung counter is a step function: a town halfway
        // through gathering a table's planks reads above one that has not
        // started, and the plot moves while the work is happening rather than
        // only when it lands.
        m.push_back(Metric{"tree progress", mean_potential(), double(kItems),
                           Metric::Higher});
        m.push_back(Metric{"alive",       alive / n,     1.0,            Metric::Higher});
        m.push_back(Metric{"mean tool tier", tier / n,   4.0,            Metric::Higher});
        // Held now against ever reached. The gap is what death keeps taking.
        m.push_back(Metric{"rung held now", now / n,     double(kItems), Metric::Higher});
        m.push_back(Metric{"food",        food / n / 20.0, 1.0,          Metric::Higher});
        // Structure is placed blocks that are still standing, normalised by the
        // town size so it does not simply reward having more agents.
        m.push_back(Metric{"blocks placed", placed / n, 200.0,           Metric::Higher});
        m.push_back(Metric{"blocks mined",  mined / n,  400.0,           Metric::Higher});
        // Not a score. Deaths are neither good nor bad on their own — an agent
        // that never dies may simply never have gone underground.
        double deaths = 0;
        for (const auto& a : agents_) deaths += a.deaths;
        m.push_back(Metric{"deaths",      deaths / n,    20.0,           Metric::Neither});
        // NOT neutral, and not cosmetic. Every firing is an agent that stood
        // choosing an impossible action for fifteen seconds of game time. Zero
        // is the only good value; anything else is a bug that has not been
        // found yet, which is why it is on the panel rather than in a log.
        m.push_back(Metric{"stuck rescues", double(rescues_) / n, 20.0, Metric::Lower});
        // Time of day, on the game's 24,000-tick cycle. Reported because "the
        // town stopped mining" and "it is night" are different explanations for
        // the same flat stretch of a plot.
        m.push_back(Metric{"daylight",    daylight(),    1.0,            Metric::Neither});
        // Hostiles alive right now, against the published cap for a world this
        // size. Neither good nor bad — it is the pressure the town is under, and
        // reading it next to `alive` is how "the town collapsed" and "it is
        // night and there are twelve zombies" stop being the same plot.
        int hostiles = 0;
        for (const auto& mo : mobs_.mobs()) if (mo.alive && mob_hostile(mo.kind)) ++hostiles;
        const int cap = std::max(1, mob_cap(CatMonster, std::max(1, (n_ / 16) * (n_ / 16))));
        m.push_back(Metric{"hostiles", double(hostiles) / double(cap), 1.0, Metric::Neither});
        // How dark it is where the town stands, on the engine's own 0..15. This
        // is the number the spawn rule reads, so a flat line here next to a
        // rising hostile count means the light field is not being updated.
        double lit = 0;
        for (const auto& a : agents_)
            lit += double(light_.internal_light(a.x, a.y, a.z, skyDarken()));
        m.push_back(Metric{"light on the town", lit / n / double(kLightMax),
                           1.0, Metric::Neither});
        return m;
    }

    // Minecraft's day is 24,000 ticks: 12,000 of daylight, then night.
    static constexpr long long kDayTicks = 24000;
    // mobs.hpp derives the whole burn window and the sky-darken curve from its
    // own copy of that number. Two constants for one fact is how a clock drifts,
    // and a drifted clock here means hostiles burning at the wrong hour with
    // nothing failing to say so.
    static_assert(kDayTicks == kDayLength,
                  "the sim's day length and mobs.hpp's have diverged");
    [[nodiscard]] double daylight() const {
        return (ticks_ % kDayTicks) < 12000 ? 1.0 : 0.0;
    }

    [[nodiscard]] std::string subtitle() const override {
        int best = 0; double rung = 0; int alive = 0;
        for (const auto& a : agents_) {
            best = std::max(best, a.rung); rung += a.rung; alive += a.alive ? 1 : 0;
        }
        const double n = double(std::max<std::size_t>(1, agents_.size()));
        char b[288];
        // Possession first, because when it is on it is the thing you are
        // looking for. The amber cube is the answer to "which one am I" in the
        // picture; this is the answer in the header, and it also names the
        // agent's own rung — the town mean next to it averages a person in with
        // seven scripted neighbours and is not the human baseline.
        if (possession_live() && std::size_t(possessed_) < agents_.size()) {
            const Agent& pa = agents_[std::size_t(possessed_)];
            std::snprintf(b, sizeof b,
                "YOU = agent %d (amber)  ·  %s  ·  your rung %d  ·  %zu agents  ·  "
                "town mean %.2f  ·  life %d",
                possessed_,
                possReplay_ ? "replaying a tape"
                            : (possAuto_ ? "AUTOPILOT — idle" : "you are driving"),
                pa.bestRung, agents_.size(), rung / n, episode_);
            return b;
        }
        std::snprintf(b, sizeof b,
                      "%zu agents  ·  %d alive  ·  mean rung %.2f  ·  best %s  ·  life %d",
                      agents_.size(), alive, rung / n,
                      best > 0 ? item_name(best - 1) : "nothing", episode_);
        return b;
    }

    // ── measurement hooks, used by the self-test ────────────────────────────
    // Action histogram, for finding out where a policy's ticks actually go.
    // Every diagnosis of this sim so far has been wrong until something counted.
    [[nodiscard]] const std::array<long long, kActions>& action_counts() const { return actionHist_; }
    void clear_action_counts() { actionHist_.fill(0); }

    // Put every agent back at a spawn with an empty pack, but KEEP the networks
    // and what they have learned. Measuring a policy means measuring what it
    // would do from a standing start, not reading off a run that has been
    // exploring for an hour — the same distinction that made the gridworld's
    // exploration result legible.
    void reset_agents_keep_brains() {
        for (std::size_t i = 0; i < agents_.size(); ++i) {
            agents_[i].brain.set_epsilon(knob("epsilon"));
            resetAgent(agents_[i], int(i), true);
        }
        publish();
    }

    void run_quiet(int ticks) {
        for (int t = 0; t < ticks; ++t) worldTick();
    }
    [[nodiscard]] const std::vector<Agent>& agents() const { return agents_; }
    [[nodiscard]] const BlockWorld& world() const { return world_; }
    // ── measurement hooks for the world subsystems ──────────────────────────
    // Every one of these exists because "wired but inert" is the failure this
    // project catches most often, and a count is the only thing that settles it.
    [[nodiscard]] const LightEngine& light() const { return light_; }
    [[nodiscard]] const Fluids&      fluids() const { return fluids_; }
    [[nodiscard]] const BiomeMap&    biomes() const { return biomes_; }
    [[nodiscard]] const MobWorld&    mob_world() const { return mobs_; }
    [[nodiscard]] long long light_updates() const { return lightUpdates_; }
    [[nodiscard]] long long mobs_spawned() const { return mobSpawned_; }
    [[nodiscard]] long long mob_hits()     const { return mobHits_; }
    [[nodiscard]] double    mob_damage()   const { return mobDamage_; }
    // Damage SWUNG against damage that landed. The gap is the 10-tick
    // invulnerability window doing its job, and reporting only one of the two
    // would make the window either invisible or indistinguishable from mobs
    // that missed.
    [[nodiscard]] double    mob_damage_applied() const { return appliedMobDamage_; }
    [[nodiscard]] long long mob_kills()    const { return mobKills_; }
    [[nodiscard]] long long mob_burning()  const { return mobBurning_; }
    [[nodiscard]] long long torches_placed() const { return torchesPlaced_; }
    [[nodiscard]] long long smelts_done()  const { return smeltsDone_; }
    [[nodiscard]] long long tools_broken() const { return toolsBroken_; }
    [[nodiscard]] long long fuel_burned()  const { return fuelBurned_; }
    [[nodiscard]] int        sky_darken_now() const { return skyDarken(); }
    [[nodiscard]] long long  world_ticks() const { return ticks_; }
    // Put a block down from outside, through the same single write path
    // everything else uses. Exists so a measurement can do the before/after that
    // settles whether light, fluids and spawning are actually coupled — "place a
    // torch in that dark cave and read the spawn predicate again" is the only
    // evidence that the torch does anything, and it cannot be taken from
    // outside without a way in.
    void edit_block(int x, int y, int z, std::uint8_t b) { setBlock(x, y, z, b); }
    // One random tick at one cell, so a sapling test does not have to wait
    // for the section lottery to land on it.
    void random_tick_for_test(int x, int y, int z) { randomTick(x, y, z); }
    // Mutable access to one agent, and one block broken start to finish.
    //
    // Both exist for the same reason action_counts() does. Several of the rules
    // this file now claims — a smelt takes 200 ticks, a wooden pickaxe lasts 59
    // blocks — only come up deep into a run, and a measurement that has to wait
    // for a policy to get there by itself is a measurement of the policy. A run
    // that reports "0 smelts" cannot distinguish a broken furnace from an agent
    // that never found iron, and that distinction is the whole question.
    [[nodiscard]] Agent& agent_for_test(std::size_t i) { return agents_[i]; }
    void spawn_mob_for_test(int kind, float x, float y, float z) {
        mobs_.add(kind, x, y, z);
        ++mobSpawned_;
    }
    void break_one_for_test(std::size_t i) {
        Agent& a = agents_[i];
        const int bx = a.x + kDX[a.facing], by = a.y + kDY[a.facing],
                  bz = a.z + kDZ[a.facing];
        setBlock(bx, by, bz, std::uint8_t(Stone));
        a.miningX = -1;
        for (int guard = 0; guard < 8000; ++guard) {
            mineFacing(a);
            if (world_.at(bx, by, bz) == Air) return;
        }
    }
    // Lifetime, not current-life. See Agent::bestRung.
    [[nodiscard]] double mean_rung() const {
        if (agents_.empty()) return 0;
        double s = 0; for (const auto& a : agents_) s += a.bestRung;
        return s / double(agents_.size());
    }
    [[nodiscard]] int best_rung() const {
        int b = 0; for (const auto& a : agents_) b = std::max(b, a.bestRung); return b;
    }
    // How far a typical LIFE gets. See Agent::rungLifeSum for why this, and not
    // the high-water mark, is the statistic every cross-policy claim should use.
    //
    // Pooled over the town rather than averaged per agent: an agent that has
    // died fourteen times and one that has died once are not two equally
    // weighted samples, and dividing sum-of-rungs by sum-of-lives weights each
    // life once, which is the thing being counted.
    // How far up the tree one agent stands, counting part-gathered rungs. See
    // techPotential(). Public because it is the honest continuous answer to
    // "how is the town doing" — `mean rung` is a step function that spends most
    // of its time flat, and a flat statistic is one nothing can be measured on.
    [[nodiscard]] double tech_potential(std::size_t i) const {
        return i < agents_.size() ? techPotential(agents_[i]) : 0.0;
    }
    [[nodiscard]] double mean_potential() const {
        if (agents_.empty()) return 0;
        double s = 0; for (const auto& a : agents_) s += techPotential(a);
        return s / double(agents_.size());
    }

    // The episode fitness dynamic scripting is actually scored on. Public for
    // probes only: a probe that re-types the formula measures the copy, and
    // three separate probes in this project have already done exactly that.
    // `terminal` is the death boundary — see lifeFitness().
    [[nodiscard]] double life_fitness_for_test(std::size_t i, bool terminal = false) const {
        return i < agents_.size() ? lifeFitness(agents_[i], terminal) : 0.0;
    }

    // Which rule of this agent's current script is actually firing, in the
    // rulebase's own words, or null if none of them holds. Dynamic scripting is
    // the one policy whose behaviour is not readable from the code — the script
    // is drawn at run time — so "why is that agent doing that" has no answer
    // without this.
    [[nodiscard]] const char* dyn_rule(std::size_t i) const {
        if (i >= agents_.size()) return nullptr;
        const Agent& a = agents_[i];
        for (int r : a.book.script()) {
            const Rule& rule = rulebase()[std::size_t(r)];
            if (holds(a, rule.cond)) return rule.text;
        }
        return nullptr;
    }

    [[nodiscard]] double mean_rung_per_life() const {
        long long rung = 0, lives = 0;
        for (const auto& a : agents_) {
            rung  += a.rungLifeSum + a.rung;   // ended lives, plus the one running
            lives += a.livesEnded + 1;
        }
        return lives ? double(rung) / double(lives) : 0.0;
    }
    // What the town holds RIGHT NOW, which is a different question: a town that
    // keeps losing its pickaxes and one that never made a pickaxe look the same
    // on the high-water mark alone.
    [[nodiscard]] double mean_rung_now() const {
        if (agents_.empty()) return 0;
        double s = 0; for (const auto& a : agents_) s += a.rung;
        return s / double(agents_.size());
    }
    void set_policy(int p) { for (auto& k : knobs_) if (k.key == "policy") k.value = float(p); }

    // ── possession, the public face ─────────────────────────────────────────
    //
    // Deliberately the same shape as set_policy(): possession IS a policy
    // setting, and the suite drives it through here exactly as a person drives
    // it through the keyboard, so the thing measured is the thing shipped.
    static constexpr int PolicyPossessed = 4;

    // Take agent i. Anything already recorded is dropped, because a tape
    // describes one continuous run from a known start and stitching two of them
    // together would produce a recording that replays to a different score —
    // which is precisely the defect the replay check exists to catch.
    void possess(std::size_t i) {
        if (agents_.empty()) return;
        possessed_ = int(std::min(i, agents_.size() - 1));
        if (!possReplay_) {
            const int pol = int(knob("policy") + 0.5f);
            if (pol != PolicyPossessed) prePossess_ = pol;
            set_policy(PolicyPossessed);
        }
        possQueue_.clear();
        possStanding_ = kStand;
        possIdle_ = 0;
        possAuto_ = false;
        possMoves_ = 0;
        // Come in close, through the camera that is already here. camera_home()
        // frames the whole 96x128x96 world from 2.6 away, where one agent is one
        // voxel: the amber marker was MEASURED at 1 cell of 9,216 in the
        // top-down field and is a single pixel-ish cube in the render, so
        // "there is an obvious way to see which agent you are" would have been
        // false in the only place it matters. 0.55 is inside camera_dolly's own
        // [0.25, 12.0] clamp and reads as about a dozen blocks of context.
        // The previous distance is remembered so release() puts the view back
        // where it was rather than leaving you zoomed into a town you are no
        // longer in.
        if (!possReplay_) {
            if (prePossessDist_ <= 0.f) prePossessDist_ = cam_.distance;
            cam_.distance = 0.55f;
        }
        if (!possReplay_) { tape_ = PossessionTape{}; captureTapeHeader(); }
        publish();
    }
    // Hand the body back. The policy returns to whatever was driving before,
    // not to a hard-coded default — releasing out of a comparison you were
    // halfway through and landing somewhere else would silently change the arm
    // being measured.
    void release() {
        possessed_ = -1;
        possQueue_.clear();
        possAuto_ = false;
        possReplay_ = false;
        if (prePossessDist_ > 0.f) { cam_.distance = prePossessDist_; prePossessDist_ = -1.f; }
        set_policy(prePossess_);
        publish();
    }
    [[nodiscard]] int possessed_index() const { return possessed_; }
    [[nodiscard]] bool possession_live() const {
        return possessed_ >= 0 && int(knob("policy") + 0.5f) == PolicyPossessed;
    }
    // Queue one command. Out-of-range codes are DROPPED rather than clamped: a
    // clamp would turn a mis-wired key into a plausible action and hide the
    // wiring bug, which is the failure this project catches most often.
    void possess_command(int code) {
        if (!poss_code_valid(code) || possessed_ < 0 || possReplay_) return;
        // Bounded, so a key held down while the sim is paused cannot bank ten
        // thousand moves that then fire in one burst. Four deep is about a
        // fifth of a second of human input at the measured step rate.
        if (possQueue_.size() >= 4) possQueue_.pop_front();
        possQueue_.push_back(code);
        possIdle_ = 0;
        possAuto_ = false;
        ++possCommands_;
    }
    [[nodiscard]] bool possessed_auto()  const { return possAuto_; }
    [[nodiscard]] int  possessed_order() const { return possStanding_; }
    [[nodiscard]] long long possessed_idle()  const { return possIdle_; }
    [[nodiscard]] long long possessed_moves() const { return possMoves_; }
    [[nodiscard]] long long possessed_commands() const { return possCommands_; }
    [[nodiscard]] const PossessionTape& tape() const { return tape_; }

    // Re-run a recorded human. The tape's own header rebuilds the world it was
    // taken in, so a replay cannot silently be scored against a different one.
    void possess_replay(const PossessionTape& t) {
        on_knob("seed",   float(t.seed));
        on_knob("size",   float(t.size));
        on_knob("agents", float(t.agents));
        on_knob("idle",   float(t.idle));
        on_knob("roles",  float(t.roles));
        on_knob("hunger", t.hunger);
        set_policy(PolicyPossessed);
        reset();
        possReplay_ = true;
        replayTape_ = t;
        replayAt_ = 0;
        replayFault_ = -1;
        // Clamped BOTH ways. std::min alone let a negative index through, so a
        // corrupt or hand-written tape gave a possessed_ of -7 that indexed
        // nothing and still reported itself live.
        possessed_ = std::clamp(t.index, 0, int(agents_.size()) - 1);
        possQueue_.clear();
        possStanding_ = kStand;
        possIdle_ = 0; possAuto_ = false; possMoves_ = 0;
        publish();
    }
    [[nodiscard]] bool possession_replaying() const { return possReplay_; }
    // The tick at which the replay stopped agreeing with the tape, or -1 if it
    // never did. Not a bool: the tick is what turns "the recording is
    // incomplete" into something you can go and look at.
    [[nodiscard]] long long replay_fault() const { return replayFault_; }
    [[nodiscard]] std::size_t replay_consumed() const { return replayAt_; }

    // One agent's run, in the terms every policy in this file is scored on.
    // Comparing a human against a machine means comparing THESE — the town-wide
    // means in metrics() average a human in with seven scripted neighbours and
    // would report a human baseline that is seven-eighths scripted.
    struct AgentScore {
        int rung = 0, tier = 0, mined = 0, placed = 0, crafted = 0, deaths = 0;
        int x = 0, y = 0, z = 0;
        long long steps = 0;
        // The rate, kept as its two integer parts rather than as a ratio: this
        // struct is compared for exact equality by the replay checks, and two
        // integers compare exactly where a double invites a tolerance nobody
        // wants in a determinism test.
        int rungLifeSum = 0, livesEnded = 0;
        [[nodiscard]] double rung_per_life() const {
            return double(rungLifeSum + rung) / double(livesEnded + 1);
        }
        friend bool operator==(const AgentScore&, const AgentScore&) = default;
    };
    [[nodiscard]] AgentScore score_of(std::size_t i) const {
        AgentScore s;
        if (i >= agents_.size()) return s;
        const Agent& a = agents_[i];
        s.rung = a.bestRung; s.tier = a.bestTier; s.mined = a.mined;
        s.placed = a.placed; s.crafted = a.crafted; s.deaths = a.deaths;
        s.x = a.x; s.y = a.y; s.z = a.z; s.steps = a.stepsAlive;
        s.rungLifeSum = a.rungLifeSum; s.livesEnded = a.livesEnded;
        return s;
    }

    // Everything a replay has to reproduce, in one number. Scores alone are too
    // coarse to prove a replay: two runs can reach rung 4 by different routes
    // and through different worlds. This walks the blocks AND the agents, so a
    // divergence anywhere shows up here even when the headline number does not.
    [[nodiscard]] std::uint64_t state_hash() const {
        std::uint64_t h = 1469598103934665603ull;
        auto mix = [&h](std::uint64_t v) {
            h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        };
        mix(std::uint64_t(ticks_));
        for (int z = 0; z < n_; ++z)
            for (int y = 0; y < kHeight; ++y)
                for (int x = 0; x < n_; ++x) mix(std::uint64_t(world_.at(x, y, z)));
        for (const auto& a : agents_) {
            mix(std::uint64_t(a.x + 1) * 7919u); mix(std::uint64_t(a.y + 1) * 104729u);
            mix(std::uint64_t(a.z + 1) * 15485863u);
            mix(std::uint64_t(a.mined)); mix(std::uint64_t(a.placed));
            mix(std::uint64_t(a.crafted)); mix(std::uint64_t(a.bestRung));
            mix(std::uint64_t(a.bestTier)); mix(std::uint64_t(a.deaths));
            mix(std::uint64_t(a.alive ? 1 : 0));
            mix(std::uint64_t(std::llround(double(a.health) * 1000.0)));
            mix(std::uint64_t(std::llround(double(a.food)   * 1000.0)));
        }
        return h;
    }

    // Where the camera is pointing, in the renderer's normalised cube. Exposed
    // so the follow can be MEASURED rather than taken on trust — "the camera
    // follows the agent" is exactly the kind of claim that is true of the code
    // and false of the running program.
    [[nodiscard]] std::array<float, 3> camera_target() const {
        return { cam_.tx, cam_.ty, cam_.tz };
    }

    // What the possessed agent sees and carries, in words. It is the same
    // observation vector the network is handed and the same inventory the
    // recipe table reads — surfaced, not recomputed, so the readout cannot
    // drift from what the agent is actually acting on.
    [[nodiscard]] std::vector<std::string> possession_readout() const {
        std::vector<std::string> out;
        if (possessed_ < 0 || std::size_t(possessed_) >= agents_.size()) {
            out.push_back("nobody possessed  ·  P takes agent 0");
            return out;
        }
        const Agent& a = agents_[std::size_t(possessed_)];
        char b[256];
        std::snprintf(b, sizeof b, "YOU are agent %d%s  ·  %s", possessed_,
                      a.alive ? "" : " (dead, respawning)",
                      possReplay_ ? "REPLAY"
                                  : (possAuto_ ? "AUTOPILOT — press a key to take over"
                                               : "driving"));
        out.emplace_back(b);
        std::snprintf(b, sizeof b, "order %s  ·  idle %lld/%d ticks  ·  moves %lld  ·  keys %lld",
                      poss_code_name(possStanding_), possIdle_,
                      int(knob("idle") + 0.5f), possMoves_, possCommands_);
        out.emplace_back(b);
        std::snprintf(b, sizeof b,
                      "at (%d,%d,%d) facing %s  ·  health %.0f/20  food %.0f/20  ·  %s",
                      a.x, a.y, a.z, kFaceName[std::size_t(std::clamp(a.facing, 0, 5))],
                      double(a.health), double(a.food),
                      block_name(faced(a)));
        out.emplace_back(b);
        std::snprintf(b, sizeof b, "rung %d (%s)  ·  pick %s  ·  mined %d  placed %d  died %d",
                      a.rung, a.rung > 0 ? item_name(a.rung - 1) : "nothing",
                      kTierName[std::size_t(std::clamp(a.tier, 0, 4))],
                      a.mined, a.placed, a.deaths);
        out.emplace_back(b);
        // The five bearings, straight out of the same Sense array observe()
        // encodes. This is the whole of what an agent knows about where things
        // are, and seeing it is what makes "why did it walk that way" answerable.
        std::string sees = "sees:";
        static const char* what[kSenses] = { "wood", "stone", "coal", "iron", "diamond" };
        for (int i = 0; i < kSenses; ++i) {
            const auto& sn = a.sense[std::size_t(i)];
            if (!sn.found) { sees += std::string("  ") + what[i] + " —"; continue; }
            const int dx = sn.x - a.x, dy = sn.y - a.y, dz = sn.z - a.z;
            std::snprintf(b, sizeof b, "  %s %d,%d,%d", what[i], dx, dy, dz);
            sees += b;
        }
        out.push_back(sees);
        std::string carry = "carries:";
        for (int i = 0; i < kInv; ++i)
            if (a.blocks[std::size_t(i)] > 0) {
                std::snprintf(b, sizeof b, "  %s %d", block_name(std::uint8_t(i)),
                              a.blocks[std::size_t(i)]);
                carry += b;
            }
        for (int i = 0; i < kItems; ++i)
            if (a.items[std::size_t(i)] > 0) {
                std::snprintf(b, sizeof b, "  %s %d", item_name(i), a.items[std::size_t(i)]);
                carry += b;
            }
        if (carry == "carries:") carry += " nothing";
        // Bounded. A full pack names a dozen block types and the line ran wider
        // than the canvas at any window under about 1600px, drawing over the
        // render — the readout is drawn with GDI text, which does not clip to
        // the box it is inside. Truncating here rather than in the workbench
        // keeps the readout self-describing wherever it is printed.
        if (carry.size() > 108) { carry.resize(105); carry += "..."; }
        out.push_back(carry);
        return out;
    }

private:
    static constexpr const char* kFaceName[6] = { "-x", "+x", "-z", "+z", "up", "down" };
    static constexpr const char* kTierName[5] = { "none", "wood", "stone", "iron", "diamond" };

    static constexpr int kRender = 560;

    [[nodiscard]] float knob(const char* k) const {
        for (auto& kn : knobs_) if (kn.key == k) return kn.value;
        return 0.f;
    }
    [[nodiscard]] std::uint64_t worldSeed() const {
        return 0xB10CC17Aull ^ (std::uint64_t(int(knob("seed") + 0.5f)) * 0x9E3779B97F4A7C15ull);
    }

    // ── the clock, in the form the two light models want ────────────────────
    //
    // mobs.hpp publishes the day curve (0 through the day, ramping to 11 across
    // dusk, back down across dawn) and light.hpp takes exactly that number as
    // its skyDarken argument. Both files independently arrive at 11 for
    // midnight, from the same wiki worked example, so they agree by derivation
    // rather than by my copying one into the other.
    [[nodiscard]] int timeOfDay() const { return int(ticks_ % kDayTicks); }
    [[nodiscard]] int skyDarken() const { return sky_darken(timeOfDay()); }

    // [W-LIGHT]: crops need an internal light of 9 or more to grow. This is what
    // makes a torch over a field do something and a field in a cave pointless.
    static constexpr int kCropMinLight = 9;
    static_assert(kSaplingLight == kCropMinLight,
                  "saplings and crops share the published light threshold of 9");

    // ── generation ──────────────────────────────────────────────────────────
    //
    // DETERMINISM. Everything below is a pure function of worldSeed(): the biome
    // noise is seeded from it, the terrain is seeded from it, and the tree
    // planter carries its own Rng seeded from it. Nothing reads the wall clock
    // and nothing draws from an agent's stream. Same seed, same world, every
    // time — asserted in the probe, not assumed.
    void generateWorld() {
        // Generation writes millions of cells; recording them would build a
        // change list nobody drains. BlockWorld::generate suspends recording for
        // its own pass, and the biome and tree passes below are wrapped here.
        world_.record_changes(false);
        // The mob population's stream is part of the world, so it moves with the
        // world seed. Without this, "same seed, same run" would hold for the
        // terrain and not for what walks on it — two seeds would share a spawn
        // and drop sequence, which is the sort of hidden correlation that makes
        // a multi-seed result a measurement of one draw. Fluids keeps its own
        // fixed stream: it uses randomness in exactly one place (the lava spread
        // delay) and a fixed seed there is still deterministic per run.
        mobs_.reseed(worldSeed() ^ 0x30B31E5Eull);
        // Same argument for the world's own stream, which drives the random
        // ticks and the spawn candidates. It was a fixed constant, so grass
        // spread and mobs appeared in the same order on every seed.
        rng_.reseed(worldSeed() ^ 0xB10CC17Aull);
        flora_.reseed(worldSeed() ^ 0x5A911Eull);
        biomes_.build(worldSeed(), n_);
        // No generic trees: they would put an oak in the middle of a desert
        // before the biome pass ever ran. Trees are planted per biome below.
        world_.generate(worldSeed(), /*plantTrees=*/false);
        applyBiomeSurface();
        plantBiomeTrees();
        for (int z = 0; z < n_; ++z) for (int x = 0; x < n_; ++x) world_.refresh_column(x, z);

        // The two O(volume) passes in the whole file, both here, both once.
        // There used to be a third, mobLight_.rebuild(world_), and its absence
        // is the point: the mob system reads light_ now, so there is nothing
        // left to rebuild and nothing left to go stale.
        light_.rebuild();
        fluids_.rebuild();
        world_.clear_changes();
        world_.record_changes(true);
    }

    // The biome a COLUMN actually gets. The biome map is climate, not height,
    // so on its own it will happily put an ocean on a hilltop; the water table
    // is the terrain's business. So height wins at the waterline — under the sea
    // is ocean floor, the strip just above it is beach — and the climate map
    // decides everything above that. This is a divergence from vanilla, where
    // continentalness drives the terrain height in the first place and the two
    // therefore agree by construction; here they are two independent fields and
    // one of them has to yield.
    [[nodiscard]] Biome effectiveBiome(int x, int z) const {
        const int sea = world_.sea_level();
        const int top = world_.surface(x, z);
        if (top >= 0 && top <= sea)     return Biome::Ocean;
        if (top >= 0 && top <= sea + 1) return Biome::Beach;
        return biomes_.biome_at(x, z);
    }

    // Repaint the top of every column in its biome's own materials. Only SOIL is
    // repainted — never ore, never bedrock, never a block a cave already opened
    // — so this changes what the map looks like and not what is in it.
    // The soil itself comes from BiomeMap::column_block rather than being
    // re-derived here. It used to be re-derived, and the two copies were the
    // reason a mutation test could paint filler where surface belongs and leave
    // the suite green: every biome assertion went through column_block, and
    // nothing shipped did. One implementation, on the path that runs.
    void applyBiomeSurface() {
        const int sea = world_.sea_level();
        for (int z = 0; z < n_; ++z)
            for (int x = 0; x < n_; ++x) {
                const int top = world_.surface(x, z);
                if (top < 1) continue;
                const Biome b = effectiveBiome(x, z);
                for (int y = top; y > top - kFillerDepth && y > 0; --y) {
                    const std::uint8_t was = world_.at(x, y, z);
                    if (was != Grass && was != Dirt && was != Sand && was != Stone) continue;
                    world_.set(x, y, z, biomes_.column_block(b, y, top, sea));
                }
            }
    }

    // Trees at the published per-biome densities, rolled the way vanilla's
    // `count` placement modifier rolls them: a two-entry weighted list, per
    // chunk. Plains gets 0.05 trees a chunk and jungle gets 50.1, which is the
    // whole reason the map now reads as different places rather than one place.
    //
    // BlockWorld has ONE wood block and ONE leaf block, so TreeKind cannot
    // change the species — it changes the SHAPE, which is the part an agent can
    // actually run into: a jungle trunk is twelve blocks of wood in one column
    // and a savanna acacia is four with a flat crown.
    void plantBiomeTrees() {
        Rng r(worldSeed() ^ 0x77EE13ull);
        const int chunks = std::max(1, n_ / 16);
        for (int cz = 0; cz < chunks; ++cz)
            for (int cx = 0; cx < chunks; ++cx) {
                // THE CHUNK'S DENSITY IS THE AVERAGE OF ITS COLUMNS', not the
                // density of whichever biome the centre column happens to be.
                //
                // Vanilla can read the count off one biome per chunk because
                // there, continentalness drives the terrain height, so a forest
                // chunk is forest all the way across. Here the climate map and
                // the height field are independent (see effectiveBiome), so a
                // chunk classified forest at its centre is routinely half ocean
                // and half beach — and asking it for forest's 10.1 trees put
                // them all on columns no tree can grow on.
                //
                // MEASURED, twice. Centre-biome with a single placement draw gave
                // 29 wood blocks in a 96x96 world, about five trees. Adding eight
                // retries took it to 89. Averaging the density over the chunk's
                // own 256 columns, below, takes it to a forest that is a forest
                // and a desert that is empty. Wood is the FIRST RUNG of this
                // sim's tech tree, so this is not cosmetic — blockworld.hpp's own
                // generator comment makes the same point: "a map an agent can
                // starve on for want of a trunk is not a fair benchmark".
                double expected = 0.0;
                for (int oz = 0; oz < 16; ++oz)
                    for (int ox = 0; ox < 16; ++ox)
                        expected += biome_def(effectiveBiome(cx * 16 + ox, cz * 16 + oz))
                                        .trees.mean();
                expected /= 256.0;
                // Whole trees plus a fractional one, rolled — so a density of
                // 0.05 trees a chunk really is one chunk in twenty and not zero
                // everywhere, which is what truncation would have made it.
                int count = int(expected);
                if (double(r.unit()) < expected - double(count)) ++count;
                for (int t = 0; t < count; ++t) {
                    // Up to eight columns tried, and the SHAPE comes from the
                    // column that takes it, so a tree that lands in a taiga
                    // corner of a forest chunk grows as a spruce.
                    for (int attempt = 0; attempt < 8; ++attempt) {
                        const int x = cx * 16 + int(r.below(16));
                        const int z = cz * 16 + int(r.below(16));
                        const TreeKind kind = biome_def(effectiveBiome(x, z)).tree;
                        if (kind == TreeKind::None) continue;
                        if (plantTree(x, z, kind, r)) break;
                    }
                }
            }
    }

    // Returns whether a tree was actually planted, so the caller can retry.
    bool plantTree(int x, int z, TreeKind kind, Rng& r) {
        if (x < 0 || z < 0 || x >= n_ || z >= n_) return false;
        const int gy = world_.surface(x, z);
        if (gy < 1) return false;
        const std::uint8_t ground = world_.at(x, gy, z);
        if (ground != Grass && ground != Dirt) return false;
        int trunk = 4, radius = 2, layers = 3;
        switch (kind) {
            case TreeKind::Spruce:                        // conifer: tall, narrow
            case TreeKind::SpruceAndOak: trunk = 6 + int(r.below(4)); radius = 2; layers = 4; break;
            case TreeKind::JungleTree:   trunk = 8 + int(r.below(5)); radius = 2; layers = 3; break;
            case TreeKind::Acacia:       trunk = 3 + int(r.below(3)); radius = 3; layers = 1; break;
            case TreeKind::SwampOak:     trunk = 4 + int(r.below(2)); radius = 3; layers = 2; break;
            default:                     trunk = 4 + int(r.below(3)); radius = 2; layers = 3; break;
        }
        if (gy + trunk + layers + 1 >= kHeight) return false;
        for (int i = 1; i <= trunk; ++i) world_.set(x, gy + i, z, Wood);
        const int cy = gy + trunk;
        for (int dy = 0; dy < layers; ++dy) {
            // Taper upward, so a canopy is a crown and not a cube.
            const int rr = std::max(1, radius - dy / 2);
            for (int dz = -rr; dz <= rr; ++dz)
                for (int dx = -rr; dx <= rr; ++dx) {
                    if (std::abs(dx) + std::abs(dz) + dy > radius + layers - 1) continue;
                    if (dx == 0 && dz == 0 && dy == 0) continue;
                    if (world_.at(x + dx, cy + dy, z + dz) == Air)
                        world_.set(x + dx, cy + dy, z + dz, Leaves);
                }
        }
        return true;
    }

    // ── craft.hpp is now the source of every recipe number ──────────────────
    //
    // The ELEVEN RUNGS AND THEIR ORDER are unchanged, because every metric in
    // this file indexes by them: `rung` is an index into bench::Item and
    // `mean rung` is normalised by kItems. What changed is where the numbers
    // come from — craft.hpp's tables, which are the published ones and are
    // asserted against the published formulas in test_craft.cpp.
    //
    // TWO REAL BEHAVIOURAL CHANGES fall out of using the real tables, and both
    // are the game's rules rather than my choices:
    //   · BREAD NEEDS A CRAFTING TABLE. Three wheat in a row is a 3x1 shape, so
    //     it does not fit a 2x2 inventory grid. This file used to let an agent
    //     bake anywhere.
    //   · IRON IS SMELTED, NOT CRAFTED. There is no iron-ingot recipe in
    //     craft.hpp because the game has none; it is a 200-tick furnace cycle
    //     that burns fuel. See startSmelt().
    [[nodiscard]] static craft::ItemId craftId(int item) {
        using craft::ItemId;
        using craft::ToolKind;
        using craft::Material;
        switch (item) {
            case ItPlank:       return ItemId::Planks;
            case ItStick:       return ItemId::Stick;
            case ItTable:       return ItemId::CraftingTable;
            case ItWoodPick:    return craft::tool_item(ToolKind::Pickaxe, Material::Wood);
            case ItStonePick:   return craft::tool_item(ToolKind::Pickaxe, Material::Stone);
            case ItFurnace:     return ItemId::Furnace;
            case ItIronIngot:   return ItemId::IronIngot;
            case ItIronPick:    return craft::tool_item(ToolKind::Pickaxe, Material::Iron);
            case ItDiamondPick: return craft::tool_item(ToolKind::Pickaxe, Material::Diamond);
            case ItBread:       return ItemId::Bread;
            case ItTorch:       return ItemId::Torch;
            default:            return ItemId::None;
        }
    }

    // Where a craft.hpp ingredient lives in this sim's inventory. The two files
    // have different alphabets on purpose (craft.hpp's own header says so), so
    // this is the translation and there is exactly one of it.
    [[nodiscard]] static bool slotOf(craft::ItemId id, bool& isItem, int& index) {
        using craft::ItemId;
        switch (id) {
            case ItemId::OakLog:        isItem = false; index = Wood;        return true;
            case ItemId::Planks:        isItem = true;  index = ItPlank;     return true;
            case ItemId::Stick:         isItem = true;  index = ItStick;     return true;
            case ItemId::Cobblestone:   isItem = false; index = Cobble;      return true;
            case ItemId::Coal:          isItem = false; index = Coal;        return true;
            case ItemId::Diamond:       isItem = false; index = DiamondOre;  return true;
            case ItemId::IronOre:       isItem = false; index = IronOre;     return true;
            case ItemId::IronIngot:     isItem = true;  index = ItIronIngot; return true;
            case ItemId::Wheat:         isItem = false; index = Wheat;       return true;
            case ItemId::CraftingTable: isItem = true;  index = ItTable;     return true;
            case ItemId::Furnace:       isItem = true;  index = ItFurnace;   return true;
            case ItemId::Torch:         isItem = true;  index = ItTorch;     return true;
            case ItemId::Bread:         isItem = true;  index = ItBread;     return true;
            default:                    return false;
        }
    }

    // craft.hpp's Recipe, in the shape the rest of this file already speaks.
    // Cached in a static table because recipe_for() walks kBook linearly and
    // canCraft() is called for all eleven items several times per agent per
    // tick — that is the one place where an O(1)-per-call table matters.
    [[nodiscard]] static const Recipe& simRecipe(int item) {
        static const std::array<Recipe, kItems> table = buildRecipeTable();
        static const Recipe none{};
        if (item < 0 || item >= kItems) return none;
        return table[std::size_t(item)];
    }

    [[nodiscard]] static std::array<Recipe, kItems> buildRecipeTable() {
        std::array<Recipe, kItems> out{};
        for (int item = 0; item < kItems; ++item) {
            Recipe& o = out[std::size_t(item)];
            o.makes = 0; o.station = Air; o.tier = -1;
            for (auto& in : o.in) in = Ingredient{false, Air, 0};
            if (item == ItIronIngot) {
                // The furnace path. The INGREDIENTS listed here are what an
                // agent must be holding to be able to start a smelt — one ore
                // and one unit of fuel — so that canCraft(), the observation
                // and the scripted policy all agree with what startSmelt()
                // will actually accept. The fuel is only spent if the furnace
                // is not already lit, exactly as a furnace works.
                o.in[0] = Ingredient{false, std::uint8_t(IronOre), 1};
                o.in[1] = Ingredient{false, std::uint8_t(Coal), 1};
                o.makes = 1; o.station = std::uint8_t(Furnace);
                continue;
            }
            const craft::Recipe r = craft::recipe_for(craftId(item));
            if (!r.ok() || r.ins > int(o.in.size())) continue;
            bool translated = true;
            for (int k = 0; k < r.ins; ++k) {
                bool isItem = false; int index = 0;
                if (!slotOf(r.in[std::size_t(k)].id, isItem, index)) { translated = false; break; }
                o.in[std::size_t(k)] = Ingredient{isItem, std::uint8_t(index),
                                                 r.in[std::size_t(k)].n};
            }
            if (!translated) { o.makes = 0; continue; }
            o.makes = r.out.n;
            o.station = (r.station == craft::Station::Inventory) ? std::uint8_t(Air)
                                                                 : std::uint8_t(Table);
            // A pickaxe confers its harvest level, in blockworld's numbering
            // (0 hand, 1 wood ... 4 diamond) — craft.hpp publishes the shift.
            if (craft::is_tool(craftId(item)))
                o.tier = craft::blockworld_tier(craft::tool_material_of(craftId(item)));
        }
        return out;
    }

    // Tier (blockworld's numbering) to the material craft.hpp speaks. Tier 0 is
    // no tool at all, which is not a material, so callers must handle it.
    [[nodiscard]] static craft::Material materialOfTier(int tier) {
        switch (tier) {
            case 1:  return craft::Material::Wood;
            case 2:  return craft::Material::Stone;
            case 3:  return craft::Material::Iron;
            default: return craft::Material::Diamond;
        }
    }
    // Which inventory slot holds the pickaxe of a given tier.
    [[nodiscard]] static int pickOfTier(int tier) {
        switch (tier) {
            case 1:  return ItWoodPick;
            case 2:  return ItStonePick;
            case 3:  return ItIronPick;
            case 4:  return ItDiamondPick;
            default: return -1;
        }
    }

    // Ticks to break a block, from craft.hpp's tables rather than blockworld's
    // single conflated tier. The two agree on everything this world contains
    // except gold, which blockworld cannot express at all (speed 12, harvest
    // level 0); craft.hpp keeps the two apart. Using it here means the mining
    // clock and the tool tables can never drift.
    [[nodiscard]] static int breakTicksFor(int block, int tier) {
        const BlockRule r = block_rule(block);
        if (r.hardness < 0.0f) return -1;                 // bedrock
        if (tier <= 0)                                     // bare hands
            return std::max(1, craft::break_ticks(r.hardness, craft::kBareHandSpeed,
                                                  r.tier == 0));
        return std::max(1, craft::break_ticks_for(block, craft::ToolKind::Pickaxe,
                                                  materialOfTier(tier)));
    }

    // ── tool durability ─────────────────────────────────────────────────────
    //
    // craft.hpp's published table: wood 59 uses, stone 131, iron 250, diamond
    // 1561. Every block broken costs the held pickaxe one use; at zero it
    // breaks and the agent falls back to whatever else is in the pack. This is
    // the first thing in this sim that can take a rung AWAY, which is why
    // `rung`, `bestRung` and `bestTier` are all high-water marks and only the
    // live `tier` moves down.
    void wearTool(Agent& a) {
        const int slot = pickOfTier(a.tier);
        if (slot < 0 || a.items[std::size_t(slot)] <= 0) return;
        if (--a.durability[std::size_t(slot)] > 0) return;
        a.items[std::size_t(slot)] -= 1;
        a.durability[std::size_t(slot)] = 0;
        ++toolsBroken_;
        recomputeTier(a);
    }
    void recomputeTier(Agent& a) {
        int best = 0;
        for (int t = 1; t <= 4; ++t) {
            const int slot = pickOfTier(t);
            if (slot >= 0 && a.items[std::size_t(slot)] > 0) best = std::max(best, t);
        }
        a.tier = best;
    }

    // ── smelting: 200 ticks, and it burns fuel ──────────────────────────────
    //
    // craft.hpp: "A furnace runs at a speed of one item every 200 game tick",
    // and fuel "starts burning ... regardless of whether the upper slot has any
    // items remaining". Both are modelled: the furnace BLOCK holds the burn
    // time so two agents at one furnace share it and so a lit furnace left
    // alone wastes what is left, and the smelt itself is a committed 200-tick
    // action — the agent stands there, exactly as a player does.
    struct Smelter { std::size_t cell = 0; int burnLeft = 0; };
    [[nodiscard]] Smelter* smelterAt(std::size_t cell) {
        for (auto& s : smelters_) if (s.cell == cell) return &s;
        return nullptr;
    }
    void tickSmelters() {
        for (std::size_t i = 0; i < smelters_.size();) {
            Smelter& s = smelters_[i];
            int x, y, z;
            world_.unpack(std::uint32_t(s.cell), x, y, z);
            if (world_.at(x, y, z) != Furnace || s.burnLeft <= 0) {
                s = smelters_.back();
                smelters_.pop_back();
                continue;
            }
            --s.burnLeft;
            ++fuelBurned_;
            ++i;
        }
    }

    // Find the furnace this agent is standing at, light it if it needs lighting,
    // and take the ore. Returns the ticks the smelt will take, or 0 if it cannot
    // start. The agent then stays put for that long — see agentStep's busy path.
    [[nodiscard]] int startSmelt(Agent& a) {
        if (a.blocks[IronOre] <= 0) return 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int x = a.x + dx, y = a.y + dy, z = a.z + dz;
                    if (!world_.inside(x, y, z) || world_.at(x, y, z) != Furnace) continue;
                    const std::size_t cell = world_.idx(x, y, z);
                    Smelter* s = smelterAt(cell);
                    if (!s) { smelters_.push_back(Smelter{cell, 0}); s = &smelters_.back(); }
                    if (s->burnLeft < craft::kSmeltTicks) {
                        // Not lit, or not lit for long enough. Feed it a coal:
                        // 1600 ticks, which craft.hpp publishes as eight items.
                        // CONTINUE, NOT RETURN. canCraft asks furnaceLit()
                        // whether ANY furnace in the 3x3x3 is lit; this scan
                        // used to give up at the FIRST one it met. With two
                        // furnaces and the cold one earlier in scan order,
                        // canCraft said yes forever and startSmelt said no
                        // forever — Craft every tick, nothing smelted, and
                        // every field stuckSig() reads unchanged, so the
                        // watchdog fired. Seventh instance in this file of a
                        // test and its actor disagreeing about the same
                        // resource. The loop's own `return 0` below still
                        // handles genuinely running out.
                        if (a.blocks[Coal] <= 0) continue;
                        a.blocks[Coal] -= 1;
                        s->burnLeft += craft::fuel_burn_ticks(craft::ItemId::Coal);
                    }
                    a.blocks[IronOre] -= 1;
                    a.smeltCell = cell;
                    return craft::kSmeltTicks;
                }
        return 0;
    }

    // One tick of standing at the furnace. The burn time is the BLOCK's, so a
    // furnace another agent drained goes out under this one — which is exactly
    // what sharing a furnace means, and is why the ore is gone either way.
    void smeltTick(Agent& a) {
        if (a.smeltLeft <= 0) return;
        int x, y, z;
        world_.unpack(std::uint32_t(a.smeltCell), x, y, z);
        const Smelter* s = smelterAt(a.smeltCell);
        if (!s || s->burnLeft <= 0 || world_.at(x, y, z) != Furnace) {
            a.smeltLeft = 0;
            a.busy = 1;             // the caller's --busy ends the option here
            return;
        }
        if (--a.smeltLeft <= 0) { finishSmelt(a); a.smeltLeft = 0; }
    }

    void finishSmelt(Agent& a) {
        // craft.hpp says what an iron ore block smelts into; it is not assumed
        // here. If it ever stops being an ingot, this stops handing out ingots.
        const craft::SmeltResult out = craft::smelt(craft::item_from_block(IronOre));
        if (out.out != craft::ItemId::IronIngot) return;
        a.items[ItIronIngot] += 1;
        ++a.crafted;
        ++smeltsDone_;
        a.rung = std::max(a.rung, int(ItIronIngot) + 1);
        a.bestRung = std::max(a.bestRung, a.rung);
    }

    // ── mobs ────────────────────────────────────────────────────────────────
    //
    // WHY THERE IS A SPAWN LOOP HERE INSTEAD OF MobWorld::spawn_cycle. That
    // function is written about ONE player, because Minecraft's spawn rules are:
    // the no-spawn bubble, the spawn radius and the despawn distance are all
    // measured from a player and there is exactly one. A town has up to
    // thirty-two. So the pack loop, the caps and the spawn table below are
    // mobs.hpp's published ones, called directly, with the distance test
    // widened to "far enough from EVERY agent, close enough to at least one".
    //
    // The LIGHT test is LightEngine's throughout — here, and inside
    // MobWorld::tick, which gets the same engine through EngineLightView. It
    // was not always: mobs.hpp's own point-source approximation used to answer
    // inside the tick while the engine answered out here, and the two disagreed
    // by construction. One field now, one answer.
    static constexpr int kSpawnAttemptsPerTick = 3;

    [[nodiscard]] bool spawnDistanceOk(int x, int y, int z) const {
        bool nearAny = false;
        for (const auto& a : agents_) {
            if (!a.alive) continue;
            const float dx = float(x) - float(a.x), dy = float(y) - float(a.y),
                        dz = float(z) - float(a.z);
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d <= kMinSpawnDistance) return false;      // inside somebody's bubble
            if (d <= kSpawnRadius) nearAny = true;
        }
        return nearAny;
    }

    // [W-SPAWN] for animals: a grass block, and a light level of 9 or more.
    [[nodiscard]] bool animalSpawnOk(int x, int y, int z) const {
        return light_.spawnable_footing(x, y, z)
            && world_.at(x, y - 1, z) == Grass
            && light_.light(x, y, z) >= kAnimalMinLight;
    }

    void spawnMobs() {
        const int chunks = std::max(1, (n_ / 16) * (n_ / 16));
        for (int c = 0; c < kMobCats; ++c) {
            const MobCat cat = MobCat(c);
            const int cap = mob_cap(cat, chunks);
            for (int attempt = 0; attempt < kSpawnAttemptsPerTick; ++attempt) {
                if (mobs_.alive(cat) >= cap) break;
                const int cx = int(rng_.below(std::uint32_t(n_)));
                const int cz = int(rng_.below(std::uint32_t(n_)));
                const int top = world_.surface(cx, cz);
                if (top < 1) continue;
                // Uniform in the column, which is why caves take the bulk of
                // hostile spawns rather than the surface. Flagged in mobs.hpp as
                // a reconstruction rather than a citation, and it is inherited
                // here with the same caveat.
                const int cy = 1 + int(rng_.below(std::uint32_t(top + 1)));
                if (!spawnDistanceOk(cx, cy, cz)) continue;
                const bool ok = (cat == CatMonster)
                              ? light_.roll_hostile_spawn(cx, cy, cz, skyDarken(), rng_)
                              : animalSpawnOk(cx, cy, cz);
                if (!ok) continue;
                const MobSpawnEntry e = pick_spawn_entry(cat, rng_);
                const int pack = e.minCount
                               + int(rng_.below(std::uint32_t(1 + e.maxCount - e.minCount)));
                int mx = cx, mz = cz;
                for (int i = 0; i < pack; ++i) {
                    if (mobs_.alive(cat) >= cap) break;
                    mx += int(rng_.below(kPackSpread)) - int(rng_.below(kPackSpread));
                    mz += int(rng_.below(kPackSpread)) - int(rng_.below(kPackSpread));
                    if (!spawnDistanceOk(mx, cy, mz)) continue;
                    const bool ok2 = (cat == CatMonster)
                                   ? light_.hostile_can_spawn(mx, cy, mz, skyDarken())
                                   : animalSpawnOk(mx, cy, mz);
                    if (!ok2) continue;
                    mobs_.add(e.kind, float(mx) + 0.5f, float(cy), float(mz) + 0.5f);
                    ++mobSpawned_;
                }
            }
        }
    }

    // Tick the population once, in slices, one per agent.
    //
    // MobWorld::tick installs one player and runs every mob against it. Ticking
    // it once per agent would tick every mob N times — N times the movement and
    // N times the damage. Instead each mob is assigned to its NEAREST living
    // agent, and each slice is swapped into the population in turn: every mob is
    // ticked exactly once, against the agent it is actually near, and the damage
    // a slice returns belongs to that agent. Cost is O(mobs x agents) for the
    // assignment plus two moves per mob — at the published cap of 70 monsters
    // that is nothing.
    void tickMobs() {
        spawnMobs();
        auto& live = mobs_.mobs();
        if (live.empty()) return;
        pool_.clear();
        pool_.swap(live);
        survivors_.clear();

        for (std::size_t i = 0; i < pool_.size(); ++i) {
            const int ai = nearestAgent(pool_[i]);
            if (ai < 0) {
                // No living agent to be near. Left alone rather than deleted: a
                // town with everybody dead is a town about to respawn, not an
                // empty world.
                survivors_.push_back(pool_[i]);
                continue;
            }
            slice_.clear();
            slice_.push_back(pool_[i]);
            mobs_.mobs().swap(slice_);            // a population of exactly one
            mobs_.set_player(float(agents_[ai].x) + 0.5f, float(agents_[ai].y),
                             float(agents_[ai].z) + 0.5f);
            const TickStats st = mobs_.tick(world_, lightView_, timeOfDay(), kDifficulty);
            applyMobDamage(agents_[ai], st);
            mobBurning_ += st.burning;
            mobs_.mobs().swap(slice_);
            if (!slice_.empty() && slice_[0].alive) survivors_.push_back(slice_[0]);
        }
        mobs_.mobs().swap(survivors_);
    }

    [[nodiscard]] int nearestAgent(const Mob& m) const {
        int best = -1;
        float bestD = 0.f;
        for (std::size_t i = 0; i < agents_.size(); ++i) {
            if (!agents_[i].alive) continue;
            const float dx = m.x - float(agents_[i].x), dy = m.y - float(agents_[i].y),
                        dz = m.z - float(agents_[i].z);
            const float d = dx * dx + dy * dy + dz * dz;
            if (best < 0 || d < bestD) { best = int(i); bestD = d; }
        }
        return best;
    }

    // ONE MOB'S SWING, gated by the game's 10-tick invulnerability window � the
    // same counter this file already uses for lava and for falls.
    //
    // WHY tickMobs TICKS ONE MOB AT A TIME, which is the whole reason this
    // function can be correct. MobWorld::tick returns the SUM of every swing
    // that connected during the call. Ticking a whole slice at once therefore
    // handed this function a batch, and a batch cannot be gated: four zombies
    // on one agent arrived as a single 12.0 and went straight through, because
    // hurtCooldown was 0 when the batch showed up and there was no way to tell
    // it apart from one very hard hit.
    //
    // MEASURED, and this is exactly the failure this project keeps catching �
    // the window was WIRED and INERT. Four zombies on one agent took it from
    // 20.00 to 8.24 health in one tick, with 12.00 damage swung, 12.00 landed
    // and 0.00 suppressed. Java's rule is that a second hit inside the window
    // lands only if it is stronger, and then only for the difference; for a
    // crowd of identical mobs that is "the first one counts", which is what a
    // per-mob call now produces.
    void applyMobDamage(Agent& a, const TickStats& st) {
        mobHits_ += st.attacks + st.explosions;
        if (st.playerDamage <= 0.f) return;
        mobDamage_ += double(st.playerDamage);
        // The comment above states Java's rule and the code did not implement
        // it: a STRONGER hit inside the window is not eaten, it lands for the
        // DIFFERENCE against the hit the window is already holding. Suppressing
        // it outright means a creeper detonating on an agent one tick after a
        // zombie punched it does nothing at all, which is the wrong answer in
        // the one case the rule exists to cover.
        float land = st.playerDamage;
        if (a.hurtCooldown > 0) {
            if (st.playerDamage <= a.lastHurt) return;   // weaker or equal: eaten
            land = st.playerDamage - a.lastHurt;
        }
        a.health -= land;
        a.lastHurt = std::max(a.lastHurt, st.playerDamage);
        a.hurtCooldown = kHurtCooldown;
        appliedMobDamage_ += double(land);
        if (a.health <= 0.f) { a.alive = false; ++mobKills_; }
    }

    // Normal, which is what a Minecraft world runs at unless told otherwise —
    // and the difficulty damage_to_player() scales by. Not a knob, because a
    // knob that nothing measures is decoration.
    static constexpr Difficulty kDifficulty = DiffNormal;

    // The nearest hostile mob, for the observation. O(mobs), and the population
    // is capped, so this is a few dozen distance tests.
    [[nodiscard]] bool nearestHostile(const Agent& a, float& dx, float& dz,
                                      float& dist) const {
        bool found = false;
        float bestD = 0.f;
        for (const Mob& m : mobs_.mobs()) {
            if (!m.alive || !mob_hostile(m.kind)) continue;
            const float ex = m.x - float(a.x), ey = m.y - float(a.y), ez = m.z - float(a.z);
            const float d = ex * ex + ey * ey + ez * ez;   // ranked in 3D...
            if (!found || d < bestD) { found = true; bestD = d; dx = ex; dz = ez; }
        }
        // ...but reported as a HEADING, which is horizontal. The agent's own
        // movement is horizontal plus a one-block step, so a vertical component
        // it cannot act on would be a feature that only adds variance.
        dist = found ? std::sqrt(bestD) : 0.f;
        return found;
    }

    void buildPalette() {
        pal_.assign(std::size_t(kBlocks) + 2, Swatch{{0,0,0}, ""});
        auto put = [&](Block b, int r, int g, int bl) {
            pal_[std::size_t(b)] = Swatch{{std::uint8_t(r), std::uint8_t(g), std::uint8_t(bl)},
                                          block_name(b)};
        };
        put(Air,        14,  18,  24);  put(Bedrock,    38,  38,  44);
        put(Stone,     132, 134, 140);  put(Dirt,      112,  82,  58);
        put(Grass,     102, 172,  92);  put(Sand,      214, 202, 150);
        put(Water,      54, 110, 190);  put(Wood,      118,  86,  50);
        put(Leaves,     58, 132,  66);  put(Coal,       48,  48,  54);
        put(IronOre,   196, 158, 122);  put(GoldOre,   226, 190,  74);
        put(DiamondOre, 96, 216, 220);  put(Plank,     176, 138,  88);
        put(Cobble,    138, 138, 142);  put(Table,     162, 112,  62);
        put(Furnace,    88,  88,  94);  put(Torch,     250, 218, 120);
        put(Chest,     150, 108,  56);  put(Farmland,   96,  70,  46);
        put(Wheat,     206, 190,  94);  put(Road,      160, 160, 168);
        put(RedstoneOre,190, 62,  56);  put(LapisOre,   52,  86, 176);
        put(EmeraldOre, 62, 200, 110);  put(Obsidian,   32,  26,  48);
        put(Gravel,    134, 130, 126);  put(Lava,      224, 116,  40);
        put(Sapling,    78, 168,  72);  put(SaplingAged, 46, 120,  58);
        pal_[std::size_t(kBlocks)] = Swatch{{255, 255, 255}, "agent"};
        // One more index than there are blocks plus the agent marker: the body
        // you are driving. A town of white cursors with no way to tell which one
        // is yours is unplayable, and "the third one from the left, probably" is
        // not a way to see anything. Amber because nothing in the block palette
        // is — the ores are red, blue, green and grey, and gold is the one
        // colour a white marker cannot be confused with at one voxel across.
        pal_[std::size_t(kBlocks) + 1] = Swatch{{255, 196, 40}, "you"};
    }

    void buildKnobs() {
        knobs_ = {
            {"policy", "who is driving", 0.f, 4.f, 0.f, 1.f,
             {"learned", "scripted", "random", "dynamic script", "possessed"}, true,
             "The comparison this sim exists to make. LEARNED is a deep Q-network per agent. "
             "SCRIPTED reads the recipe table and walks to whatever the next rung needs — it "
             "is not intelligent, it is informed, and it is the bar a learner has to clear. "
             "RANDOM draws uniformly from the action space and is the floor. Any claim that "
             "learning works here means beating scripted on the same world seed, not beating "
             "random. POSSESSED hands ONE agent to the keyboard and leaves the rest on the "
             "scripted policy: it is the HUMAN BASELINE, the number this comparison has never "
             "had. It is a policy like the others — same action space, same metrics, same "
             "seeds — so a person's run is read off the same axis as the machines'."},
            // How long the possessed agent waits for you before it goes back to
            // work. An empty chair is not a policy: an agent left standing still
            // scores like a broken one and would publish that as "what a person
            // achieves".
            //
            // 12,000 ticks is Minecraft's daylight period, which daylight()
            // above already uses — so the agent is yours for half a game day
            // after your last keystroke, and the constant is one this file
            // already had rather than a second number to keep in step.
            //
            // In SECONDS it depends on how fast the app runs, so that was
            // measured rather than assumed (poss_rate.cpp, 8 agents, size 96,
            // the shipped defaults, this machine, three runs):
            //
            //     speed  1   50.5 / 43.3 / 28.7 frames/s     51 /   43 /   29 t/s
            //     speed 20   42.7 / 40.0 / 28.1 frames/s    854 /  800 /  562 t/s
            //     speed 50   35.4 / 38.4 / 27.7 frames/s   1769 / 1922 / 1384 t/s
            //
            // The spread is wide — the slowest run was taken with the self-test
            // suite on the other cores — so the honest statement is a RANGE:
            // 12,000 ticks is 14 to 21 seconds of hands-off wall clock at the
            // shipped speed, and it scales with whatever else the machine is
            // doing. Quoting a single second figure here would be quoting the
            // load on this machine at one moment.
            //
            // The first draft used 2,400 ticks, sized against a HEADLESS timing
            // of 17,600 ticks/s from run_quiet — which skips publish() and so
            // is not the rate anybody plays at. That would have been under
            // three real seconds: the timeout would have fired on anyone who
            // paused to think, and the human baseline would have been mostly
            // scripted without ever saying so.
            // requires policy = possessed, and that declaration is load-bearing.
            // Without it the suite's dead-knob test swept this control with the
            // policy at its default and found four identical trajectories —
            // correctly, because with nobody possessed nothing reads it — and
            // reported "voxelcity: knob 'idle' changes the outcome  FAIL". The
            // knob is not dead, it is CONDITIONAL, and the two are different
            // defects with different fixes. Naming the dependency also lets the
            // panel say the control is inert right now instead of leaving it to
            // look broken.
            {"idle", "possession idle timeout, ticks", 0.f, 24000.f, 12000.f, 60.f, {}, false,
             "Ticks of no keyboard input before the possessed agent is handed back to the "
             "scripted policy. It exists so that walking away from the keyboard does not "
             "quietly publish a human baseline of zero. The readout says AUTOPILOT the moment "
             "it fires, and any input takes the agent straight back. At 0 the agent is never "
             "yours at all, which is the control arm the suite measures possession against.",
             false, false, "policy", float(PolicyPossessed)},
            {"agents", "how many agents", 1.f, 32.f, 8.f, 1.f, {}, true,
             "One world, this many learners in it. They compete for the same veins, so this "
             "is not simply N independent runs — doubling the town does not double what it "
             "collects, and finding out by how much it falls short is the point of the knob."},
            {"size", "world size", 0.f, 3.f, 1.f, 1.f, {"64", "96", "160", "256"}, true,
             "Blocks across; the world is this wide and deep, and 64 tall regardless. Wide "
             "and shallow on purpose - a town needs ground, and a cube spends its volume on "
             "stone nobody visits."},
            {"seed", "world seed", 1.f, 40.f, 1.f, 1.f, {}, true,
             "Which world is generated, and which agent brains are initialised. Same seed, "
             "same run, every time."},
            {"lr", "learning rate", 0.001f, 0.20f, 0.01f, 0.001f, {}, false,
             "Step size for every agent's network. Read live, so it can be turned down once "
             "a town has stopped improving.", false},
            {"epsilon", "exploration", 0.f, 1.f, 0.25f, 0.01f, {}, false,
             "Chance an agent ignores its network and acts at random. Each agent rolls on "
             "its OWN stream — sharing one would make the town explore in lockstep, and a "
             "population result from correlated explorers measures the sampler."},
            {"roles", "division of labour", 0.f, 1.f, 1.f, 1.f, {"all-rounders", "specialists"}, true,
             "ALL-ROUNDERS gives every agent the same job — get as far up the tech tree as you "
             "can — which is eight identical maximisers sharing a map rather than a town, and "
             "leaves each of them the entire eleven-rung chain to solve alone. SPECIALISTS "
             "splits them into foresters, miners, builders and provisioners, each paid mainly "
             "for its own trade and still paid for the town's progress. This is a knob and not "
             "a default because 'specialisation helps' is a claim, and the sim should be able "
             "to show it false."},
            {"share", "share experience", 0.f, 1.f, 0.f, 1.f, {"private", "pooled"}, false,
             "PRIVATE gives every agent only what it lived through. POOLED copies each "
             "transition to every agent's buffer, so twenty agents learn from twenty times "
             "the experience at twenty times the cost. This is the collective-learning "
             "claim, and it is a switch precisely so it can be measured rather than assumed."},
            {"hunger", "hunger rate", 0.f, 4.f, 1.f, 0.1f, {}, false,
             "How fast food drains. At 0 nothing starves and the sim is pure advancement; "
             "turn it up and staying alive starts to compete with climbing the tree."},
            {"speed", "ticks per frame", 1.f, 200.f, 20.f, 1.f, {}, false,
             "How many world ticks happen per drawn frame. Display rate only.", true},
        };
    }

    void rebuild() {
        static const int sizes[4] = {64, 96, 160, 256};
        n_ = sizes[std::clamp(int(knob("size") + 0.5f), 0, 3)];
        world_.resize(n_, kHeight);
        view_ = Field(n_, n_);
        // Clamped to the knob's DECLARED range, not merely to 1. rebuild()
        // took whatever it was handed, so on_knob("agents", 5000) built five
        // thousand agents and a tape header could ask for a hundred thousand.
        // A knob whose published maximum the code does not enforce is a maximum
        // by convention only.
        const int want = std::clamp(int(knob("agents") + 0.5f), 1, kMaxAgents);
        agents_.resize(std::size_t(want));
        // Shrinking the town must not leave possession pointing past the end of
        // it. agentStep compares the possessed index against a pointer offset,
        // so a stale index does not crash — it silently possesses NOBODY, which
        // is the quiet kind of failure worth spending two lines to rule out.
        if (possessed_ >= want) possessed_ = want - 1;
        // Observation width is fixed by the encoder, so the network shape is
        // decided once here rather than being discovered at the first forward
        // pass — a mismatch there is a silent read past the end of a vector.
        //
        // And it is now CHECKED, at the bottom of this function, rather than
        // left as a comment. kObs is a sum of seven terms and this change added
        // two more to it; getting that arithmetic wrong costs nothing visible,
        // because a too-short observation still forwards and still trains — on
        // whatever happens to sit past the end of the vector.
        DqnConfig cfg;
        // Two hidden layers. The bearings are directional and the useful
        // features are products of them ("wood is close AND I have none"),
        // which a single layer has to memorise case by case.
        cfg.layers  = {kObs, 96, 64, kActions};
        cfg.gamma   = 0.99f;      // the tech tree pays out a long way off
        cfg.batch   = 8;
        cfg.capacity= 20000;
        cfg.syncEvery = 400;
        cfg.epsMin  = 0.05f;
        cfg.epsDecay= 0.004f;     // per life, so exploration fades as it learns
        for (std::size_t i = 0; i < agents_.size(); ++i) {
            cfg.lr      = knob("lr");
            cfg.epsilon = knob("epsilon");
            agents_[i].brain = Dqn(cfg, worldSeed() ^ (std::uint64_t(i + 1) * 0x2545F491ull));
            // Round-robin, so a town of eight has two of each rather than
            // whatever a random draw happens to give it. A settlement with no
            // miner is a different experiment, not a worse sample.
            agents_[i].role = (knob("roles") > 0.5f) ? int(i % kRoles) : -1;
            std::vector<int> prio;
            prio.reserve(rulebase().size());
            for (const auto& r : rulebase()) prio.push_back(r.priority);
            DynScriptConfig dc;
            dc.scriptSize = 8;
            agents_[i].book = DynamicScript(int(rulebase().size()), std::move(prio), dc,
                                            worldSeed() ^ (std::uint64_t(i + 1) * 0x1D5C0DEull));
            agents_[i].book.new_script();
        }
        reset();
        // The check the comment above used to only promise. Once per rebuild,
        // which is never in a hot path.
        if (!agents_.empty() && int(observe(agents_[0]).size()) != kObs) {
            std::fprintf(stderr,
                         "voxelcity: observation is %d wide, network expects %d\n",
                         int(observe(agents_[0]).size()), kObs);
            std::abort();
        }
    }

    void resetAgent(Agent& a, int i, bool full) {
        // Mixed with the DEATH COUNT, not just the agent index.
        //
        // Keyed on the index alone, every respawn of agent i drew the identical
        // sequence and therefore landed in the identical spot. An agent that
        // spawned somewhere unworkable respawned there forever — which is why
        // exactly two of eight agents stayed stuck on every seed after the
        // spawn-viability check went in, a number too steady to be luck. The
        // check improved WHERE they first land; it could not help an agent
        // condemned to return to the same square for the whole run.
        //
        // Still fully deterministic: deaths is part of the run's own state, so
        // the same seed replays the same lives in the same order.
        Rng r(worldSeed() ^ (std::uint64_t(i + 1) * 0xA24BAull)
                          ^ (std::uint64_t(a.deaths) * 0x9E3779B97F4A7C15ull));
        // Spawn on solid ground, never in water and never inside rock.
        // A SPAWN AN AGENT CAN ACTUALLY WORK FROM.
        //
        // This used to take the first patch of land that was not water, and
        // nothing checked whether anything was REACHABLE from it. At seed 1 on
        // the shipped defaults, agent 0 landed with no harvestable block inside
        // its 20-block sense radius and sat there: measured stuck at (29,56,88)
        // from tick 1000 to tick 5000, all five senses reporting nothing,
        // cycling food 20 to 0 and back, finishing 6,000 ticks at rung 0.
        //
        // It is worse than one wasted agent. That cell scores rung 0 under
        // scripted, random, DQN and possession alike, so every cross-policy
        // comparison including that seed averages in a square where NO policy
        // can do anything, and quietly drags all of them toward zero. It is a
        // measurement bug wearing the costume of a weak agent.
        //
        // So a candidate must have wood within sense range. Checked on a coarse
        // stride rather than every column — this runs on every respawn, and an
        // exact answer costs more than the difference is worth. The best
        // candidate seen is kept, so a genuinely treeless world still spawns
        // somebody rather than looping.
        int bestX = -1, bestZ = -1, bestY = 0, bestWood = -1;
        for (int tries = 0; tries < 64; ++tries) {
            const int x = int(r.unit() * float(n_)) % n_;
            const int z = int(r.unit() * float(n_)) % n_;
            const int s = world_.surface(x, z);
            if (s < 0 || s + 1 >= kHeight) continue;
            if (world_.at(x, s, z) == Water) continue;
            int wood = 0;
            for (int dz = -20; dz <= 20 && wood < 4; dz += 4)
                for (int dx = -20; dx <= 20 && wood < 4; dx += 4) {
                    const int cx = x + dx, cz = z + dz;
                    if (cx < 0 || cz < 0 || cx >= n_ || cz >= n_) continue;
                    const int top = world_.surface(cx, cz);
                    if (top < 0) continue;
                    // A trunk stands above the ground it grew from.
                    for (int dy = 0; dy <= 5; ++dy)
                        if (world_.at(cx, top - dy, cz) == Wood) { ++wood; break; }
                }
            if (wood > bestWood) { bestWood = wood; bestX = x; bestZ = z; bestY = s + 1; }
            if (wood >= 4) break;                 // good enough, stop looking
        }
        if (bestX >= 0) { a.x = bestX; a.z = bestZ; a.y = bestY; }
        a.health = kMaxHealth; a.food = kMaxFood; a.alive = true; a.stepsAlive = 0;
        a.blocks.fill(0); a.items.fill(0);
        a.tier = 0; a.rung = 0;
        // The ladder went with the corpse, so the script that starts the next
        // life starts from the bottom of it.
        a.scriptStartRung = 0; a.scriptStartTier = 0;
        a.scriptStartPot = 0.f; a.scriptStartPlaced = a.placed;
        if (full) { a.bestRung = 0; a.bestTier = 0; a.rungLifeSum = 0; a.livesEnded = 0; }
        a.epTicks = 0;
        a.lastObs.clear(); a.lastAction = -1; a.episodeReward = 0.f;
        a.tgtX = a.tgtY = a.tgtZ = -1; a.tgtWhat = 0; a.tgtAge = 0;
        a.searchCooldown = 0; a.roamDir = 0; a.facing = 0;
        a.wanderX = a.wanderZ = -1; a.wanderAge = 0;
        a.sense.fill(Agent::Sense{}); a.senseAge = 0;
        a.miningX = a.miningY = a.miningZ = -1; a.miningLeft = 0; a.airTicks = 0;
        a.busy = 0; a.busyAction = -1; a.busyReward = 0.f; a.busyObs.clear();
        a.building = false;
        a.hurtCooldown = 0; a.lastHurt = 0.f; a.exhaustion = 0.f; a.saturation = 5.f;
        // A corpse does not keep its pickaxe, so it does not keep the wear on
        // it either. Cleared with the inventory above, not separately, or a
        // respawned agent's first tool would inherit a used-up bar.
        a.durability.fill(0);
        a.smeltLeft = 0; a.smeltCell = 0;
        if (full) {
            a.mined = a.placed = a.crafted = a.deaths = 0;
            a.rng.reseed(worldSeed() ^ (std::uint64_t(i + 1) * 0x9E3779B1ull));
        }
    }

    // ── observation ─────────────────────────────────────────────────────────
    //
    // Deliberately local and small. A network that is handed the whole world has
    // not solved navigation, it has been given the answer; and an agent in
    // Minecraft cannot see through stone either. Six neighbours, four facts
    // each, plus its own condition and what it is carrying.
    // Kept next to the reward that uses it, so the shaping and the learner
    // cannot drift apart: a potential-based term discounted at one gamma and
    // bootstrapped at another is not potential-based shaping at all.
    static constexpr float kGamma = 0.99f;

    static constexpr int kSenses = 5;
    // Three light features and four for the nearest hostile.
    //
    // THE LIGHT ONES because the world now has a variable an agent's survival
    // depends on and could not see: where it is dark, hostiles spawn. THE MOB
    // ONES for the harder reason — a mob that hits an agent changes the reward
    // (health, and the -1 for dying) and was invisible in the observation, which
    // is not a hard credit-assignment problem, it is label noise in the TD
    // target. This file already learned that lesson once, when the builder and
    // provisioner rewards were added without the state they were a function of.
    static constexpr int kLightObs = 3;
    static constexpr int kMobObs   = 4;
    static constexpr int kObs = kSenses * 5 + 11 + 8 + kRoles + 6 + kLightObs + kMobObs;

    // Refresh the bearings, staggered across agents so a town of thirty does
    // not run thirty shell searches on the same tick.
    void refreshSenses(Agent& a, int index) {
        if (a.senseAge > 0 && (a.senseAge % 24) != (index % 24)) { ++a.senseAge; return; }
        ++a.senseAge;
        for (int i = 0; i < kSenses; ++i) {
            const std::uint8_t what = kGoTarget[i];
            // Do not look for what this agent could not harvest anyway. The
            // scripted policy acts on the same fact, through wantedBlock.
            if (!drops_for(what, a.tier)) { a.sense[std::size_t(i)] = Agent::Sense{}; continue; }
            int tx, ty, tz;
            if (nearest(a, what, 20, tx, ty, tz))
                a.sense[std::size_t(i)] = Agent::Sense{tx, ty, tz, true};
            else
                a.sense[std::size_t(i)] = Agent::Sense{};
        }
    }

    [[nodiscard]] std::vector<float> observe(const Agent& a) const {
        std::vector<float> o;
        o.reserve(kObs);
        for (int i = 0; i < kSenses; ++i) {
            const auto& sn = a.sense[std::size_t(i)];
            if (!sn.found) { o.insert(o.end(), {0.f, 0.f, 0.f, 0.f, 1.f}); continue; }
            const float dx = float(sn.x - a.x), dy = float(sn.y - a.y), dz = float(sn.z - a.z);
            const float d  = std::max(1.f, std::sqrt(dx*dx + dy*dy + dz*dz));
            o.push_back(1.f);
            o.push_back(dx / d); o.push_back(dy / d); o.push_back(dz / d);
            o.push_back(std::min(1.f, d / 24.f));
        }
        o.push_back(float(a.y) / float(kHeight));
        o.push_back(a.health / kMaxHealth);
        o.push_back(a.food / kMaxFood);
        o.push_back(float(a.tier) * 0.25f);
        o.push_back(float(a.rung) / float(kItems));
        o.push_back(nearStation(a, Table)   ? 1.f : 0.f);
        o.push_back(nearStation(a, Furnace) ? 1.f : 0.f);
        // Is anything craftable right now? The scripted policy reads the recipe
        // table; withholding the same fact would make the contest about
        // bookkeeping rather than about strategy.
        bool any = false;
        for (int it = 0; it < kItems && !any; ++it) any = canCraft(a, it);
        o.push_back(any ? 1.f : 0.f);
        o.push_back(float(daylight()));
        o.push_back(block_solid(world_.at(a.x, a.y - 1, a.z)) ? 1.f : 0.f);
        // What it is looking at, since that is the only block it can break.
        o.push_back(std::min(1.f, float(block_rule(faced(a)).tier) * 0.25f));
        auto carry = [&](int b) { return std::min(1.f, float(a.blocks[std::size_t(b)]) / 8.f); };
        o.push_back(carry(Wood));       o.push_back(carry(Cobble));
        o.push_back(carry(Coal));       o.push_back(carry(IronOre));
        o.push_back(carry(DiamondOre)); o.push_back(carry(Dirt));
        o.push_back(std::min(1.f, float(a.items[ItPlank]) / 8.f));
        o.push_back(std::min(1.f, float(a.items[ItStick]) / 8.f));
        // Which trade this one follows. Without it a shared network could not
        // tell a forester from a miner and would learn the average of the two,
        // which is neither.
        for (int r = 0; r < kRoles; ++r) o.push_back(a.role == r ? 1.f : 0.f);

        // ── everything the REWARD is a function of, which this was missing ──
        //
        // progressScore() pays the builder for `placed`, for tables and for
        // furnaces, and pays the provisioner for bread and wheat. None of those
        // appeared here. The network was being paid for state changes it could
        // not perceive, which is not a hard credit-assignment problem — it is
        // label noise in the TD target, and it is why adding the two new trades
        // could not possibly have helped.
        //
        // The rule this enforces: the reward must be a function of the
        // observation. DreamerV3's Minecraft environment ships a second
        // `inventory_max` vector alongside the inventory for exactly this
        // reason — to make a milestone reward Markovian.
        o.push_back(std::min(1.f, float(a.placed) / 64.f));
        o.push_back(std::min(1.f, float(a.items[ItTable])));
        o.push_back(std::min(1.f, float(a.items[ItFurnace])));
        o.push_back(std::min(1.f, float(a.items[ItBread]) / 3.f));
        o.push_back(std::min(1.f, float(a.blocks[Wheat]) / 8.f));
        // The high-water mark of the tech tree, which is what the town term is
        // actually paid on. `rung` above is the same number but saturates
        // differently; this one is the milestone signal.
        o.push_back(float(a.tier) / 4.f);

        // ── light ──
        // internal_light is the number the game's own rules run on — spawning,
        // crops, daylight sensors — not the displayed max(sky, block). The third
        // feature is the spawn predicate itself, because "it is dark enough here
        // for something to appear" is the fact that matters and asking a network
        // to derive a threshold it could be handed is free difficulty.
        const int dark = skyDarken();
        o.push_back(float(light_.internal_light(a.x, a.y, a.z, dark)) / float(kLightMax));
        o.push_back(float(light_.block_light(a.x, a.y, a.z)) / float(kLightMax));
        o.push_back(light_.light_allows_hostile_spawn(a.x, a.y, a.z, dark) ? 1.f : 0.f);

        // ── the nearest hostile ──
        float mx = 0.f, mz = 0.f, md = 0.f;
        if (nearestHostile(a, mx, mz, md) && md > 0.f) {
            o.push_back(mx / md);
            o.push_back(mz / md);
            o.push_back(std::min(1.f, md / 32.f));
            o.push_back(1.f);
        } else {
            o.insert(o.end(), {0.f, 0.f, 1.f, 0.f});
        }
        return o;
    }

    // ── texture ─────────────────────────────────────────────────────────────
    //
    // Every block was one flat colour, so a cliff of stone was one enormous
    // grey polygon and the world read as coloured cubes rather than as ground.
    // There are no image files here and there is nothing to load: the grain is
    // a hash of the block's own coordinates, which costs nothing to store, is
    // identical every frame (a texture that shimmers when the camera moves is
    // worse than none), and never repeats the way a tiled bitmap does.
    //
    // Three things happen here, in order of how much they matter:
    //
    //   1. GRAIN. A per-voxel brightness jitter, wider on rough materials than
    //      on smooth ones — stone and gravel are speckled, water and obsidian
    //      nearly flat. This alone is most of the effect.
    //   2. FACE. A grass block is green on top and dirt down the sides, with a
    //      fringe of green over the top of the side faces. Sand, farmland and
    //      road get gentler versions of the same idea.
    //   3. DEPTH. Stone darkens as it goes down, so a cutaway reads as strata
    //      rather than as one grey mass, and the ore bands are legible without
    //      counting blocks.
    // THE FLIP THAT WAS HERE, AND WHY IT IS GONE. This used to be
    //
    //     int flipY(int y) const { return kHeight - 1 - y; }
    //
    // on the stated grounds that "the renderer's Y grows downward". It does not.
    // VoxelRenderer places voxel y at world coordinate (y + 0.5) * scale - 1 and
    // the camera's eye sits at +y for a positive pitch, so render y is ordinary
    // up. The flip and camera_home() therefore cancelled, and the sim framed the
    // BEDROCK UNDERSIDE of the map.
    //
    // MEASURED, because two comments in this file disagreed with the arithmetic
    // and a third disagreed with both. Rendering the shipped default camera and
    // histogramming the output gave 212,017 background pixels and, of the rest,
    // nothing but bedrock and deep-stone greys — no grass, no water, no wood, on
    // a world that generates all three. With the flip removed the same histogram
    // is grass, water, sand and leaves. That is the whole change: face index 2 is
    // world-up, which is what texture() already assumed when it put turf on it.
    [[nodiscard]] static int flipY(int y) { return y; }

    // How much light is falling on the block at (x, y, z), for shading.
    //
    // Sampled from the six NEIGHBOURS as well as the cell itself. An opaque
    // block's own cell holds light 0 by definition — that is what opaque means —
    // so shading by it alone would paint the entire world black. What a viewer
    // sees of a block is the light falling ON it, which lives in whichever open
    // cell is beside it. Taking the brightest of the six is also independent of
    // which face happens to point at the camera, which matters because this
    // file's face convention was wrong for a long time and nothing noticed.
    //
    // A neighbour OUTSIDE THE MAP counts as open sky, not as rock. LightEngine
    // reports out-of-bounds as dark, which is exactly right for the spawn rules
    // — nothing spawns off the edge — and exactly wrong here: the map boundary
    // is a cut through a world, not a wall around one. Treating it as rock
    // painted the entire cliff face black, and that face is most of the picture
    // at the default camera. MEASURED: with the boundary dark, the whole render
    // came out in the #060606-#090909 range, values of 6 to 9 out of 255. It is
    // the same "a guard written for one caller becomes a wall for another" that
    // BlockWorld::at's own comment records about reading Bedrock off the edge.
    //
    // internal_light, not light(): the internal value is the one the game's own
    // rules run on, and it is what makes the surface dim at night instead of
    // staying noon-bright while zombies spawn on it.
    [[nodiscard]] int litAround(int x, int y, int z) const {
        const int dark = skyDarken();
        const int offMap = std::max(0, kLightMax - dark);
        auto sample = [&](int ax, int ay, int az) {
            return world_.inside(ax, ay, az) ? light_.internal_light(ax, ay, az, dark)
                                             : offMap;
        };
        static constexpr int dxs[6] = { 1, -1, 0, 0, 0, 0 };
        static constexpr int dys[6] = { 0, 0, 1, -1, 0, 0 };
        static constexpr int dzs[6] = { 0, 0, 0, 0, 1, -1 };
        int lit = world_.inside(x, y, z) ? light_.internal_light(x, y, z, dark) : offMap;
        for (int d = 0; d < 6; ++d) lit = std::max(lit, sample(x + dxs[d], y + dys[d],
                                                               z + dzs[d]));
        return lit;
    }

    [[nodiscard]] static std::uint32_t texHash(int x, int y, int z, int salt) {
        std::uint32_t h = std::uint32_t(x) * 374761393u
                        + std::uint32_t(y) * 668265263u
                        + std::uint32_t(z) * 2246822519u
                        + std::uint32_t(salt) * 3266489917u;
        h ^= h >> 15; h *= 0x2C1B3C6Du;
        h ^= h >> 12; h *= 0x297A2D39u;
        h ^= h >> 15;
        return h;
    }
    // How much grain a material has, as a fraction of its own brightness.
    [[nodiscard]] static float grainOf(std::uint8_t b) {
        switch (b) {
            case Stone: case Cobble: case Gravel:      return 0.15f;
            case Dirt:  case Farmland:                 return 0.13f;
            case Grass: case Leaves:                   return 0.12f;
            case Sand:                                 return 0.09f;
            case Wood:  case Plank: case Table:        return 0.11f;
            case Coal:  case IronOre: case GoldOre:
            case DiamondOre: case RedstoneOre:
            case LapisOre: case EmeraldOre:            return 0.18f;   // speckled ore
            case Water: case Lava:                     return 0.05f;
            case Obsidian:                             return 0.06f;
            default:                                    return 0.08f;
        }
    }

    [[nodiscard]] Rgb texture(int x, int y, int z, int face) const {
        std::uint8_t b = world_.at(x, flipY(y), z);
        const int wy = flipY(y);

        // 2. Face-dependent identity, before any shading.
        //    Faces are +x -x +y -y +z -z, so 2 is the top and 3 the underside.
        std::uint8_t shown = b;
        bool fringe = false;
        if (b == Grass) {
            if (face == 2)      shown = Grass;      // turf on top
            else if (face == 3) shown = Dirt;       // plain dirt underneath
            else {
                shown = Dirt;                        // dirt down the sides...
                // ...with a lip of turf, if this block's top is exposed.
                fringe = (world_.at(x, wy + 1, z) == Air);
            }
        } else if (b == Farmland && face != 2) {
            shown = Dirt;
        }

        // Clamp to the LAST palette entry, not to kBlocks. It was kBlocks, which
        // was correct while the agent marker was the last swatch and became a
        // silent bug the moment the possessed marker went in behind it: the
        // amber "you" cube would have been clamped back to the white "agent"
        // colour and the one thing possession has to make visible would have
        // been invisible in the 3D view while looking perfectly correct in the
        // top-down one.
        Rgb c = pal_[std::size_t(std::clamp<int>(int(shown), 0, int(pal_.size()) - 1))].colour;
        float r = float(c.r), g = float(c.g), bl = float(c.b);

        // 1. Grain.
        const float amp = grainOf(shown);
        const float n = float(texHash(x, wy, z, 1) & 0xFFFFu) / 65535.0f;   // 0..1
        const float jitter = 1.0f + amp * (n - 0.5f) * 2.0f;
        r *= jitter; g *= jitter; bl *= jitter;

        // A second, coarser band for the ores, so a vein looks like flecks in
        // rock rather than a solid painted cube.
        if (shown == Coal || shown == IronOre || shown == GoldOre
         || shown == DiamondOre || shown == RedstoneOre || shown == LapisOre
         || shown == EmeraldOre) {
            const bool fleck = (texHash(x, wy, z, 7) & 3u) == 0u;
            if (!fleck) {
                const Rgb host = pal_[std::size_t(Stone)].colour;
                r = r * 0.45f + float(host.r) * 0.55f;
                g = g * 0.45f + float(host.g) * 0.55f;
                bl = bl * 0.45f + float(host.b) * 0.55f;
            }
        }

        // The turf lip on a side face.
        if (fringe) {
            const Rgb turf = pal_[std::size_t(Grass)].colour;
            r = r * 0.55f + float(turf.r) * 0.45f;
            g = g * 0.55f + float(turf.g) * 0.45f;
            bl = bl * 0.55f + float(turf.b) * 0.45f;
        }

        // 3. Depth. Only on the rock, and only below the surface, so the sky
        //    side of the world keeps its colour.
        if (shown == Stone || shown == Cobble || shown == Gravel) {
            const float t = std::clamp(float(wy) / float(std::max(1, world_.sea_level())), 0.f, 1.f);
            // Floor at 0.78, not 0.62. Depth-darkening stacks on top of the
            // renderer's own ambient occlusion, and the two together took the
            // deep rock almost to black — legible as strata and useless as a
            // picture. The gradient is what carries the meaning; the absolute
            // darkness was just lost detail.
            const float dark = 0.78f + 0.22f * t;
            r *= dark; g *= dark; bl *= dark;
        }

        // ── 4. LIGHT ────────────────────────────────────────────────────────
        //
        // The reason the light engine is worth having at all: a cave should be
        // dark, and a torch in it should not be.
        //
        // Sampled from the six NEIGHBOURS, not from the block itself. An opaque
        // block's own cell holds light 0 by definition — that is what opaque
        // means — so shading by it would paint the entire world black. What a
        // viewer sees of a block is the light falling ON it, which lives in
        // whichever open cell is next to it, and taking the brightest of the six
        // is also independent of which face happens to be pointing at the
        // camera. Non-opaque blocks (water, leaves, torches) hold light in their
        // own cell, so that is included too.
        //
        // internal_light, not light(): the internal value is the one the game's
        // own rules run on, and it is what makes the surface dim at night
        // instead of staying noon-bright while zombies spawn on it.
        {
            // Floor at 0.16 rather than 0. A cave at pure black is not "dark",
            // it is a hole in the picture — the same argument the depth-darkening
            // above records for its own 0.78 floor, learned the same way.
            const float f = 0.16f + 0.84f * (float(litAround(x, wy, z)) / float(kLightMax));
            r *= f; g *= f; bl *= f;
        }

        // Torches and lava are emitters: keep them bright whatever the shading
        // is about to do to them, or the one warm thing in a cave goes grey.
        if (shown == Torch || shown == Lava) { r = std::min(255.f, r * 1.35f);
                                               g = std::min(255.f, g * 1.2f); }
        // The possessed marker is not lit by the world, it is UNLIT — the one
        // place you most need to know where your body is standing is the bottom
        // of an unlit shaft, and the light term above would take it to 16% of
        // its colour there, which is exactly the same near-black as the rock
        // around it.
        if (int(shown) == kBlocks + 1) {
            const Rgb you = pal_[std::size_t(kBlocks) + 1].colour;
            r = float(you.r); g = float(you.g); bl = float(you.b);
        }

        return Rgb{ std::uint8_t(std::clamp(r,  0.f, 255.f)),
                    std::uint8_t(std::clamp(g,  0.f, 255.f)),
                    std::uint8_t(std::clamp(bl, 0.f, 255.f)) };
    }

    // The six directions, and the block the agent is looking at.
    static constexpr int kDX[6] = {-1, 1, 0, 0, 0, 0};
    static constexpr int kDY[6] = { 0, 0, 0, 0, 1,-1};
    static constexpr int kDZ[6] = { 0, 0,-1, 1, 0, 0};
    [[nodiscard]] std::uint8_t faced(const Agent& a) const {
        return world_.at(a.x + kDX[a.facing], a.y + kDY[a.facing], a.z + kDZ[a.facing]);
    }

    [[nodiscard]] bool nearStation(const Agent& a, Block what) const {
        for (int dy = -1; dy <= 1; ++dy)
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                    if (world_.at(a.x+dx, a.y+dy, a.z+dz) == what) return true;
        return false;
    }

    // ── the step ────────────────────────────────────────────────────────────
    void agentStep(Agent& a) {
        // An episode ends at death OR after a fixed window. Six lives in forty
        // thousand ticks is six weight updates, and dynamic scripting is a
        // per-episode method — Spronck's results are over hundreds. Bounding the
        // episode turns the same run into twenty times the learning without
        // changing the world at all.
        if (a.alive && int(knob("policy") + 0.5f) == 3 && ++a.epTicks >= kEpisodeTicks) {
            a.book.end_episode(lifeFitness(a));
            a.book.new_script();
            a.epTicks = 0;
            a.lifeStartScore = float(a.mined + a.placed);
            a.scriptStartRung = a.rung; a.scriptStartTier = a.tier;
            a.scriptStartPot = float(techPotential(a)); a.scriptStartPlaced = a.placed;
        }
        if (!a.alive) {
            ++a.deaths; ++deaths_;
            // Close the books on the life before resetAgent() clears `rung`.
            a.rungLifeSum += a.rung; ++a.livesEnded;
            // A life is an episode. Score the script that lived it, then draw a
            // fresh one — this is the whole learning loop, and it is the reason
            // the method needs no gradient, no discount and no bootstrap.
            if (int(knob("policy") + 0.5f) == 3) {
                // TERMINAL. resetAgent() below takes the rung and the pack to
                // zero on this same tick; scoring the script on the potential
                // it is about to lose is the episodic case Ng, Harada & Russell
                // require Phi(terminal)=0 for. Measured: 32 deaths over
                // 4 seeds x 12,000 ticks credited potential that the next tick
                // destroyed, and it is what made "die on purpose" a paying
                // loop. See lifeFitness().
                a.book.end_episode(lifeFitness(a, /*terminal=*/true));
                a.book.new_script();
                a.lifeStartScore = float(a.mined + a.placed);
            }
            resetAgent(a, int(&a - agents_.data()), false);
            ++episode_; return;
        }
        ++a.stepsAlive;

        // ── the stuck watchdog, and it COUNTS ────────────────────────────────
        //
        // Four separate bugs in this file put an agent into a state where it
        // chose an action every tick that could not possibly act: PlaceBlock
        // with nowhere to place, Craft with a station it could not put down,
        // Ascend with nothing to pillar with, and Mine on a block its tier
        // refused to swing at. Each is fixed where it lives. This is the
        // backstop for the fifth.
        //
        // The fifth was stepToward()'s ungated step-up, undone by gravity. The
        // SIXTH was scriptedAction() choosing Ascend on the depth test alone
        // and never calling canAscend(), which had been written for exactly
        // that job and had zero call sites — see the guard there. With it
        // closed the scripted policy measures 0.0000 rescues per agent on 24
        // seeds x 20,000 ticks and on seeds 101-108 x 40,000 ticks at 3, 8 and
        // 16 agents: 0 of 48 runs nonzero, where every one of them was nonzero
        // before. Which is the metric doing its job — it named its own bug.
        //
        // It is deliberately NOT silent. "A silent fallback hides the bug it
        // works around" is already a rule in this project, so every rescue is
        // counted and published as a metric: if `stuck rescues` is climbing,
        // there is another one of these to find, and the number says how bad.
        // A committed action counts as progress, so the 150 ticks of a
        // bare-handed dig at stone are not mistaken for paralysis.
        {
            const std::uint64_t sig = stuckSig(a);
            if (sig != a.lastSig) { a.lastSig = sig; a.idleFor = 0; }
            else if (++a.idleFor >= kStuckTicks) {
                a.idleFor = 0; ++rescues_;
                a.facing = int(a.rng.next() % 6u);
                mineFacing(a);
                return;
            }
        }

        const int mode = int(knob("policy") + 0.5f);

        // ── a committed action, still running ────────────────────────────────
        //
        // Mining a block takes between 6 and 300 ticks by the game's own
        // arithmetic, and it only progresses while you stay on the same block —
        // in the game you hold the button down. An agent that re-decides every
        // tick therefore never breaks anything at all: with real hardness the
        // random policy mined exactly zero blocks across three seeds and twenty
        // thousand ticks, and the learner managed twelve.
        //
        // So Mine is a COMMITTED action: chosen once, it runs to completion.
        // That is the options formulation (Sutton, Precup & Singh, Artificial
        // Intelligence 112 (1999) 181-211) and, for the learner, exactly the
        // frame-skipping of the original DQN — the reward earned over the whole
        // committed stretch is summed into one transition, so the network sees
        // "mine that block" as a single decision with a single consequence
        // rather than a hundred identical states it cannot tell apart.
        if (a.busy > 0) {
            const float before = progressScore(a);
            // Two kinds of commitment now share this path: swinging at a block,
            // and standing at a furnace for its 200 ticks. Both are options in
            // exactly the same sense — chosen once, run to completion, one
            // transition — so they share the SMDP bookkeeping below rather than
            // growing a second copy of it.
            if (a.smeltLeft > 0) smeltTick(a);
            else                 mineFacing(a);
            physics(a);
            // Accumulated DISCOUNTED over the option, as SMDP requires.
            a.busyReward += a.busyDiscount * (kGamma * progressScore(a) - before
                                              + livingReward(a));
            a.busyDiscount *= kGamma;
            ++a.busyTicks;
            --a.busy;
            if (a.busy > 0 && a.alive) return;
            // Finished, or died trying: close the transition.
            if (mode == 0 && !a.busyObs.empty()) {
                std::vector<float> next = observe(a);
                Transition t{a.busyObs, next, a.busyAction, a.busyReward,
                             !a.alive, std::max(1, a.busyTicks)};
                deliver(a, std::move(t));
                if (!a.alive) a.brain.end_episode();
            }
            a.episodeReward += a.busyReward;
            a.busy = 0; a.busyObs.clear(); a.busyReward = 0.f;
            a.busyTicks = 0; a.busyDiscount = 1.f;
            return;
        }

        refreshSenses(a, int(&a - agents_.data()));
        std::vector<float> obs = observe(a);
        int action;
        if (mode == PolicyPossessed) {
            // ONE agent is yours. Everybody else keeps running the scripted
            // policy, so what is being measured is a person IN a town rather
            // than a person alone in an empty world — which is a different and
            // much easier problem, since nobody else is taking your veins.
            obeyHuman_ = false;
            action = (int(&a - agents_.data()) == possessed_) ? possessedAction(a)
                                                              : scriptedAction(a);
        }
        else if (mode == 2) action = int(a.rng.unit() * float(kActions)) % kActions;
        else if (mode == 1) action = scriptedAction(a);
        else if (mode == 3) action = dynamicAction(a);
        else {
            a.brain.config().lr = knob("lr");
            a.brain.set_epsilon(knob("epsilon"));
            action = a.brain.act(obs);
        }
        if (action >= 0 && action < kActions) ++actionHist_[std::size_t(action)];

        const float before = progressScore(a);
        applyAction(a, action);
        physics(a);
        // F = gamma * PHI(s') - PHI(s), NOT PHI(s') - PHI(s).
        //
        // Ng, Harada & Russell (ICML 1999) prove that potential-based shaping
        // leaves the optimal policy unchanged, and the gamma is necessary as
        // well as sufficient. Dropping it is the documented common error: the
        // residual (1-gamma)*PHI is a standing bonus for merely HOLDING a high
        // potential, so an agent is paid to sit on a full pack rather than to
        // spend it, and the guarantee no longer applies.
        const float reward = kGamma * progressScore(a) - before + livingReward(a);

        // A smelt that just started is a 200-tick commitment, same as a swing.
        if (a.smeltLeft > 0 && a.alive) {
            a.busy = a.smeltLeft;
            a.busyAction = action;
            a.busyObs = std::move(obs);
            a.busyReward = reward;
            a.busyTicks = 1;
            a.busyDiscount = kGamma;
            return;
        }
        // Mining that did not finish this tick becomes a commitment — whatever
        // action started it. A Go* option that walked up to a vein and swung at
        // it has to hold that swing exactly as a bare Mine does, or the option
        // is strictly worse than the primitive it is built from.
        if (a.miningLeft > 0 && a.alive) {
            a.busy = a.miningLeft;
            a.busyAction = action;
            a.busyObs = std::move(obs);
            a.busyReward = reward;
            a.busyTicks = 1;
            a.busyDiscount = kGamma;
            return;
        }

        a.episodeReward += reward;
        if (mode == 0) {
            std::vector<float> next = observe(a);
            deliver(a, Transition{std::move(obs), std::move(next), action, reward, !a.alive});
            if (!a.alive) a.brain.end_episode();
        }
    }

    // Hand a transition to whoever should learn from it.
    void deliver(Agent& a, Transition t) {
        if (knob("share") > 0.5f) {
            // Pooled: every agent learns from this. Expensive on purpose — the
            // cost is the honest half of the claim.
            for (auto& other : agents_) other.brain.observe(t);
        } else {
            a.brain.observe(std::move(t));
        }
    }

    // What the agent has achieved, as one number. The reward is the CHANGE in
    // this, so nothing pays twice and standing still pays nothing.
    // Scaled so a single step's reward lands near 1 and never near 10.
    //
    // The first version paid 10 per rung against a learning rate of 0.03, and
    // the networks collapsed: one action taken on 73.5% of ticks, which is what
    // a diverging Q-function looks like from outside. Q-learning bootstraps on
    // its own estimates, so a reward scale the step size cannot absorb does not
    // merely learn slowly, it runs away. Nothing about the task changed here —
    // only the units it is paid in.
    // What this agent has achieved, as one number. Reward is the CHANGE in it,
    // so nothing pays twice and standing still pays nothing.
    //
    // Two terms. The TOWN term is the same for everybody — how far up the tree
    // the settlement has come — so no trade is rewarded for hoarding against
    // the others. The TRADE term is what this one is for, and it is what makes
    // the problem learnable: a forester is paid for wood within a few hundred
    // ticks of cutting it, instead of waiting eleven rungs for a diamond that
    // its wood made possible but that somebody else will mine.
    [[nodiscard]] float progressScore(const Agent& a) const {
        const float town = float(a.rung) * 1.0f + float(a.tier) * 0.4f;
        if (a.role < 0) {
            // All-rounders: the original single objective, kept exactly as it
            // was so the two settings are comparable.
            float s = town;
            s += 0.035f * float(a.blocks[Wood] + a.blocks[Cobble]);
            s += 0.12f  * float(a.blocks[Coal]);
            s += 0.25f  * float(a.blocks[IronOre]);
            s += 0.60f  * float(a.blocks[DiamondOre]);
            s += 0.05f  * float(a.placed);
            return s;
        }
        float trade = 0.f;
        switch (a.role) {
            case Forester:
                trade = 0.20f * float(a.blocks[Wood])
                      + 0.10f * float(a.items[ItPlank] + a.items[ItStick]);
                break;
            case Miner:
                trade = 0.06f * float(a.blocks[Cobble])
                      + 0.20f * float(a.blocks[Coal])
                      + 0.40f * float(a.blocks[IronOre])
                      + 0.90f * float(a.blocks[DiamondOre]);
                break;
            case Builder:
                // Paid for what stands, and for the two blocks that make a
                // settlement more than a heap: a table and a furnace are what
                // everybody else needs in order to use what they gathered.
                trade = 0.10f * float(a.placed)
                      + 0.60f * float(a.items[ItTable] + a.items[ItFurnace]);
                break;
            case Provisioner:
                // Staying alive and keeping others fed. The only role whose
                // objective is mostly about not dying, which is why the town
                // needs one.
                trade = 0.30f * float(a.items[ItBread])
                      + 0.02f * a.food
                      + 0.02f * a.health
                      + 0.05f * float(a.blocks[Wheat]);
                break;
            default: break;
        }
        return town + trade;
    }

    [[nodiscard]] float livingReward(const Agent& a) const {
        if (!a.alive) return -1.0f;            // dying is the one large penalty
        return -0.001f;                        // a small cost of time
    }

    // A KEYSTROKE IS AN ORDER, NOT A SUGGESTION.
    //
    // The action-level fallbacks exist so that an autonomous policy cannot
    // choose an impossible action forever. Applying them to a DRIVEN agent is a
    // different thing entirely: the person presses "build" against a wall and
    // the agent wanders off instead. Possession's whole contract is that the
    // run scored is the run the person drove, and the moment the fallbacks went
    // in the driven baseline fell from 4.667 to 0.667 rungs per life while
    // scripted and random were untouched.
    //
    // So a human's action does exactly nothing when it cannot act, which is
    // what happens in the game. Autopilot stretches inside a possession are
    // NOT human (possAuto_ is set from the same flag), so they keep the
    // fallback — an abandoned body should not stand in a field either.
    void idleFallback(Agent& a) {
        if (obeyHuman_) return;
        applyPrimitive(a, roam(a));
    }

    void applyAction(Agent& a, int action) {
        switch (action) {
            case GoWood: case GoStone: case GoCoal: case GoIron: case GoDiamond:
                goToward(a, action); break;
            case Mine: {
                // Swinging at air does nothing and the rulebase has an
                // unconditional "break whatever is in front", so an agent
                // standing in the open chose it forever. Face something
                // breakable first; if there is nothing, go somewhere there is.
                // Not while a person is driving: in the game you swing at
                // what you are LOOKING at, and if that is air nothing happens.
                // Turning the camera for them is the same overreach as walking
                // them away from a wall they chose to build against.
                const std::uint8_t front = faced(a);
                if (!obeyHuman_ && (!block_solid(front) || front == Bedrock)) {
                    int found = -1;
                    for (int d = 0; d < 6; ++d) {
                        const std::uint8_t b2 = world_.at(a.x + kDX[d], a.y + kDY[d],
                                                          a.z + kDZ[d]);
                        if (block_solid(b2) && b2 != Bedrock) { found = d; break; }
                    }
                    if (found < 0) { idleFallback(a); break; }
                    a.facing = found;
                }
                mineFacing(a);
                break;
            }
            case Craft:      craftNext(a);    break;
            case PlaceBlock: placeUnder(a);   break;
            case Eat:        eat(a);          break;
            case Explore:    applyPrimitive(a, roam(a)); break;
            case Ascend:     ascend(a);       break;
            default:
                // Possession's six movement codes, and its stand. They live
                // ABOVE the shared action space so that nothing which enumerates
                // kActions — the histogram, the network's output layer, the
                // random policy's draw — can ever produce one, and so that a
                // human steering is routed through exactly the applyPrimitive()
                // that Explore and every Go* option already use. A code outside
                // both ranges falls through and does nothing, which is only
                // reachable from a caller that ignored poss_code_valid().
                if (action >= kMoveBase && action < kMoveBase + 6)
                    applyPrimitive(a, action - kMoveBase);
                break;
        }
    }

    // ── the possessed agent's decision ──────────────────────────────────────
    //
    // Three sources, in priority order, and every one of them is recorded:
    //   1. a queued keystroke, which also becomes the new standing order
    //   2. the standing order, repeated — see possStanding_ for why
    //   3. the scripted policy, once the idle timeout has expired
    //
    // The recording is of the ACTION TAKEN, not of the keystrokes. That is the
    // difference between a tape that reproduces a run and a tape that
    // reproduces your typing: the autopilot stretches are part of what the score
    // was earned by, and leaving them out would make a replay of a half-attended
    // run disagree with the run itself.
    [[nodiscard]] int possessedAction(Agent& a) {
        int code = kStand;
        bool human = false;
        if (possReplay_) {
            if (replayAt_ < replayTape_.entries.size()) {
                const TapeEntry& e = replayTape_.entries[replayAt_++];
                // The loud failure. Reaching decision k on a different tick
                // means the world underneath the tape did not reproduce, and
                // continuing would score a DIFFERENT run against a human's
                // name. Recorded rather than thrown so the suite can report the
                // exact tick, and so a divergent replay still runs to the end
                // and can be diffed rather than just aborting.
                if (e.tick != ticks_ && replayFault_ < 0) replayFault_ = ticks_;
                code = e.code;
                human = e.human;
                // Keep the readout honest while a tape is playing back. Without
                // these two the panel reported "order stand" through a replay
                // that was in fact issuing Go* and Mine, and showed a driven
                // stretch and an autopilot stretch identically — a readout that
                // describes something other than what is happening is worse
                // than no readout, because it is believed.
                possStanding_ = code;
                possAuto_ = !human;
            } else if (replayFault_ < 0) {
                // The tape ran out before the run did: the recording is short,
                // which is the same defect seen from the other end.
                replayFault_ = ticks_;
            }
        } else if (!possQueue_.empty()) {
            code = possQueue_.front();
            possQueue_.pop_front();
            possStanding_ = code;
            possIdle_ = 0;
            possAuto_ = false;
            human = true;
        } else if (possIdle_ < (long long)(knob("idle") + 0.5f)) {
            code = possStanding_;
            human = true;
            possAuto_ = false;
        } else {
            // Nobody is driving. Hand it back rather than leave a body standing
            // in a field: an abandoned agent scores zero and would publish that
            // zero as "what a human achieves".
            possAuto_ = true;
            code = scriptedAction(a);
            possStanding_ = kStand;
        }
        if (code >= kMoveBase && code < kMoveBase + 6) ++possMoves_;
        if (!possReplay_) tape_.entries.push_back(TapeEntry{ticks_, code, human});
        obeyHuman_ = human;          // see idleFallback()
        return code;
    }

    // A tape has to carry the world it was taken in or it replays into a
    // different one. Read from the knobs at the moment possession starts,
    // because that is the only moment they are guaranteed to be the ones the
    // run will use.
    void captureTapeHeader() {
        tape_.seed   = int(knob("seed")   + 0.5f);
        tape_.size   = int(knob("size")   + 0.5f);
        tape_.agents = int(knob("agents") + 0.5f);
        tape_.idle   = int(knob("idle")   + 0.5f);
        tape_.roles  = int(knob("roles")  + 0.5f);
        tape_.hunger = knob("hunger");
        tape_.index  = std::max(0, possessed_);
        tape_.ticks  = 0;
    }

    // One step of "go and get that": turn toward it, walk, and break whatever
    // is in the way. The same behaviour a person uses and the same one the
    // scripted policy navigates with.
    void goToward(Agent& a, int action) {
        const auto& sn = a.sense[std::size_t(action - GoWood)];
        if (!sn.found) { applyPrimitive(a, roam(a)); return; }
        const std::uint8_t want = kGoTarget[std::size_t(action - GoWood)];
        // Already beside it: look at it and cut.
        for (int d = 0; d < 6; ++d)
            if (world_.at(a.x + kDX[d], a.y + kDY[d], a.z + kDZ[d]) == want) {
                a.facing = d; mineFacing(a); return;
            }
        // A TARGET THAT CANNOT BE APPROACHED IS NOT A TARGET. stepToward
        // returns -1 when every candidate step is rejected, and applyPrimitive
        // does nothing with -1 — so the agent stands and re-decides forever.
        // That is why guarding the vertical step ALONE measured worse when it
        // was tried: it swapped a rise gravity undid for no move at all, which
        // is the same frozen signature. The guard needs this fallback beside it.
        const int prim = stepToward(a, sn.x, sn.y, sn.z);
        if (prim < 0) { applyPrimitive(a, roam(a)); return; }
        applyPrimitive(a, prim);
    }

    // Climb back toward daylight. Its own action because being stuck below
    // ground is the commonest way an agent wastes a life, and leaving the only
    // escape implicit in roam() meant a policy had to find it by accident.
    // What the agent would leave underfoot while climbing, or -1 for nothing.
    //
    // ONE list, called by both the test and the act. It used to be written out
    // twice — {Dirt, Cobble, Stone, Sand, Grass, Plank} in tryMove and again in
    // canPillar — and both copies omitted Wood, Gravel and Road. Caught in the
    // act: an agent holding ELEVEN WOOD was treated as having nothing to stand
    // on and jumped at open sky for 290 ticks.
    //
    // Ordered cheapest-first, and WOOD AND PLANK ARE NOT ON IT. Adding them
    // looked like a strict improvement — more material means fewer agents
    // stranded — and measured as the opposite: rung per life fell from 3.25 to
    // 2.62 across three seeds, because wood is the scarce end of the tech tree
    // and an agent that spends it on a staircase cannot craft the planks the
    // ladder is built from. Mobility bought with the tree's own root is not a
    // gain. Gravel and Road are the additions worth having: both are junk.
    [[nodiscard]] static int pillarPick(const Agent& a) {
        for (int b : {Dirt, Gravel, Cobble, Sand, Grass, Stone, Road})
            if (a.blocks[std::size_t(b)] > 0) return b;
        return -1;
    }
    [[nodiscard]] static bool canPillar(const Agent& a) { return pillarPick(a) >= 0; }

    // Cut a step into the wall: move onto a neighbouring block one higher if
    // that is walkable, otherwise break the neighbour and make it walkable.
    // This is what a player does in a hole with an empty pack, and it uses the
    // ledge-step tryMove already implements rather than a second copy of it.
    bool stairStep(Agent& a) {
        for (int d = 0; d < 4; ++d) {
            const int nx = a.x + kDX[d], nz = a.z + kDZ[d];
            if (!world_.inside(nx, a.y, nz)) continue;
            if (!block_solid(world_.at(nx, a.y, nz))) continue;   // not a wall
            if (!block_solid(world_.at(nx, a.y + 1, nz))
                && !block_solid(world_.at(a.x, a.y + 1, a.z))) {
                a.facing = d; tryMove(a, kDX[d], 0, kDZ[d]);      // walk up the ledge
                return true;
            }
            if (world_.at(nx, a.y + 1, nz) != Bedrock) {
                a.facing = d; mineFacing(a);                      // carve the step
                return true;
            }
        }
        return false;
    }

    // Can this agent gain height at all from where it stands? Either there is
    // rock overhead to dig through, or something to leave underfoot, or a wall
    // beside it to cut a step into. If none of the three, Ascend is a no-op and
    // the policy must not choose it. Mirrors ascend() exactly — the two are
    // next to each other so they stay that way.
    // Is this agent actually underground?
    //
    // NOT `a.y < surface(x,z)`, which is what three call sites used. surface()
    // is the highest SOLID block in the column and block_solid(Leaves) is true,
    // so an agent standing on open grass under an oak read as "below the
    // surface" — and the policy then spent the rest of its life trying to climb
    // out of a forest, rising into open sky and being pulled back by gravity
    // every tick. Rock over your head is underground; a canopy is shade.
    //
    // O(height) and called only from decisions, of which there are ~2,300 per
    // 16,000 agent-ticks because mining is committed.
    [[nodiscard]] bool underground(const Agent& a) const {
        for (int y = a.y + 1; y < kHeight; ++y) {
            const std::uint8_t b = world_.at(a.x, y, a.z);
            if (b == Leaves || b == Wood) continue;      // a tree is not a roof
            if (block_solid(b)) return true;
        }
        return false;
    }

    [[nodiscard]] bool canAscend(const Agent& a) const {
        const std::uint8_t above = world_.at(a.x, a.y + 1, a.z);
        if (block_solid(above)) return above != Bedrock;
        if (canPillar(a)) return true;
        for (int d = 0; d < 4; ++d) {
            const int nx = a.x + kDX[d], nz = a.z + kDZ[d];
            if (!world_.inside(nx, a.y, nz)) continue;
            if (block_solid(world_.at(nx, a.y, nz))) return true;   // a wall to step up
        }
        return false;
    }

    void ascend(Agent& a) {
        a.facing = 4;
        const std::uint8_t above = faced(a);
        if (!block_solid(above)) {
            // OPEN AIR ABOVE IS NOT THE SAME AS A CLIMB THAT STICKS. Rising
            // into air is undone by gravity on the same tick unless a block is
            // left underfoot, so an agent below the surface with an EMPTY PACK
            // jumped at the ceiling forever: measured, one agent spent 2,020 of
            // 2,000 decisions on Ascend and moved nowhere. Cut a staircase
            // instead, which is what the pack-less player does.
            if (!canPillar(a) && stairStep(a)) return;
            // AND IF THE CLIMB STILL CANNOT STICK, GO SOMEWHERE ELSE.
            //
            // The guard for this used to live in scriptedAction, which the
            // DYNAMIC SCRIPTING policy never calls — its rulebase fires Ascend
            // directly ("if below the surface, climb out"). So the same stall
            // survived in the one policy that reaches the action by another
            // door: 22 of 40 sampled stalls, every one of them facing up with
            // air above, solid ground below and an empty pack, rising a block
            // and being pulled straight back. Guarding the CHOOSER only ever
            // fixes the chooser you guarded; the ACTION has to be the thing
            // that cannot no-op, because every policy shares it.
            // The test has to be the SAME ONE tryMove uses to pillar, which is
            // canPillar AND climbing out. Testing canPillar alone left the case
            // that stalls: an agent ABOVE the surface (y 81, surface 80) with a
            // full pack, hurt, firing "get back to the surface" at open sky —
            // climbingOut is false, so nothing is left underfoot, so gravity
            // undoes the rise. 18 of 40 sampled stalls after the first pass at
            // this guard, which is the second time this exact half-test has
            // been written here.
            if (!(canPillar(a) && world_.surface(a.x, a.z) > a.y)) {
                idleFallback(a); return;
            }
            tryMove(a, 0, 1, 0);
            return;
        }
        // No harvest-tier test. Breaking below the tier yields nothing and
        // takes five times as long, but it BREAKS — and digging straight up is
        // how anything gets out of a hole. Gating it on drops_for was the
        // second copy of that mistake; mineFacing owns the rule now, and
        // refuses Air, Water, Lava and Bedrock on its own.
        if (above != Bedrock) mineFacing(a);
    }

    // Turn a movement primitive into its effect. Moving in a direction also
    // TURNS to face it, which is what makes "walk at the tree then mine" work
    // without a separate turn action for every step.
    void applyPrimitive(Agent& a, int prim) {
        if (prim < 0) return;
        if (prim >= 10) { a.facing = prim - 10; mineFacing(a); return; }
        a.facing = prim;
        tryMove(a, kDX[prim], kDY[prim], kDZ[prim]);
    }

    void tryMove(Agent& a, int dx, int dy, int dz) {
        const int nx = a.x + dx, ny = a.y + dy, nz = a.z + dz;
        if (!world_.inside(nx, ny, nz)) return;
        if (block_solid(world_.at(nx, ny, nz))) {
            // Step up one block, the way a player walks over a ledge.
            if (dy == 0 && !block_solid(world_.at(nx, ny + 1, nz))
                        && !block_solid(world_.at(a.x, a.y + 1, a.z))) {
                a.x = nx; a.y = ny + 1; a.z = nz;
                // A LEDGE COSTS A JUMP. You cannot WALK onto a full block in
                // Java Edition — you jump, and jumpFromGround charges 0.05. This
                // branch lifted the agent a whole block for free, so every ledge,
                // staircase and stairStep() ascent was unpriced: measured over 8
                // seeds x 6000 ticks x 8 agents, 5,078 dry step-ups = 253.90
                // exhaustion never levied, 7.6% on top of the 3,338.58 charged
                // and five times mining (49.53) and swimming (35.08) together.
                // Stepping up INTO water stays a swim, not a jump: a fluid move
                // is charged per metre swum and jumpFromGround is never called in
                // one, and those 580 step-ups already read 0.010000 each.
                if (world_.at(a.x, a.y, a.z) == Water) exhaust(a, kExhSwim);
                else                                   exhaust(a, kExhJump);
            }
            return;
        }
        a.x = nx; a.y = ny; a.z = nz;
        // A METRE SWUM IS A BLOCK ENTERED. kExhSwim is declared "per metre" and
        // used to be levied in physics(), which runs every tick whether or not
        // the agent moved — so it was per tick, and a metre of actual swimming
        // cost 0.000000. It belongs on the movement primitive for the same
        // reason kExhJump does further down: the cost is the act, not the clock.
        // Measured after the move: 0.010000 per block entered while submerged,
        // and 0.000000 for a floater that never moves.
        if (world_.at(a.x, a.y, a.z) == Water) exhaust(a, kExhSwim);
        // PILLARING. Rising into open air is undone by gravity on the same tick,
        // so without this an agent at the bottom of a shaft it dug itself climbs
        // and falls forever — 99% of the informed policy's ticks were a MoveUp
        // that could not stick, eight blocks under a surface it could see. It is
        // also how a player actually gets out of a hole: place a block under
        // your feet as you go up. Costs a block, which is why it only happens
        // when the agent would otherwise fall straight back.
        // Only when genuinely climbing OUT of somewhere, and only with spare
        // material. Unconditional pillaring was a treadmill: an agent mined a
        // block, spent it rising, fell back and mined again — 1,559 blocks
        // broken and an empty inventory to show for it, which looked like a
        // mining bug and was a spending one.
        const int surf = world_.surface(a.x, a.z);
        const bool climbingOut = surf >= 0 && a.y < surf;
        // NOT WHILE SWIMMING. Rising through water is swimming, charged 0.01
        // per metre fifteen lines up; charging the 0.05 jump on top made a
        // metre swum upward cost 0.06 against the published 0.01 — measured
        // 0.058750/metre on the up axis against exactly 0.010000 on the other
        // three. A jump is something you do from the ground.
        if (dy > 0 && world_.at(a.x, a.y, a.z) != Water) exhaust(a, kExhJump);
        if (dy > 0 && climbingOut && !block_solid(world_.at(a.x, a.y - 1, a.z))) {
            const int b = pillarPick(a);        // the one list; see pillarPick
            if (b >= 0) {
                setBlock(a.x, a.y - 1, a.z, std::uint8_t(b));
                a.blocks[std::size_t(b)] -= 1;
                ++a.placed; ++structures_;
            }
        }
    }

    // Mine the block the agent is facing — here, the first solid one among the
    // six neighbours, preferring downward and then the richest. Tools gate it:
    // a block whose tier exceeds the held pickaxe simply cannot be broken.
    void mineFacing(Agent& a) {
        // The block in front, and only that one. A player breaks what they are
        // looking at; the first version surveyed all six neighbours and took
        // whichever was worth most, which is a machine with a hand on every
        // side rather than someone standing in a world.
        const int bx = a.x + kDX[a.facing], by = a.y + kDY[a.facing], bz = a.z + kDZ[a.facing];
        const std::uint8_t b = world_.at(bx, by, bz);
        if (b == Air || b == Water || b == Lava || b == Bedrock) { a.miningX = -1; return; }
        // BELOW THE HARVEST TIER THE BLOCK STILL BREAKS. It drops nothing, and
        // it takes hardness * 5 rather than hardness * 1.5 / speed, but it
        // breaks — that is the game's rule, and this line used to state it in a
        // comment and then implement the opposite, returning without swinging
        // because mining it was "not worth it".
        //
        // Refusing to swing is not a harmless optimisation. It is the ONLY way
        // out of a hole. Measured at 20,000 ticks: five of eight agents stood
        // motionless underground facing stone, tier 0, packs empty, health 20.0
        // and food 20.0 — and they would have stood there forever, because
        // standing still costs no exhaustion (correctly: walking has been free
        // since 1.11), so they could not even starve. A pickaxe is lost on
        // death; an agent that respawned, dug down and died once was entombed
        // permanently. The town was six statues, one agent stuck in a
        // mine/place cycle, and one worker.
        //
        // The tech tree is untouched by this: breaking without the tier still
        // yields NOTHING, so gathering iron still needs a stone pickaxe. What
        // comes back is mobility.
        const bool yields = drops_for(b, a.tier);

        // Mining TAKES TIME, by the game's arithmetic: hardness * 1.5 / tool
        // speed with the right tool, hardness * 5 without. Instant mining made
        // the tech tree free — an agent emptied a vein in a tick, and the
        // difference between a wooden and an iron pickaxe was only which blocks
        // it could touch, never what touching them cost.
        if (a.miningX != bx || a.miningY != by || a.miningZ != bz) {
            a.miningX = bx; a.miningY = by; a.miningZ = bz;
            a.miningLeft = breakTicksFor(b, a.tier);
        }
        if (--a.miningLeft > 0) return;

        a.miningX = -1;
        // Stone drops cobblestone; ore drops itself; a block broken below its
        // harvest tier drops nothing. Leaves do not enter the pack: the oak
        // sapling drop is planted by releaseLeaf, the same roll a rotting
        // leaf uses, because there is no item on the ground to pick up.
        if (b == Leaves) releaseLeaf(bx, by, bz);
        else {
            setBlock(bx, by, bz, Air);
            const std::uint8_t drop = (b == Stone) ? std::uint8_t(Cobble) : b;
            if (yields) give(a, drop, 1);
        }
        ++a.mined;
        exhaust(a, kExhMine);
        wearTool(a);
    }

    // Put something in the pack, up to what a pack holds: 36 slots of 64.
    // Unbounded pockets are their own quiet cheat — an agent that can carry a
    // mountain never has to decide what is worth keeping.
    void give(Agent& a, std::uint8_t what, int n) {
        const int cap = kSlots * kStack;
        int total = 0;
        for (int i = 0; i < kInv; ++i) total += a.blocks[std::size_t(i)];
        for (int i = 0; i < kItems; ++i) total += a.items[std::size_t(i)];
        if (total >= cap) return;
        a.blocks[std::size_t(what)] = std::min(kSlots * kStack,
                                               a.blocks[std::size_t(what)] + n);
    }

    [[nodiscard]] static int oreValue(std::uint8_t b) {
        switch (b) {
            case DiamondOre: return 100; case GoldOre: return 60;
            case IronOre:    return 50;  case Coal:    return 30;
            case Wood:       return 25;  case Stone:   return 10;
            case Grass: case Dirt: case Sand: return 5;
            default: return 1;
        }
    }

    // Place a carried block underfoot. This is what makes a town: placed blocks
    // persist and become everyone's terrain.
    // Place a carried block into an adjacent empty cell.
    //
    // This was "place underfoot" and could never fire once: an agent always
    // stands ON something, so the cell below it is solid by definition and the
    // check rejected every attempt. Every policy reported exactly zero blocks
    // placed, which read as "agents do not build" rather than as a dead action —
    // a whole third of what this sim claims to be about, silently absent.
    void placeUnder(Agent& a) {
        // A TORCH FIRST, if it is dark here and the agent has one. This is the
        // only reason to craft a torch: [W-SPAWN] 1.18+ says any block light at
        // all stops a hostile spawn, so one torch turns a stretch of cave from
        // a spawner into a safe one. Without this the top rung of the tech tree
        // produced an item that did nothing — a rung that pays nothing is a rung
        // no policy has a reason to reach.
        if (a.items[ItTorch] > 0
            && light_.internal_light(a.x, a.y, a.z, skyDarken()) <= kHostileMaxInternalSky
            && placeAdjacent(a, std::uint8_t(Torch))) {
            a.items[ItTorch] -= 1;
            ++a.placed; ++structures_; ++torchesPlaced_;
            return;
        }
        // A sapling in the pack is a tree waiting for dirt. Nothing else in
        // the place list plants one, so without this a broken sapling sits in
        // a slot forever and the stand it came from does not come back.
        if (a.blocks[Sapling] > 0 || a.blocks[SaplingAged] > 0) {
            static constexpr int dx[5] = {-1, 1, 0, 0, 0};
            static constexpr int dy[5] = { 0, 0, 0, 0, 1};
            static constexpr int dz[5] = { 0, 0,-1, 1, 0};
            const int d = placeSlot(a);
            if (d >= 0) {
                const int x = a.x + dx[d], y = a.y + dy[d], z = a.z + dz[d];
                const std::uint8_t floor = world_.at(x, y - 1, z);
                if (floor == Dirt || floor == Grass) {
                    const int held = a.blocks[Sapling] > 0 ? int(Sapling) : int(SaplingAged);
                    setBlock(x, y, z, Sapling);
                    a.blocks[std::size_t(held)] -= 1;
                    ++a.placed; ++structures_;
                    return;
                }
            }
        }
        int pick = -1;
        for (int b : {Cobble, Plank, Road, Dirt, Stone, Sand, Grass}) {
            if (a.blocks[std::size_t(b)] > 0) { pick = b; break; }
        }
        if (pick < 0) { idleFallback(a); return; }
        // Cobblestone laid down becomes road, which is what makes a path
        // between two agents' workings legible as a thing they built.
        if (placeAdjacent(a, std::uint8_t(pick == Cobble ? Road : pick))) {
            a.blocks[std::size_t(pick)] -= 1;
            ++a.placed; ++structures_;
            return;
        }
        // NOWHERE TO PUT IT — GO SOMEWHERE THERE IS. The chooser-side guard for
        // this lives in scriptedAction and in CSurplus, and the dynamic
        // scripting rulebase reaches PlaceBlock through neither: 16 of 40
        // sampled stalls were an agent walled in with sand and gravel, choosing
        // to build every tick against five solid faces. Third action in this
        // file to need the fallback, and the reason it now goes on the ACTION:
        // guarding a chooser only ever fixes the policy that uses it.
        idleFallback(a);
    }

    // Which cell beside the agent a block would go into, or -1 if there is
    // nowhere. Sideways before upward, so agents build outward rather than
    // pillaring.
    //
    // Split out from placeAdjacent because THE POLICY HAS TO ASK THE SAME
    // QUESTION. An agent that walls itself into a one-block box of its own
    // roads has nowhere left to build, placeAdjacent returns false, and the
    // policy — which only checked that the pack held a surplus — chose
    // PlaceBlock again on the next tick, and the next. Measured over a
    // 2,000-tick window: 10,000 of 13,497 decisions were PlaceBlock, exactly
    // five agents' worth, and `placed` rose by zero. Choosing an action that
    // cannot act is indistinguishable from being frozen, and it read in the
    // metrics as a town of busy builders.
    [[nodiscard]] int placeSlot(const Agent& a) const {
        static constexpr int dx[5] = {-1, 1, 0, 0, 0};
        static constexpr int dy[5] = { 0, 0, 0, 0, 1};
        static constexpr int dz[5] = { 0, 0,-1, 1, 0};
        for (int d = 0; d < 5; ++d) {
            const int x = a.x + dx[d], y = a.y + dy[d], z = a.z + dz[d];
            if (!world_.inside(x, y, z)) continue;
            const std::uint8_t there = world_.at(x, y, z);
            if (there == Air || there == Water) return d;
        }
        return -1;
    }

    bool placeAdjacent(const Agent& a, std::uint8_t what) {
        static constexpr int dx[5] = {-1, 1, 0, 0, 0};
        static constexpr int dy[5] = { 0, 0, 0, 0, 1};
        static constexpr int dz[5] = { 0, 0,-1, 1, 0};
        const int d = placeSlot(a);
        if (d < 0) return false;
        setBlock(a.x + dx[d], a.y + dy[d], a.z + dz[d], what);
        return true;
    }

    // Craft the lowest item the agent does not yet have and can afford. The
    // tree order is the item order, so "next rung" is just a scan.
    void craftNext(Agent& a) {
        // Goes through canCraft() rather than repeating its conditions. The two
        // were written separately and disagreed: the chooser said "something is
        // craftable" and the executor picked a different item, so an agent could
        // return Craft every tick and craft nothing.
        for (int it = 0; it < kItems; ++it) {
            if (!canCraft(a, it)) continue;
            // Iron is the one rung that is not a craft. It is a 200-tick furnace
            // cycle and the agent has to stand there for it, which is why it
            // becomes a committed action rather than a single-tick recipe.
            if (it == ItIronIngot) {
                // Put the carried furnace down first. Without this, canCraft
                // says "yes, you have a furnace in your pack" and startSmelt
                // says "there is no furnace block beside you" — the agent then
                // returns Craft forever and smelts nothing, which is the exact
                // shape of the bug tryCraft's station-placement comment records.
                if (!nearStation(a, Furnace) && a.items[ItFurnace] > 0
                    && placeAdjacent(a, std::uint8_t(Furnace))) {
                    a.items[ItFurnace] -= 1;
                    ++a.placed; ++structures_;
                }
                a.smeltLeft = startSmelt(a);
                return;
            }
            // TRY THE NEXT ONE. This returned unconditionally, so a single
            // disagreement between canCraft (the chooser) and tryCraft (the
            // actor) about the FIRST affordable item stopped the whole scan —
            // and the policy chose Craft again on the next tick, and the next.
            // 16 of 19 remaining dynamic-scripting stalls. The two are supposed
            // to agree and mostly do; making the loop survive it when they do
            // not is cheaper than proving they always will, and the same shape
            // as startSmelt's `return 0` that had to become `continue`.
            if (tryCraft(a, it)) return;
        }
        // Nothing was actually made. Do not stand here deciding to craft again.
        idleFallback(a);
    }

    // Does this agent want another one of these?
    //
    // One-off tools are wanted only if absent. Materials are wanted up to a cap,
    // and the cap is what makes the tree passable at all: sticks sit BEFORE the
    // crafting table in item order and are repeatable, so without a limit every
    // plank was immediately spent on sticks and the four planks a table needs
    // never accumulated. The agent cycled wood -> planks -> sticks forever and
    // stalled at rung 2 on every seed, which read as a stupid agent rather than
    // an unsatisfiable order.
    [[nodiscard]] bool wantsMore(const Agent& a, int item) const {
        switch (item) {
            case ItPlank:     return a.items[ItPlank] < 8;
            case ItStick:     return a.items[ItStick] < 4
                                  && (a.items[ItPlank] >= 6 || a.items[ItTable] > 0
                                      || nearStation(a, Table));
            case ItBread:     return a.items[ItBread] < 3;
            case ItTorch:     return a.items[ItTorch] < 8;
            case ItIronIngot: return a.items[ItIronIngot] < 3;
            default:          return a.items[std::size_t(item)] == 0;   // a tool, once
        }
    }

    bool tryCraft(Agent& a, int item) {
        const Recipe& r = simRecipe(item);
        if (r.makes == 0) return false;
        if (r.station != Air && !nearStation(a, Block(r.station))) {
            // A crafting table can be PUT DOWN if carried, which is how an
            // agent that has one in its pack ever gets to use it.
            // Into an adjacent empty cell, NOT underfoot. Underfoot is solid by
            // definition — the agent is standing on it — so the table could
            // never be set down, canCraft went on reporting the recipe as
            // available, and the informed policy returned Craft on 37% of its
            // ticks and crafted nothing. Same defect as placeUnder had, in a
            // second place, found only by counting actions.
            if ((r.station == Table && a.items[ItTable] > 0)
             || (r.station == Furnace && a.items[ItFurnace] > 0)) {
                const std::uint8_t what = (r.station == Table) ? std::uint8_t(Table)
                                                               : std::uint8_t(Furnace);
                if (placeAdjacent(a, what)) {
                    if (what == Table) a.items[ItTable] -= 1; else a.items[ItFurnace] -= 1;
                    ++a.placed; ++structures_;
                }
            }
            if (!nearStation(a, Block(r.station))) return false;
        }
        for (const auto& in : r.in) {
            if (in.count == 0) continue;
            const int have = in.isItem ? a.items[std::size_t(in.what)]
                                       : a.blocks[std::size_t(in.what)];
            if (have < in.count) return false;
        }
        for (const auto& in : r.in) {
            if (in.count == 0) continue;
            if (in.isItem) a.items[std::size_t(in.what)] -= in.count;
            else           a.blocks[std::size_t(in.what)] -= in.count;
        }
        a.items[std::size_t(item)] += r.makes;
        // A fresh tool comes with a full bar. craft.hpp's table, not a guess:
        // 59 uses for wood, 131 stone, 250 iron, 1561 diamond.
        if (r.tier > 0)
            a.durability[std::size_t(item)] = craft::tool_durability(materialOfTier(r.tier));
        if (r.tier > a.tier) a.tier = r.tier;
        ++a.crafted;
        // Rung is how far up the tree this agent has ever been, so it is a
        // staircase and cannot fall when a pickaxe is spent on a recipe.
        a.rung = std::max(a.rung, item + 1);
        a.bestRung = std::max(a.bestRung, a.rung);
        a.bestTier = std::max(a.bestTier, a.tier);
        return true;
    }

    void eat(Agent& a) {
        // Bread restores 5 hunger in the game. Raw wheat is not edible at all,
        // so it gives nothing here either.
        // Bread: 5 hunger and 6.0 saturation (2.5 x 2.4) in Java Edition.
        if (a.items[ItBread] > 0) {
            a.items[ItBread] -= 1;
            a.food = std::min(kMaxFood, a.food + 5.f);
            a.saturation = std::min(a.food, a.saturation + 6.f);
        }
    }

    void wanderStep(Agent& a) {
        const int d = int(a.rng.unit() * 4.0f) % 4;
        const int dx[4] = {-1, 1, 0, 0}, dz[4] = {0, 0, -1, 1};
        tryMove(a, dx[d], 0, dz[d]);
    }

    // Gravity, drowning and starvation. Everything that can kill an agent.
    // Gravity, drowning, lava, hunger and regeneration, on the game's numbers:
    //
    //   fall damage   ceil(distance - 3) HEALTH POINTS, i.e. half-hearts
    //   drowning      15 seconds of air, then 2 health a second
    //   lava          4 health a hit, throttled to one hit per 10 ticks
    //   regeneration  at food 18 or above, 1 HEALTH POINT every 4 seconds,
    //                 costing food — see kExhRegen
    //   starvation    at food 0, 1 HEALTH POINT every 4 seconds
    //
    // Those last two said "1 heart" and the code does one health point, which
    // is HALF a heart — the line directly above sets out that distinction and
    // then the next two lines dropped it. The code matches the published rule
    // and the comment did not; found by a gauntlet critic measuring 80.00
    // ticks per health point against the comment's implied 40. Third comment
    // in this file caught describing something the code does not do.
    //
    // 20 ticks to the second, 20 health, 20 food, all as published.
    static constexpr float kTicksPerSecond = 20.0f;
    static constexpr int   kAirTicks       = 300;   // 15 seconds
    // Damage in this game is never smooth. Every source is gated by a 10-tick
    // invulnerability window, which is why lava is survivable for a moment and
    // why a per-tick trickle is the wrong shape however you scale it. One
    // counter fixes lava, drowning and anything added later at once.
    static constexpr int   kHurtCooldown   = 10;

    // ── hunger, as the game actually models it ──────────────────────────────
    //
    // Food is not a clock. It is an EXHAUSTION economy: actions add exhaustion,
    // and every time it crosses 4.0 the agent spends one point of the hidden
    // saturation buffer, or one point of food once saturation is gone.
    //
    // The headline fact, and the one this sim had completely wrong: WALKING
    // COSTS NOTHING in Java Edition since 1.11. Only sprinting, jumping,
    // swimming, breaking blocks, attacking and REGENERATION cost anything — and
    // regeneration is by far the largest consumer at 6.0 exhaustion per health
    // point restored. That is what makes food a wound economy rather than a
    // timer: an agent that never gets hurt barely eats, and an agent that keeps
    // falling down a shaft starves from healing itself.
    //
    // The old model drained a flat amount every tick, which made hunger a
    // function of how long you existed rather than what you did.
    static constexpr float kExhaustPerPoint = 4.0f;
    // NOT kExhSprint, and not an attack cost either. Both are published costs
    // and neither is implemented, because this sim has no sprint action and no
    // attack action — agents walk and are attacked. A constant declared for a
    // capability that does not exist is the inert control this project keeps
    // finding in other people's code: it reads as coverage of the published
    // model and is referenced by nothing. Two gauntlet critics flagged
    // kExhSprint independently, each noting its only occurrence was its own
    // declaration. Removed rather than left as decoration; if a sprint or an
    // attack is ever added, the cost comes back WITH the action it belongs to.
    static constexpr float kExhSwim    = 0.01f;   // per metre
    static constexpr float kExhJump    = 0.05f;
    static constexpr float kExhMine    = 0.005f;  // per block broken
    static constexpr float kExhRegen   = 6.0f;    // per health point healed

    void exhaust(Agent& a, float amount) {
        a.exhaustion += amount * std::max(0.f, knob("hunger"));
        while (a.exhaustion >= kExhaustPerPoint) {
            a.exhaustion -= kExhaustPerPoint;
            if (a.saturation > 0.f) a.saturation = std::max(0.f, a.saturation - 1.f);
            else                    a.food       = std::max(0.f, a.food - 1.f);
        }
    }
    // One dynamic-scripting episode, when nothing kills the agent first.
    static constexpr int   kEpisodeTicks   = 2000;
    // The build band. Start at a surplus worth building with, stop well below
    // it, so mining cannot walk the pack back over the line on the next tick.
    static constexpr int   kBuildStart     = 24;
    static constexpr int   kBuildStop      = 8;
    // 300 ticks is fifteen seconds of game time and comfortably longer than the
    // slowest legitimate swing in the file (obsidian is not in this world;
    // bare-handed stone is 150), so a rescue means genuinely nothing happening.
    static constexpr int   kStuckTicks     = 300;

    // Everything an agent that is doing ANYTHING will change. Position, work
    // done, the ladder, and whether it is mid-commitment.
    [[nodiscard]] static std::uint64_t stuckSig(const Agent& a) {
        std::uint64_t h = 1469598103934665603ull;
        auto mix = [&h](std::uint64_t v) {
            h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        };
        mix(std::uint64_t(a.x + 1)); mix(std::uint64_t(a.y + 1));
        mix(std::uint64_t(a.z + 1));
        mix(std::uint64_t(a.mined)); mix(std::uint64_t(a.placed));
        mix(std::uint64_t(a.crafted)); mix(std::uint64_t(a.rung));
        mix(std::uint64_t(a.busy));   mix(std::uint64_t(a.miningLeft + 1));
        return h;
    }
    // The agents knob's published maximum, enforced rather than assumed.
    static constexpr int   kMaxAgents      = 32;
    static constexpr float kMaxHealth      = 20.0f;
    static constexpr float kMaxFood        = 20.0f;

    void physics(Agent& a) {
        const int fellFrom = a.y;
        while (a.y > 0 && !block_solid(world_.at(a.x, a.y - 1, a.z))
               && world_.at(a.x, a.y - 1, a.z) != Water
               && world_.at(a.x, a.y - 1, a.z) != Lava) --a.y;
        const int fell = fellFrom - a.y;
        // ceil(distance - 3) in HEALTH POINTS, which is half-hearts. The code
        // was already right; the comment claimed hearts and so doubled it.
        if (fell > 3) { a.health -= float(fell - 3); a.hurtCooldown = kHurtCooldown; }

        // Clearing lastHurt when the window expires matters: without it the
        // held value ratchets upward forever and a later small hit is measured
        // against the largest blow the agent ever took.
        if (a.hurtCooldown > 0 && --a.hurtCooldown == 0) a.lastHurt = 0.f;
        const std::uint8_t here = world_.at(a.x, a.y, a.z);
        if (here == Water) {
            // 2 health a second once the air is gone, not 1. The air budget of
            // 300 ticks was right; the rate underneath it was half.
            if (++a.airTicks > kAirTicks) a.health -= 2.0f / kTicksPerSecond;
        } else {
            a.airTicks = 0;
        }
        // 4 health a HIT, once per invulnerability window — 8 health a second,
        // twice what a per-tick 4/20 trickle delivers. The comment above this
        // function had the published figure right and the code did not.
        if (here == Lava && a.hurtCooldown == 0) {
            a.health -= 4.0f;
            a.hurtCooldown = kHurtCooldown;
        }
        // Swimming costs a little, but it is charged in tryMove, per block
        // ENTERED — not here, per tick submerged. Charging it on the clock was
        // exactly the "hunger is a timer" defect this section says it removed:
        // measured, a motionless floater paid 0.010000 every tick (2.999998
        // over 300 ticks having moved 0 blocks) and, with drowning suppressed,
        // starved from food 20 to 0 in 10,001 ticks without swimming a metre.
        // Standing still costs nothing, in water or out of it.
        if (a.food <= 0.f) {
            a.health -= 1.0f / (4.0f * kTicksPerSecond);
        } else if (a.food >= 18.f && a.health < kMaxHealth) {
            const float healed = 1.0f / (4.0f * kTicksPerSecond);
            a.health = std::min(kMaxHealth, a.health + healed);
            exhaust(a, kExhRegen * healed);      // healing is what really costs
        }
        a.saturation = std::min(a.saturation, a.food);
        if (a.health <= 0.f) a.alive = false;
    }

    // ── the informed baseline ───────────────────────────────────────────────
    //
    // Not intelligent — informed. It reads the same recipe table the learner has
    // to discover, and heads for whatever the next rung needs. This is the bar,
    // and a learner that only beats `random` has demonstrated nothing.
    // Run the agent's CURRENT script: the first rule whose condition holds.
    [[nodiscard]] int dynamicAction(Agent& a) {
        const auto& rb = rulebase();
        for (int r : a.book.script()) {
            const Rule& rule = rb[std::size_t(r)];
            if (holds(a, rule.cond)) return rule.action;
        }
        return Explore;    // an empty or wholly inapplicable script still acts
    }

    [[nodiscard]] bool holds(const Agent& a, int cond) const {
        switch (cond) {
            case CAlways:     return true;
            case CHungry:     return a.food < 6.f && a.items[ItBread] > 0;
            case CCanCraft:   { for (int it = 0; it < kItems; ++it) if (canCraft(a, it)) return true;
                                return false; }
            // Somewhere to put it, as well as something to put. Same defect as
            // scriptedAction's surplus test — see placeSlot().
            case CSurplus:    return placeSlot(a) >= 0
                                  && (a.blocks[Cobble] > 12 || a.blocks[Dirt] > 12);
            case CBuried:     { const int s2 = world_.surface(a.x, a.z);
                                return s2 >= 0 && a.y < s2 - 2; }
            case CNoWood:     return a.blocks[Wood] < 4 && a.items[ItPlank] < 4;
            case CNeedStone:  return a.tier >= 1 && a.blocks[Cobble] < 11;
            case CNeedCoal:   return a.tier >= 2 && a.blocks[Coal] < 3;
            case CNeedIron:   return a.tier >= 2 && a.blocks[IronOre] < 3;
            case CCanDiamond: return a.tier >= 3;
            case CHurt:       return a.health < 8.f;
            case CSeeWood:    return a.sense[0].found;
            case CSeeIron:    return a.sense[3].found;
            // STANDING AT A TABLE IS NOT HAVING SOMETHING TO MAKE. This rule
            // fires Craft, and craftNext() does nothing at all when no recipe
            // is affordable — so an agent that built a table and ran out of
            // wood chose Craft every tick beside it forever. 15 of 40 sampled
            // dynamic-scripting stalls, the second largest cause. Eighth
            // instance in this file of a condition testing that a resource
            // EXISTS where the actor needs it to be USABLE.
            case CNearTable:  { if (!nearStation(a, Table)) return false;
                                for (int it = 0; it < kItems; ++it)
                                    if (canCraft(a, it)) return true;
                                return false; }
            default:          return false;
        }
    }

    // Fitness for one life, in [0,1]. Weighted to what a settler is FOR:
    // climbing the tech tree, then gathering, then not dying pointlessly.
    // ── how far up the tree this agent stands, CONTINUOUSLY ─────────────────
    //
    // The integer part is the rung. The fractional part is how much of the NEXT
    // rung's recipe is already in hand, so gathering three of the four planks a
    // crafting table needs moves this by 0.75 of a rung with no rung crossed.
    //
    // This exists because the rung counter could not rank the scripts dynamic
    // scripting was drawing. MEASURED: 86-88% of episodes crossed ZERO rungs,
    // so the term carrying 0.45 of the fitness weight was tied for six of every
    // seven scripts, and the weight updates were noise — which is why running
    // MORE episodes made it worse (1.64 -> 0.89 rungs per life from 12,000 to
    // 60,000 ticks) rather than better.
    //
    // The fix is a denser signal, not a different set of coefficients: score
    // the CHANGE in a potential over the episode. Two properties, from two
    // different places — this comment used to run them together and credit
    // both to Ng, Harada & Russell (1999), which was wrong:
    //   * Partial credit cannot invent a rung. That is kPartialCredit < 1
    //     below, an inequality this file enforces itself. Nothing to do with Ng.
    //   * Equal states pay equal fitness, so no round trip pays. That needs the
    //     scored quantity to be the raw difference Phi(end) - Phi(start) with
    //     Phi(terminal) = 0, and it is the shape of Ng's F rather than his
    //     theorem — an episodic fitness handed to a weight update is not the
    //     per-step reward the policy-invariance result is about. See
    //     lifeFitness(): both halves were missing and both are measured there.
    //
    // `a.rung` is the highest item index crafted PLUS ONE, so the next rung's
    // recipe is recipe[a.rung] with no search.
    [[nodiscard]] double techPotential(const Agent& a) const {
        double pot = double(a.rung);
        if (a.rung >= kItems) return pot;                 // tree complete
        const Recipe& r = simRecipe(a.rung);
        if (r.makes == 0) return pot;
        double need = 0, have = 0;
        for (const auto& in : r.in) {
            if (in.count == 0) continue;
            need += double(in.count);
            const int got = in.isItem ? a.items[std::size_t(in.what)]
                                      : a.blocks[std::size_t(in.what)];
            have += double(std::min(got, in.count));
        }
        // Standing at the right workbench is part of being ready to craft, and
        // it is the part an agent has to travel for, so it counts as one more
        // ingredient rather than as nothing.
        if (r.station != Air) {
            need += 1.0;
            if (nearStation(a, Block(r.station))
                || (r.station == Table   && a.items[ItTable]   > 0)
                || (r.station == Furnace && a.items[ItFurnace] > 0)) have += 1.0;
        }
        // PARTIAL CREDIT MUST NEVER EQUAL COMPLETION. Without this factor an
        // agent holding every ingredient and standing at the station scored
        // rung + 1.0 — exactly what crafting the thing pays — so a script that
        // gathered and never crafted was worth as much as one that finished,
        // and floor(Phi) was no longer the rung. Caught by the invariant test
        // rather than by reading: 27,543 of 48,000 samples carried partial
        // credit and the largest was 1.000.
        //
        // The value is not the point; the strict inequality is. Any factor
        // below one makes crafting always the better move, and 0.9 keeps the
        // signal strong enough to still rank a nearly-finished script.
        static constexpr double kPartialCredit = 0.9;
        return need > 0.0 ? pot + kPartialCredit * have / need : pot;
    }

    // `terminal` marks the death boundary. An episodic task needs Phi of the
    // absorbing state to be zero or the telescoping identity does not close —
    // see the rung term below.
    [[nodiscard]] double lifeFitness(const Agent& a, bool terminal = false) const {
        // RUNGS CLIMBED BY THIS SCRIPT, not rungs the agent happens to stand on.
        //
        // This term carries 0.45 of the weight, and it used to read `a.rung`
        // outright. `rung` does not reset when a script is replaced, only when
        // the agent dies — so an agent that climbed to rung 5 under script A
        // then drew script B handed B a fitness of 0.45*5/kItems for doing
        // nothing at all, and every script after it in that life scored the
        // same. The dominant signal was near-constant within a life, which
        // means the weight updates were noise: measured over five seeds,
        // dynamic scripting came LAST at 1.91 rungs per life against random's
        // 2.39. The comment below already identified the defect and fixed it
        // for the 0.15 term while the 0.45 and 0.20 terms kept it.
        //
        // Scored on the POTENTIAL rather than the rung counter, so the term is
        // dense: see techPotential(). Same weight, same meaning at the rung
        // boundaries, and something to say in between.
        //
        // THE RAW DIFFERENCE, AND ZERO AT THE TERMINAL STATE. This read
        // `std::max(0.0, techPotential(a) - a.scriptStartPot)` and used
        // techPotential(a) even at death, and a rectified difference is not
        // potential-based shaping — it is the one thing the citation buys you,
        // sum-zero around any closed loop in state space, thrown away. Both
        // halves were exploitable and both were measured, not argued:
        //   * The rectifier made the downhill leg free. Six script windows at
        //     rung 5, gather 8 cobble / put them back, ending byte-identical to
        //     the start, paid 0.09818 MORE than six windows of standing still.
        //     Now -0.00000: the down leg is charged exactly what the up leg was
        //     paid, window by window (0.13273 up, 0.06727 down, either side of
        //     the 0.10000 a null window pays).
        //   * Phi(at death) was credited and then destroyed by resetAgent() on
        //     the next tick, charged to nobody. "Hold one wood block, die,
        //     respawn" — rung 0 for twenty straight lives — paid 0.03682 every
        //     life, 8% of the maximum this term can pay, for zero rungs. The
        //     sparse counter it replaced paid exactly 0. Now 0.00000.
        //   * Over 4 seeds x 12,000 ticks, 197 episodes and 32 deaths: credited
        //     Phi 148.400 against the 64.950 telescoping permits, a 128%
        //     overpay, leaking on all four seeds. Now 77.050 credited against
        //     77.050 permitted — leak 0.000 on every seed, to three decimals.
        //   * It also measures better, which was not the reason to do it:
        //     dynamic scripting 2.579 -> 2.775 rungs per life over eight seeds
        //     (5 up, 2 unchanged, 1 down), scripted control identical on every
        //     seed to three decimals, which is what proves the change reached
        //     only the path it was meant to. Ties on this term 64.5% -> 61.4%.
        // Losing potential now COSTS the script that lost it, which is the
        // point: a negative term here is not a bug to be clamped away, it is
        // the downhill leg being charged for. 21 of 197 episodes are charged.
        //
        // WHAT IS STILL NOT NG, HARADA & RUSSELL, stated here because the
        // citation is easy to over-read. This is an episodic FITNESS in [0,1]
        // fed to a weight update, not an additive per-step reward in an MDP,
        // so the policy-invariance theorem does not transfer — there is no
        // value function for the shaping to cancel out of, and dynamic
        // scripting's rule (see dynscript.hpp end_episode, which clamps to
        // [0,1] itself) is not policy improvement. What the raw difference
        // buys, and it is worth having, is the weaker property the exploits
        // above were breaking: equal states pay equal fitness, so no round
        // trip pays. Even that holds only where the [0,1] floor does not
        // bind — measured, the floor truncates the charge on 12 of 197
        // episodes, and it cannot be dropped here because dynamic scripting
        // requires a bounded fitness.
        //
        // That last sentence was written of THIS term and was true of it, and
        // for another full session it was false of the fitness, because the
        // 0.20 term below kept the rectifier. It is true of all five terms now,
        // and the floor is the only thing left holding it back: over 8 seeds
        // x 12,000 ticks, 29 of 389 episodes owe a tier charge totalling 2.350
        // and the floor refuses 1.400 of it on 14 of them. See below, and see
        // the three round-trip assertions in self_test.cpp, which are the ones
        // this paragraph should have had from the start.
        const double phiEnd = terminal ? 0.0 : techPotential(a);
        const double rung = (phiEnd - double(a.scriptStartPot)) / double(kItems);
        // THE SAME TWO CHANGES, ON THE 0.20 TERM. This read
        // `std::max(0, a.tier - a.scriptStartTier)` and took a.tier at death —
        // the exact rectified, terminal-non-zero form the block above declares
        // fixed, one line below the line it was fixed on. Tier genuinely falls
        // in play (wearTool -> recomputeTier when a pickaxe breaks) and is
        // zeroed by resetAgent() on death, so both halves were live:
        //   * The rectifier made the downhill leg free. Six windows of "make a
        //     stone pick, tier 0->2, it breaks, 2->0", ending byte-identical to
        //     the start on every field this function reads, paid +0.60000
        //     against standing still — +0.10000 per cycle, repeatable without
        //     bound. Now +0.00000.
        //   * The tier at death was credited and destroyed by resetAgent() on
        //     the next tick, charged to nobody. Ten lives of "hold a diamond
        //     pick at the moment of death" paid 0.225 each against 0.025 for an
        //     empty death, +2.00000 over the ten. Now +0.00000.
        //   * Over 8 seeds x 12,000 ticks, 389 episodes and 54 deaths: credited
        //     21.5000 against the 7.2500 telescoping permits, a 197% overpay
        //     leaking on 8 of 8 seeds, 2.8500 of raw fitness. 14 episodes had
        //     the tier fall with the rectifier refusing the charge. Now
        //     credited == telescoped, leak 0.0000 on every seed.
        // 5.4x the wood-block leak the 0.45 term was fixed for, and it sat one
        // line under the paragraph explaining the fix — the fourth term of this
        // return statement to carry a bug a neighbouring comment described.
        //
        // IT BUYS NOTHING MEASURABLE AND IS KEPT ANYWAY. Rung per life for
        // dynamic scripting, same build, only this line differing: 8 seeds
        // 3.225 -> 3.248, 16 seeds 3.295 -> 3.160, 32 seeds 3.079 -> 2.982,
        // 64 seeds 3.018 -> 3.015 (t = -0.06, 24 up / 22 down / 18 unchanged).
        // Eight said it helped, sixteen said it hurt by 4%, sixty-four say it
        // does nothing; the scripted and random controls are identical on all
        // 64 seeds to three decimals, which is what proves the change reached
        // only the path it was meant to. A repeatable paying loop is a
        // reward-hacking surface whether or not anything has found it yet,
        // which is the same argument kPartialCredit was kept on.
        const double tierEnd = terminal ? 0.0 : double(a.tier);
        const double tier = (tierEnd - double(a.scriptStartTier)) / 4.0;
        // Work done SINCE this script was drawn, not since the agent was born —
        // otherwise every script inherits credit for its predecessors' work and
        // the comparison between scripts washes out.
        const double since = double(a.mined + a.placed) - double(a.lifeStartScore);
        const double got  = std::min(1.0, std::max(0.0, since) / 90.0);
        // The same defect, third term: `built` read lifetime `placed`, so a
        // script drawn late in a long life started with this maxed out.
        const double built= std::min(1.0, double(std::max(0, a.placed - a.scriptStartPlaced))
                                          / 60.0);
        const double lived= std::min(1.0, double(a.epTicks) / double(kEpisodeTicks));
        return std::clamp(0.45 * rung + 0.20 * tier + 0.15 * got
                        + 0.10 * built + 0.10 * lived, 0.0, 1.0);
    }

    [[nodiscard]] int scriptedAction(Agent& a) {
        if (a.food < 6.f && a.items[ItBread] > 0) return Eat;
        for (int it = 0; it < kItems; ++it) if (canCraft(a, it)) return Craft;
        // SURPLUS IS A BAND, NOT A LINE.
        //
        // This was `> 12` on its own, and a bare threshold next to an action
        // that moves the quantity across it is an oscillator: place one (13 ->
        // 12), mine one (12 -> 13), place one... Measured at 20,000 ticks, one
        // agent sat on a single coordinate racking up exactly +125 mined and
        // +125 placed per thousand ticks — equal counts and zero displacement,
        // which is the signature. It looked like a busy builder in the metrics.
        //
        // Hysteresis: start building at a real surplus, keep building until the
        // pack is genuinely down, and only then go back to work.
        // ...and only when there is somewhere to put it. See placeSlot().
        if (placeSlot(a) >= 0) {
            if (a.building) {
                if (a.blocks[Cobble] > kBuildStop || a.blocks[Dirt] > kBuildStop)
                    return PlaceBlock;
                a.building = false;
            } else if (a.blocks[Cobble] > kBuildStart || a.blocks[Dirt] > kBuildStart) {
                a.building = true;
                return PlaceBlock;
            }
        } else {
            a.building = false;      // walled in: stop trying, go and dig out
        }
        // Pursue the resource FIRST, and only climb out if there is nothing to
        // pursue. Ascend sat above this and fired on every tick spent below
        // ground — so an agent that dug down to reach stone immediately climbed
        // back out, dug down again, and mined three blocks in six thousand
        // ticks. It read as a weak heuristic and was an ordering bug, and a
        // learner "beating" it would have been beating nothing.
        const int want = roleWant(a);
        for (int i = 0; i < kSenses; ++i)
            if (kGoTarget[i] == std::uint8_t(want) && a.sense[std::size_t(i)].found)
                return GoWood + i;
        // ...and only if the climb can actually be made. THE canAscend() CALL
        // IS THE GUARD; the geometry test alone is not.
        //
        // `surface(x,z)` is the highest solid block in the COLUMN, so an agent
        // standing in a cavern or under an overhang reads as "below the
        // surface" while having open sky directly above it and nothing to climb.
        // Ascend then rose into air, gravity undid it on the same tick, and it
        // was chosen again — caught six times out of six at 290 idle ticks,
        // every one of them facing up, below the surface, with air above and
        // solid ground underfoot. Falling through to Explore lets roam() take
        // the agent somewhere it can actually get out.
        //
        // THAT PARAGRAPH DESCRIBED A GUARD THE LINE BELOW DID NOT HAVE. The
        // predicate it names was written (canAscend(), above, with its own
        // comment saying "the policy must not choose it") and then never
        // called — zero call sites in src/ — so the depth test was the whole
        // condition and the stall it was written to stop was still live. The
        // shape, from outside the sim at idleFor==295 so nothing mid-dig is
        // counted: air pocket under a roof, empty pack, no solid block at foot
        // level on any of the four sides. ascend() then finds no block to mine,
        // nothing to pillar with, no wall for stairStep(), and falls to
        // tryMove(0,1,0); physics() undoes it the same tick, so every field
        // stuckSig() reads is unchanged and idleFor runs to 300. Measured over
        // 8 seeds x 8 agents x 20,000 ticks: 144 of 144 stalls carried exactly
        // aboveSolid=0 canPillar=0 wall=0 canAscend=0 busy=0 with this depth
        // test true — one signature, no second shape, 121 of them Builders.
        //
        // It costs nothing. 48 seeds x 20,000 ticks: rescues per agent 2.4375
        // -> 0.0000, from 48 of 48 seeds nonzero to 0 of 48; blocks mined
        // 641.5 -> 712.7 (+11%, t = +4.81, 36 seeds up / 12 down); rung per
        // life 3.374 -> 3.368, which is nothing (t = -0.05, 24 up / 24 down);
        // alive 1.00 either way. Fresh seeds 101-108 at 40,000 ticks with 3, 8
        // and 16 agents: 4.71 / 6.47 / 5.88 rescues per agent and every seed
        // nonzero, against 0.0000 and 0 of 24 after. Random and dynamic
        // scripting are byte-identical across the change on 8 seeds — neither
        // reaches this function — which is what proves it touched only the
        // path it was aimed at.
        //
        // TWELVE SEEDS SAID rung ROSE 3.33 -> 3.44 AND TWENTY-FOUR SAID IT
        // FELL 3.27 -> 3.22. Forty-eight say it does not move at all. Same
        // trap as the other four times a small batch pointed the wrong way in
        // this file; the honest claim is "flat", not either of the first two.
        //
        // AND THE SUITE'S OWN THREE-SEED CHECK IS THE SAME TRAP. "a town that
        // gets somewhere" runs seeds 1-3 at 12,000 ticks and asserts scripted
        // > random; that draw reads 2.8470 against 2.8660 after this and FAILS
        // on the mean, while wins >= 2 of 3 still passes. Over 48 seeds at the
        // identical length scripted goes 3.6512 -> 3.6756 against random's
        // bit-identical 3.4083. The assertion is left exactly as it was —
        // whether to widen it is a decision about the benchmark, not about
        // this bug — but it will go red until somebody makes that call.
        const int surf = world_.surface(a.x, a.z);
        if (surf >= 0 && a.y < surf - 2 && canAscend(a)) return Ascend;
        return Explore;
    }

    // Nearest block of a kind within a radius, searched in expanding shells so
    // the first hit really is the closest rather than the first in scan order.
    [[nodiscard]] bool nearest(const Agent& a, std::uint8_t what, int radius,
                               int& ox, int& oy, int& oz) const {
        for (int r = 1; r <= radius; ++r) {
            for (int dy = -r; dy <= r; ++dy)
                for (int dz = -r; dz <= r; ++dz)
                    for (int dx = -r; dx <= r; ++dx) {
                        // Shell only: skip anything an earlier radius covered.
                        if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != r) continue;
                        const int x = a.x+dx, y = a.y+dy, z = a.z+dz;
                        if (!world_.inside(x, y, z)) continue;
                        if (world_.at(x, y, z) != what) continue;
                        ox = x; oy = y; oz = z; return true;
                    }
        }
        return false;
    }

    // One step toward a target, MINING whatever is in the way.
    //
    // The mining is the whole point. Without it the policy returned a move that
    // could not happen — MoveUp into solid rock — and returned it again on every
    // subsequent tick, so an agent that dug itself into a hole stayed there for
    // the rest of the run. Deadlock, not stupidity: it never chose a different
    // action because the situation never changed.
    // One step toward a target, MINING whatever is in the way. Returns a
    // movement PRIMITIVE (0..5 for the six directions, 6 for mine, -1 for
    // nothing available) rather than an action, because both the scripted
    // policy and the learner's Go* options are built out of it.
    [[nodiscard]] int stepToward(const Agent& a, int tx, int ty, int tz) {
        const int ddx = tx - a.x, ddy = ty - a.y, ddz = tz - a.z;
        struct Cand { int mag, prim, nx, ny, nz; };
        std::array<Cand, 3> c{
            Cand{std::abs(ddx), ddx < 0 ? 0 : 1, a.x + (ddx < 0 ? -1 : 1), a.y, a.z},
            Cand{std::abs(ddz), ddz < 0 ? 2 : 3, a.x, a.y, a.z + (ddz < 0 ? -1 : 1)},
            Cand{std::abs(ddy), ddy < 0 ? 5 : 4, a.x, a.y + (ddy < 0 ? -1 : 1), a.z},
        };
        std::sort(c.begin(), c.end(), [](const Cand& p, const Cand& q) { return p.mag > q.mag; });
        // HORIZONTAL FIRST while there is any horizontal distance left. There is
        // no jumping and gravity runs every tick, so stepping up into open air
        // is undone immediately — an agent aiming at wood three blocks up in a
        // canopy climbed and fell for 62% of its ticks and never arrived.
        // Unconditionally. This used to be gated on horizontal distance
        // remaining, which skipped it in exactly the case that stalls: a target
        // stacked STRAIGHT OVERHEAD, where ddx and ddz are both zero and the
        // vertical step is the only candidate with any magnitude, so it won
        // whatever the partition would have said.
        std::stable_partition(c.begin(), c.end(), [](const Cand& k) {
            return k.prim != 4 && k.prim != 5;
        });
        // A STEP UP INTO OPEN AIR IS NOT A STEP. Gravity undoes it on the same
        // tick unless there is something to leave underfoot, so offering prim 4
        // to a pack that cannot pillar is offering a no-op — the third caller
        // to make this mistake, after ascend() and roam(). De-prioritising it
        // above was not enough: with nothing else on the list it still won.
        // Rising sticks only where tryMove will actually pillar, and tryMove
        // pillars only when climbing OUT of somewhere with material to spend.
        // Testing canPillar alone was not enough: an agent above ground with
        // one grass block in its pack was still offered the step, rose into
        // open sky, and fell back every tick for 290 ticks.
        // The guard the comment above has been describing. It was written,
        // measured as part of a batch that regressed, reverted with the rest of
        // the batch, and the COMMENT WAS LEFT BEHIND — so this function has
        // been documenting a protection it did not have. A blind critic found
        // it by reading the two against each other.
        const bool canRise = canPillar(a) && world_.surface(a.x, a.z) > a.y;
        for (const auto& k : c) {
            if (k.mag == 0) continue;
            if (k.prim == 4 && !canRise
                && !block_solid(world_.at(a.x, a.y + 1, a.z))) continue;
            const std::uint8_t b = world_.at(k.nx, k.ny, k.nz);
            if (!block_solid(b)) return k.prim;
            if (b == Bedrock) continue;
            // "Mine, in THIS direction". A bare "mine" loses the direction and
            // the agent swings at whatever it happened to be facing — which,
            // once mining became facing-based, meant the scripted policy broke
            // exactly zero blocks and looked like a policy failure.
            // Tunnel through it whatever the tier — slowly and for no drops if
            // the pickaxe is wrong or absent, which is the game's rule and the
            // difference between a detour and a dead end. Only reached when
            // every non-solid step has already been rejected above, so this
            // does not make agents burrow through hills they could walk round.
            return 10 + k.prim;
        }
        return -1;
    }

    // Travel, rather than mill about. A persistent heading with the sense to
    // climb out of its own hole. Returns a primitive, like stepToward.
    [[nodiscard]] int roam(Agent& a) {
        const int surf = world_.surface(a.x, a.z);
        if (surf >= 0 && a.y < surf) {
            const std::uint8_t above = world_.at(a.x, a.y + 1, a.z);
            // Only climb if the climb will stick — see ascend(). Otherwise fall
            // through to the waypoint walk below, whose stepToward already
            // steps up ledges and tunnels through what blocks it.
            if (!block_solid(above)) { if (canPillar(a)) return 4; }
            else if (above != Bedrock) return 14;   // mine upward, tier or no tier
        }

        // GO SOMEWHERE, rather than mill about.
        //
        // nearest() gives up past 20 blocks, and this used to answer that by
        // stepping one square in a heading it re-rolled every time something
        // blocked it. On open ground that is a random walk, which covers
        // distance proportional to the SQUARE ROOT of the steps taken — an
        // agent on a treeless plain therefore stayed on it. Measured: two of
        // eight agents ended 6,000 ticks with 0-1 blocks mined and nothing
        // whatever inside their sense radius, one of them still at its original
        // spawn.
        //
        // A committed waypoint 30-60 blocks off turns that square root back
        // into a straight line. It is re-picked on arrival or after 600 ticks,
        // so an agent that cannot reach one does not chase it forever.
        if (a.wanderX < 0 || ++a.wanderAge > 600
            || (std::abs(a.x - a.wanderX) <= 2 && std::abs(a.z - a.wanderZ) <= 2)) {
            const float ang = float(a.rng.unit()) * 6.2831853f;
            const int   len = 30 + int(a.rng.unit() * 30.f);
            a.wanderX = std::clamp(a.x + int(std::cos(ang) * float(len)), 0, n_ - 1);
            a.wanderZ = std::clamp(a.z + int(std::sin(ang) * float(len)), 0, n_ - 1);
            a.wanderAge = 0;
        }
        {
            const int step = stepToward(a, a.wanderX, a.y, a.wanderZ);
            if (step >= 0) return step;
            a.wanderX = -1;          // blocked outright: pick somewhere else next tick
        }
        const int dxs[4] = {-1, 1, 0, 0}, dzs[4] = {0, 0, -1, 1};
        const int prim[4] = {0, 1, 2, 3};
        for (int tries = 0; tries < 4; ++tries) {
            const int d = a.roamDir & 3;
            const int nx = a.x + dxs[d], nz = a.z + dzs[d];
            if (!block_solid(world_.at(nx, a.y, nz))) return prim[d];
            if (!block_solid(world_.at(nx, a.y + 1, nz))) return prim[d];   // step up
            a.roamDir = int(a.rng.unit() * 4.0f) % 4;
        }
        return -1;
    }

    [[nodiscard]] bool canCraft(const Agent& a, int item) const {
        const Recipe& r = simRecipe(item);
        if (r.makes == 0) return false;
        if (!wantsMore(a, item)) return false;
        // A STATION IN THE PACK ONLY COUNTS IF IT CAN BE PUT DOWN. Carrying a
        // furnace satisfies "you have a furnace" and then placeAdjacent fails
        // because the agent has walled itself in, startSmelt finds no furnace,
        // and canCraft says iron is craftable again on the next tick. Measured:
        // one agent spent 2,000 of 2,000 ticks choosing Craft and crafted
        // nothing. Third instance of this shape in one file — a test that asks
        // whether a resource EXISTS while the actor needs it to be USABLE.
        if (r.station != Air && !nearStation(a, Block(r.station))
            && !(r.station == Table   && a.items[ItTable]   > 0 && placeSlot(a) >= 0)
            && !(r.station == Furnace && a.items[ItFurnace] > 0 && placeSlot(a) >= 0))
            return false;
        // A LIT FURNACE IS ALREADY FUELLED. The iron recipe lists a coal so that
        // an agent with an unlit furnace knows it needs one, but one coal is
        // 1600 ticks — eight smelts — and demanding a fresh coal for each of
        // them is wrong twice over: it is not how a furnace works, and it made
        // the second smelt impossible. MEASURED: an agent standing at a furnace
        // with 1,400 ticks of burn left and one ore in the pack sat there for
        // 600 ticks and smelted nothing, because canCraft was still asking for
        // a coal it had already spent.
        const bool fuelled = (item == ItIronIngot) && furnaceLit(a);
        for (const auto& in : r.in) {
            if (in.count == 0) continue;
            if (fuelled && !in.isItem && in.what == Coal) continue;
            const int have = in.isItem ? a.items[std::size_t(in.what)]
                                       : a.blocks[std::size_t(in.what)];
            if (have < in.count) return false;
        }
        return true;
    }

    // Is there a furnace beside this agent with enough burn left for one more
    // 200-tick cycle?
    [[nodiscard]] bool furnaceLit(const Agent& a) const {
        for (int dy = -1; dy <= 1; ++dy)
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int x = a.x + dx, y = a.y + dy, z = a.z + dz;
                    if (!world_.inside(x, y, z) || world_.at(x, y, z) != Furnace) continue;
                    const std::size_t cell = world_.idx(x, y, z);
                    for (const auto& s : smelters_)
                        if (s.cell == cell && s.burnLeft >= craft::kSmeltTicks) return true;
                }
        return false;
    }
    // What this agent goes looking for, given its trade. The scripted policy
    // specialises too — comparing specialists against a generalist baseline
    // would measure the split rather than the learning.
    [[nodiscard]] int roleWant(const Agent& a) const {
        if (a.role < 0) return wantedBlock(a);
        switch (a.role) {
            case Forester: return Wood;
            case Builder:  return (a.blocks[Cobble] < 16) ? Stone : Wood;
            case Miner:
                if (a.tier < 1) return Wood;             // needs a pickaxe first
                if (a.blocks[Cobble] < 11) return Stone;
                if (a.tier >= 3) return DiamondOre;
                if (a.tier >= 2) return IronOre;
                return Coal;
            case Provisioner: return (a.blocks[Wood] < 4) ? Wood : Stone;
            default: return wantedBlock(a);
        }
    }

    [[nodiscard]] int wantedBlock(const Agent& a) const {
        if (a.blocks[Wood] < 4 && a.items[ItPlank] < 4) return Wood;
        if (a.tier >= 1 && a.blocks[Cobble] < 11) return Stone;
        if (a.tier >= 2 && a.blocks[Coal] < 3) return Coal;
        if (a.tier >= 2 && a.blocks[IronOre] < 3) return IronOre;
        if (a.tier >= 3) return DiamondOre;
        return Wood;
    }
    [[nodiscard]] int targetDepth(const Agent& a, int want) const {
        const int top = std::max(6, world_.surface(a.x, a.z));
        switch (want) {
            case Wood:       return top + 1;
            case Stone:      return top / 2;
            case Coal:       return top / 2;
            case IronOre:    return top / 4;
            case DiamondOre: return 3;
            default:         return top;
        }
    }

    void publish() {
        // Agents are drawn into the volume as a marker index so they are visible
        // in the render, then taken out again. Cheaper and simpler than a second
        // pass in the renderer, and it cannot desynchronise from the world.
        struct Saved { int x, y, z; std::uint8_t was; };
        std::vector<Saved> saved;
        saved.reserve(agents_.size());
        // Recording is OFF across the marker poke. These writes are undone
        // before this function returns, so they are not world changes and must
        // not reach the light engine: relighting each agent's cell twice per
        // frame would be pure waste, and it would also relight cells whose real
        // contents never moved.
        const bool wasRecording = world_.recording();
        world_.record_changes(false);
        const int you = possession_live() ? possessed_ : -1;
        for (const auto& a : agents_) {
            if (!a.alive || !world_.inside(a.x, a.y, a.z)) continue;
            saved.push_back(Saved{a.x, a.y, a.z, world_.at(a.x, a.y, a.z)});
            const bool mine = (int(&a - agents_.data()) == you);
            world_.set(a.x, a.y, a.z, std::uint8_t(kBlocks + (mine ? 1 : 0)));
        }
        // The renderer normalises a CUBE of side n. This world is n x 64 x n, so
        // the height guard below is what keeps it a slab instead of the renderer
        // walking 256 layers of nothing looking for blocks that stop at 64.
        // Extending the renderer to take three extents would have been the
        // tidier change and a much larger one, for a picture that is identical.
        // The renderer walks a cube of side `side`, so it has to be at least as
        // tall as the world or the top of a narrow map is silently cut off.
        const int side = std::max(n_, kHeight);

        // ── the camera follows the body ─────────────────────────────────────
        //
        // The EXISTING camera, moved — not a second one. cam_.tx/ty/tz is the
        // point the orbit camera circles, and it is already the thing every
        // other camera control here works in terms of, so following costs three
        // assignments and leaves orbit, dolly, the standard views and the
        // projection toggle all working exactly as they did. Writing a separate
        // first-person camera would have meant two cameras that disagree about
        // where they are pointing, and the one the user is driving being the
        // one the render ignores.
        //
        // The mapping is the renderer's own, from voxel.hpp: a cell sits at
        // (cell + 0.5) * 2/side - 1 in the normalised cube. It is duplicated
        // here rather than shared because render/voxel.hpp is not mine to edit;
        // camera_follows_the_possessed_agent in the suite asserts the two agree
        // to within half a voxel, so a change on either side is caught.
        if (you >= 0 && std::size_t(you) < agents_.size()) {
            const Agent& pa = agents_[std::size_t(you)];
            const float k = 2.0f / float(side);
            cam_.tx = (float(pa.x) + 0.5f) * k - 1.0f;
            cam_.ty = (float(flipY(pa.y)) + 0.5f) * k - 1.0f;
            cam_.tz = (float(pa.z) + 0.5f) * k - 1.0f;
        }
        // The renderer's Y grows DOWNWARD — that is the convention the other
        // voxel sims use, where y=0 is sky and the surface is found by scanning
        // down from it. This world is the other way up: y=0 is bedrock, because
        // the game's ore depths are quoted that way and inverting them in every
        // band would be far worse than inverting them once, here. Without the
        // flip the whole map renders as its own underside — a featureless black
        // slab that reads as a broken renderer.

        // Bound-check EXPLICITLY. world_.at() reports out-of-bounds as Bedrock
        // on purpose — it stops an agent walking off the edge of the world — but
        // the renderer walks a cube of side 128 while the map is 96 across, so
        // every cell outside came back solid and the whole world rendered as a
        // featureless block encasing itself. A guard written for one caller
        // becoming a wall for another.
        vox_.render(side,
            [&](int x, int y, int z) {
                if (x >= n_ || z >= n_ || y >= kHeight) return false;
                return world_.at(x, flipY(y), z) != Air;
            },
            [&](int x, int y, int z, int face) { return texture(x, y, z, face); },
            cam_, Rgb{16, 20, 28});
        for (auto it = saved.rbegin(); it != saved.rend(); ++it)
            world_.set(it->x, it->y, it->z, it->was);
        world_.record_changes(wasRecording);

        // Top-down slice for the timeline: the highest block of every column,
        // which is what a map of a town should show.
        view_.fill(0);
        for (int z = 0; z < n_; ++z)
            for (int x = 0; x < n_; ++x) {
                const int s = world_.surface(x, z);
                view_.set(x, z, s < 0 ? std::uint8_t(Air) : world_.at(x, s, z));
            }
        for (const auto& a : agents_)
            if (a.alive) view_.set(a.x, a.z, std::uint8_t(kBlocks));
        // Yours goes on LAST, so a neighbour standing in the same column of the
        // top-down slice cannot paint over it.
        if (you >= 0 && std::size_t(you) < agents_.size() && agents_[std::size_t(you)].alive)
            view_.set(agents_[std::size_t(you)].x, agents_[std::size_t(you)].z,
                      std::uint8_t(kBlocks + 1));
    }
    // 128, so the game's published ore bands (diamond Y0-15, gold Y0-31, iron
    // Y0-63) and its sea level at Y=63 can be used as written.
    static constexpr int kHeight = 128;

    Provenance about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    int n_ = 96;
    BlockWorld world_;
    // Declared AFTER world_ on purpose: both hold a reference to it and member
    // initialisation runs in declaration order, so putting either first is a
    // read of an object that does not exist yet.
    LightEngine light_{world_};
    Fluids      fluids_{world_, Dimension::Overworld, 0xF1D5EEDull};
    BiomeMap    biomes_;
    MobWorld    mobs_{0x30B31E5Eull};
    // Not a second light field — a view onto light_, so mobs.hpp can ask its
    // two questions without including light.hpp. Declared after light_, and for
    // the same reason the others are.
    EngineLightView lightView_{light_};
    std::vector<Agent> agents_;
    Field view_;
    VoxelRenderer vox_;
    Camera cam_;
    std::uint64_t gen_ = 0;
    long long ticks_ = 0;
    int episode_ = 0;
    int epochs_ = 0;
    long long deaths_ = 0;
    long long rescues_ = 0;     // watchdog firings; see the stuck watchdog
    bool obeyHuman_ = false;    // this tick's action came from a keystroke
    long long structures_ = 0;
    std::array<long long, kActions> actionHist_{};
    // ── possession state ────────────────────────────────────────────────────
    // possStanding_ is the STANDING ORDER, and it is the whole reason a person
    // can play this at all. The world runs 20 ticks per frame by default, so a
    // model where the agent acts only on the tick a key is pressed would have a
    // human idle through ~19 decisions out of every 20 and score like a corpse.
    // A held order is also what the machines get for free: Mine is committed for
    // up to 300 ticks and every Go* option runs until it arrives. Repeating the
    // last order until a new one arrives is the same bargain, not a favour.
    int  possessed_ = -1;
    int  prePossess_ = 1;            // the policy to hand the body back to
    float prePossessDist_ = -1.f;    // the framing to hand the view back to
    std::deque<int> possQueue_;
    int  possStanding_ = kStand;
    long long possIdle_ = 0;
    bool possAuto_ = false;
    long long possMoves_ = 0;        // decisions spent on the six primitives
    long long possCommands_ = 0;     // keystrokes actually accepted
    PossessionTape tape_;
    bool possReplay_ = false;
    PossessionTape replayTape_;
    std::size_t replayAt_ = 0;
    long long replayFault_ = -1;
    // Lit furnaces. A vector rather than a map because a town has a handful, and
    // a linear scan over five entries beats a hash of a 64-bit key.
    std::vector<Smelter> smelters_;
    // Scratch for tickMobs's slice swap. Members, not locals, so the per-tick
    // path allocates nothing after warm-up — the same reason light.hpp keeps its
    // queues as members.
    std::vector<Mob> pool_, slice_, survivors_;
    // Counters. Every one of these exists so a claim about this file can be
    // checked by reading a number instead of by reading the code.
    long long lightUpdates_ = 0, mobSpawned_ = 0, mobKills_ = 0, mobHits_ = 0;
    long long mobBurning_ = 0, smeltsDone_ = 0, toolsBroken_ = 0, fuelBurned_ = 0;
    long long torchesPlaced_ = 0;
    double    mobDamage_ = 0.0, appliedMobDamage_ = 0.0;
    // The world's own stream, separate from every agent's. Random ticks are a
    // property of the world, not of anybody in it, and drawing them from an
    // agent's stream would make the grass grow differently depending on who
    // happened to act first.
    Rng rng_{0xB10CC17Aull};
    Rng flora_{0x5A911Eull};
};

inline SimPtr make_voxelcity(int n = 96) { return std::make_unique<VoxelCity>(n); }

} // namespace bench
