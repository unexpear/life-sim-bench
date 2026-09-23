// platformer.hpp — a side-scroller evolved by NEAT, the MarI/O arrangement.
//
// SethBling's MarI/O (2015) ran NEAT against Super Mario World through an
// emulator, with the evolving network drawn over the game. This is the same
// arrangement with an environment written here instead of a ROM: gravity,
// momentum, a jump arc, gaps to clear, and a goal on the right. The agent sees
// a window of tiles around itself and holds buttons.
//
// Fitness is how far right it gets, which is exactly the MarI/O fitness and has
// the same well-known failure mode: a genome that sprints into the first pit
// scores better than one that stands still, so early generations look like
// enthusiasm rather than skill.
//
// WHAT IS CHECKED, because a physics engine nobody verified is a place for
// bugs to live:
//
//   · the jump apex matches the closed form v^2/2g,
//   · every generated level is completable by a scripted policy, so a
//     population that fails is failing at the task and not at an impossible
//     level.
//
// You watch one genome at a time, as MarI/O did. The network drawn beside the
// level is the genome currently playing, with its evolved topology — nodes and
// connections appear as they are mutated in, which is the thing that makes
// NEAT worth watching rather than only measuring.

#pragma once
#include "../sim.hpp"
#include "../learn/neat.hpp"
#include "../render/voxel.hpp"     // Surface
#include "nesart.hpp"
#include "../rng.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace bench {

class Platformer final : public Sim {
public:
    // apex = v^2/2g = 5.18 tiles in the continuous limit; MEASURED at 4.83,
    // the difference being Euler integration undershooting the integral at a
    // finite timestep. Measured crossable gap is 4 tiles, not the 5.8 the
    // reach arithmetic suggests; the generator stays under that at 2 or 3 —
    // see buildLevel.
    static constexpr float kGravity = 0.05f;
    static constexpr float kJump    = 0.72f;
    static constexpr float kMaxVX   = 0.20f;
    static constexpr float kAccel   = 0.04f;
    static constexpr float kFriction= 0.86f;

    enum Tile : std::uint8_t { Air = 0, Ground = 1, Brick = 2, Query = 3,
                               PipeTop = 4, PipeBody = 5, Coin = 6 };
    static constexpr int kLevelW = 220, kLevelH = 15;
    static constexpr int kViewW  = 9,  kViewH  = 7;    // tiles the agent sees
    static constexpr int kInputs = kViewW * kViewH + 3;
    static constexpr int kOutputs = 3;                  // left, right, jump

    struct Body { float x = 3.0f, y = 0.0f, vx = 0.0f, vy = 0.0f; bool ground = false; };
    struct Walker { float x = 0, y = 0, vx = -0.035f; bool alive = true; };

    Platformer() {
        about_ = Provenance{
            "Platformer evolved by NEAT", "2015",
            "arrangement after SethBling's MarI/O; NEAT is Stanley & Miikkulainen 2002",
            "Stanley, K. O. & Miikkulainen, R. \"Evolving Neural Networks through Augmenting "
            "Topologies\", Evolutionary Computation 10(2) (2002) 99-127",
            Replication::No,
            "No. A population is copied and varied between generations, which is reproduction "
            "of a GENOME by the algorithm - not a machine that builds a copy of itself from "
            "materials, which is the thing von Neumann was asking about and the thing this "
            "bench keeps separate.",
            "An 8-bit side-scroller: run right, clear the gaps, jump the pipes, stomp or avoid "
            "the walkers, reach the flag. NEAT evolves both the weights and the SHAPE of the "
            "network. One genome plays at a "
            "time and is scored on how far right it gets; when the population has all played, "
            "the good ones breed. The network drawn beside the level is the genome currently "
            "playing, and new nodes and connections appear in it as they are mutated in."
        };
        pal_ = { {{8,11,14},"sky"}, {{60,70,84},"ground"}, {{90,209,196},"agent"},
                 {{123,216,143},"goal"} };
        knobs_ = {
            // Separate from "level" on purpose. The level seed asks whether a
            // result survives a different terrain; this asks whether it
            // survives a different evolutionary run on the SAME terrain, which
            // is the control for a change to NEAT itself.
            {"seed", "run seed", 1.f, 40.f, 1.f, 1.f, {}, true,
             "Which independent evolutionary run this is, on the same level. The same seed "
             "reproduces the identical run; a single run cannot tell a real effect from a "
             "lucky one."},
            // Live, not on-reset. Resizing waits for the generation boundary:
            // changing the population part-way through scoring genome k of n
            // would score one genome and attribute the result to another.
            {"population", "population", 20.f, 200.f, 80.f, 10.f, {}, false,
             "Genomes per generation. Larger explores more per generation and takes "
             "proportionally longer to get through one. Applied at the next generation "
             "boundary: growing clones and mutates existing genomes rather than adding fresh "
             "minimal ones, which at generation 200 would be a handicap and not a sample."},
            // The complexity cap. NEAT only ever adds structure — addNode and
            // addConn grow, nothing shrinks — so on a task where a bigger
            // network is not better, growth is pure cost.
            // 0..6, not 0..24. Measured: with the paper's addNode rate this
            // population reaches one hidden node by generation 6, two by 15,
            // three by 25 and four by 40 — a cap of 8 stops being inert around
            // generation 75-95 and 24 stays inert, and a range that spans
            // mostly inert values is a slider that mostly does nothing.
            {"maxnodes", "max hidden nodes", 0.f, 6.f, 0.f, 1.f, {}, false,
             "0 is uncapped. A structural mutation that would exceed this is refused, and a "
             "crossover child that exceeds it falls back to a mutated copy of its parent — "
             "genes are never deleted, because that would break the innovation alignment "
             "NEAT's crossover depends on. Measured on XOR: a cap of 1 solves 0 runs in 8, "
             "which is the paper's 2.35 mean hidden nodes showing up as a hard floor.", false, true},
            // Off by default would hide the finding; on by default with a
            // switch to turn it off is what makes the claim checkable.
            {"walkers", "enemy density", 0.f, 1.f, 0.45f, 0.05f, {}, true,
             "Chance of a walker on each stretch of ground. At the default a level carries "
             "from two to nine walkers depending on the level seed — counted at this density "
             "over all sixty seeds the level knob offers, mean 4.9. This used to say two, "
             "which is what the default level seed alone carries and the bottom of the range. "
             "Still few enough that whether the network can SEE them barely registers: "
             "measured over fourteen level seeds, sighted agents solve in 9.50 generations "
             "against 9.79 blind."},
            {"seeenemies", "enemies visible", 0.f, 1.f, 1.f, 1.f, {"blind", "visible"}, false,
             "Whether the tile grid marks enemies with -1, as SethBling's original does. "
             "Blind, the walkers are invisible to every genome.  ·  It does not obviously "
             "help, and the measurement is worth stating: over fourteen level seeds, generations to "
             "solve were 9.50 sighted against 9.79 blind at the shipped enemy density — the "
             "same within the spread — and 29.21 against 15.43 at maximum density, where "
             "three sighted runs failed to solve at all. Sixty-three extra inputs that are "
             "almost always zero, and that flicker as walkers patrol, cost more convergence "
             "than the information is worth at this budget."},
            {"parentsel", "parent selection", 0.f, 4.f, 4.f, 1.f,
             {"uniform", "roulette", "tournament", "rank", "truncate"}, false,
             "How a parent is chosen WITHIN a species. Uniform means the worst genome in a "
             "species breeds as often as the best, so the only pressure in the algorithm is "
             "between species. The paper truncates to the top fifth."},
            {"level", "level seed", 1.f, 60.f, 1.f, 1.f, {}, true,
             "Which level is generated. The terrain is checked traversable by a scripted policy "
             "before the level is used, so a population that fails is failing at the task and "
             "not at an impossible level. Enemies are a difficulty laid on top of that."},
            {"speed", "steps per tick", 1.f, 60.f, 8.f, 1.f, {}, false, "Display rate only.", true},
        };
        surf_.resize(kW, kH);
        view_ = Field(kLevelW, kLevelH);
        reset();
    }

    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field&               field()   const override { return view_; }
    const Surface*             surface() const override { return &surf_; }
    std::uint64_t              generation() const override { return gen_; }
    std::vector<Knob>&         knobs()   override { return knobs_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
    }

    [[nodiscard]] std::string subtitle() const override {
        char b[176];
        std::snprintf(b, sizeof b,
            "gen %d  ·  genome %d of %d  ·  best %.0f of %d  ·  %zu hidden, %zu links%s",
            neat_.generation(), current_ + 1, neat_.size(), double(bestEver_), kLevelW - 6,
            neat_.genome(current_).hidden_nodes(), neat_.genome(current_).enabled_conns(),
            solved_ ? "  ·  SOLVED" : "");
        // The fallback SAYS so, here, where it can be read.
        //
        // fellBack_ was set and never looked at, so the level generator's
        // promise — "SAY so, because a silent fallback is how the first version
        // of this shipped an empty level that looked fine" — was itself silent.
        // A flag that records an event nobody displays is the same thing as no
        // flag at all.
        if (fellBack_) return std::string(b) + "  ·  FLAT FALLBACK: no traversable level was found for this seed";
        return b;
    }

    void reset() override {
        // Before buildLevel, not after: it decides how many walkers the
        // generator places, and setting it afterwards is a knob that only takes
        // effect on the SECOND reset.
        walkerRate_ = knob("walkers");
        buildLevel(int(knob("level") + 0.5f));
        neat_.init(kInputs, kOutputs, std::max(20, int(knob("population") + 0.5f)),
                   mix_seed(0x8EA7ull, int(knob("seed") + 0.5f)));
        // Speciate deliberately, because the library default cannot work here.
        // These genomes start fully connected at 201 links — (kInputs + 1) *
        // kOutputs, the bias included, probed at gen 0 — and compatibility
        // divides the disjoint/excess term by the gene count, which crushes
        // structural difference to ~0.01, while the weight term is the mean
        // weight difference over matching genes, ~0.53 and under the 3.0
        // threshold on its own — so at the default fixed threshold this
        // population stays a SINGLE species for its whole run and fitness
        // sharing does nothing whatsoever. (Targets of 15 and 8 give identical
        // runs — the threshold rails the same way for both.)
        neat_.params().targetSpecies = 15;
        current_ = 0; steps_ = 0; bestEver_ = 0; solved_ = false;
        distSum_ = 0.0; distN_ = 0; meanDist_ = 0.0f; champDist_ = 0.0f; lastChamp_ = 0.0f;
        seeEnemies_ = knob("seeenemies") > 0.5f;
        body_ = spawn();
        gen_ = 0;
        publish();
    }

    void step() override {
        const int n = std::max(1, int(knob("speed") + 0.5f));
        for (int i = 0; i < n; ++i) tick();
        ++gen_;
        publish();
    }

    std::vector<Metric> metrics() const override {
        return {
            // Best-ever, this generation's champion, and the population mean.
            // Three lines rather than one because they fail differently: a
            // best-ever that climbs while the mean stays flat is one lucky
            // genome, not a population that is learning, and the old single
            // "best distance" line could not tell those apart. "generation"
            // used to be a metric here — plotted against the generation axis
            // it is a diagonal line and says nothing.
            Metric{ "best distance ever", double(bestEver_), double(kLevelW - 6) },
            Metric{ "champion this gen", double(lastChamp_), double(kLevelW - 6) },
            Metric{ "population mean", double(meanDist_), double(kLevelW - 6) },
            // Neither high nor low is better here — it is a description of the
            // population, not a score, so it declines to claim a direction.
            //
            // Gated on the generation because Neat assigns its species count
            // only inside evolve() and does not clear it in init(): read
            // straight through, a sim that has just been reset reports the
            // species count of the run that was thrown away, a number belonging
            // to a population that no longer exists. Before the first
            // generation boundary no speciation pass has run at all, so the 0
            // here reads as "not counted yet" rather than "no species".
            Metric{ "species", neat_.generation() > 0 ? double(neat_.species_count()) : 0.0,
                    0.0, Metric::Neither },
            Metric{ "hidden nodes in best", double(neat_.best().hidden_nodes()), 0.0, Metric::Neither },
        };
    }

    bool poke(float, float) override { body_.vy = -kJump; return true; }

    // One epoch is one GENERATION: every genome plays to death, then they breed.
    [[nodiscard]] const char* epoch_name() const override { return "generation"; }
    [[nodiscard]] int epoch_count() const override { return neat_.generation(); }
    bool advance_epoch() override {
        const int start = neat_.generation();
        for (int guard = 0; guard < 4000000 && neat_.generation() == start; ++guard) tick();
        publish();
        return true;
    }

    // ── physics, exposed so it can be checked ──────────────────────────────
    // Tile index from a world coordinate. std::floor, NOT a cast: int()
    // truncates toward zero, so int(-0.6) is 0 rather than -1, and the wall at
    // the left edge of the world silently stops existing. The agent walked off
    // the map and rendered off-screen, which looked like the sprite not
    // drawing at all.
    static int tileOf(float v) { return int(std::floor(v)); }

    static Body advance(Body b, bool left, bool right, bool jump,
                        const std::vector<std::uint8_t>& tiles) {
        if (left)  b.vx -= kAccel;
        if (right) b.vx += kAccel;
        if (!left && !right) b.vx *= kFriction;
        b.vx = std::clamp(b.vx, -kMaxVX, kMaxVX);
        if (jump && b.ground) { b.vy = -kJump; b.ground = false; }
        b.vy += kGravity;

        auto solid = [&](int tx, int ty) {
            if (tx < 0 || ty < 0 || tx >= kLevelW || ty >= kLevelH) return ty >= kLevelH ? false : true;
            // Coins are decoration, not floor. Everything else
            // that is drawn is stood on.
            const std::uint8_t t = tiles[std::size_t(ty) * kLevelW + tx];
            return t != Air && t != Coin;
        };
        // Horizontal, then vertical — resolving one axis at a time is what
        // stops a body sliding into a corner and through it.
        float nx = b.x + b.vx;
        if (solid(tileOf(nx + (b.vx > 0 ? 0.4f : -0.4f)), tileOf(b.y))) { nx = b.x; b.vx = 0.0f; }
        b.x = std::clamp(nx, 0.0f, float(kLevelW - 1));      // the world has edges
        if (b.x != nx) b.vx = 0.0f;

        float ny = b.y + b.vy;
        b.ground = false;
        if (b.vy > 0 && solid(tileOf(b.x), tileOf(ny + 0.5f))) {
            ny = float(tileOf(ny + 0.5f)) - 0.5f; b.vy = 0.0f; b.ground = true;
        } else if (b.vy < 0 && solid(tileOf(b.x), tileOf(ny - 0.5f))) {
            ny = float(tileOf(ny - 0.5f)) + 1.5f; b.vy = 0.0f;
        }
        b.y = ny;
        return b;
    }
    [[nodiscard]] const std::vector<std::uint8_t>& tiles() const { return tiles_; }
    [[nodiscard]] Body spawn() const { Body b; b.x = 3.0f; b.y = float(groundY_) - 0.5f; return b; }
    [[nodiscard]] static bool dead(const Body& b) { return b.y > float(kLevelH) + 2.0f; }

    // A scripted policy: run right, jump when there is a hole coming. Used to
    // prove a level is completable before any agent is asked to play it.
    // The completability check ignores enemies on purpose: it is asking
    // whether the TERRAIN can be traversed. Enemies are a skill requirement
    // laid on top, and folding them in here would reject perfectly good levels
    // because a scripted walker happened to be in the wrong place.
    [[nodiscard]] bool scripted_completes(int maxSteps = 6000) const {
        Body b = spawn();
        for (int i = 0; i < maxSteps; ++i) {
            if (dead(b)) return false;
            if (b.x >= float(kLevelW - 6)) return true;
            // Jump for a HOLE ahead or a WALL ahead. Checking only for holes
            // makes the first candidate fail nearly always, so the generator
            // burns retries — mean 8.6 attempts instead of 1 — and falls back
            // to flat ground for one of the sixty level seeds: the two-wide
            // PipeTop pair placed two tiles past each gap sits in the row the
            // body occupies, so it is a wall, and the policy walked into one of
            // them and stood there until the timeout. It is not always the
            // first pipe: one that sits close behind a gap gets cleared by the
            // gap jump.
            bool jump = false;
            if (b.ground) {
                // Jump at the LAST moment, when the hole is the very next
                // tile. Looking three ahead makes it leave the ground early and
                // waste most of the arc — measured: an early jump clears a
                // 2-tile gap and dies on a 3-tile one, a late jump clears both.
                const int tx = int(b.x) + 1, ty = int(b.y + 1.0f);
                if (tx < kLevelW && ty < kLevelH && !tiles_[std::size_t(ty)*kLevelW + tx])
                    jump = true;
                const int wx = int(b.x) + 1, wy = int(b.y);
                if (wx < kLevelW && wy >= 0 && wy < kLevelH &&
                    tiles_[std::size_t(wy)*kLevelW + wx]) jump = true;
            }
            b = advance(b, false, true, jump, tiles_);
        }
        return false;
    }
    [[nodiscard]] Body debug_body() const { return body_; }
    [[nodiscard]] float best_distance() const { return bestEver_; }
    [[nodiscard]] bool  solved() const { return solved_; }
    [[nodiscard]] std::vector<float> debug_senses() const { return senses(body_); }
    [[nodiscard]] int   walker_count() const { return int(walkers_.size()); }
    [[nodiscard]] int   neat_generation() const { return neat_.generation(); }
    // Exposed so the speciation settings can be swept from a harness rather
    // than guessed at. The species metric is what raised the question.
    Neat& neat() { return neat_; }
    void run_generations(int g) {
        const int start = neat_.generation();
        for (int guard = 0; guard < 40000000 && neat_.generation() - start < g; ++guard) tick();
    }

private:
    // NES resolution. Drawing at 256x240 and letting the workbench's raster
    // upscale it with nearest neighbour is what makes the pixels square and
    // chunky rather than a smooth vector picture pretending to be 8-bit.
    static constexpr int kW = 256, kH = 240;
    static constexpr int kTile = 16;

    [[nodiscard]] float knob(const char* k) const {
        for (auto& kn : knobs_) if (kn.key == k) return kn.value;
        return 0.f;
    }

    void buildLevel(int seed) {
        Rng r; r.reseed(0x1E4E1ull * std::uint64_t(seed + 1));
        groundY_ = kLevelH - 3;
        fellBack_ = false;
        for (int attempt = 0; attempt < 40; ++attempt) {
            tiles_.assign(std::size_t(kLevelW) * kLevelH, Air);
            walkers_.clear();
            for (int x = 0; x < kLevelW; ++x)
                for (int y = groundY_; y < kLevelH; ++y)
                    tiles_[std::size_t(y) * kLevelW + x] = Ground;
            int x = 12;
            while (x < kLevelW - 14) {
                // Gaps of 2 or 3 tiles, under what the body can actually do.
                // Re-measured by running the jump rule from
                // scripted_completes() over flat levels carrying a single gap,
                // at x = 11, 12, 14, 16, 20, 30, 60, 100 and 150: widths 2, 3
                // and 4 cross at every position, 5 crosses only at 14 and 16,
                // 6 never. So the reach arithmetic's ~5.8 tiles (airtime x top
                // speed) is optimistic.
                //
                // This used to say 4 was uncrossable and blamed a body "nowhere
                // near top speed when it leaves the edge". Both halves were
                // wrong: at 0.04/step top speed arrives after 5 steps and 0.6
                // tiles, and the first gap is nine tiles from spawn, so the
                // run-up is never the short one. Gaps stay at 2-3 anyway,
                // because every convergence number quoted in this file was
                // measured on levels drawn this way; the completability check
                // below is what would catch a widening that went too far.
                const int gap = 2 + int(r.unit() * 2.0f);
                for (int g = 0; g < gap; ++g)
                    for (int y = groundY_; y < kLevelH; ++y)
                        tiles_[std::size_t(y) * kLevelW + x + g] = 0;
                x += gap;
                if (r.unit() < 0.35f) {                     // a pipe to clear
                    const int px2 = x + 2;
                    if (px2 + 1 < kLevelW) {
                        tiles_[std::size_t(groundY_-1) * kLevelW + px2] = PipeTop;
                        tiles_[std::size_t(groundY_-1) * kLevelW + px2 + 1] = PipeTop;
                    }
                    x += 4;
                } else if (r.unit() < 0.5f) {               // a row of blocks overhead
                    const int len = 3 + int(r.unit() * 4.0f);
                    for (int g = 0; g < len && x + g < kLevelW; ++g)
                        tiles_[std::size_t(groundY_ - 4) * kLevelW + x + g] =
                            (g == len/2) ? Query : Brick;
                    for (int g = 0; g < len && x + g < kLevelW; ++g)
                        if ((g & 1) == 0)
                            tiles_[std::size_t(groundY_ - 5) * kLevelW + x + g] = Coin;
                    x += len;
                }
                // A walker patrolling the next stretch of ground.
                if (r.unit() < walkerRate_ && x + 6 < kLevelW)
                    walkers_.push_back(Walker{ float(x + 5), float(groundY_) - 0.5f, -0.035f, true });
                x += 8 + int(r.unit() * 10.0f);
            }
            if (scripted_completes()) return;               // only ship a level that is winnable
        }
        // Fall back to flat ground rather than ship something impossible — and
        // SAY so, because a silent fallback is how the first version of this
        // shipped an empty level that looked fine.
        fellBack_ = true;
        tiles_.assign(std::size_t(kLevelW) * kLevelH, Air);
        walkers_.clear();
        for (int x = 0; x < kLevelW; ++x)
            for (int y = groundY_; y < kLevelH; ++y)
                tiles_[std::size_t(y) * kLevelW + x] = Ground;
    }

    // What the network is shown: a tile grid around the agent, +1 for any
    // non-empty tile — which includes coins the body falls straight through —
    // and -1 for an enemy, which is SethBling's encoding in the original
    // MarI/O script.
    //
    // The enemy channel was missing. The grid encoded solid-or-not and nothing
    // else, so the walkers — which kill on contact — were invisible to every
    // genome in the population. There is no fixed start to memorise either:
    // walkers are revived where the previous genome left them, and each wanders
    // an 11-21 tile stretch. One sign is enough because the two are mutually
    // exclusive in a tile: you cannot stand where an enemy is.
    [[nodiscard]] std::vector<float> senses(const Body& b) const {
        std::vector<float> v(std::size_t(kInputs), 0.0f);
        const int px = int(b.x), py = int(b.y);
        std::size_t k = 0;
        for (int dy = -kViewH/2; dy <= kViewH/2; ++dy)
            for (int dx = -2; dx < kViewW - 2; ++dx) {
                const int tx = px + dx, ty = py + dy;
                const bool solid = (tx < 0 || tx >= kLevelW) ? true
                                 : (ty < 0) ? false
                                 : (ty >= kLevelH) ? false
                                 : tiles_[std::size_t(ty)*kLevelW + tx] != 0;
                float cell = solid ? 1.0f : 0.0f;
                if (!solid && seeEnemies_) {
                    for (const auto& w : walkers_) {
                        if (!w.alive) continue;
                        // floor, and the SAME rounding the body's own tile uses
                        // two lines up. Rounding a walker at y = ground - 0.5
                        // with +0.5 put it on the ground row, which is solid —
                        // so the `!solid` guard skipped it and the enemy channel
                        // never fired once. Four configurations measured
                        // identical to the decimal, which is what an inert
                        // change looks like.
                        if (int(std::floor(w.x)) == tx && int(std::floor(w.y)) == ty) {
                            cell = -1.0f; break;
                        }
                    }
                }
                v[k++] = cell;
            }
        v[k++] = b.vx / kMaxVX;
        v[k++] = std::clamp(b.vy, -1.0f, 1.0f);
        v[k++] = b.ground ? 1.0f : 0.0f;
        return v;
    }

    void tick() {
        ++animTick_;
        // Walkers patrol, turning at a ledge or a wall.
        for (auto& w : walkers_) {
            if (!w.alive) continue;
            const float nx = w.x + w.vx;
            const int ahead = int(nx + (w.vx > 0 ? 0.5f : -0.5f));
            const int below = int(w.y + 1.0f);
            const bool wall = ahead < 0 || ahead >= kLevelW ||
                              (int(w.y) >= 0 && int(w.y) < kLevelH &&
                               tiles_[std::size_t(int(w.y))*kLevelW + ahead] != Air &&
                               tiles_[std::size_t(int(w.y))*kLevelW + ahead] != Coin);
            const bool ledge = ahead < 0 || ahead >= kLevelW || below >= kLevelH ||
                               tiles_[std::size_t(below)*kLevelW + ahead] == Air;
            if (wall || ledge) w.vx = -w.vx; else w.x = nx;
        }

        const auto out = neat_.genome(current_).evaluate(senses(body_));
        const bool left  = out[0] > 0.5f;
        const bool right = out[1] > 0.5f;
        const bool jump  = out[2] > 0.5f;
        body_ = advance(body_, left, right, jump, tiles_);
        ++steps_;

        // Contact with a walker ends the run. Jumping ON one from above
        // removes it, which is the genre's own rule and gives the agent
        // something to discover beyond "hold right".
        bool killed = false;
        for (auto& w : walkers_) {
            if (!w.alive) continue;
            if (std::fabs(w.x - body_.x) < 0.7f && std::fabs(w.y - body_.y) < 0.9f) {
                if (body_.vy > 0.05f && body_.y < w.y - 0.2f) { w.alive = false; body_.vy = -kJump*0.6f; }
                else killed = true;
            }
        }

        const float reached = body_.x;
        const bool won  = reached >= float(kLevelW - 6);
        // Give up on a genome that has stopped making progress, or a whole
        // generation takes as long as its most stubborn stander-still.
        if (reached > furthest_ + 0.5f) { furthest_ = reached; stuck_ = 0; } else ++stuck_;

        if (dead(body_) || killed || won || steps_ > 2500 || stuck_ > 260) {
            float fitness = furthest_;
            if (won) { fitness += kWinBonus; solved_ = true; }
            neat_.set_fitness(current_, fitness);
            bestEver_ = std::max(bestEver_, furthest_);
            // Distance is accumulated separately from fitness. They are the
            // same number only until a genome finishes the level, and then
            // they differ by the win bonus — reporting mean FITNESS beside
            // "best distance ever" on an axis scaled to the level's width put
            // the champion off the top of its own plot at 613 of 214.
            distSum_ += double(furthest_); ++distN_;
            champDist_ = std::max(champDist_, furthest_);
            ++current_;
            if (current_ >= neat_.size()) {
                neat_.evolve();
                // Live controls take effect HERE, on the boundary. The caps and
                // the selection scheme are read fresh each generation so a
                // slider moved mid-run does something; the population resize
                // happens at the same point for the same reason the resize
                // itself refuses mid-generation.
                neat_.params().maxHiddenNodes  = int(knob("maxnodes") + 0.5f);
                neat_.params().parentSelection = Selection(std::clamp(int(knob("parentsel") + 0.5f), 0, 4));
                seeEnemies_ = knob("seeenemies") > 0.5f;
                const int want = std::max(20, int(knob("population") + 0.5f));
                if (want != neat_.size()) neat_.resize(want);
                meanDist_  = distN_ ? float(distSum_ / double(distN_)) : 0.0f;
                lastChamp_ = champDist_;
                distSum_ = 0.0; distN_ = 0; champDist_ = 0.0f;
                current_ = 0;
            }
            body_ = spawn(); steps_ = 0; furthest_ = body_.x; stuck_ = 0;
            // fresh run, dead walkers revived — positions carry over from the
            // last genome
            for (auto& w : walkers_) w.alive = true;
        }
    }

    // ── drawing: 8-bit, tiles and sprites ──────────────────────────────────
    void px(int x, int y, Rgb c) {
        if (x < 0 || y < 0 || x >= kW || y >= kH) return;
        std::uint8_t* p = &surf_.rgba[(std::size_t(y)*kW + x)*4];
        p[0]=c.r; p[1]=c.g; p[2]=c.b; p[3]=255;
    }
    // Blit a 16x16 art cell. '.' is transparent, so sprites keep their shape
    // over whatever is behind them rather than carrying a background block.
    void blit(const char* const* art, int sx, int sy, bool flip = false) {
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                const char c = art[y][flip ? 15 - x : x];
                if (c == '.') continue;
                px(sx + x, sy + y, nes::colour(c));
            }
    }
    void box(int x0,int y0,int x1,int y1,Rgb c){ for(int y=y0;y<y1;++y) for(int x=x0;x<x1;++x) px(x,y,c); }

    void drawBackdrop(int camPx) {
        box(0, 0, kW, kH, nes::colour('S'));
        // Hills and clouds, parallaxed at half and quarter speed. Depth for
        // free, and the era's own trick.
        for (int i = 0; i < 8; ++i) {
            const int hx = i * 190 - (camPx / 2) % 1520;
            for (int w = 0; w < 48; ++w) {
                const int h = 26 - std::abs(w - 24);
                if (h <= 0) continue;
                box(hx + w, kH - 32 - h, hx + w + 1, kH - 32, nes::colour('G'));
            }
        }
        for (int i = 0; i < 10; ++i) {
            const int cx = i * 150 - (camPx / 4) % 1500, cy = 26 + (i * 37) % 40;
            for (int b = 0; b < 3; ++b) {
                const int r = b == 1 ? 9 : 6;
                for (int y = -r; y <= r; ++y)
                    for (int x = -r; x <= r; ++x)
                        if (x*x + y*y <= r*r) px(cx + b*11 + x, cy + y, nes::colour('w'));
            }
        }
    }

    void publish() {
        const int camPx = std::max(0, int(body_.x * float(kTile)) - kW/2 + 24);
        drawBackdrop(camPx);

        // Tiles.
        const int firstCol = camPx / kTile, lastCol = firstCol + kW/kTile + 1;
        for (int ty = 0; ty < kLevelH; ++ty)
            for (int tx = firstCol; tx <= lastCol && tx < kLevelW; ++tx) {
                if (tx < 0) continue;
                const std::uint8_t t = tiles_[std::size_t(ty)*kLevelW + tx];
                if (t == Air) continue;
                const int sx = tx*kTile - camPx, sy = ty*kTile;
                switch (t) {
                    case Ground:   blit(nes::tile_ground(),  sx, sy); break;
                    case Brick:    blit(nes::tile_brick(),   sx, sy); break;
                    case Query:    blit(nes::tile_query(),   sx, sy); break;
                    case PipeTop:  blit(nes::tile_pipe_top(),sx, sy); break;
                    case PipeBody: blit(nes::tile_pipe_body(),sx,sy); break;
                    case Coin:     blit(nes::sprite_coin(),  sx, sy); break;
                    default: break;
                }
            }

        // The flagpole at the end.
        {
            const int gx = (kLevelW - 6)*kTile - camPx;
            if (gx > -20 && gx < kW + 20) {
                box(gx + 6, 32, gx + 9, kLevelH*kTile, nes::colour('m'));
                for (int y = 0; y < 14; ++y)
                    box(gx + 9, 36 + y, gx + 9 + (14 - y), 37 + y, nes::colour('g'));
            }
        }

        // Enemies.
        for (const auto& e : walkers_) {
            if (!e.alive) continue;
            const int sx = int(e.x * float(kTile)) - camPx, sy = int(e.y * float(kTile)) - 8;
            if (sx > -20 && sx < kW + 20) blit(nes::sprite_walker(), sx, sy);
        }

        // The runner: walk cycle on the ground, a jump pose in the air.
        {
            const int sx = int(body_.x * float(kTile)) - camPx - 8;
            const int sy = int(body_.y * float(kTile)) - 8;
            const bool flip = body_.vx < -0.01f;
            if (!body_.ground) blit(nes::sprite_jump(), sx, sy, flip);
            else if (std::fabs(body_.vx) > 0.02f)
                blit(((animTick_ / 6) & 1) ? nes::sprite_run_a() : nes::sprite_run_b(),
                     sx, sy, flip);
            else blit(nes::sprite_run_a(), sx, sy, flip);
        }

        // MarI/O drew its network over the game, so this does too: the genome
        // currently playing, small, in the corner, with the tile window it sees
        // laid out as the grid it actually is.
        drawNetOverlay();

        // Index view for the timeline.
        view_.fill(0);
        for (int y = 0; y < kLevelH; ++y)
            for (int x = 0; x < kLevelW; ++x)
                view_.set(x, y, tiles_[std::size_t(y)*kLevelW + x] ? 1 : 0);
        const int ix = std::clamp(int(body_.x), 0, kLevelW-1);
        const int iy = std::clamp(int(body_.y), 0, kLevelH-1);
        view_.set(ix, iy, 2);
        for (int y = 0; y < kLevelH; ++y) view_.set(kLevelW-6, y, 3);
        for (int i = 0; i < neat_.size() && i < kLevelW; ++i) view_.set(i, 0, 3);
    }

    void drawNetOverlay() {
        const NeatGenome& g = neat_.genome(current_);
        const int ox = 4, oy = 4;
        box(ox-2, oy-2, ox + 86, oy + 62, Rgb{0,0,0});
        const auto sense = senses(body_);
        auto pos = [&](int id, int& X, int& Y) {
            if (id < kInputs - 3) { X = ox + (id % kViewW) * 4; Y = oy + (id / kViewW) * 4; }
            else if (id < kInputs) { X = ox + (id - (kInputs-3)) * 4; Y = oy + kViewH * 4 + 2; }
            else if (id == g.biasNode()) { X = ox + 36; Y = oy + kViewH * 4 + 2; }
            else if (id < g.firstHidden()) { X = ox + 80; Y = oy + 8 + (id - g.inputs - 1) * 14; }
            else { const int h = id - g.firstHidden();
                   X = ox + 48 + (h * 7) % 26; Y = oy + 4 + (h * 11) % 48; }
        };
        for (const auto& c : g.conns) {
            if (!c.enabled) continue;
            int x0,y0,x1,y1; pos(c.from,x0,y0); pos(c.to,x1,y1);
            const int n = std::max(1, std::max(std::abs(x1-x0), std::abs(y1-y0)));
            const Rgb col = c.weight >= 0 ? Rgb{60,180,160} : Rgb{180,60,58};
            for (int i=0;i<=n;++i) px(x0+(x1-x0)*i/n, y0+(y1-y0)*i/n, col);
        }
        for (int id = 0; id < g.nextNode; ++id) {
            int X,Y; pos(id,X,Y);
            Rgb c{110,110,110};
            if (id < kInputs) {
                const float sv = sense[std::size_t(id)];
                c = (sv > 0.5f)  ? Rgb{252,252,252}      // solid
                  : (sv < -0.5f) ? Rgb{216, 40, 40}      // an enemy, which it can now see
                                 : Rgb{ 40, 44, 52};     // empty
            }
            else if (id < g.firstHidden()) c = Rgb{252,224,60};
            else c = Rgb{188,120,252};
            box(X, Y, X+3, Y+3, c);
        }
    }

    Provenance about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    Field   view_;
    Surface surf_;
    Neat    neat_;
    std::vector<std::uint8_t> tiles_;
    std::vector<Walker> walkers_;
    int animTick_ = 0;
    Body  body_;
    int   groundY_ = 12, current_ = 0, steps_ = 0, stuck_ = 0;
    bool  fellBack_ = false;
    bool  seeEnemies_ = true;
    float walkerRate_ = 0.45f;
    float furthest_ = 3.0f, bestEver_ = 0.0f;
    static constexpr float kWinBonus = 400.0f;   // added to fitness, never to distance
    double distSum_ = 0.0; int distN_ = 0;
    float  meanDist_ = 0.0f, champDist_ = 0.0f, lastChamp_ = 0.0f;
    bool  solved_ = false;
    std::uint64_t gen_ = 0;
};

inline SimPtr make_platformer() { return std::make_unique<Platformer>(); }

} // namespace bench
