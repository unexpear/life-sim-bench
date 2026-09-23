// voxelcraft.hpp — the block world, and why it is the hard benchmark.
//
//   Guss, W. H. et al. "MineRL: A Large-Scale Dataset of Minecraft Demonstrations",
//   IJCAI 2019. The NeurIPS MineRL competitions (2019-2021) set ObtainDiamond as
//   the headline task and it was never solved from scratch by a competition entry.
//
// The difficulty is not the controls. It is the TECH TREE: to mine a diamond you
// need an iron pickaxe, which needs a furnace, which needs iron, which needs a
// stone pickaxe, which needs stone, which needs a wooden pickaxe, which needs a
// crafting table, which needs planks, which need wood. Ten rungs, nine nested
// prerequisites, and the reward for the last one arrives after thousands of
// steps of doing things that pay nothing.
//
// WHAT THIS MEASURES, and what it does not. Steps needed to reach each rung:
//
//   agent             wood   stone    iron   DIAMOND
//   scripted           115     130     143       227
//   scripted+noise     136     154     175       179
//   random             631    1215    1949    11,437
//
// The middle row is not a third policy. It is the scripted one with 35% of its
// travel steps sent to a random depth, and it is here as an ablation: it reads
// the same recipe table, so it knows the tree just as well. It gets to diamond
// sooner on this world only because a random vertical step tends to dig, and
// digging is what the late rungs want. Running each of the two to diamond on
// world seeds 1 through 8, the noisy one wins five and loses three — noise, not
// skill.
//
// A fiftyfold cost for not knowing what leads where. But note the honest limit:
// this world is 24^3, about fourteen thousand blocks, and a random agent CAN
// brute-force it. Minecraft is millions of blocks and random search cannot.
// The tree here is faithful; the search space is not, so this gap is a floor on
// the real one rather than a reproduction of it.
//
// An earlier version had all three agents reaching diamond in equal time, which
// meant the benchmark measured nothing. Two reasons, both mine: crafting had no
// material cost, so a single adjacency unlocked a whole rung; and the "random"
// agent attempted all ten recipes every step, which is exhaustive search, not a
// random baseline. A baseline that is not actually the thing it claims to be is
// worse than no baseline.
//
// The world is real voxels rendered by render/voxel.hpp — the same perspective
// camera, surface extraction and lighting as the 3D life rules, with per-block
// colour. It is not a picture of a block world; it is one.

#pragma once
#include "../sim.hpp"
#include "../render/voxel.hpp"
#include "../rng.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace bench {

class VoxelCraft final : public Sim {
public:
    enum Block : std::uint8_t { Air = 0, Grass, Dirt, Stone, Wood, Leaves, Iron, Diamond, Bedrock };
    // The tech tree, in order. Each needs the one before it.
    enum Item : int { None = -1, GotWood = 0, Planks, Table, WoodPick, GotStone,
                      StonePick, GotIron, Furnace, IronPick, GotDiamond, kItems };

    static const char* item_name(int i) {
        static const char* n[kItems] = { "wood","planks","crafting table","wooden pickaxe",
                                         "stone","stone pickaxe","iron ore","furnace",
                                         "iron pickaxe","DIAMOND" };
        return (i >= 0 && i < kItems) ? n[i] : "-";
    }

    VoxelCraft(int n = 24) : n_(n), world_(std::size_t(n)*n*n, Air) {
        // The offered sizes, with whatever size this instance was actually
        // constructed at folded in. The bench builds this sim at 24 and the
        // self-test builds it at 20, and a control that cannot express the size
        // its own world is currently at would be a control that lies the moment
        // you look at it.
        sizes_ = { 24, 48, 96, 192, 256 };
        if (std::find(sizes_.begin(), sizes_.end(), n_) == sizes_.end())
            sizes_.push_back(n_);
        std::sort(sizes_.begin(), sizes_.end());
        std::vector<std::string> sizeNames;
        int sizeIndex = 0;
        for (std::size_t i = 0; i < sizes_.size(); ++i) {
            sizeNames.push_back(std::to_string(sizes_[i]));
            if (sizes_[i] == n_) sizeIndex = int(i);
        }

        about_ = Provenance{
            "Block world: the diamond problem", "2019",
            "task after the MineRL benchmark (Guss et al.)",
            "Guss, W. H. et al. \"MineRL: A Large-Scale Dataset of Minecraft Demonstrations\", "
            "IJCAI 2019; the NeurIPS 2019-2021 MineRL competitions set ObtainDiamond",
            Replication::No,
            "No. Agents gather and craft; nothing builds a copy of itself.",
            "Ten rungs and nine nested prerequisites stand between an empty inventory and a "
            "diamond. "
            "Measured here: a scripted agent that knows the recipe reaches diamond in 227 steps, "
            "a random one in 11,437 - a fiftyfold cost for not knowing what leads where. "
            "HONEST LIMIT: this world is 24 cubed, about fourteen thousand blocks, so random "
            "search does eventually work. Minecraft is millions of blocks and random search "
            "does NOT. The tech tree here is faithful; the search space is not, and the gap "
            "this measures is therefore a floor on the real one."
        };
        pal_ = { {{8,11,14},"air"}, {{86,168,72},"grass"}, {{134,96,67},"dirt"},
                 {{128,128,132},"stone"}, {{110,78,44},"wood"}, {{58,132,52},"leaves"},
                 {{198,170,140},"iron ore"}, {{92,224,232},"diamond"}, {{40,40,44},"bedrock"} };
        knobs_ = {
            // Rendering quality. A voxel volume is nothing but hard edges, so
            // every silhouette is a staircase; there is no cheaper fix than
            // more samples, because the aliasing is geometric and no blur can
            // recover what the single sample never had. Measured at 900x700 on
            // a 64-cubed shell, fastest of seven: threading it is worth about
            // 1.8x at 1x sampling and 1.8x again at 2x, and four times the
            // samples costs under three times the frame because the geometry
            // pass is shared.
            //
            // Ratios, not milliseconds. This used to quote absolute frame times
            // and name no machine, which makes a comment that is false for
            // every reader who is not on that one: the same benchmark on other
            // hardware ran two and a half times slower in absolute terms and
            // showed a LARGER speedup. The ratio is the portable half, and even
            // it moves with core count.
            {"aa", "supersampling", 1.f, 3.f, 1.f, 1.f, {"off", "2x", "3x"}, false,
             "Render at this multiple and average down. 2x is the useful setting; the cost "
             "lands on rasterisation, which is the half that threads well.", true},
            {"agent", "which agent", 0.f, 2.f, 0.f, 1.f,
             {"scripted (knows the tree)", "scripted + 35% noise", "random"}, true,
             "Scripted follows the recipe and shows the task IS solvable. Random shows what "
             "sparse reward means. The middle setting is NOT a third policy: it is scripted "
             "with 35% of its travel steps sent one block up or down at random instead of "
             "toward the depth the next resource lives at. Nothing here learns — this file "
             "holds no value table, no visit count and no reward of any kind, so an agent that "
             "explored because novelty paid would have to be written first. Run to diamond on "
             "world seeds 1-8, the noise gets there sooner on five and later on three."},
            {"steps", "agent steps per tick", 1.f, 400.f, 40.f, 1.f, {}, false,
             "Display rate only.", true},
            // The world is resizable, and this is the one knob on this sim that
            // addresses the limitation its own blurb admits to. 24 cubed is
            // fourteen thousand blocks, small enough that a random agent
            // brute-forces it; 256 cubed is sixteen million, and it does not.
            //
            // A choices list rather than a slider, because the steps are large:
            // each option is roughly eight times the volume of the one before,
            // and a continuous slider across that range would spend most of its
            // travel on sizes nobody wants.
            //
            // Measured here, one step at 40 agent steps a tick: 2.7 ms at 24,
            // 4.3 ms at 48, 10.0 ms at 96, 36 ms at 192 and 56 ms at 256, so
            // the top size is one to look at rather than to run fast. Almost all
            // of that is the renderer, which is already threaded and measured at
            // 3.7x on 96 and 4.2x on 192 against one worker; the AGENT costs
            // under 30 microseconds per forty steps at every size, which is why
            // there is nothing to thread on this sim's own side.
            {"size", "world size", 0.f, float(sizes_.size() - 1), float(sizeIndex), 1.f,
             sizeNames, true,
             "Blocks across, cubed. 24 is the size every published number on this sim was "
             "measured at, so leave it there to reproduce them; the larger sizes exist "
             "because the honest limit stated above is a search-space limit, and this is the "
             "control that moves it. Ore and tree DENSITY is held constant as the world "
             "grows, so a bigger world is the same world with more of it rather than the "
             "same handful of resources lost in more space."},
            {"seed", "world seed", 1.f, 40.f, 1.f, 1.f, {}, true,
             "Which world is generated. Ore placement changes; the tree does not."},
        };
        vox_.resize(kRender, kRender);
        view_ = Field(n_, n_);
        cam_.distance = 2.6f; cam_.yaw = 0.7f; cam_.pitch = 0.55f;
        reset();
    }

    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field&               field()   const override { return view_; }
    const Surface*             surface() const override { return &vox_.surface(); }
    std::uint64_t              generation() const override { return gen_; }
    std::vector<Knob>&         knobs()   override { return knobs_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        if (k == "aa") { vox_.set_supersample(int(v + 0.5f)); publish(); }
        // Resizing has to go through reset(): the buffers are rebuilt there,
        // and a world whose volume changed without being regenerated is an
        // empty box with the agent standing outside it.
        if (k == "size" && knobSize() != n_) reset();
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
        cam_.distance = std::clamp(cam_.distance * std::pow(0.85f, steps), 0.3f, 12.0f);
        publish(); return true;
    }
    bool camera_pick(float nx, float ny) override {
        const Surface& s = vox_.surface();
        float px, py, pz;
        if (!pick_voxel(n_, [&](int x,int y,int z){ return at(x,y,z) != Air; },
                        cam_, nx*float(s.w), ny*float(s.h), s.w, s.h, px, py, pz)) return false;
        cam_.tx = px; cam_.ty = py; cam_.tz = pz; publish(); return true;
    }
    void camera_home() override { cam_.tx = cam_.ty = cam_.tz = 0; cam_.distance = 2.6f; publish(); }
    bool camera_view(StdView v) override {
        const float P = 3.14159265f;
        switch (v) {
            case StdView::Front: cam_.yaw=0; cam_.pitch=0; break;
            case StdView::Back:  cam_.yaw=P; cam_.pitch=0; break;
            case StdView::Right: cam_.yaw=P*0.5f; cam_.pitch=0; break;
            case StdView::Left:  cam_.yaw=-P*0.5f; cam_.pitch=0; break;
            case StdView::Top:   cam_.yaw=0; cam_.pitch=1.5533f; break;
            case StdView::Bottom:cam_.yaw=0; cam_.pitch=-1.5533f; break;
            case StdView::Iso:   cam_.yaw=P*0.25f; cam_.pitch=0.6155f; break;
        }
        publish(); return true;
    }
    bool camera_fit() override { camera_home(); return true; }
    bool camera_ortho(bool on) override { cam_.ortho = on; publish(); return true; }
    [[nodiscard]] bool camera_is_ortho() const override { return cam_.ortho; }

    // The rung the agent is on, and what the next one takes. Recipe::how carries
    // that second sentence and nothing displayed it, so the one line that says
    // what the agent is currently trying to DO lived where only a reader of the
    // source could find it.
    [[nodiscard]] std::string subtitle() const override {
        char b[224];
        const int next = highest_ + 1;
        if (next < kItems)
            std::snprintf(b, sizeof b,
                          "%d steps  ·  reached: %s  ·  %d of %d tree steps  ·  next: %s (%s)",
                          steps_, item_name(highest_), highest_ + 1, int(kItems),
                          item_name(next), recipe(next).how);
        else
            std::snprintf(b, sizeof b,
                          "%d steps  ·  reached: %s  ·  %d of %d tree steps  ·  DIAMOND",
                          steps_, item_name(highest_), highest_ + 1, int(kItems));
        return b;
    }

    void reset() override {
        // REBUILD, not refill. generate() only fills the volume it already has,
        // so without this the size knob would move, the label would change, and
        // the world would stay exactly the size it was — a no-op that looks like
        // it worked. view_ has to be rebuilt too, or the timeline keeps
        // publishing a slice of the wrong shape.
        if (knobSize() != n_) {
            n_ = knobSize();
            world_.assign(std::size_t(n_) * n_ * n_, Air);
            view_ = Field(n_, n_);
        }
        rng_.reseed(0xB10C4ull * std::uint64_t(int(knob("seed")) + 1) + worldSalt_);
        generate();
        have_.fill(false);
        held_.fill(0);
        highest_ = -1; steps_ = 0; gen_ = 0;
        lastRung_ = -1; rungStart_ = 0; lastRungCost_ = 0;
        ax_ = n_/2; ay_ = surfaceY(ax_, n_/2); az_ = n_/2;
        publish();
    }

    void step() override {
        const int n = std::max(1, int(knob("steps") + 0.5f));
        for (int i = 0; i < n; ++i) agentStep();
        ++gen_;
        publish();
    }

    std::vector<Metric> metrics() const override {
        return {
            Metric{ "tree rungs reached", double(highest_ + 1), double(kItems) },
            Metric{ "steps for last rung", double(lastRungCost_), 0.0, Metric::Lower },
            Metric{ "steps on this rung", double(steps_ - rungStart_), 0.0, Metric::Lower },
            // Neither, because more blocks is not a better run — it is the mark
            // of the worst one. The random agent tunnels 321 blocks on its way
            // to diamond and the scripted agent 56, so taking the maximum as
            // "best" would report the flailing run as the good one, and a
            // comparison against a kept run would word 321 against 56 as "265
            // better". It describes what happened; it does not score it.
            Metric{ "blocks mined", double(mined_), 0.0, Metric::Neither },
        };
    }

    // A NEW world, not the same one again. reset() reproduces the identical
    // seeded world, so using it here was a control that controlled nothing —
    // the fourth time that exact mistake has been made in this bench. The
    // useful question a re-roll answers is whether a result was a property of
    // the agent or of one world's ore placement.
    bool poke(float, float) override {
        worldSalt_ += 7919;
        reset();
        return true;
    }

    // One epoch is a rung of the tech tree — run until the agent gains
    // something new, or give up. That is the only boundary in this task that
    // means anything.
    [[nodiscard]] const char* epoch_name() const override { return "tree rung"; }
    [[nodiscard]] int epoch_count() const override { return highest_ + 1; }
    bool advance_epoch() override {
        const int start = highest_;
        // The give-up bound scales with the world, because the thing it is
        // bounding does: a rung is reached by travelling to where that resource
        // lives, and that distance grows with the size of the world. Left fixed
        // at 60000, the epoch button would start reporting "gave up" at the
        // larger sizes for a run that was making perfectly good progress. The
        // multiplier is exactly 1 at the published size, so nothing there moves.
        // max(), so this can only ever grow: the self-test builds a 20-cubed
        // world, and scaling blindly would have quietly SHORTENED its budget.
        const int bound = std::max(60000, 60000 * n_ / 24);
        for (int guard = 0; guard < bound && highest_ == start; ++guard) agentStep();
        publish();
        return true;
    }

    // ── measurement ────────────────────────────────────────────────────────
    [[nodiscard]] int highest_item() const { return highest_; }
    [[nodiscard]] int steps() const { return steps_; }
    [[nodiscard]] bool has(int item) const { return item >= 0 && item < kItems && have_[std::size_t(item)]; }
    // No render per step: publish() walks the whole volume, and calling it
    // once per agent step made measurement a rendering benchmark.
    void run(int steps) { for (int i = 0; i < steps; ++i) agentStep(); publish(); }
    void run_quiet(int steps) { for (int i = 0; i < steps; ++i) agentStep(); }
    void set_agent(int a) { for (auto& k : knobs_) if (k.key == "agent") k.value = float(a); }

    // The tree, as data. Each entry names what it needs and what it is made of.
    // `count` is how many of the gathered block are required. Without
    // quantities the whole tree is reachable by being adjacent to one of each
    // thing once, which any wandering agent manages — and the benchmark stops
    // measuring anything. `how` is the sentence the subtitle prints for the
    // rung the agent is working on.
    struct Recipe { int needs; Block mines; int count; const char* how; };
    static Recipe recipe(int item) {
        switch (item) {
            case GotWood:    return { None,      Wood,    4, "chop a tree" };
            case Planks:     return { GotWood,   Air,     0, "craft from wood" };
            case Table:      return { Planks,    Air,     0, "craft from planks" };
            case WoodPick:   return { Table,     Air,     0, "craft at the table" };
            case GotStone:   return { WoodPick,  Stone,   8, "mine with a wooden pickaxe" };
            case StonePick:  return { GotStone,  Air,     0, "craft at the table" };
            case GotIron:    return { StonePick, Iron,    3, "mine with a stone pickaxe" };
            case Furnace:    return { GotIron,   Air,     0, "craft from stone" };
            case IronPick:   return { Furnace,   Air,     0, "smelt, then craft" };
            case GotDiamond: return { IronPick,  Diamond, 1, "mine with an iron pickaxe" };
            default:         return { None,      Air,     0, "-" };
        }
    }

private:
    static constexpr int kRender = 560;
    // The world the published measurements were taken in: 24 across, and ore
    // bands 4 deep. Every feature count in generate() is a density relative to
    // these, which is what makes the default world bit-identical to the one
    // this sim shipped with.
    static constexpr double kBaseArea = 24.0 * 24.0;
    static constexpr double kBaseBand = kBaseArea * 4.0;

    [[nodiscard]] float knob(const char* k) const {
        for (auto& kn : knobs_) if (kn.key == k) return kn.value;
        return 0.f;
    }
    // The size the knob is currently asking for, in blocks across.
    [[nodiscard]] int knobSize() const {
        if (sizes_.empty()) return n_;
        const int i = std::clamp(int(knob("size") + 0.5f), 0, int(sizes_.size()) - 1);
        return sizes_[std::size_t(i)];
    }
    [[nodiscard]] std::size_t idx(int x, int y, int z) const {
        return (std::size_t(y) * n_ + z) * n_ + x;
    }
    [[nodiscard]] Block at(int x, int y, int z) const {
        if (x < 0 || y < 0 || z < 0 || x >= n_ || y >= n_ || z >= n_) return Air;
        return Block(world_[idx(x,y,z)]);
    }
    void set(int x, int y, int z, Block b) {
        if (x < 0 || y < 0 || z < 0 || x >= n_ || y >= n_ || z >= n_) return;
        world_[idx(x,y,z)] = b;
    }
    [[nodiscard]] int surfaceY(int x, int z) const {
        for (int y = 0; y < n_; ++y) if (at(x,y,z) != Air) return y - 1;
        return n_ - 1;
    }

    void generate() {
        std::fill(world_.begin(), world_.end(), std::uint8_t(Air));
        const int ground = n_ / 2;
        for (int z = 0; z < n_; ++z)
            for (int x = 0; x < n_; ++x) {
                const int h = ground + int(2.0f * std::sin(float(x) * 0.4f)
                                         + 1.5f * std::cos(float(z) * 0.33f));
                for (int y = h; y < n_; ++y)
                    set(x, y, z, (y == h) ? Grass : (y < h + 3 ? Dirt : Stone));
                set(x, n_ - 1, z, Bedrock);
            }
        // Trees, then ore. Iron shallow, diamond deep — so reaching diamond
        // means digging, which means a pickaxe, which is the point.
        //
        // Counts scale with the region each feature occupies, so what stays
        // fixed as the world grows is DENSITY. This is not decoration: with the
        // counts left at their 24-cubed values, the scripted agent — the one
        // that knows the whole recipe — failed to reach diamond on two of three
        // seeds at 96 cubed within twenty thousand steps, and on one of them
        // never found wood at all. Eight trees on a 96x96 surface is not a
        // harder search, it is an empty one, and a size nobody can finish is
        // not a feature.
        //
        // Both scale factors are exactly 1 at n = 24, so the published world is
        // generated block for block as it always was, from the identical
        // sequence of random draws.
        const double area = double(n_) * double(n_);
        const int trees = std::max(1, int(8.0 * area / kBaseArea + 0.5));
        for (int t = 0; t < trees; ++t) {
            const int x = 2 + int(rng_.unit() * float(n_ - 4));
            const int z = 2 + int(rng_.unit() * float(n_ - 4));
            const int y = surfaceY(x, z);
            for (int k = 0; k < 4; ++k) set(x, y - k, z, Wood);
            for (int dz = -2; dz <= 2; ++dz)
                for (int dx = -2; dx <= 2; ++dx)
                    for (int dy = -6; dy <= -4; ++dy)
                        if (std::abs(dx) + std::abs(dz) <= 2 && at(x+dx, y+dy, z+dz) == Air)
                            set(x+dx, y+dy, z+dz, Leaves);
        }
        // `baseCount` is the number of veins the published 24-cubed world has.
        // The band this ore lives in is n^2 cells across by (hi-lo) deep, and
        // both of those bands are exactly 4 deep at n = 24, so kBaseBand is the
        // volume the base count was tuned against. Iron's band deepens with the
        // world and diamond's does not, and scaling each by its own band gets
        // that right without either being special-cased.
        auto vein = [&](Block b, int lo, int hi, int baseCount, int size) {
            const double band = area * double(std::max(1, hi - lo));
            const int count = std::max(1, int(double(baseCount) * band / kBaseBand + 0.5));
            for (int v = 0; v < count; ++v) {
                const int x = 1 + int(rng_.unit() * float(n_ - 2));
                const int z = 1 + int(rng_.unit() * float(n_ - 2));
                const int y = lo + int(rng_.unit() * float(std::max(1, hi - lo)));
                for (int k = 0; k < size; ++k) {
                    const int ox = x + int(rng_.unit()*3.0f) - 1;
                    const int oy = y + int(rng_.unit()*3.0f) - 1;
                    const int oz = z + int(rng_.unit()*3.0f) - 1;
                    if (at(ox,oy,oz) == Stone) set(ox,oy,oz, b);
                }
            }
        };
        vein(Iron,    n_/2 + 4, n_ - 4, 12, 6);
        vein(Diamond, n_ - 6,   n_ - 2,  5, 4);
    }

    // Try to advance one step up the tree. Returns true if something was gained.
    bool tryAdvance(int item) {
        if (item < 0 || item >= kItems || have_[std::size_t(item)]) return false;
        const Recipe r = recipe(item);
        if (r.needs != None && !have_[std::size_t(r.needs)]) return false;   // prerequisite
        if (r.mines == Air) {                       // a craft
            have_[std::size_t(item)] = true;
            highest_ = std::max(highest_, item);
            return true;
        }
        // A gather: the agent must be standing next to that block, and must
        // collect `count` of them before the step counts as done.
        for (int dy = -1; dy <= 1; ++dy)
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (at(ax_+dx, ay_+dy, az_+dz) != r.mines) continue;
                    set(ax_+dx, ay_+dy, az_+dz, Air);
                    ++mined_;
                    if (++held_[std::size_t(r.mines)] >= r.count) {
                        have_[std::size_t(item)] = true;
                        highest_ = std::max(highest_, item);
                    }
                    return true;
                }
        return false;
    }

    // Move one step, digging through whatever is in the way.
    void wander(int towardY) {
        const int dir = int(rng_.unit() * 4.0f) % 4;
        int nx = ax_, nz = az_;
        if (dir == 0) ++nx; else if (dir == 1) --nx; else if (dir == 2) ++nz; else --nz;
        nx = std::clamp(nx, 1, n_ - 2); nz = std::clamp(nz, 1, n_ - 2);
        int ny = ay_;
        if (towardY > ay_ && at(ax_, ay_ + 1, az_) != Bedrock) ++ny;
        else if (towardY < ay_) --ny;
        ny = std::clamp(ny, 1, n_ - 2);
        if (at(nx, ny, nz) != Air && at(nx, ny, nz) != Bedrock) { set(nx, ny, nz, Air); ++mined_; }
        ax_ = nx; ay_ = ny; az_ = nz;
    }

    void agentStep() {
        ++steps_;
        // Cost of the rung the agent is currently working on, and of the one it
        // just finished. Rungs reached is a staircase that only ever goes up —
        // it cannot show an agent getting BETTER, only further. Steps per rung
        // can, and it is the number that separates a policy that searches from
        // one that knows where it is going.
        if (highest_ != lastRung_) {
            lastRungCost_ = steps_ - rungStart_;
            rungStart_    = steps_;
            lastRung_     = highest_;
        }
        const int mode = int(knob("agent") + 0.5f);

        if (mode == 2) {
            // ONE random action from the action space. The first version tried
            // all ten recipes every step, which is not a random agent — it is
            // an exhaustive search, and it reached diamond, which made the
            // benchmark measure nothing. A random baseline has to actually be
            // random or it is not a baseline.
            const int a = int(rng_.unit() * float(kItems + 1)) % (kItems + 1);
            if (a == kItems) wander(ay_ + (rng_.unit() < 0.5f ? 1 : -1));
            else tryAdvance(a);
            return;
        }

        // Modes 0 and 1 are the SAME policy: both ask the tree for the next
        // unmet rung and head for where that resource lives. Mode 1 differs only
        // in the 35% roll at the bottom of this function, which sends it to a
        // random depth instead — a noise ablation of the informed policy, not a
        // second kind of agent. It knows the order exactly as well as mode 0
        // does, because it reads the same recipe() call.
        const int want = highest_ + 1;
        if (want >= kItems) return;
        const Recipe r = recipe(want);
        if (r.mines == Air) { tryAdvance(want); return; }
        if (tryAdvance(want)) return;
        // Not adjacent to what is needed: head toward the depth it lives at.
        const int targetY = (r.mines == Wood) ? surfaceY(ax_, az_) - 1
                          : (r.mines == Stone) ? n_/2 + 3
                          : (r.mines == Iron)  ? n_ - 6
                          : n_ - 3;
        if (mode == 1 && rng_.unit() < 0.35f) wander(ay_ + (rng_.unit() < 0.5f ? 1 : -1));
        else wander(targetY);
    }

    void publish() {
        vox_.render(n_,
            [&](int x, int y, int z) { return at(x,y,z) != Air; },
            [&](int x, int y, int z) {
                const Rgb c = pal_[std::size_t(at(x,y,z))].colour;
                return c;
            },
            cam_, Rgb{18, 22, 30});

        // A slice through the agent's depth, for the timeline and metrics.
        view_.fill(0);
        for (int z = 0; z < n_; ++z)
            for (int x = 0; x < n_; ++x)
                view_.set(x, z, std::uint8_t(at(x, ay_, z)));
    }

    Provenance about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    int n_;
    std::vector<int> sizes_;           // what the size knob can select
    std::vector<std::uint8_t> world_;
    Field   view_;
    VoxelRenderer vox_;
    Camera  cam_;
    std::array<bool, kItems> have_{};
    std::array<int, 16> held_{};   // how many of each block gathered
    int ax_ = 0, ay_ = 0, az_ = 0;
    int highest_ = -1, steps_ = 0, mined_ = 0;
    int lastRung_ = -1, rungStart_ = 0, lastRungCost_ = 0;
    std::uint64_t worldSalt_ = 0;
    Rng rng_{0xB10C4ull};
    std::uint64_t gen_ = 0;
};

inline SimPtr make_voxelcraft(int n = 24) { return std::make_unique<VoxelCraft>(n); }

} // namespace bench
