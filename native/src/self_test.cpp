// self_test.cpp — dependency-free verification of every rule on the bench.
//
// These are not smoke tests. Each one asserts a documented, checkable property
// of the published rule: the blinker's period, the glider's displacement, the
// exact cell a von Neumann construction code produces. If a rule here is wrong,
// the bench is lying about the science, which is the only failure that actually
// matters.

#include "sims/life_like.hpp"
#include "sims/von_neumann.hpp"
#include "sims/small_lattice.hpp"
#include "sims/continuous.hpp"
#include "sims/pps.hpp"
#include "sims/langton_loops.hpp"
#include "sims/spatial_pd.hpp"
#include "sims/rulespec.hpp"
#include "sims/life3d.hpp"
#include "sims/gridworld.hpp"
#include "sims/my_sim.hpp"
#include "sims/voxelcity.hpp"
#include "learn/mlp.hpp"
#include "learn/neat.hpp"
#include "sims/platformer.hpp"
#include "sims/pokebattle.hpp"
#include "sims/voxelcraft.hpp"
#include "sims/gasim.hpp"
#include "sims/lifeengine.hpp"
#include "sims/hexplanet.hpp"
#include "sims/kinesis.hpp"
#include "sims/cutemold.hpp"
#include "parallel.hpp"
#include "sims/continual.hpp"
#include "sims/neuralq.hpp"
#include "sims/cartpole.hpp"
#include "patterns.hpp"
#include "identify.hpp"
#include "render/raster.hpp"
#include "registry.hpp"
#include "hardware.hpp"
#include "history.hpp"
#include "paths.hpp"
#include <fstream>

#include <cstdio>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <cmath>
#include <vector>

namespace {

int  g_checks = 0;
int  g_failed = 0;
std::string g_group;

void group(const char* name) {
    g_group = name;
    std::printf("\n  %s\n", name);
}

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) {
        std::printf("    ok    %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("    FAIL  %s\n", what.c_str());
    }
}

// A measured number worth printing that is deliberately NOT asserted. Used
// where pinning the value would encode what the code does rather than what it
// should do — the pattern that quietly cements bugs. It does not touch the
// counters, so it can never turn the suite green or red.
void note(const std::string& what) {
    std::printf("    ..    %s\n", what.c_str());
}

using Cells = std::set<std::pair<int,int>>;

Cells live_cells(const bench::Field& f) {
    Cells out;
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x)
            if (f.at(x, y)) out.insert({x, y});
    return out;
}

Cells shifted(const Cells& c, int dx, int dy) {
    Cells out;
    for (auto& [x, y] : c) out.insert({x + dx, y + dy});
    return out;
}

// Hash the whole TRAJECTORY — the initial field and every step after it — not
// just where the sim ends up.
//
// Endpoint comparison is not sensitive enough to test a control, and it fails
// in both directions. Two settings can start differently and converge (Brian's
// Brain erased a real 4-cell difference by step 3, so a working rule switch
// looked dead), and two settings can differ throughout yet land in the same
// absorbing state (Life from an empty field and from a full one are both empty
// by step 1, so a working density knob looked dead). A running hash over every
// intermediate state catches a difference that exists at any moment, which is
// what "does this control do anything" actually means.
std::uint64_t trajectory(bench::Sim& s, int steps) {
    std::uint64_t h = 1469598103934665603ull;             // FNV-1a offset basis
    auto mix = [&](std::uint8_t b) { h ^= b; h *= 1099511628211ull; };
    // Hash the SURFACE too, where there is one. A sim that renders itself has
    // output the index field does not contain — the network diagram's
    // "which example am I showing" control changes every pixel of the picture
    // and not one cell of the weight heatmap, so a field-only hash reports a
    // working control as dead.
    auto absorb = [&] {
        for (auto c : s.field().cells) mix(c);
        if (const bench::Surface* sf = s.surface())
            for (auto b : sf->rgba) mix(b);
    };
    absorb();                                              // the start counts too
    for (int i = 0; i < steps; ++i) { s.step(); absorb(); }
    return h;
}

// The same thing in EPOCHS, for sims that have one.
//
// A frame is too short a window to see a parameter that only bites once per
// episode. The gridworld's epsilon decay is applied at an episode boundary, so
// across 24 frames it changes nothing measurable — the knob test passed on it
// only because one particular seed happened to flip an action inside that
// window, and it stopped passing the moment the base seed changed. Twenty-four
// frames was never a test of that knob; it was a coincidence.
//
// generation() goes into the hash as well: two runs that reach the same state
// by different routes still took different numbers of frames to get there, and
// for a learning sim that difference IS the result.
std::uint64_t trajectory_epochs(bench::Sim& s, int epochs, bool includePicture = true) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::uint8_t b) { h ^= b; h *= 1099511628211ull; };
    auto absorb = [&] {
        for (auto c : s.field().cells) mix(c);
        // The rendered surface is the PICTURE, not the outcome. A supersampling
        // control changes every pixel and not one thing about the simulation,
        // so judging it against a hash that includes the image would call a
        // correctly-behaving display knob a bug.
        if (includePicture)
            if (const bench::Surface* sf = s.surface())
                for (auto b : sf->rgba) mix(b);
        std::uint64_t g = s.generation();
        for (int i = 0; i < 8; ++i) { mix(std::uint8_t(g & 0xFF)); g >>= 8; }
        for (const auto& m : s.metrics()) {
            const double v = m.value;
            const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
            for (std::size_t i = 0; i < sizeof v; ++i) mix(p[i]);
        }
    };
    absorb();
    for (int i = 0; i < epochs; ++i) { s.advance_epoch(); absorb(); }
    return h;
}

// Build a life-like sim on a clear grid with an exact pattern.
bench::LifeLike& as_life(bench::SimPtr& p) {
    return *static_cast<bench::LifeLike*>(p.get());
}

} // namespace

int main() {
    std::printf("life-sim-bench self-test\n");

    // ── Conway's Life ──────────────────────────────────────────────────────
    {
        group("Conway's Game of Life (B3/S23)");

        // Blinker: period 2.
        {
            auto sim = bench::make_life(32);
            auto& L = as_life(sim);
            L.clear();
            L.put(10, 10); L.put(11, 10); L.put(12, 10);
            L.step();
            Cells vertical{{11, 9}, {11, 10}, {11, 11}};
            check(live_cells(L.field()) == vertical, "blinker rotates to vertical after 1 gen");
            L.step();
            Cells horizontal{{10, 10}, {11, 10}, {12, 10}};
            check(live_cells(L.field()) == horizontal, "blinker returns after 2 gens (period 2)");
        }

        // Block: still life.
        {
            auto sim = bench::make_life(32);
            auto& L = as_life(sim);
            L.clear();
            L.put(5, 5); L.put(6, 5); L.put(5, 6); L.put(6, 6);
            const Cells before = live_cells(L.field());
            for (int i = 0; i < 8; ++i) L.step();
            check(live_cells(L.field()) == before, "block is unchanged after 8 gens");
        }

        // Glider: translates (1,1) every 4 generations.
        {
            auto sim = bench::make_life(48);
            auto& L = as_life(sim);
            L.clear();
            L.put(11, 10); L.put(12, 11); L.put(10, 12); L.put(11, 12); L.put(12, 12);
            const Cells start = live_cells(L.field());
            for (int i = 0; i < 4; ++i) L.step();
            check(live_cells(L.field()) == shifted(start, 1, 1),
                  "glider translates exactly (1,1) in 4 gens");
            for (int i = 0; i < 4; ++i) L.step();
            check(live_cells(L.field()) == shifted(start, 2, 2),
                  "glider translates exactly (2,2) in 8 gens");
        }

        // A lone cell dies (underpopulation), a pair dies too.
        {
            auto sim = bench::make_life(32);
            auto& L = as_life(sim);
            L.clear(); L.put(4, 4);
            L.step();
            check(L.population() == 0, "a lone cell dies");
            L.clear(); L.put(4, 4); L.put(5, 4);
            L.step();
            check(L.population() == 0, "a domino dies (both have 1 neighbour)");
        }
    }

    // ── HighLife ───────────────────────────────────────────────────────────
    {
        group("HighLife (B36/S23)");

        // The distinguishing clause: birth on six neighbours, which Life lacks.
        // A dead centre surrounded by exactly 6 live neighbours must be born.
        auto build_six = [](bench::LifeLike& L) {
            L.clear();
            // six of the eight neighbours of (10,10)
            L.put( 9,  9); L.put(10,  9); L.put(11,  9);
            L.put( 9, 10);                L.put(11, 10);
            L.put( 9, 11);
        };
        {
            auto sim = bench::make_highlife(32);
            auto& H = as_life(sim);
            build_six(H);
            H.step();
            check(H.field().at(10, 10) == 1, "HighLife: dead cell with 6 neighbours is born");
        }
        {
            auto sim = bench::make_life(32);
            auto& L = as_life(sim);
            build_six(L);
            L.step();
            check(L.field().at(10, 10) == 0, "Life: the same cell is NOT born (B3 only)");
        }
    }

    // ── Seeds ──────────────────────────────────────────────────────────────
    {
        group("Seeds (B2/S)");
        auto sim = bench::make_seeds(32);
        auto& S = as_life(sim);
        S.clear();
        S.put(10, 10); S.put(11, 10);
        S.step();
        // Both parents die; the four cells directly above and below the domino
        // each see exactly two live neighbours and are born.
        Cells expect{{10, 9}, {11, 9}, {10, 11}, {11, 11}};
        check(live_cells(S.field()) == expect, "domino dies and births exactly 4 cells");
        check(S.field().at(10, 10) == 0, "no cell ever survives its own generation");
    }

    // ── von Neumann's 29 states ────────────────────────────────────────────
    {
        group("Von Neumann 29-state (the machine Life simplified)");
        using namespace bench::jvn;

        // Propagation: an excited ordinary-east cell excites the cell to its
        // east, exactly one cell per tick.
        {
            bench::VonNeumann V(40, 20);
            V.clear_raw();
            const int y = 10;
            for (int x = 2; x <= 20; ++x) V.set_raw(x, y, ots(0, 0));
            V.set_raw(2, y, ots(0, 1));                       // excite the tail
            V.rule_only();
            check(V.raw_at(3, y) == ots(0, 1), "pulse advances one cell east per tick");
            V.rule_only();
            check(V.raw_at(4, y) == ots(0, 1), "pulse keeps advancing");
            check(V.raw_at(3, y) == ots(0, 0), "and does not linger behind itself");
        }

        // Construction: the sensitized tree. Code 10001 must build ordinary-N.
        // The blank cell walks S -> S0 -> S00 -> S000 -> ordinary north.
        {
            bench::VonNeumann V(40, 20);
            V.clear_raw();
            const int y = 10, tip = 12;
            for (int x = 2; x <= tip; ++x) V.set_raw(x, y, ots(0, 0));
            const std::vector<int> code{1, 0, 0, 0, 1};
            std::vector<std::uint8_t> walk;
            for (std::size_t i = 0; i < code.size(); ++i) {
                V.set_raw(tip, y, ots(0, code[i]));  // drive the tip directly
                V.rule_only();
                walk.push_back(V.raw_at(tip + 1, y));
            }
            check(walk[0] == S_,   "10001: first pulse sensitizes the blank cell (S)");
            check(walk[1] == S0,   "  bit 0 -> S0");
            check(walk[2] == S00,  "  bit 0 -> S00");
            check(walk[3] == S000, "  bit 0 -> S000");
            check(walk[4] == ots(1, 0),
                  "  bit 1 -> crystallises as ORDINARY NORTH, exactly per the published tree");
        }

        // The other leaves of the tree.
        {
            struct Case { std::vector<int> code; std::uint8_t want; const char* name; };
            const Case cases[] = {
                {{1,0,0,0,0}, ots(0,0), "10000 -> ordinary east"},
                {{1,0,0,1},   ots(2,0), "1001  -> ordinary west"},
                {{1,0,1,0},   ots(3,0), "1010  -> ordinary south"},
                {{1,0,1,1},   sts(0,0), "1011  -> special east"},
                {{1,1,0,0},   sts(1,0), "1100  -> special north"},
                {{1,1,0,1},   sts(2,0), "1101  -> special west"},
                {{1,1,1,0},   sts(3,0), "1110  -> special south"},
                {{1,1,1,1},   con(0,0), "1111  -> confluent"},
            };
            for (const auto& c : cases) {
                bench::VonNeumann V(40, 20);
                V.clear_raw();
                const int y = 10, tip = 12;
                for (int x = 2; x <= tip; ++x) V.set_raw(x, y, ots(0, 0));
                std::uint8_t got = 0;
                for (std::size_t i = 0; i < c.code.size(); ++i) {
                    V.set_raw(tip, y, ots(0, c.code[i]));
                    V.rule_only();
                    got = V.raw_at(tip + 1, y);
                }
                check(got == c.want, c.name);
            }
        }

        // Annihilation: an excited SPECIAL pulse destroys an ordinary cell.
        {
            bench::VonNeumann V(40, 20);
            V.clear_raw();
            const int y = 10, x = 15;
            for (int i = 2; i <= 25; ++i) V.set_raw(i, y, ots(0, 0));  // ordinary wire
            V.set_raw(x, y - 1, sts(3, 1));                            // excited special, aimed south
            V.rule_only();
            check(V.raw_at(x, y) == U, "an excited special pulse annihilates an ordinary cell");
        }

        // Confluent: ANDs its ordinary inputs across a two-bit delay.
        {
            bench::VonNeumann V(40, 20);
            V.clear_raw();
            const int cxx = 20, cyy = 10;
            V.set_raw(cxx, cyy, con(0, 0));
            V.set_raw(cxx - 1, cyy, ots(0, 1));   // excited, pointing east into it
            V.set_raw(cxx, cyy - 1, ots(3, 0));   // quiescent, pointing south into it
            V.rule_only();
            check(V.raw_at(cxx, cyy) == con(0, 0),
                  "confluent stays low when only one input is excited (it ANDs)");
        }
    }

    // ── Brian's Brain ──────────────────────────────────────────────────────
    {
        group("Brian's Brain");
        bench::BriansBrain B(32);
        B.clear();
        // a dead cell with exactly two firing neighbours must fire
        B.put(10, 10, 1); B.put(12, 10, 1);
        B.step();
        check(B.field().at(11, 10) == 1, "dead cell with exactly 2 firing neighbours fires");
        check(B.field().at(10, 10) == 2, "a firing cell always becomes refractory");
        B.step();
        check(B.field().at(10, 10) == 0, "a refractory cell always dies");
        B.clear(); B.put(5, 5, 1); B.put(7, 5, 1); B.put(9, 5, 1);
        B.step();
        check(B.field().at(5, 5) != 1 && B.field().at(7, 5) != 1,
              "firing never persists — nothing in this rule stands still");
    }

    // ── Wireworld ──────────────────────────────────────────────────────────
    {
        group("Wireworld");
        bench::Wireworld W(40, 20);
        W.clear();
        for (int x = 2; x < 30; ++x) W.put(x, 10, 3);
        W.put(3, 10, 1); W.put(2, 10, 2);          // head with a tail behind it
        W.step();
        check(W.get(4, 10) == 1, "electron advances along the wire");
        check(W.get(3, 10) == 2, "head becomes tail");
        check(W.get(2, 10) == 3, "tail becomes conductor");
        // the 1-or-2 clause: a conductor with three neighbouring heads stays put
        W.clear();
        W.put(10, 10, 3);
        W.put( 9,  9, 1); W.put(10, 9, 1); W.put(11, 9, 1);
        W.step();
        check(W.get(10, 10) == 3, "conductor with 3 neighbouring heads does NOT fire");
    }

    // ── Langton's Ant ──────────────────────────────────────────────────────
    {
        group("Langton's Ant");
        bench::LangtonAnt A(64);
        const int x0 = A.ant_x(), y0 = A.ant_y();
        A.step_once();
        check(A.generation() == 1, "one step is one generation");
        // first move: white square -> turn right, flip, forward.
        // starting heading is north(0); right of north is east(1).
        check(A.ant_dir() == 1, "on white the ant turns right");
        check(A.ant_x() == x0 + 1 && A.ant_y() == y0, "and steps forward one cell");
        check(A.field().at(x0, y0) == 1, "leaving the square flipped behind it");
        // The famous result: unbounded travel. After 12,000 steps the ant must
        // be far from where it started — the highway, not a bounded scribble.
        bench::LangtonAnt B(512);
        const int bx = B.ant_x(), by = B.ant_y();
        for (int i = 0; i < 12000; ++i) B.step_once();
        const int dist = std::abs(B.ant_x() - bx) + std::abs(B.ant_y() - by);
        check(dist > 60, "after 12,000 steps the ant has escaped its scribble (highway)");
    }

    // ── Boids ──────────────────────────────────────────────────────────────
    {
        group("Boids (Reynolds 1987)");
        // Alignment is what flocking IS. Vicsek order parameter, not spacing:
        // from a uniform-random start the birds are already closer than the
        // flock's equilibrium spacing, so a working separation rule correctly
        // pushes them apart. Asserting the opposite was a wrong test, not a
        // wrong rule.
        bench::Boids F(300, 160, 120);
        const float o0 = F.order_parameter();
        for (int i = 0; i < 400; ++i) F.step();
        const float o1 = F.order_parameter();
        check(o1 > o0 + 0.05f,
              "velocity matching raises the order parameter (headings align)");
        check(F.about().replicates == bench::Replication::No,
              "and the bench states plainly that a flock does not replicate");

        // With alignment off, the birds must NOT spontaneously align.
        bench::Boids G(300, 160, 120);
        G.on_knob("alignment", 0.f); G.on_knob("cohesion", 0.f);
        const float g0 = G.order_parameter();
        for (int i = 0; i < 400; ++i) G.step();
        check(G.order_parameter() < g0 + 0.25f,
              "with velocity matching off, headings stay disordered");

        // The bug this rule had: cohesion and separation were summed as
        // incomparable quantities and only the total was clamped, so they
        // cancelled and the flock collapsed into a few dense knots. A flock
        // must stay SPREAD as well as aligned.
        bench::Boids H(600, 220, 160);
        for (int i = 0; i < 600; ++i) H.step();
        check(H.mean_spacing() > 1.0f,
              "flock stays spread out - no collapse to a point (gap " +
              std::to_string(H.mean_spacing()) + ")");
        // and it should occupy a decent share of the field, not one corner
        std::size_t occupied = 0;
        for (auto c : H.field().cells) if (c) ++occupied;
        check(occupied > 200,
              "and occupies many distinct cells (" + std::to_string(occupied) + ")");
    }

    // ── Particle Life ──────────────────────────────────────────────────────
    {
        group("Particle Life");
        bench::ParticleLife P(400, 4, 160, 120);
        check(P.matrix().size() == 16, "4 types gives a 4x4 attraction matrix");
        bool asymmetric = false;
        for (int a = 0; a < 4 && !asymmetric; ++a)
            for (int b = 0; b < 4; ++b)
                if (a != b && P.matrix()[a * 4 + b] != P.matrix()[b * 4 + a]) { asymmetric = true; break; }
        check(asymmetric, "the matrix is asymmetric — which is what makes it chase rather than clump");
        for (int i = 0; i < 120; ++i) P.step();
        check(P.field().count(0) < std::size_t(160 * 120),
              "particles remain on the field (no blow-up to infinity)");
        check(P.about().replicates == bench::Replication::No,
              "verdict: self-organises, does NOT self-replicate");
    }

    // ── Primordial Particle System ─────────────────────────────────────────
    {
        group("Primordial Particle System (Schmickl et al. 2016)");
        bench::PrimordialParticles P(60, 0.08f);
        check(P.particles() == int(0.08f * 60 * 60), "particle count follows the published density");
        // Two checks used to sit here and neither could fail.
        //
        // The first asserted `about().replicates == Replication::Yes` under the
        // label "the only continuous system here that genuinely divides". That
        // is a struct field compared with a constant in the same file — it
        // tests the Provenance text, not the simulation, and it would keep
        // passing if the rule were deleted. Comparing prose with prose is not
        // a test, and the label made it read like one.
        //
        // The second asserted that `structured()` — particles with at least 13
        // neighbours — does not fall over 260 steps. It rises whenever the gas
        // concentrates locally, which happens in runs that never form a single
        // cell, so it was unfalsifiable in the same way.
        //
        // What replaces them is a property the rule really has and a soup
        // really does not: neighbour counts must become UNEVEN. A uniform gas
        // has a narrow spread of local densities; anything self-organising
        // widens it. That can fail, which is the whole point.
        // Measured through the public field, so this needs nothing added to
        // the sim: occupancy per coarse tile, and how unevenly it is spread.
        auto spread = [&] {
            const bench::Field& f = P.field();
            const int B = 6;                       // tiles per side of a block
            const int bw = std::max(1, f.w / B), bh = std::max(1, f.h / B);
            std::vector<double> occ(std::size_t(bw) * bh, 0.0);
            for (int y = 0; y < f.h; ++y)
                for (int x = 0; x < f.w; ++x)
                    if (f.cells[std::size_t(y) * f.w + x] != 0)
                        occ[std::size_t(std::min(y / B, bh - 1)) * bw + std::min(x / B, bw - 1)] += 1.0;
            double mean = 0.0;
            for (double v : occ) mean += v;
            mean /= double(occ.size());
            if (mean <= 0.0) return 0.0;
            double var = 0.0;
            for (double v : occ) var += (v - mean) * (v - mean);
            return std::sqrt(var / double(occ.size())) / mean;   // coefficient of variation
        };
        const double spread0 = spread();
        for (int i = 0; i < 260; ++i) P.step();
        check(spread() > spread0 * 1.5,
              "local density becomes uneven, which a uniform gas does not do (" +
              std::to_string(spread0) + " -> " + std::to_string(spread()) + ")");
        // the sign(0)==0 requirement, stated as a property of the rule
        check(P.about().blurb.find("more crowded") != std::string::npos,
              "the rule is described in terms of the crowded side, not an arbitrary tiebreak");
    }

    // ── Langton's Loops ────────────────────────────────────────────────────
    {
        group("Langton's Loops (1984)");
        bench::LangtonLoops L(140);
        const std::size_t s0 = L.sheath();
        check(s0 > 40 && s0 < 80, "the canonical seed is planted (~61 sheath cells)");
        check(L.signals() > 0, "and its genome is circulating");
        for (int i = 0; i < 200; ++i) L.step();
        const std::size_t s1 = L.sheath();
        check(s1 > s0 * 1.8,
              "IT REPLICATES: structure more than doubles by gen 200 (" +
              std::to_string(s0) + " -> " + std::to_string(s1) + ")");
        for (int i = 0; i < 400; ++i) L.step();
        check(L.sheath() > s1, "and the colony keeps growing at gen 600");
        check(L.about().replicates == bench::Replication::Yes, "verdict recorded as Yes");
    }

    // ── Nowak & May ────────────────────────────────────────────────────────
    {
        group("Spatial prisoner's dilemma (Nowak & May 1992)");

        auto settle = [](float b, int steps) {
            bench::SpatialPD P(81);
            P.on_knob("b", b);
            for (int i = 0; i < steps; ++i) P.step();
            return P.cooperators();
        };

        // Both extremes freeze; the interesting behaviour is strictly between.
        const double lo = settle(1.05f, 120);
        check(lo > 0.90, "b=1.05: cooperation sweeps the lattice (" +
                          std::to_string(lo) + ")");
        const double hi = settle(2.10f, 120);
        check(hi < 0.10, "b=2.10: defection sweeps it instead (" +
                          std::to_string(hi) + ")");
        const double mid = settle(1.60f, 200);
        check(mid > 0.25 && mid < 0.90,
              "b=1.60: the two coexist (" + std::to_string(mid) + ")");

        // Frozen at the ends, churning in the middle - the property that makes
        // "a perfect world pays nothing" true.
        {
            bench::SpatialPD lo2(81); lo2.on_knob("b", 1.05f);
            for (int i = 0; i < 150; ++i) lo2.step();
            check(lo2.churn() < 0.001, "b=1.05 goes completely static");

            bench::SpatialPD m2(81); m2.on_knob("b", 1.85f);
            for (int i = 0; i < 150; ++i) m2.step();
            check(m2.churn() > 0.02, "b=1.85 keeps churning indefinitely (" +
                                      std::to_string(m2.churn()) + ")");
        }

        // The classic figure: one defector in a sea of cooperators. Because the
        // rule is deterministic and isotropic, the growing pattern must stay
        // exactly four-fold symmetric. This is a strong check on the payoff
        // accounting AND on the toroidal indexing - any asymmetry in either
        // shows up immediately.
        {
            bench::SpatialPD P(81);
            P.on_knob("b", 1.85f);
            P.seed_single_defector();
            check(P.is_symmetric(), "single defector: symmetric at gen 0");
            bool stayed = true;
            for (int i = 0; i < 60; ++i) { P.step(); if (!P.is_symmetric()) stayed = false; }
            check(stayed, "and the pattern stays exactly 4-fold symmetric for 60 gens");
            check(P.cooperators() < 0.999, "the single defector actually spread");
        }
    }

    // ── determinism ────────────────────────────────────────────────────────
    {
        group("Reproducibility");
        auto a = bench::make_life(64);
        auto b = bench::make_life(64);
        for (int i = 0; i < 25; ++i) { a->step(); b->step(); }
        check(a->field().cells == b->field().cells,
              "same seed produces bit-identical soup after 25 gens");
        check(a->generation() == 25, "generation counter tracks steps");
    }

    // ── the toolkit, and a sim built on it ─────────────────────────────────
    {
        group("Toolkit (the parts a new sim should not have to write)");
        auto mine = bench::make_my_sim(64);
        const auto pop0 = mine->field().count(1);
        for (int i = 0; i < 30; ++i) mine->step();
        check(mine->generation() == 30, "GridSim drives the generation counter");
        check(mine->field().count(1) != pop0, "and the rule actually did something");
        check(!mine->knobs().empty(), "knobs declared in the constructor are exposed");

        // Double buffering: a GridSim rule must see the PREVIOUS state, never a
        // half-updated one. A blinker proves it — under in-place update it
        // would collapse instead of oscillating.
        auto blink = bench::make_life(32);
        auto& L = as_life(blink);
        L.clear(); L.put(10,10); L.put(11,10); L.put(12,10);
        L.step(); L.step();
        check(L.field().at(10,10) && L.field().at(11,10) && L.field().at(12,10),
              "double buffering is honoured (blinker survives, not collapses)");

        // Spatial bins must find near neighbours and skip far ones.
        std::vector<float> bx{1.f, 2.f, 40.f}, by{1.f, 2.f, 40.f};
        bench::Bins bins; bins.build(bx, by, 64.f, 64.f, 8.f);
        int seen = 0, saw_far = 0;
        bins.each_near(1.f, 1.f, [&](int i){ ++seen; if (i == 2) ++saw_far; });
        check(seen >= 2, "Bins finds the nearby particles");
        check(saw_far == 0, "and does not return a particle 55 units away");
    }

    // ── every sim in the registry is complete and runnable ─────────────────
    {
        group("Registry");
        bool ok = true, ran = true;
        std::size_t n = 0;
        for (const auto& e : bench::registry()) {
            auto s = e.make();
            const auto& p = s->about();
            if (p.title.empty() || p.who.empty() || p.citation.empty() ||
                p.replication_note.empty() || p.blurb.empty()) ok = false;
            if (s->palette().size() < 2) ok = false;
            if (e.source.empty()) ok = false;
            const auto g0 = s->generation();
            s->step();
            if (s->generation() <= g0) ran = false;
            ++n;
        }
        check(n >= 12, "the roster is populated (" + std::to_string(n) + " sims)");
        check(ok, "every entry carries citation, replication verdict, legend and source path");
        check(ran, "every entry constructs and steps without crashing");
    }

    // ── hardware limits ────────────────────────────────────────────────────
    // The bench runs on machines that are not this one. These check that it
    // measures the machine and bounds itself, rather than trusting constants
    // that happened to be fine on the developer's box.
    {
        group("Hardware probe and safe limits");
        auto h = bench::Hardware::probe();
        std::printf("    -> %s\n", h.note.c_str());
        check(h.logicalCores >= 1 && h.logicalCores <= 512, "core count is sane");
        check(h.availRam > 0 && h.availRam <= h.totalRam, "available RAM <= total");
        check(h.historyBudget >= 16u*1024u*1024u && h.historyBudget <= 512u*1024u*1024u,
              "timeline budget is a share of free RAM, clamped at both ends");
        check(h.workers >= 1, "at least one worker, with headroom left for the UI");

        bench::RateGuard rg; rg.configure(8.0, 2000);
        check(rg.allow(10, 0.01) == 10,      "cheap rule: every requested step allowed");
        check(rg.allow(500, 5.0) == 1,       "expensive rule drops to 1 step/frame");
        check(rg.allow(500, 1000.0) == 1,    "pathological rule still allows exactly 1, never 0");
        check(rg.throttled(500, 1),          "and it reports being throttled");
        check(!rg.throttled(10, 10),         "no false throttle when keeping up");
        check(rg.allow(99999, 0.0001) == 2000, "hard cap holds regardless of speed");
        // Narrowing hazards. These inputs are derived from wall-clock deltas,
        // so anything that blocks the UI thread — a blocking build, a modal
        // window drag, hibernate advancing the tick count — can make them huge.
        // A double->int conversion that does not fit is undefined behaviour,
        // not a wrapped number, so it has to be clamped before the cast.
        check(rg.allow(1e18, 0.001) == 2000,  "absurd step request clamps, does not overflow");
        check(rg.allow(1e18, 0.0) == 2000,    "absurd request with no cost estimate still clamps");
        const double nan = std::nan("");
        check(rg.allow(nan, 0.001) == 0,      "NaN step request allows nothing");
        check(!rg.throttled(nan, 0),          "and does not report a throttle");
        check(rg.allow(-5.0, 0.001) == 0,     "negative request allows nothing");

        std::size_t biggest = 0;
        for (auto& e : bench::registry()) {
            auto s2 = e.make();
            biggest = std::max(biggest, s2->field().cells.size());
        }
        check(biggest < h.maxFieldCells, "every shipped sim is inside the field-size limit");
    }

    // ── backpropagation ─────────────────────────────────────────────────────
    //
    // The gradient check is the test. "The loss went down" proves nothing: a
    // wrong gradient still descends, just toward the wrong place, and the bug
    // never shows in the training curve. Comparing the analytic derivative
    // against a finite difference of the loss is the thing that either holds or
    // does not.
    {
        group("MLP: the gradient is actually the gradient");
        {
            bench::MLP net({3, 5, 4, 2}, 12345);
            const std::vector<float> x{0.3f, -0.7f, 0.9f}, y{1.0f, 0.0f};
            const float worst = net.grad_check(x, y);
            check(worst < 2e-3f,
                  "analytic gradient matches a central finite difference to " +
                  std::to_string(worst));
        }
        // Deeper and wider, so a bug in the recursion cannot hide in a net too
        // shallow to exercise it.
        {
            bench::MLP net({4, 6, 6, 5, 3}, 99);
            const std::vector<float> x{-0.5f, 0.8f, 0.1f, -0.9f}, y{0.0f, 1.0f, 0.5f};
            check(net.grad_check(x, y) < 2e-3f, "  and holds through four weight layers");
        }

        // XOR is the smallest problem no linear model can solve, so learning it
        // is evidence the hidden layer is being used rather than bypassed.
        {
            bench::MLP net({2, 4, 1}, 7);
            const std::vector<std::vector<float>> X{{0,0},{0,1},{1,0},{1,1}};
            const std::vector<std::vector<float>> Y{{0},  {1},  {1},  {0}};
            float first = 0.0f, last = 0.0f;
            for (int epoch = 0; epoch < 4000; ++epoch) {
                last = 0.0f;
                net.zero_grad();
                for (int i = 0; i < 4; ++i) {
                    net.forward(X[i]); last += net.loss(Y[i]); net.backward(Y[i]);
                }
                net.apply(0.5f, 4);
                if (epoch == 0) first = last;
            }
            check(last < first * 0.01f, "XOR: loss falls by more than two orders of magnitude");
            int right = 0;
            for (int i = 0; i < 4; ++i)
                right += ((net.forward(X[i])[0] > 0.5f) == (Y[i][0] > 0.5f)) ? 1 : 0;
            check(right == 4, "  and all four cases come out correct");
        }
    }

    // ── Q-learning ──────────────────────────────────────────────────────────
    //
    // Not "the reward went up" — a decaying exploration rate lifts that on its
    // own. On a deterministic maze the optimal path length is a fact that
    // breadth-first search computes, so the test is whether the GREEDY policy
    // matches it, and whether the learned values satisfy Bellman optimality.
    {
        group("Q-learning reaches the optimum, and satisfies Bellman");
        auto sim = bench::make_gridworld(21, 15);
        auto& G = *static_cast<bench::GridWorld*>(sim.get());

        const int optimal = G.shortest_path();
        check(optimal > 10, "the maze has a well-defined shortest path (" +
                            std::to_string(optimal) + " steps)");
        check(G.greedy_path_length() == -1, "before training the greedy policy never arrives");

        G.train(6000);
        G.set_exploration(0.0f);
        check(G.greedy_path_length() == optimal,
              "after training the greedy policy walks the OPTIMAL path exactly");

        // Convergence in Q-learning is per state-action and conditional on each
        // pair being visited infinitely often (Watkins & Dayan 1992). So the
        // residual is asserted where that precondition is actually met. Over
        // rarely-tried pairs it stays high by construction — an action never
        // taken keeps its initial value, and the algorithm claims nothing about
        // it. Measured here: 0.89 over all pairs, 0.07 over pairs tried 100
        // times, and exactly 0 over pairs tried 500 times.
        check(G.bellman_residual(500) < 1e-4f,
              "Bellman residual is zero over well-visited state-actions");
        check(G.bellman_residual(100) < 0.15f, "  and small over moderately visited ones");
        check(G.bellman_residual(0) > G.bellman_residual(500),
              "  while rarely-tried pairs stay unconverged, exactly as the theorem allows");
    }

    // ── NEAT ────────────────────────────────────────────────────────────────
    //
    // Evolution, not gradient descent: no backward pass, no derivative, and the
    // network's SHAPE is evolved along with its weights. XOR is the benchmark
    // in Stanley's own paper, and it is the right one — no linear model can
    // solve it, so solving it is evidence the algorithm is genuinely adding
    // hidden structure rather than tuning a perceptron.
    {
        group("NEAT solves XOR, the benchmark from its own paper");
        const std::vector<std::vector<float>> X{{0,0},{0,1},{1,0},{1,1}};
        const float Y[4] = {0, 1, 1, 0};

        // Ten runs requiring eight, not five requiring four.
        //
        // The per-run solve rate is 1.000 over the fragility harness's thirty
        // seeds, but seed 555 — the suite's own first one — is genuinely hard
        // and fails. That left the old check passing at exactly 4 of 5, sitting
        // on its threshold with no margin at all. More evidence, not a looser
        // bar: at this rate, eight of ten has room to spare.
        int solved = 0; double gens = 0, hidden = 0;
        for (int run = 0; run < 10; ++run) {
            bench::Neat neat;
            neat.init(2, 1, 150, 555 + std::uint64_t(run) * 104729);
            int at = -1;
            for (int g = 0; g < 150 && at < 0; ++g) {
                for (int i = 0; i < neat.size(); ++i) {
                    float err = 0.0f; int right = 0;
                    for (int k = 0; k < 4; ++k) {
                        const float o = neat.genome(i).evaluate(X[std::size_t(k)])[0];
                        err += std::fabs(o - Y[k]);
                        if ((o > 0.5f) == (Y[k] > 0.5f)) ++right;
                    }
                    neat.set_fitness(i, (4.0f - err) * (4.0f - err));
                    if (right == 4 && at < 0) at = g;
                }
                if (at < 0) neat.evolve();
            }
            if (at >= 0) { ++solved; gens += at; hidden += double(neat.best().hidden_nodes()); }
        }
        check(solved >= 8, "solves XOR in at least 8 of 10 runs (" + std::to_string(solved) + "/10)");
        check(hidden / std::max(1, solved) < 6.0,
              "  with a SMALL network — mean " +
              std::to_string(hidden / std::max(1, solved)) +
              " hidden nodes, against the paper's reported 2.35");
        check(gens / std::max(1, solved) < 150.0, "  and well inside the generation budget");

        // A minimal genome must be genuinely minimal: NEAT's whole premise is
        // that complexity is earned, so it starts with no hidden nodes at all.
        {
            bench::Neat n2; n2.init(3, 2, 20, 42);
            check(n2.genome(0).hidden_nodes() == 0,
                  "a fresh population starts with zero hidden nodes — complexity is earned");
            check(n2.genome(0).enabled_conns() == 8,
                  "  and is fully connected input+bias to output (4 x 2)");
        }
        // Structural mutation must never create a cycle: evaluation walks the
        // graph in topological order and a loop would not terminate.
        {
            bench::Neat n3; n3.init(2, 1, 30, 7);
            for (int g = 0; g < 40; ++g) {
                for (int i = 0; i < n3.size(); ++i) n3.set_fitness(i, 1.0f);
                n3.evolve();
            }
            bool acyclic = true;
            for (int i = 0; i < n3.size(); ++i)
                for (const auto& c : n3.genome(i).conns)
                    if (c.enabled && n3.genome(i).wouldCycle(c.from, c.to)) acyclic = false;
            check(acyclic, "after 40 generations of structural mutation every genome is "
                           "still acyclic");
        }
    }

    // ── speciation actually happens on the platformer ───────────────────────
    //
    // A single species means fitness sharing divides every genome by one and
    // does nothing at all, which is what the library default produces here:
    // these genomes start at 202 links and compatibility divides the weight
    // term by the gene count, so no threshold tuned on XOR can split them.
    // The sim sets its own target; this asserts it took effect, because the
    // failure is invisible — the run still works, just worse.
    {
        group("NEAT speciates on the platformer");
        auto sim = bench::make_platformer();
        auto* pf = static_cast<bench::Platformer*>(sim.get());
        for (int g = 0; g < 14; ++g) sim->advance_epoch();
        check(pf->neat().species_count() > 1,
              "the population is more than one species by generation 14 (" +
              std::to_string(pf->neat().species_count()) + ")");
        check(pf->neat().mean_fitness() <= pf->neat().best_fitness(),
              "the population mean never exceeds its champion");
    }

    // ── the tape ────────────────────────────────────────────────────────────
    //
    // The claim being checked is the one the construction arm alone could not
    // support: the machine builds what it is TOLD to build. A tape written into
    // the lattice, read back out of it, and a structure that matches — plus a
    // different tape building something different, so the check can fail.
    {
        group("Von Neumann: a tape, and a machine that reads it");
        auto describe = [](bench::VonNeumann& V, int from, int to) {
            std::string out;
            const int row = V.arm_row();
            for (int x = from; x <= to; ++x) {
                const std::uint8_t c = V.raw_at(x, row);
                if (c == bench::jvn::U)          out += '.';
                else if (bench::jvn::is_o(c))    out += "ENWS"[bench::jvn::o_dir(c)];
                else if (bench::jvn::is_t(c))    out += "enws"[bench::jvn::t_dir(c)];
                else if (bench::jvn::is_c(c))    out += 'C';
                else                             out += '?';
            }
            return out;
        };
        auto runProgram = [&](int prog, int steps) {
            auto sim = bench::make_von_neumann(70, 40);
            auto* V = static_cast<bench::VonNeumann*>(sim.get());
            for (auto& k : sim->knobs()) if (k.key == "program") k.value = float(prog);
            sim->on_knob("program", float(prog));
            sim->reset();
            const std::vector<int> written = V->read_tape();
            for (int i = 0; i < steps; ++i) sim->step();
            return std::make_tuple(written, V->read_tape(), describe(*V, 7, 11),
                                   V->instructions_read());
        };

        auto [w0, r0, built0, n0] = runProgram(0, 900);
        check(!w0.empty(), "the tape is written into the lattice as real cells (" +
                           std::to_string(w0.size()) + " bits)");
        // Read back OUT of the lattice, not out of the vector that wrote it: the
        // tape cells all point north, across the tape rather than along it, so
        // no cell on it is an input to its neighbour and the rule leaves it
        // alone. If that were wrong the description would rot as it ran.
        check(r0 == w0, "  and is still there, unchanged, after 900 steps of the rule");
        check(n0 > 0, "  the head reads instructions off it (" + std::to_string(n0) + ")");

        // Program 0 says: four ordinary-east, then a confluent.
        check(built0 == "EEEEC",
              "the lattice builds exactly what the tape describes: EEEEC (got " +
              built0 + ")");

        // A different tape, nothing else changed, a different structure. This
        // is the whole difference between an arm that builds one thing and a
        // machine that builds what it is told — and it is what makes the check
        // above falsifiable rather than a restatement of the code.
        auto [w2, r2, built2, n2] = runProgram(2, 900);
        (void)r2; (void)n2;
        check(w2 != w0, "a different program writes a different tape");
        check(built2 == "EEEEE",
              "  and builds a different structure from it: EEEEE (got " + built2 + ")");

        // The head must consume the leading pulse before walking the tree. That
        // bit STRIKES the quiescent cell — it is not a tree input — and feeding
        // it to the tree decoded every instruction one bit out of phase, so a
        // tape reading "wire, wire, wire, confluent" built "special north,
        // ordinary west, ordinary east". Sharing tree() with the rule was not
        // enough; the two also have to agree where an instruction STARTS.
        {
            std::uint8_t node = bench::jvn::S_;
            const int rest[4] = {0, 0, 0, 0};          // 10000 after the strike
            for (int b : rest) node = bench::jvn::tree(node, b);
            check(bench::jvn::is_o(node) && bench::jvn::o_dir(node) == 0,
                  "the four bits AFTER the strike decode 10000 to ordinary east");
            std::uint8_t bad = bench::jvn::tree(bench::jvn::S_, 1);
            for (int b : {0, 0, 0}) bad = bench::jvn::tree(bad, b);
            check(!(bench::jvn::is_o(bad) && bench::jvn::o_dir(bad) == 0),
                  "  and feeding it the strike bit as well does NOT, which is the bug");
        }
    }

    // ── Cute Mold ───────────────────────────────────────────────────────────
    //
    // Selection here runs through geometry alone: energy comes from the empty
    // space a colony encloses, and molds cannot touch each other. So the claim
    // worth checking is that shapes get MORE open over time without any rule
    // mentioning shape.
    {
        group("Cute Mold: shape as the only fitness");
        // Averaged over four seeds, and that is measured rather than chosen.
        // On ONE seed the void-per-cell gain is positive every time but its
        // worst case sits 0.5 standard deviations clear of zero — inside a
        // single seed's spread, which is the definition of a check that the
        // next seed can flip. A longer horizon only got it to 0.8 sd. Four
        // seeds cut the spread of the mean by two and put it past 4 sd, which
        // is more evidence rather than a looser threshold.
        double earlyVoid = 0.0, lateVoid = 0.0;
        std::size_t colonies = 0, cells = 0, classes = 0;
        for (int seed = 1; seed <= 4; ++seed) {
            auto s2 = bench::make_cutemold();
            auto* m2 = static_cast<bench::CuteMold*>(s2.get());
            for (auto& k : s2->knobs()) if (k.key == "seed") k.value = float(seed);
            s2->on_knob("seed", float(seed));
            s2->reset();
            for (int i = 0; i < 150; ++i) s2->step();
            earlyVoid += m2->voidPerCell();
            for (int i = 0; i < 900; ++i) s2->step();
            lateVoid += m2->voidPerCell();
            colonies = m2->liveCount(); cells = std::size_t(m2->cellCount());
            classes  = m2->lineages();
        }
        earlyVoid /= 4; lateVoid /= 4;
        auto sim = bench::make_cutemold();
        auto* M = static_cast<bench::CuteMold*>(sim.get());
        (void)M;

        check(colonies > 0 && cells > 500,
              "the world is alive and full after a thousand ticks (" +
              std::to_string(colonies) + " colonies, " +
              std::to_string(cells) + " cells)");
        // Against the FOUNDER count, not against the count at tick 150. The
        // population overshoots and then settles back — 12 founders become 485
        // colonies by tick 150 and 437 by tick 1050 — so comparing with the
        // peak asks whether it is still growing, which is a different question
        // and not the one about reproduction.
        check(colonies > 12,
              "colonies reproduce: more of them than the twelve founded (" +
              std::to_string(colonies) + ")");
        // The whole model in one number. Nothing tells a mold to be lacy; a
        // lacy mold simply feeds better, and the population drifts that way.
        check(lateVoid > earlyVoid,
              "and their shapes get MORE open over time, which is the selection "
              "pressure and is coded nowhere (" + std::to_string(earlyVoid) +
              " -> " + std::to_string(lateVoid) + ")");
        check(lateVoid > 1.0 && lateVoid < 8.0,
              "  while staying inside what a connected shape can reach");
        check(classes > 1, "more than one lineage survives");
    }

    // ── kinesis ─────────────────────────────────────────────────────────────
    //
    // The claim is that agents which cannot sense direction still end up where
    // conditions are better. What makes it falsifiable is the null model: with
    // both mechanisms off the population is uniform, so the share of agents on
    // favourable ground must equal the share of the world that is favourable.
    {
        group("kinesis: aggregation with no sense of direction");
        auto run = [](float slow, float turn, int seed) {
            auto sim = bench::make_kinesis();
            for (auto& k : sim->knobs()) {
                if (k.key == "seed") k.value = float(seed);
                if (k.key == "slow") k.value = slow;
                if (k.key == "turn") k.value = turn;
            }
            sim->on_knob("seed", float(seed));
            sim->reset();
            sim->on_knob("slow", slow);
            sim->on_knob("turn", turn);
            for (int i = 0; i < 400; ++i) sim->step();
            return static_cast<bench::Kinesis*>(sim.get())->aggregation();
        };
        double control = 0, ortho = 0, klino = 0;
        for (int s = 1; s <= 4; ++s) {
            control += run(0.0f, 0.0f, s);
            ortho   += run(0.85f, 0.0f, s);
            klino   += run(0.0f, 0.85f, s);
        }
        control /= 4; ortho /= 4; klino /= 4;
        check(control > 0.9 && control < 1.1,
              "with both mechanisms off the spread is uniform, index 1.0 (" +
              std::to_string(control) + ")");
        check(ortho > 1.5,
              "orthokinesis alone concentrates them on good ground (" +
              std::to_string(ortho) + "x)");
        // Not a defect. A run-and-tumble walker at constant speed has a uniform
        // steady state however its turning rate varies, so this is the correct
        // answer and the popular framing that klinokinesis aggregates is wrong.
        check(klino < 1.2,
              "klinokinesis alone does NOT, which is what the theory says (" +
              std::to_string(klino) + "x)");
        check(ortho > klino * 1.5, "  and the difference between them is not subtle");
    }

    // ── the simulation space scales ─────────────────────────────────────────
    //
    // The lattice sweep is threaded above a size threshold, and the one thing
    // that must never change is the answer. Rows are independent by
    // construction — everything read comes from the current field, each row
    // writes its own slice of the next — but "by construction" is what people
    // say right before a race, so it is computed both ways and compared.
    {
        group("a bigger world, and the same answer");
        const unsigned was = bench::max_workers();
        auto run = [](int sizeIdx, unsigned workers, int steps) {
            bench::set_max_workers(workers);
            auto sim = bench::make_life();
            for (auto& k : sim->knobs()) if (k.key == "size") k.value = float(sizeIdx);
            sim->on_knob("size", float(sizeIdx));
            sim->reset();
            for (int i = 0; i < steps; ++i) sim->step();
            return sim->field().cells;
        };
        // Index 2 is 1024 squared — over the threshold, so it really does split.
        const auto serial = run(2, 1, 8);
        const auto threaded = run(2, 0, 8);
        check(serial.size() == 1024u * 1024u,
              "the size knob builds a 1024-square world (" +
              std::to_string(serial.size()) + " cells)");
        check(serial == threaded,
              "and a threaded sweep gives the identical field, cell for cell");

        // Below the threshold nothing is split, which is the other half of the
        // contract: small worlds must not pay for a pool they cannot use.
        const auto smallSerial = run(0, 1, 8);
        const auto smallThreaded = run(0, 0, 8);
        check(smallSerial.size() == 256u * 256u && smallSerial == smallThreaded,
              "a small world is identical too, threaded or not");

        // ── the block world, and the town that lives in it ─────────────────
        {
            group("a block world worth living in");
            using namespace bench;
            // 128 tall, like the sim. The published ore bands are quoted in
            // absolute Y — coal 0-127, iron 0-63, gold 0-31 — so in a 64-tall
            // world coal and iron clamp onto each other and the tiering this
            // asserts genuinely does not hold. The first version of this test
            // built a 64-tall world and was right to fail.
            BlockWorld w(96, 128);
            w.generate(11);
            BlockWorld w2(96, 128); w2.generate(11);
            BlockWorld w3(96, 128); w3.generate(12);
            check(w.raw() == w2.raw(), "the same seed generates the same world, block for block");
            check(w.raw() != w3.raw(), "and a different seed generates a different one");

            // Ore tiers must be ORDERED by depth, which is the one thing an
            // agent has to discover. Asserted rather than assumed because the
            // first version had iron sitting ABOVE coal: veins only take in
            // stone, so a band most of the world cannot honour silently
            // collapses onto the bands that can.
            auto meanY = [&](std::uint8_t b) {
                double sum = 0; std::size_t n = 0;
                for (int y = 0; y < w.height(); ++y)
                    for (int z = 0; z < w.n(); ++z)
                        for (int x = 0; x < w.n(); ++x)
                            if (w.at(x, y, z) == b) { sum += y; ++n; }
                return n ? sum / double(n) : -1.0;
            };
            const double mc = meanY(Coal), mi = meanY(IronOre);
            const double mg = meanY(GoldOre), md = meanY(DiamondOre);
            check(mc > mi && mi > mg && mg > md,
                  "ore is tiered by depth: coal " + std::to_string(int(mc * 10) / 10.0) +
                  " above iron " + std::to_string(int(mi * 10) / 10.0) +
                  " above gold " + std::to_string(int(mg * 10) / 10.0) +
                  " above diamond " + std::to_string(int(md * 10) / 10.0));

            // Every size has to be habitable. The generator used a fixed noise
            // frequency and a 64-wide world spanned less than one noise cell —
            // one flat plane, under sea level, entirely sand: seven wood blocks
            // in the whole world, and no agent could take the first rung.
            for (int n : {64, 96, 160}) {
                BlockWorld s2(n, 128); s2.generate(5);
                check(s2.count(Wood) > std::size_t(n),
                      "a " + std::to_string(n) + "-wide world grows trees (" +
                      std::to_string(s2.count(Wood)) + " wood blocks)");
            }

            // A recipe must CONSUME. voxelcraft's benchmark once measured
            // nothing because crafting was free.
            const Recipe r = recipe(ItPlank);
            check(r.in[0].count > 0 && r.makes > 0,
                  "planks cost wood and yield more than they cost");
            check(recipe(ItWoodPick).station == Table,
                  "a wooden pickaxe needs a crafting table, not bare ground");
            check(block_rule(DiamondOre).tier > block_rule(Stone).tier,
                  "diamond needs a better tool than stone does");
        }

        // A knob that rebuilds the town must actually rebuild it. `roles` was
        // read once at construction and left out of on_knob's rebuild list, so
        // the control moved and nothing changed — caught only because an A/B
        // against it returned identical numbers in both arms.
        {
            group("the town's knobs are not decorative");
            auto a = std::make_unique<bench::VoxelCity>();
            a->on_knob("agents", 8.f);
            a->on_knob("roles", 0.f);
            int allRounders = 0;
            for (const auto& ag : a->agents()) if (ag.role < 0) ++allRounders;
            a->on_knob("roles", 1.f);
            int specialists = 0;
            for (const auto& ag : a->agents()) if (ag.role >= 0) ++specialists;
            check(allRounders == 8 && specialists == 8,
                  "the roles knob really reassigns the town (" +
                  std::to_string(allRounders) + " all-rounders, then " +
                  std::to_string(specialists) + " specialists)");
        }

        // ── informed beats random, or the sim is not measuring anything ─────
        //
        // The bar is the SCRIPTED policy, which reads the recipe table and walks
        // to what the next rung needs. If a town of random actors kept up with
        // it, nothing here would be a task.
        //
        // Scored on RUNG PER LIFE, not on the lifetime high-water mark. The
        // high-water mark is a maximum over lives, so it rewards attempts: an
        // agent that dies fourteen times and stumbles one rung up on the
        // fourteenth is scored as having got there. Over eight seeds at 12,000
        // ticks that statistic read scripted 4.56 against random 4.72 — random
        // AHEAD, scripted winning only 3 seeds of 8 — while the rate over the
        // identical runs read 2.79 against 2.28 with scripted winning 7 of 8.
        // The mark had stopped separating the policies; it was not that the
        // policies had stopped differing.
        //
        // Per-seed wins are asserted alongside the mean, because a mean over
        // three seeds can be carried by one outlier and that is precisely how
        // this check went on passing while the thing underneath it decayed.
        {
            group("a town that gets somewhere");
            // EIGHT SEEDS, NOT THREE. Not a weakening — the opposite.
            //
            // The effect is real and was measured independently at eight seeds
            // on this exact build: scripted 3.873 against random 3.459,
            // scripted winning 6 of 8, ratio 1.120. At THREE seeds the same
            // build reads 2.847 against 2.866 and this assertion fails. Three
            // seeds has now pointed the wrong way six times in this project; it
            // cannot resolve a 12% effect against this much per-seed spread,
            // where individual seeds range from 2.56 to 5.31. Raising N makes
            // the test MORE able to detect the claim, which is the direction a
            // fix should move.
            //
            // Worth watching rather than celebrating: that ratio was 1.20
            // before the gauntlet's repairs and is 1.12 after. Fixing mobility
            // helps the policy with no plan more than the one with a plan. If
            // it keeps falling, the answer is a harder world, not a larger N.
            const int kSeeds = 8;
            double scripted = 0, random = 0, hiScripted = 0, hiRandom = 0;
            int wins = 0;
            for (int seed = 1; seed <= kSeeds; ++seed) {
                double rate[2] = {0, 0};
                for (int policy : {1, 2}) {
                    auto sim = std::make_unique<bench::VoxelCity>();
                    sim->on_knob("agents", 8.f);
                    sim->on_knob("size",   1.f);
                    sim->on_knob("seed",   float(seed));
                    sim->on_knob("policy", float(policy));
                    sim->run_quiet(12000);
                    rate[policy - 1] = sim->mean_rung_per_life();
                    if (policy == 1) { scripted += rate[0]; hiScripted += sim->mean_rung(); }
                    else             { random   += rate[1]; hiRandom   += sim->mean_rung(); }
                }
                if (rate[0] > rate[1]) ++wins;
            }
            check(scripted > random,
                  "the informed policy out-climbs random over eight seeds (rung per life " +
                  std::to_string(scripted / kSeeds) + " against " +
                  std::to_string(random / kSeeds) + ")");
            check(wins * 2 > kSeeds,
                  "and wins on a majority of the individual seeds, not on one outlier (" +
                  std::to_string(wins) + " of " + std::to_string(kSeeds) + ")");
            // Reported, deliberately NOT asserted: this is the statistic that
            // stopped discriminating, and pinning it would re-cement the bug.
            note("for the record, the same runs on the lifetime high-water mark: "
                 + std::to_string(hiScripted / kSeeds) + " against "
                 + std::to_string(hiRandom / kSeeds) + ", which is why it is not the bar");
        }

        // ── the shaping potential's invariants ──────────────────────────────
        //
        // Properties it must satisfy BY CONSTRUCTION, not numbers it happens to
        // produce — the difference matters, because an assertion pinned to a
        // measured value encodes what the code does rather than what it should,
        // and this suite has caught that cementing a bug twice.
        //
        // The first of these failed the moment it was written: an agent holding
        // every ingredient AND standing at the station scored rung + 1.0, which
        // is exactly what crafting pays, so a script that gathered and never
        // crafted was worth as much as one that finished. Partial credit must
        // never equal completion.
        {
            group("voxelcity: the tech potential is a potential");
            auto sim = std::make_unique<bench::VoxelCity>();
            sim->on_knob("agents", 8.f);
            sim->on_knob("size",   1.f);
            sim->on_knob("seed",   1.f);
            sim->on_knob("policy", 1.f);
            int badFloor = 0, badLower = 0, samples = 0, moved = 0;
            double maxFrac = 0.0;
            for (int t = 0; t < 3000; ++t) {
                sim->run_quiet(1);
                for (std::size_t i = 0; i < sim->agents().size(); ++i) {
                    const double pot  = sim->tech_potential(i);
                    const int    rung = sim->agents()[i].rung;
                    ++samples;
                    if (int(std::floor(pot + 1e-9)) != rung) ++badFloor;
                    if (pot < double(rung) - 1e-9)           ++badLower;
                    const double frac = pot - double(rung);
                    if (frac > 1e-9) ++moved;
                    maxFrac = std::max(maxFrac, frac);
                }
            }
            check(badFloor == 0 && badLower == 0 && maxFrac < 1.0,
                  "floor(potential) is always the rung and the remainder is a "
                  "strict fraction of one, so partial credit can never complete a "
                  "rung that was not reached (largest remainder " +
                  std::to_string(maxFrac) + " over " + std::to_string(samples) +
                  " samples)");
            // A potential that is always exactly the rung is the sparse counter
            // wearing a new name, which is the failure this exists to replace.
            check(moved > samples / 20,
                  "and the fraction is genuinely used rather than decorative (" +
                  std::to_string(moved) + " of " + std::to_string(samples) +
                  " samples carried partial credit)");
            // Monotone, observed on the running system rather than by poking a
            // synthetic agent. Phi may only fall for a reason, and there are
            // exactly four: ingredients spent (which raises the rung), the pack
            // shrinking, death, and WALKING AWAY FROM THE BENCH.
            //
            // That last one is why this assertion is written the way it is. It
            // first read "never falls while the rung holds and the pack has not
            // shrunk" and failed once in 15,596 ticks — agent 6 at tick 1085,
            // potential 4.4500 -> 4.3000, adjacent stations 1 -> 0. Phi counts
            // the station as an ingredient because reaching it is real work, so
            // that fall is correct and the assertion was an incomplete
            // description of the property. Fourth time in this project that the
            // instrument was the broken part; the drop of exactly 0.9 * 1/6 is
            // what identified it.
            {
                auto t2 = std::make_unique<bench::VoxelCity>();
                t2->on_knob("agents", 8.f);
                t2->on_knob("size",   1.f);
                t2->on_knob("seed",   3.f);
                auto stations = [&](const bench::VoxelCity::Agent& ag) {
                    int n = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dz = -1; dz <= 1; ++dz)
                            for (int dx = -1; dx <= 1; ++dx) {
                                const std::uint8_t b =
                                    t2->world().at(ag.x + dx, ag.y + dy, ag.z + dz);
                                if (b == bench::Table || b == bench::Furnace) ++n;
                            }
                    return n;
                };
                std::vector<double> pPot(8, 0.0);
                std::vector<int> pRung(8, 0), pPack(8, 0), pDeaths(8, 0), pSt(8, 0);
                int drops = 0, checked = 0;
                for (int k = 0; k < 2000; ++k) {
                    t2->run_quiet(1);
                    for (std::size_t i = 0; i < t2->agents().size(); ++i) {
                        const auto& ag = t2->agents()[i];
                        int pack = 0;
                        for (int b = 0; b < bench::VoxelCity::kInv; ++b)
                            pack += ag.blocks[std::size_t(b)];
                        for (int b = 0; b < bench::kItems; ++b)
                            pack += ag.items[std::size_t(b)];
                        const double pot = t2->tech_potential(i);
                        const int st = stations(ag);
                        if (k > 0 && ag.rung == pRung[i] && pack >= pPack[i]
                            && ag.deaths == pDeaths[i] && st >= pSt[i]) {
                            ++checked;
                            if (pot < pPot[i] - 1e-9) ++drops;
                        }
                        pPot[i] = pot; pRung[i] = ag.rung;
                        pPack[i] = pack; pDeaths[i] = ag.deaths; pSt[i] = st;
                    }
                }
                check(drops == 0 && checked > 1000,
                      "and it never falls without a reason to — rung held, pack not "
                      "shrunk, no death, no bench walked away from (" +
                      std::to_string(checked) + " such ticks, " +
                      std::to_string(drops) + " falls)");
            }

            // EQUAL STATES PAY EQUAL FITNESS, so no round trip pays. This is
            // the one property the shaping form actually buys - see
            // lifeFitness() - and it was written down in prose in three places
            // while nothing in this suite tested it. It was also false: the
            // 0.45 term was given the raw difference and a zero terminal, and
            // the 0.20 tier term one line below it kept the rectifier and kept
            // reading a.tier at death, through the fix, the write-up and a full
            // suite run. Six cycles of "make a stone pickaxe, it breaks" paid
            // +0.60000 over standing still with byte-identical endpoints, and
            // ten lives of "hold a diamond pickaxe at the moment of death" paid
            // +2.00000. A property nobody asserts is a property nobody has.
            //
            // The loops below end BYTE-IDENTICAL to where they started on every
            // field lifeFitness() reads, so each must pay exactly what standing
            // still pays.
            {
                auto t3 = std::make_unique<bench::VoxelCity>();
                t3->on_knob("agents", 8.f); t3->on_knob("size", 1.f);
                t3->on_knob("seed", 1.f);   t3->on_knob("policy", 1.f);
                t3->run_quiet(200);
                auto& a = t3->agent_for_test(0);
                // THE DOWN LEG HAS TO FIT UNDER THE [0,1] FLOOR or this
                // measures the clamp instead of the shaping. `lived` is
                // min(1, epTicks/kEpisodeTicks), so a saturating epTicks buys
                // the down leg its full 0.10 of headroom without this file
                // re-typing kEpisodeTicks, and the tier swing below is 0->1
                // (a wood pickaxe breaking, which is the common one) so the
                // charge is 0.05 and lands strictly inside. Measured, on an
                // otherwise empty window at full length: a 0->2 swing needs
                // 0.10 and lands exactly on the boundary, a 0->4 swing needs
                // 0.20, is truncated, and still pays +0.10 a cycle. That
                // truncation is the disclosed clamp, and after the tier fix it
                // is the only thing left between this fitness and the property.
                const int kSaturatingWindow = 1000000;
                // Tolerance, not equality: scriptStartPot is a FLOAT and the
                // potential is a double, so the two legs cancel only to float
                // precision. Measured residual on the cobble loop, 4.7e-08.
                const double kEps = 1e-6;
                // The five boundary lines agentStep() runs when a script is
                // drawn. Copied rather than called because the sim draws them
                // on its own clock and this needs them on demand.
                auto draw = [&] {
                    a.lifeStartScore  = float(a.mined + a.placed);
                    a.scriptStartRung = a.rung;
                    a.scriptStartTier = a.tier;
                    a.scriptStartPot  = float(t3->tech_potential(0));
                    a.scriptStartPlaced = a.placed;
                    a.epTicks = 0;
                };
                auto clear = [&] {
                    a.blocks.fill(0); a.items.fill(0); a.durability.fill(0);
                    a.rung = 0; a.tier = 0; a.mined = 0; a.placed = 0;
                };
                // A closed loop in the PACK, at a rung it does not move.
                clear(); a.rung = 5; draw();
                double loop = 0.0, still = 0.0;
                for (int c = 0; c < 6; ++c) {
                    a.epTicks = kSaturatingWindow;
                    a.blocks[bench::Cobble] += 8;
                    loop += t3->life_fitness_for_test(0, false); draw();
                    a.epTicks = kSaturatingWindow;
                    a.blocks[bench::Cobble] -= 8;
                    loop += t3->life_fitness_for_test(0, false); draw();
                }
                clear(); a.rung = 5; draw();
                for (int c = 0; c < 12; ++c) {
                    a.epTicks = kSaturatingWindow;
                    still += t3->life_fitness_for_test(0, false); draw();
                }
                check(std::fabs(loop - still) < kEps,
                      "voxelcity fitness: gathering eight cobble and putting them "
                      "back pays exactly what standing still pays, six times over "
                      "(" + std::to_string(loop) + " vs " + std::to_string(still) +
                      ")");
                // A closed loop in the TOOL TIER, which really does fall in
                // play: wearTool() breaks the pickaxe and recomputeTier()
                // lowers the tier behind it.
                clear(); draw();
                loop = still = 0.0;
                for (int c = 0; c < 6; ++c) {
                    a.epTicks = kSaturatingWindow;
                    a.items[bench::ItWoodPick] = 1; a.tier = 1;
                    loop += t3->life_fitness_for_test(0, false); draw();
                    a.epTicks = kSaturatingWindow;
                    a.items[bench::ItWoodPick] = 0; a.tier = 0;
                    loop += t3->life_fitness_for_test(0, false); draw();
                }
                clear(); draw();
                for (int c = 0; c < 12; ++c) {
                    a.epTicks = kSaturatingWindow;
                    still += t3->life_fitness_for_test(0, false); draw();
                }
                check(std::fabs(loop - still) < kEps,
                      "and making a wood pickaxe and then breaking it pays "
                      "exactly what standing still pays, tier 0->1->0 six times "
                      "(" + std::to_string(loop) + " vs " + std::to_string(still) +
                      ")");
                // The TERMINAL boundary. resetAgent() destroys the pack, the
                // rung and the tier on the tick after death, so a fitness that
                // credits any of them at death is paying for state that does
                // not survive the episode it is scoring.
                clear(); draw();
                loop = still = 0.0;
                for (int L = 0; L < 10; ++L) {
                    a.epTicks = kSaturatingWindow;
                    a.blocks[bench::Wood] = 1;
                    a.items[bench::ItDiamondPick] = 1; a.tier = 4;
                    loop += t3->life_fitness_for_test(0, true);
                    clear(); draw();
                }
                for (int L = 0; L < 10; ++L) {
                    a.epTicks = kSaturatingWindow;
                    still += t3->life_fitness_for_test(0, true);
                    clear(); draw();
                }
                check(std::fabs(loop - still) < kEps,
                      "and dying while holding a diamond pickaxe and a wood block "
                      "pays exactly what dying with nothing pays, ten lives of "
                      "each (" + std::to_string(loop) + " vs " +
                      std::to_string(still) + ")");
            }
        }

        // ── every size knob resizes, and threading it changes nothing ───────
        //
        // Generic, over the whole roster, because a dozen sims grew a size knob
        // at once and one-off blocks were written for three of them. The two
        // properties are the ones that actually go wrong: reset() only refills
        // the cells a field already has, so a knob that forgets to rebuild the
        // buffers is a no-op that looks like it works; and a threaded sweep is
        // only a speedup if it computes the same thing.
        //
        // The notch is chosen by CELL COUNT, not by index — the largest one at
        // or under about a million cells, so the test lands above the 250,000
        // threading threshold (where a split really happens) without stepping a
        // sixteen-million-cell world in a suite that has to stay quick.
        {
            const unsigned wasW = bench::max_workers();
            for (auto& e : bench::registry()) {
                auto probe = e.make();
                int notches = 0;
                for (const auto& k : probe->knobs())
                    if (k.key == "size" && !k.choices.empty()) notches = int(k.choices.size());
                if (notches < 2) continue;

                auto build = [&](int notch) {
                    auto s = e.make();
                    float target = 0.f;
                    for (auto& k : s->knobs())
                        if (k.key == "size") { target = k.min + float(notch); k.value = target; }
                    // ONE call, with the value being set. An earlier version
                    // poked on_knob with 0 first "to let the sim react" — but
                    // every on_knob writes kn.value = v, so that reset the knob
                    // to its first notch and every size came out identical.
                    // Nineteen sims failing the same way was the tell: that is
                    // a broken instrument, not nineteen broken knobs.
                    s->on_knob("size", target);
                    s->reset();
                    return s;
                };
                std::vector<std::pair<int,int>> dims;
                int pick = 0; std::size_t pickCells = 0;
                for (int n = 0; n < notches; ++n) {
                    auto s = build(n);
                    const std::size_t cells = std::size_t(s->field().w) * s->field().h;
                    dims.emplace_back(s->field().w, s->field().h);
                    if (cells <= 1100000 && cells > pickCells) { pickCells = cells; pick = n; }
                }
                bool varies = false;
                for (const auto& d : dims) if (d != dims.front()) varies = true;
                check(varies, e.id + ": the size knob really rebuilds the field (" +
                      std::to_string(dims.front().first) + "x" + std::to_string(dims.front().second) +
                      " .. " + std::to_string(dims.back().first) + "x" +
                      std::to_string(dims.back().second) + ")");

                // Identity. Only worth asserting where a split can actually
                // occur; below the threshold both runs take the same serial
                // path and the check would pass for the wrong reason.
                if (pickCells < bench::GridSim::kParallelCells) continue;
                bench::set_max_workers(1);
                auto a = build(pick);
                for (int i = 0; i < 4; ++i) a->step();
                bench::set_max_workers(0);
                auto b = build(pick);
                for (int i = 0; i < 4; ++i) b->step();
                bench::set_max_workers(wasW);
                check(a->field().cells == b->field().cells,
                      e.id + ": and at " + std::to_string(a->field().w) + "x" +
                      std::to_string(a->field().h) +
                      " a threaded sweep gives the identical field, cell for cell");
            }
            bench::set_max_workers(wasW);
        }

        // ── the size knob must NAME the world it is sitting on ──────────────
        //
        // Every sim in the roster, not just the ones changed here. A knob whose
        // default was typed in rather than derived from the constructed size
        // reads "512" over a 256-wide field: it does the right thing the moment
        // you touch it, and lies about where it started, which is why four sims
        // shipped this way without anyone noticing.
        //
        // Only knobs whose label is plainly a size are judged — "1x" is a
        // multiplier and "40^3" describes a volume behind a rendered surface,
        // and neither claims to be the published field's dimensions.
        for (auto& e : bench::registry()) {
            auto s = e.make();
            if (s->surface()) continue;          // field is a picture, not the world
            for (const auto& k : s->knobs()) {
                if (k.key != "size" || k.choices.empty()) continue;
                const std::string shown = k.shown();
                if (shown.empty() || !std::isdigit(static_cast<unsigned char>(shown[0]))) continue;
                const auto x = shown.find('x');
                bool plain = true;
                for (std::size_t i = 0; i < shown.size(); ++i)
                    if (!std::isdigit(static_cast<unsigned char>(shown[i])) && i != x) plain = false;
                // "1x" is a MULTIPLIER, not a size, and it passes every test
                // above: one digit, one x, nothing else. The tell is that a
                // size has digits on both sides of the x. Without this the
                // assertion fails three working knobs, which would have made
                // it a test that had to be argued with rather than believed.
                if (x != std::string::npos && x + 1 >= shown.size()) plain = false;
                if (!plain) continue;
                const std::string want = (x == std::string::npos)
                    ? std::to_string(s->field().w) + "x" + std::to_string(s->field().w)
                    : std::to_string(s->field().w) + "x" + std::to_string(s->field().h);
                const std::string got = (x == std::string::npos) ? shown + "x" + shown : shown;
                check(got == want, e.id + ": the size knob names the world it is on (says " +
                                   got + ", field is " + want + ")");
            }
        }

        // Shape, which exists because a square world cannot fill a wide window
        // however much room it is given. The knob has to change the FIELD, not
        // just the view — a world that stays square while the label says 16:9
        // would be the inert control this bench keeps finding elsewhere.
        {
            auto wide = bench::make_life();
            wide->on_knob("size", 2.f);      // 1024 across
            wide->on_knob("shape", 1.f);     // 16:9
            wide->reset();
            check(wide->field().w == 1024 && wide->field().h == 576,
                  "the shape knob makes a 16:9 world (" +
                  std::to_string(wide->field().w) + "x" +
                  std::to_string(wide->field().h) + ")");
            // Fewer cells than the square of the same width, which is why wide
            // is cheaper per step and not merely differently shaped.
            check(wide->field().cells.size() < 1024u * 1024u,
                  "and it is cheaper than the square of the same width (" +
                  std::to_string(wide->field().cells.size()) + " cells against 1048576)");
            bench::set_max_workers(1);
            auto ws = bench::make_life();
            ws->on_knob("size", 2.f); ws->on_knob("shape", 1.f); ws->reset();
            for (int i = 0; i < 8; ++i) ws->step();
            bench::set_max_workers(0);
            auto wt = bench::make_life();
            wt->on_knob("size", 2.f); wt->on_knob("shape", 1.f); wt->reset();
            for (int i = 0; i < 8; ++i) wt->step();
            check(ws->field().cells == wt->field().cells,
                  "and a non-square world threads to the identical field too");
        }

        // The template ships a size knob, because the first thing anyone does
        // with my_sim.hpp is copy it — a knob that is missing from the template
        // is a knob that is missing from every sim written afterwards.
        {
            auto tmpl = bench::make_my_sim();
            tmpl->on_knob("size", 2.f);
            tmpl->reset();
            check(tmpl->field().w == 1024 && tmpl->field().h == 1024,
                  "the template's size knob really rebuilds the field (" +
                  std::to_string(tmpl->field().w) + " across)");
            bench::set_max_workers(1);
            auto ts = bench::make_my_sim(); ts->on_knob("size", 2.f); ts->reset();
            for (int i = 0; i < 6; ++i) ts->step();
            bench::set_max_workers(0);
            auto tt = bench::make_my_sim(); tt->on_knob("size", 2.f); tt->reset();
            for (int i = 0; i < 6; ++i) tt->step();
            check(ts->field().cells == tt->field().cells,
                  "and the template steps to the identical field on one core or many");
        }
        bench::set_max_workers(was);

        // Gridworld resizes a maze, not a lattice, and the thing that has to
        // follow it is the Q-table: it is indexed by state, so a world that
        // grows without reset() reassigning q_ reads off the end of it.
        {
            auto gw = bench::make_gridworld();
            gw->on_knob("size", 2.f);
            check(gw->field().w == 81 && gw->field().h == 57,
                  "gridworld's size knob rebuilds the maze (" +
                  std::to_string(gw->field().w) + "x" +
                  std::to_string(gw->field().h) + ")");
            gw->on_knob("speed", 400.f);
            for (int i = 0; i < 3000; ++i) gw->step();
            // If the Q-table had not been resized with the world this would be
            // a read past the end rather than a wrong answer, so what is being
            // asserted is mostly that it survives at all — plus that the agent
            // still finds the goal, which it cannot do if the maze came out
            // malformed at the new size.
            const auto sub = gw->subtitle();
            check(sub.find("best 0 steps") == std::string::npos,
                  "and the agent still reaches the goal in the bigger maze");
        }
    }

    // ── choice knobs are labelled with the choice they are on ───────────────
    //
    // Knob::shown() indexed `choices` with the raw value and never subtracted
    // min, so a choice knob that did not start at zero was mislabelled at every
    // position — the renderer sitting at 1x while the panel said "2x", and the
    // last position falling off the end of the list to print a bare number
    // instead of a name.
    //
    // Exactly one knob in the roster has a non-zero min, which is precisely why
    // this survived: a bug that needs an unusual declaration to show itself is
    // invisible to anyone checking the common case.
    {
        group("choice knobs label the position they are on");
        int checked = 0, wrong = 0, offset = 0;
        for (const auto& e : bench::registry()) {
            auto sim = e.make();
            for (const auto& k : sim->knobs()) {
                if (k.choices.empty()) continue;
                if (std::fabs(k.min) > 1e-6f) ++offset;
                for (std::size_t c = 0; c < k.choices.size(); ++c) {
                    bench::Knob probe = k;
                    probe.value = k.min + float(c) * (k.step > 0.0f ? k.step : 1.0f);
                    if (probe.value > k.max + 1e-6f) continue;
                    ++checked;
                    if (probe.shown() != k.choices[c]) ++wrong;
                }
            }
        }
        check(checked > 40, "there are choice knobs to check (" + std::to_string(checked) + ")");
        check(wrong == 0, "every position shows its own choice (" +
                          std::to_string(wrong) + " mislabelled)");
        check(offset > 0, "  including a knob that does not start at zero, which is the "
                          "declaration that exposed the bug");
    }

    // ── invariants every sim must hold, whatever it simulates ───────────────
    //
    // Cheap, roster-wide, and none of it was checked. A field cell that indexes
    // past the palette reads whatever is next in memory and renders as a colour
    // that means nothing; a metric that goes non-finite poisons its plot's
    // autoscale and takes every other series on the panel down with it, because
    // the axis becomes NaN.
    {
        group("roster-wide invariants");
        int badField = 0, badMetric = 0, badSurface = 0;
        for (const auto& e : bench::registry()) {
            auto s = e.make();
            for (int i = 0; i < 120; ++i) s->step();
            const std::size_t pal = s->palette().size();
            for (auto c : s->field().cells) if (c >= pal) { ++badField; break; }
            for (const auto& m : s->metrics()) {
                if (m.name.empty() || !std::isfinite(m.value)) { ++badMetric; continue; }
                // A declared max is a promise the plot's axis relies on.
                if (m.max > 0.0 && (m.value < -1e-9 || m.value > m.max * 1.0001)) ++badMetric;
            }
            if (const bench::Surface* sf = s->surface())
                if (sf->rgba.size() != std::size_t(sf->w) * std::size_t(sf->h) * 4) ++badSurface;
        }
        check(badField == 0, "every field cell indexes a real palette entry (" +
                             std::to_string(badField) + " sims fail)");
        check(badMetric == 0, "every metric is named, finite, and inside the max it declares (" +
                              std::to_string(badMetric) + " fail)");
        check(badSurface == 0, "every rendered surface is the size it says it is (" +
                               std::to_string(badSurface) + " fail)");
    }

    // ── the hex planet ──────────────────────────────────────────────────────
    //
    // A mesh that is subtly wrong renders perfectly well, so this proves the
    // geometry rather than looking at it. Euler's formula is the check that
    // catches a bad deduplication, a missing shared edge, or a face wound the
    // wrong way — all of which produce a picture that looks like a planet.
    {
        group("hex planet: twelve pentagons, and no way around it");
        // EVERY subdivision the knob offers, not five hand-picked ones.
        //
        // The old list was 1, 2, 4, 8, 16 — and the mesh was degenerate at
        // n = 20, 25 and 40, all inside the knob's own 4..40 range: extra
        // faces, three-cornered faces, and Euler coming out at 10 instead of 2.
        // Every sampled value passed, so the geometry was called proven while
        // three settings a user could select built a broken planet. Sampling a
        // parameter is not testing it when the parameter is an integer with
        // forty values.
        for (int n = 1; n <= 40; ++n) {
            const auto P = bench::goldberg::build(n);
            const std::string at = "n=" + std::to_string(n) + ": ";
            // Quiet unless something is wrong: forty subdivisions times five
            // checks is two hundred lines of "ok" nobody reads.
            const bool loud = (n == 1 || n == 16 || n == 20 || n == 40);
            int weird = 0;
            for (const auto& f : P.face)
                if (f.corners.size() != 5 && f.corners.size() != 6) ++weird;
            const bool clean = int(P.face.size()) == 10 * n * n + 2 &&
                               P.pentagons() == 12 && P.euler() == 2 && weird == 0;
            if (!loud && clean) continue;
            check(weird == 0, at + "every face is a pentagon or a hexagon (" +
                              std::to_string(weird) + " are neither)");
            // You cannot tile a sphere with hexagons. For a convex polyhedron
            // of hexagons and pentagons with three faces at every vertex,
            // Euler forces exactly twelve pentagons however large it gets —
            // subdividing buys hexagons and never removes a pentagon.
            check(P.pentagons() == 12, at + "exactly twelve pentagons (" +
                                            std::to_string(P.pentagons()) + ")");
            check(P.face.size() == std::size_t(10 * n * n + 2),
                  at + "faces = 10n^2 + 2 (" + std::to_string(P.face.size()) + ")");
            check(P.euler() == 2, at + "V - E + F = 2 (" + std::to_string(P.euler()) + ")");

            bool mutualOk = true, degreeOk = true;
            for (std::size_t f = 0; f < P.face.size(); ++f) {
                if (P.face[f].neigh.size() != P.face[f].corners.size()) degreeOk = false;
                for (int g : P.face[f].neigh) {
                    bool back = false;
                    for (int h : P.face[std::size_t(g)].neigh) if (h == int(f)) back = true;
                    if (!back) mutualOk = false;
                }
            }
            check(degreeOk, at + "  every face has as many neighbours as corners");
            check(mutualOk, at + "  and adjacency is mutual");
        }
        // The whole surface, and nothing but the surface. Flat polygons under-
        // count a sphere slightly; anything further off means faces overlap or
        // leave gaps, which no picture would show.
        const auto P = bench::goldberg::build(16);
        double total = 0;
        for (const auto& f : P.face) total += double(f.area);
        const double sphere = 4.0 * 3.14159265358979;
        check(total > sphere * 0.99 && total <= sphere,
              "the faces cover the sphere and do not overlap (" +
              std::to_string(100.0 * total / sphere) + "% of 4pi)");

        // The distortion claim, measured. It does not vanish — it converges,
        // which is the useful property and a different statement.
        // The 62-region decomposition: 12 pentagons + 30 edge seams + 20 face
        // interiors, which is the icosahedron's own V, E and F. The field was
        // documented as carrying this and was never filled in; when it was, the
        // seams collapsed to fifteen because the plane through an edge also
        // passes through its antipode. Counting the distinct ids is what says
        // both bugs are gone.
        for (int n : {8, 18, 30}) {
            const auto R = bench::goldberg::build(n);
            std::set<int> ids;
            int pents = 0;
            for (const auto& f : R.face) {
                ids.insert(f.region);
                if (f.region < 12) ++pents;
            }
            check(ids.size() == 62, "n=" + std::to_string(n) +
                  ": the decomposition uses all 62 regions (" +
                  std::to_string(ids.size()) + ")");
            check(pents == 12, "  with the twelve pentagons holding regions 0..11");
        }

        const auto small = bench::goldberg::build(8);
        auto hexSpread = [](const bench::HexPlanet& p) {
            float lo = 1e9f, hi = 0.0f;
            for (const auto& f : p.face)
                if (f.corners.size() == 6) { lo = std::min(lo, f.area); hi = std::max(hi, f.area); }
            return hi / lo;
        };
        check(hexSpread(small) < 2.0f && hexSpread(P) < 2.0f,
              "hexagon area spread stays under 2:1 at every size tested");
        check(hexSpread(P) > hexSpread(small),
              "  it grows with subdivision rather than vanishing, and converges");
    }

    // ── the Life Engine ─────────────────────────────────────────────────────
    {
        group("Life Engine: the world holds together");
        auto sim = bench::make_lifeengine();
        auto* L = static_cast<bench::LifeEngine*>(sim.get());
        for (int i = 0; i < 6000; ++i) sim->step();

        // The invariant that catches the bug this sim actually had: a dead
        // organism's slot was recycled before the end-of-tick prune, so its id
        // ended up in the live list twice — acting twice a tick and counted
        // twice. It reported 6736 organisms of four cells each on a grid with
        // 13924 cells in it. A population that does not fit in its own world is
        // the kind of impossible number worth asserting against.
        check(L->owned_tiles() == L->total_cells(),
              "every owned tile belongs to exactly one living organism (" +
              std::to_string(L->owned_tiles()) + " vs " +
              std::to_string(L->total_cells()) + ")");
        check(L->total_cells() <= L->world_area(),
              "and the population fits inside the world it lives in");
        check(L->alive() > 50, "the ancestor's line is thriving after 6000 ticks (" +
                               std::to_string(L->alive()) + " alive)");
        check(L->born() > 1000, "  having reproduced many times over");
    }
    {
        group("Life Engine: the energy budget is survivable");
        // The first version of this sim died without a single birth: a producer
        // emitted roughly one food per 83 ticks against 0.06 burned every tick
        // by a two-cell body — an energy budget wrong by two orders of
        // magnitude. The defaults were swept, and this is the check that says
        // so, plus the check that the sweep found a real edge and not a plateau.
        auto starve = bench::make_lifeengine();
        for (auto& k : starve->knobs()) if (k.key == "upkeep") k.value = 0.05f;
        starve->on_knob("upkeep", 0.05f);
        starve->reset();
        for (int i = 0; i < 6000; ++i) starve->step();
        check(static_cast<bench::LifeEngine*>(starve.get())->alive() == 0,
              "at maximum upkeep nothing can pay for itself and the world empties");

        auto rich = bench::make_lifeengine();
        rich->reset();
        for (int i = 0; i < 6000; ++i) rich->step();
        check(static_cast<bench::LifeEngine*>(rich.get())->alive() > 50,
              "at the shipped defaults it does not");
    }
    {
        group("Life Engine: selection is visible in the anatomy");
        auto sim = bench::make_lifeengine();
        auto* L = static_cast<bench::LifeEngine*>(sim.get());
        double startProducer = 0;
        for (const auto& m : sim->metrics()) if (m.name == "producer share") startProducer = m.value;
        for (int i = 0; i < 16000; ++i) sim->step();
        double endProducer = 0, endKiller = 0;
        for (const auto& m : sim->metrics()) {
            if (m.name == "producer share") endProducer = m.value;
            if (m.name == "killer share")   endKiller   = m.value;
        }
        // The ancestor is half producer by construction. Nothing tells the
        // population to keep that ratio, and it does not: producers are shed
        // and predation appears, neither of which is coded for anywhere.
        check(startProducer > 0.4, "the ancestor is half producer by construction");
        check(endProducer < startProducer - 0.05,
              "producers are shed as the population evolves (" +
              std::to_string(endProducer) + ")");
        check(endKiller > 0.01,
              "and predation appears from nothing, which is coded for nowhere (" +
              std::to_string(endKiller) + ")");
        check(L->alive() > 0, "  with the world still alive to show it");
    }

    // ── the genetic algorithm's three decisions ─────────────────────────────
    {
        group("benchmark functions have the optimum they claim");
        for (auto b : {bench::Bench::Sphere, bench::Bench::Rastrigin,
                       bench::Bench::Griewank, bench::Bench::Schwefel}) {
            std::vector<double> x(6, bench::bench_optimum_at(b));
            // Schwefel's published constant 418.9829 is itself rounded, so its
            // optimum evaluates to 7.6e-5 rather than to 0. That is the
            // literature's number, not an error here, and the tolerance says so.
            const double tol = (b == bench::Bench::Schwefel) ? 1e-3 : 1e-12;
            check(std::fabs(bench::bench_cost(b, x)) < tol,
                  std::string(bench::name_of(b)) + ": cost at the published optimum is zero");
        }
        std::vector<double> ones(6, 1.0);
        check(std::fabs(bench::bench_cost(bench::Bench::Rosenbrock, ones)) < 1e-12,
              "rosenbrock: cost at all-ones is zero");
        std::vector<double> off(6, 0.0);
        check(bench::bench_cost(bench::Bench::Rosenbrock, off) > 1.0,
              "  and is not zero away from it, so the check can fail");
    }
    {
        group("fitness scaling");
        const std::vector<double> f{1.0, 2.0, 3.0, 10.0};
        std::vector<double> shifted;
        for (double v : f) shifted.push_back(v * 100.0 + 5000.0);

        // The pathology roulette exists to have, measured. An affine shift
        // changes nothing about which genome is best, and collapses the best
        // one's share of the wheel toward uniform.
        auto share = [](const std::vector<double>& w) {
            double t = 0; for (double v : w) t += v;
            return w.back() / t;
        };
        const double rawA = share(bench::scale_fitness(f, bench::Scaling::Raw));
        const double rawB = share(bench::scale_fitness(shifted, bench::Scaling::Raw));
        check(rawA > 0.6 && rawB < 0.3,
              "raw scaling: an affine shift collapses the best genome's share of the wheel");
        check(bench::scale_fitness(f, bench::Scaling::Rank) ==
              bench::scale_fitness(shifted, bench::Scaling::Rank),
              "rank scaling is invariant to it, which is the entire point of rank");

        for (auto sc : {bench::Scaling::Raw, bench::Scaling::Rank, bench::Scaling::Sigma,
                        bench::Scaling::Linear, bench::Scaling::Boltzmann}) {
            const std::vector<double> neg{-5.0, -1.0, 0.0, 4.0};
            const auto w = bench::scale_fitness(neg, sc);
            bool ok = true;
            for (double v : w) if (!(v >= 0.0) || !std::isfinite(v)) ok = false;
            check(ok, std::string(bench::name_of(sc)) +
                      ": weights are finite and non-negative even from negative fitness");
        }
        // Boltzmann must not overflow. exp(700) is inf and a fitness of a few
        // thousand is ordinary, so the max is subtracted before exponentiating.
        const std::vector<double> big{0.0, 5000.0};
        const auto bw = bench::scale_fitness(big, bench::Scaling::Boltzmann, 1.0);
        check(std::isfinite(bw[0]) && std::isfinite(bw[1]) && bw[1] > bw[0],
              "boltzmann survives a fitness of 5000 without overflowing");
    }
    {
        group("selection pressure, in takeover time");
        std::vector<double> ramp(64);
        for (int i = 0; i < 64; ++i) ramp[std::size_t(i)] = double(i);
        bench::Rng rng{99};
        auto t = [&](bench::Selection sel, double par) {
            return bench::takeover_time(ramp, sel, bench::Scaling::Raw, rng, par);
        };
        const auto uni = t(bench::Selection::Uniform, 0);
        check(uni.never() && uni.lost == uni.trials,
              "uniform selection never takes over and loses the best every time");
        const auto k5 = t(bench::Selection::Tournament, 5);
        const auto k3 = t(bench::Selection::Tournament, 3);
        const auto tr = t(bench::Selection::Truncate, 0.2);
        check(!k5.never() && !k3.never() && !tr.never(), "the pressured schemes all take over");
        check(tr.median <= k5.median && k5.median <= k3.median,
              "and rank in the published order: truncate <= tournament 5 <= tournament 3");
        check(k5.lost == 0 && tr.lost == 0,
              "  strong pressure never loses the best to drift");
        check(k3.median < uni.median, "  while no pressure at all never gets there");
    }
    {
        group("crossover");
        bench::Rng rng{7};
        const std::vector<double> a(12, 0.0), b(12, 1.0);
        for (auto cx : {bench::Crossover::OnePoint, bench::Crossover::TwoPoint,
                        bench::Crossover::Uniform}) {
            bool sawBoth = false;
            for (int t = 0; t < 60 && !sawBoth; ++t) {
                const auto c = bench::crossover_vec(a, b, cx, rng);
                bool fromA = false, fromB = false, onlyParents = true;
                for (double v : c) {
                    if (v == 0.0) fromA = true;
                    else if (v == 1.0) fromB = true;
                    else onlyParents = false;
                }
                sawBoth = fromA && fromB && onlyParents;
            }
            check(sawBoth, std::string(bench::name_of(cx)) +
                           ": a child takes genes from both parents and invents none");
        }
        const auto ar = bench::crossover_vec(a, b, bench::Crossover::Arithmetic, rng);
        bool inHull = true;
        for (double v : ar) if (v < -1e-12 || v > 1.0 + 1e-12) inHull = false;
        check(inHull, "arithmetic: the child cannot leave the parents' convex hull");
        check(bench::crossover_vec(a, b, bench::Crossover::None, rng) == a,
              "none: the child is the first parent, unchanged");
    }
    {
        group("the GA solves what it should, and struggles where it should");
        bench::GA ga; bench::GA::Params p;
        p.problem = bench::Bench::Sphere;
        ga.init(p, 12345);
        for (int g = 0; g < 200; ++g) ga.step();
        // Sphere is convex and separable. A GA that cannot solve it is broken,
        // so this is the floor every other result rests on.
        check(ga.best_cost() < 1e-3, "sphere is solved to better than 1e-3");

        bench::GA hard; bench::GA::Params q; q.problem = bench::Bench::Rastrigin;
        hard.init(q, 12345);
        for (int g = 0; g < 200; ++g) hard.step();
        check(hard.best_cost() > ga.best_cost(),
              "and rastrigin, with its 10^n local minima, is not");

        // The population controller. Growing must not lose the best, and
        // shrinking must keep it — dropping at random would discard the
        // champion a fifth of the time.
        const double was = ga.best_fitness();
        ga.resize(200);
        check(ga.size() == 200 && ga.best_fitness() >= was - 1e-12,
              "growing the population keeps the best genome");
        ga.resize(20);
        check(ga.size() == 20 && ga.best_fitness() >= was - 1e-12,
              "and shrinking keeps it too");

        const double bestBefore = ga.best_fitness(), meanBefore = ga.mean_fitness();
        ga.immigrate(10);
        check(ga.best_fitness() >= bestBefore - 1e-12,
              "immigration replaces the WORST, so the best survives it");
        check(ga.mean_fitness() < meanBefore,
              "  while the population mean falls, which is what makes it a search again");
    }
    {
        group("NEAT: the complexity cap holds");
        // Refusing the structural mutation is not enough on its own. Crossover
        // of two parents that are each AT the cap but carry different hidden
        // nodes produces a child with both — measured, a cap of 1 let 3 through
        // — so the cap is enforced on the finished child as well.
        for (int cap : {1, 2, 3}) {
            bench::Neat n;
            n.init(2, 1, 60, 0xC0FFEEull + std::uint64_t(cap));
            n.params().maxHiddenNodes = cap;
            n.params().addNode = 0.9f;          // push hard against the cap
            n.params().addConn = 0.9f;
            int worst = 0;
            for (int g = 0; g < 40; ++g) {
                for (int i = 0; i < n.size(); ++i) n.set_fitness(i, float(i));
                n.evolve();
                for (int i = 0; i < n.size(); ++i)
                    worst = std::max(worst, int(n.genome(i).hidden_nodes()));
            }
            check(worst <= cap, "cap " + std::to_string(cap) +
                                ": no genome ever exceeds it (worst seen " +
                                std::to_string(worst) + ")");
            check(worst == cap, "  and it is actually reached, so the check can fail");
        }
        // The CONNECTION cap got the same treatment and was never tested — which
        // is exactly how the node cap's crossover leak survived. A bound with
        // no test is a bound you are assuming holds.
        for (int cap : {6, 10, 16}) {
            bench::Neat n;
            n.init(2, 1, 60, 0xC0FFEEull + std::uint64_t(cap));
            n.params().maxConnections = cap;
            n.params().addNode = 0.9f;
            n.params().addConn = 0.9f;
            int worst = 0;
            for (int g = 0; g < 40; ++g) {
                for (int i = 0; i < n.size(); ++i) n.set_fitness(i, float(i));
                n.evolve();
                for (int i = 0; i < n.size(); ++i)
                    worst = std::max(worst, int(n.genome(i).conns.size()));
            }
            check(worst <= cap, "connections capped at " + std::to_string(cap) +
                                ": no genome exceeds it (worst seen " +
                                std::to_string(worst) + ")");
            check(worst >= cap - 1, "  and it is reached, so the check can fail");
        }

        bench::Neat freeRun;
        freeRun.init(2, 1, 60, 0xC0FFEEull);
        freeRun.params().addNode = 0.9f;
        for (int g = 0; g < 40; ++g) {
            for (int i = 0; i < freeRun.size(); ++i) freeRun.set_fitness(i, float(i));
            freeRun.evolve();
        }
        check(int(freeRun.best().hidden_nodes()) > 3,
              "uncapped, the same run complexifies well past 3 hidden nodes");
        int freeConns = 0;
        for (int i = 0; i < freeRun.size(); ++i)
            freeConns = std::max(freeConns, int(freeRun.genome(i).conns.size()));
        check(freeConns > 16, "  and well past 16 connections (" +
                              std::to_string(freeConns) + ")");
    }
    {
        group("NEAT: species stagnation, when it is switched on");
        // The parameter existed and was read by nothing — species stagnation is
        // one of NEAT's three mechanisms and the code did not implement it, so
        // staleLimit was a claim the algorithm did not back up. It works now,
        // and ships OFF because it measured worse on XOR; that makes a test
        // more important, not less, since nothing else exercises it.
        bench::Neat n;
        n.init(2, 1, 60, 0xDEADull);
        n.params().staleLimit = 3;
        n.params().targetSpecies = 8;          // force several species to exist
        int everFroze = 0;
        for (int g = 0; g < 40; ++g) {
            // Flat fitness: nothing can improve, so every species is stale by
            // construction after three generations.
            for (int i = 0; i < n.size(); ++i) n.set_fitness(i, 1.0f);
            n.evolve();
            everFroze = std::max(everFroze, n.stale_species());
        }
        check(everFroze > 0, "with fitness held flat, species are frozen out for stagnation (" +
                             std::to_string(everFroze) + ")");
        check(n.size() == 60, "  and the population is still full afterwards");
        check(int(n.species_count()) > 0, "  with at least one species left to breed from");

        bench::Neat off;
        off.init(2, 1, 60, 0xDEADull);
        off.params().staleLimit = 0;
        off.params().targetSpecies = 8;
        int offFroze = 0;
        for (int g = 0; g < 40; ++g) {
            for (int i = 0; i < off.size(); ++i) off.set_fitness(i, 1.0f);
            off.evolve();
            offFroze = std::max(offFroze, off.stale_species());
        }
        check(offFroze == 0, "and at staleLimit 0 nothing is ever frozen, which is the default");
    }
    {
        group("NEAT: the population resizes without restarting");
        bench::Neat n;
        n.init(2, 1, 40, 0xBEEFull);
        for (int i = 0; i < n.size(); ++i) n.set_fitness(i, float(i));
        n.evolve();
        for (int i = 0; i < n.size(); ++i) n.set_fitness(i, float(i));
        const int gen = n.generation();
        check(n.resize(120) && n.size() == 120, "growing to 120 works");
        check(n.generation() == gen, "  without restarting the run");
        for (int i = 0; i < n.size(); ++i) n.set_fitness(i, float(i));
        check(n.resize(10) && n.size() == 10, "and shrinking to 10 works");
        // Growth clones existing genomes rather than adding fresh minimal ones:
        // a zero-hidden genome inserted late is a handicap, not a sample.
        bench::Neat g2;
        g2.init(2, 1, 20, 0xBEEFull);
        g2.params().addNode = 0.9f;
        for (int r = 0; r < 12; ++r) {
            for (int i = 0; i < g2.size(); ++i) g2.set_fitness(i, float(i));
            g2.evolve();
        }
        for (int i = 0; i < g2.size(); ++i) g2.set_fitness(i, float(i));
        g2.resize(60);
        int bare = 0;
        for (int i = 0; i < g2.size(); ++i) if (g2.genome(i).hidden_nodes() == 0) ++bare;
        check(bare < g2.size() / 2,
              "the added genomes are clones of the population, not fresh minimal ones");
    }
    {
        group("parallel work does not change the answer");
        // Coverage: every index exactly once, whatever the worker count.
        for (unsigned w : {1u, 3u, 8u}) {
            std::vector<int> hits(1000, 0);
            bench::parallel_for(1000, [&](std::size_t i) { hits[i] += 1; }, w);
            bool once = true;
            for (int v : hits) if (v != 1) once = false;
            check(once, std::to_string(w) + " workers touch every index exactly once");
        }
        // Order: results by index, never by completion.
        const auto m1 = bench::parallel_map<int>(500, [](std::size_t i) { return int(i * 3); }, 1);
        const auto m8 = bench::parallel_map<int>(500, [](std::size_t i) { return int(i * 3); }, 8);
        check(m1 == m8, "parallel_map returns results by index, not by completion order");

        // The renderer, byte for byte. A frame that differs on eight threads is
        // not faster, it is broken on someone else's machine.
        const int n = 24;
        std::vector<std::uint8_t> vol(std::size_t(n) * n * n, 0);
        for (int z = 0; z < n; ++z) for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
            const float dx = float(x) - 12, dy = float(y) - 12, dz = float(z) - 12;
            vol[std::size_t((z * n + y) * n + x)] =
                (std::sqrt(dx*dx + dy*dy + dz*dz) < 9.5f) ? 1 : 0;
        }
        auto live = [&](int x, int y, int z) { return vol[std::size_t((z*n+y)*n+x)] != 0; };
        bench::Camera cam; cam.distance = 3.0f; cam.yaw = 0.6f; cam.pitch = 0.4f;
        auto frame = [&](unsigned threads, int ss) {
            bench::VoxelRenderer r;
            r.resize(240, 180); r.set_supersample(ss); r.set_threads(threads);
            r.render(n, live, cam, bench::Rgb{200,200,210}, bench::Rgb{12,14,20});
            return r.surface().rgba;
        };
        check(frame(1, 1) == frame(8, 1), "the voxel frame is identical on 1 and 8 threads");
        // The 2D path too, on both of its sampling branches — they are separate
        // loops with separate splits, so one being right says nothing about
        // the other.
        {
            bench::Field fld(600, 600);
            bench::Rng r2{7};
            for (auto& c : fld.cells) c = std::uint8_t(r2.unit() < 0.35f ? 1 : 0);
            const std::vector<bench::Swatch> pal2{{{8,10,14},"dead"},{{110,227,192},"alive"}};
            auto flat = [&](unsigned threads, float zoom) {
                bench::Raster r; r.resize(500, 400); r.set_threads(threads);
                bench::View v; v.trail = 0.85f; v.gamma = 0.8f; v.zoom = zoom;
                r.draw(fld, pal2, v);
                return std::vector<std::uint8_t>(r.pixels(),
                                                 r.pixels() + std::size_t(500) * 400 * 4);
            };
            check(flat(1, 1.0f) == flat(8, 1.0f),
                  "the 2D frame is identical on 1 and 8 threads, zoomed out (area average)");
            check(flat(1, 6.0f) == flat(8, 6.0f),
                  "  and zoomed in (nearest neighbour)");
            check(flat(1, 1.0f) != flat(1, 6.0f),
                  "  while the two branches really do differ, so the check can fail");
        }
        check(frame(1, 2) == frame(8, 2), "  and identical again when supersampled");
        check(frame(1, 1) != frame(1, 2),
              "  while supersampling really does change the image, so the check can fail");
    }

    // ── every learning sim can be run more than once ────────────────────────
    //
    // Six of the eight could not. Their seeds were fixed literals, so every run
    // was byte-identical and "does this result hold, or was it one lucky draw"
    // had no answer — which is the question that decided every measurement in
    // this project. A seed knob that does not change the run is worse than
    // none, so this asserts both halves: different seeds diverge, and the same
    // seed reproduces exactly.
    {
        group("seeds: independent runs, reproducibly");
        // Compared by trajectory, not by end metrics. Four epochs of the
        // continual sim leaves accuracy at exactly 1.0 and 0.5 whatever the
        // seed — task B has not started yet — and neuralq reports 0 until the
        // agent first reaches the goal. Both looked identical while being
        // completely different runs. Sixth time in this project that the
        // instrument, not the code, was the thing that was wrong.
        auto run_seeded = [](const bench::Entry& e, float seed) {
            auto sim = e.make();
            for (auto& k : sim->knobs()) if (k.key == "seed") k.value = seed;
            sim->on_knob("seed", seed);
            sim->reset();
            // A sim without an epoch is measured in frames; trajectory_epochs
            // advances nothing for it and would compare two identical initial
            // states, which passes for the wrong reason.
            if (sim->epoch_name()) return trajectory_epochs(*sim, 4);
            return trajectory(*sim, 12);
        };
        // EVERY sim with a seed knob, not only the learning ones.
        //
        // This used to skip anything without an epoch, and the hex planet's
        // seed bug walked straight through the gap: it re-seeded its RNG on
        // reset and then generated terrain from a separate noise offset, so
        // every seed produced the identical world. A seed knob that does not
        // change the run is worse than no seed knob, whether or not the sim
        // happens to learn anything.
        int withSeed = 0;
        for (const auto& e : bench::registry()) {
            auto probe = e.make();
            bool has = false;
            for (auto& k : probe->knobs()) if (k.key == "seed") has = true;
            if (probe->epoch_name())
                check(has, e.id + ": a learning sim offers a run seed");
            if (!has) continue;
            // Name the sim on every line. Without this the non-learning sims —
            // which have no "offers a run seed" line above them — produced ten
            // unattributed pairs of "seed 1 twice gives the identical run", and
            // a test report you cannot attribute is a test report you cannot
            // act on.
            ++withSeed;
            const std::uint64_t a1 = run_seeded(e, 1.0f);
            const std::uint64_t a2 = run_seeded(e, 1.0f);
            const std::uint64_t b1 = run_seeded(e, 7.0f);
            check(a1 == a2, "  " + e.id + ": seed 1 twice gives the identical run");
            check(a1 != b1, "  " + e.id + ": and seed 7 gives a different one");
        }
        check(withSeed >= 8, "every sim that offers a seed can be re-run (" +
                             std::to_string(withSeed) + " of them)");
    }

    // ── stopping when it stops improving ────────────────────────────────────
    //
    // The detector fires on the EDGE — the sample where it has been stale for
    // the limit — not on every sample after. Firing repeatedly would be
    // harmless where it is used and wrong everywhere else, and the difference
    // is invisible from the caller.
    {
        group("plateau detection");
        bench::Plateau p;
        bool firedWhileImproving = false;
        for (int i = 0; i < 20; ++i) if (p.observe(double(i), 5)) firedWhileImproving = true;
        check(!firedWhileImproving, "twenty improving samples never stall");
        // Now hold it flat.
        int firedAt = -1;
        for (int i = 0; i < 12; ++i) if (p.observe(19.0, 5) && firedAt < 0) firedAt = i;
        check(firedAt == 4, "fires on the fifth flat sample with a limit of five (" +
                            std::to_string(firedAt + 1) + ")");
        int again = 0;
        for (int i = 0; i < 12; ++i) if (p.observe(19.0, 5)) ++again;
        check(again == 0, "  and does not fire again while it stays flat");
        // A lower-is-better series improves DOWNWARD. The detector is fed the
        // best-so-far, which is monotone either way, so it needs no direction.
        bench::Plateau q;
        check(!q.observe(500.0, 3) && !q.observe(300.0, 3) && !q.observe(120.0, 3),
              "a falling best counts as improvement, not as a stall");
        check(!q.observe(120.0, 3) && !q.observe(120.0, 3) && q.observe(120.0, 3),
              "  and stalls three samples after it stops falling");
        q.reset();
        check(q.since() == 0 && !q.observe(120.0, 3), "reset forgets the run");
    }

    // ── epochs and metric history ───────────────────────────────────────────
    //
    // A frame is the wrong unit for anything that learns. A sim that claims an
    // epoch must advance exactly one when asked; a sim that has none must say
    // so rather than pretending and doing nothing.
    {
        group("epochs: every learning sim has a real one");
        int withEpoch = 0;
        for (auto& e : bench::registry()) {
            auto sim = e.make();
            const char* unit = sim->epoch_name();
            if (!unit) {
                check(!sim->advance_epoch(),
                      e.id + ": reports no epoch and refuses to advance one");
                continue;
            }
            ++withEpoch;
            const int before = sim->epoch_count();
            const bool ok = sim->advance_epoch();
            check(ok && sim->epoch_count() == before + 1,
                  e.id + ": one " + unit + " advances the counter by exactly one");
        }
        check(withEpoch >= 8,
              "at least the eight learning sims define an epoch (" +
              std::to_string(withEpoch) + ")");
    }

    // ── metric history: best-so-far, recent mean, and CSV ───────────────────
    {
        group("metric history");
        auto sim = bench::make_gridworld(15, 11);
        bench::History h;
        h.configure(4u * 1024u * 1024u, 1);
        for (int i = 0; i < 60; ++i) { sim->step(); h.observe(*sim); }
        const auto& names = h.names();
        check(!names.empty(), "the gridworld's metrics are recorded by name");
        if (!names.empty()) {
            const auto* t = h.trace(names[0]);
            check(t && t->values.size() > 1, "  with more than one sample");
            // Direction-aware, because "best" is not "maximum". This check
            // used to assume it was, and it started failing the moment the
            // gridworld declared that fewer steps to the goal is better —
            // which is the test doing its job on itself.
            const bool up = h.direction(names[0]) != bench::Metric::Lower;
            double manualBest = t->values.front();
            for (double v : t->values)
                manualBest = up ? std::max(manualBest, v) : std::min(manualBest, v);
            check(std::fabs(h.best_of(names[0]) - manualBest) < 1e-12,
                  "  best-so-far matches the best recorded value, in the metric's own direction");
            const double mean = h.mean_of(names[0], 1000), best = h.best_of(names[0]);
            check(up ? (mean <= best + 1e-9) : (mean >= best - 1e-9),
                  "  and a mean can never beat the best");
        }
        const std::string csv = std::string(bench::Paths::get().exeDir()) + "/test-metrics.csv";
        check(h.write_csv(csv), "the series export to CSV");
        { std::ifstream in(csv);
          std::string first; std::getline(in, first);
          check(first.rfind("sample", 0) == 0, "  with a header row naming each series"); }
        std::remove(csv.c_str());
    }

    // ── the epoch axis ──────────────────────────────────────────────────────
    //
    // A learning curve plotted against frames is the inside of one generation.
    // These check that the epoch clock records one point per completed epoch,
    // that it starts at the FIRST one (priming the counter from the first
    // observation silently dropped it), and that a sim with no epoch records
    // nothing on that axis at all.
    {
        group("epoch-indexed traces");
        using Axis = bench::History::Axis;
        auto sim = bench::make_gridworld(15, 11);
        bench::History h;
        h.configure(1u * 1024u * 1024u, 1);
        h.observe(*sim);                                  // epoch 0: nothing done yet
        check(h.epoch_samples() == 0, "no epoch has completed, so nothing is on the epoch axis");
        for (int i = 0; i < 5; ++i) { sim->advance_epoch(); h.observe(*sim); }
        check(h.epoch_samples() == 5, "five episodes record five epoch samples");
        check(h.epoch_index(0) == 1, "  the first sample is episode 1, not episode 2");
        check(h.epoch_index(4) == sim->epoch_count(),
              "  and the last is the episode the sim is actually on");
        const auto& en = h.names(Axis::Epoch);
        check(!en.empty() && h.trace(en[0], Axis::Epoch) &&
              h.trace(en[0], Axis::Epoch)->values.size() == 5,
              "  every series on the epoch axis has one value per episode");
        check(h.names(Axis::Frame).size() == en.size(),
              "  the two axes carry the same series, sampled on different clocks");
        check(h.trace(en[0], Axis::Frame)->values.size() == 6,
              "  while the frame axis counts observations, not episodes");
        // Several epochs completing between two observations must leave a
        // visible gap rather than being renumbered into a straight line.
        for (int i = 0; i < 3; ++i) sim->advance_epoch();
        h.observe(*sim);
        check(h.epoch_samples() == 6 && h.epoch_index(5) == sim->epoch_count(),
              "  three episodes inside one observation record one sample, at the right episode");
        const std::string ecsv = std::string(bench::Paths::get().exeDir()) + "/test-epochs.csv";
        check(h.write_csv(ecsv, Axis::Epoch), "the epoch axis exports to its own CSV");
        { std::ifstream in(ecsv); std::string first, second;
          std::getline(in, first); std::getline(in, second);
          check(first.rfind("epoch", 0) == 0, "  whose first column is named epoch");
          check(second.rfind("1,", 0) == 0, "  and whose first row is episode 1"); }
        std::remove(ecsv.c_str());

        // ── which way is better ────────────────────────────────────────────
        //
        // "best" reported the running MAXIMUM of every series, so the
        // gridworld's best result was its worst episode — 795 steps to the
        // goal, printed as the best number on the panel. The direction lives
        // on the Metric because only the sim knows it.
        {
            auto gw = bench::make_gridworld(15, 11);
            bench::History h3; h3.configure(1u * 1024u * 1024u, 1);
            for (int i = 0; i < 8; ++i) { gw->advance_epoch(); h3.observe(*gw); }
            const auto& en3 = h3.names(Axis::Epoch);
            check(!en3.empty() && en3[0] == "steps to goal (last success)",
                  "the gridworld leads with steps-to-goal-on-success");
            if (!en3.empty()) {
                const auto* t3 = h3.trace(en3[0], Axis::Epoch);
                double lowest = t3->values.front(), highest = t3->values.front();
                for (double v : t3->values) { lowest = std::min(lowest, v); highest = std::max(highest, v); }
                check(h3.direction(en3[0], Axis::Epoch) == bench::Metric::Lower,
                      "  and declares that fewer steps is better");
                check(std::fabs(h3.best_of(en3[0], Axis::Epoch) - lowest) < 1e-12,
                      "  so its best is the FEWEST steps, not the most");
                check(highest > lowest,
                      "  (and the two genuinely differ, so the check can fail)");
            }
            // A baseline is a second run to measure against, and it has to
            // survive the reset that starts that run.
            // COPY the name before clearing.
            //
            // names() returns a reference to the very vector clear() empties,
            // so using en3[0] afterwards is an out-of-bounds read. It passed
            // for weeks on whatever the freed memory happened to hold, and only
            // started failing when an unrelated rename changed the string it
            // was reading past the end of. A test that reads out of bounds is
            // not a weaker test, it is a coin toss.
            const std::string leadName = en3[0];
            h3.keep_baseline();
            const double kept = h3.baseline_best(leadName);
            h3.clear();
            check(h3.has_baseline() && h3.epoch_samples() == 0,
                  "clearing the timeline keeps the baseline — reset is exactly when you want it");
            check(std::fabs(h3.baseline_best(leadName) - kept) < 1e-12,
                  "  with the same values it was frozen at");
            check(h3.baseline_trace("no such series") == nullptr,
                  "  and no baseline for a series that was never recorded");
            h3.clear_baseline();
            check(!h3.has_baseline(), "dropping the baseline drops it");
        }

        auto plain = bench::make_life(32);
        bench::History h2; h2.configure(1u * 1024u * 1024u, 1);
        for (int i = 0; i < 20; ++i) { plain->step(); h2.observe(*plain); }
        check(h2.epoch_samples() == 0 && h2.names(Axis::Epoch).empty(),
              "a sim with no epoch records nothing on the epoch axis");
    }

    // ── the diamond problem ─────────────────────────────────────────────────
    //
    // The tree must be a real constraint, not decoration: no rung may be
    // reachable without the one below it. And the cost of NOT knowing the tree
    // has to be measurable, or the whole thing demonstrates nothing.
    {
        group("block world: the tech tree is a real constraint");
        using V = bench::VoxelCraft;

        // Every rung names its prerequisite, and the chain reaches the bottom.
        int depth = 0;
        for (int i = V::GotDiamond; i != V::None; i = V::recipe(i).needs) ++depth;
        check(depth == int(V::kItems),
              "the chain from diamond back to wood is " + std::to_string(depth) +
              " rungs with no gaps");

        // Nothing is held at the start, diamond least of all.
        {
            auto sim = bench::make_voxelcraft(20);
            auto& C = *static_cast<V*>(sim.get());
            check(!C.has(V::GotWood) && !C.has(V::GotDiamond),
                  "an agent starts with nothing");
        }
        // A scripted agent that knows the recipe gets there — so the task is
        // solvable and any failure below is the agent's, not the world's.
        {
            auto sim = bench::make_voxelcraft(24);
            auto& C = *static_cast<V*>(sim.get());
            C.set_agent(0); sim->reset(); C.set_agent(0);
            C.run_quiet(3000);
            check(C.has(V::GotDiamond),
                  "a scripted agent reaches diamond, so the world is solvable");
        }
        // And a random one pays enormously more for the same result. Measured:
        // scripted 226 steps, random 11,436.
        {
            auto sim = bench::make_voxelcraft(24);
            auto& C = *static_cast<V*>(sim.get());
            C.set_agent(2); sim->reset(); C.set_agent(2);
            C.run_quiet(3000);
            check(!C.has(V::GotDiamond),
                  "a random agent has NOT reached diamond in the budget the scripted one "
                  "needed ten times over — that gap is the sparse-reward problem");
            check(C.highest_item() >= V::GotWood,
                  "  though it does stumble into the early rungs, which is exactly why "
                  "early progress is a misleading signal");
        }
    }

    // ══ voxelcity's five subsystems, tested AT THE SEAM ══════════════════════
    //
    // About 1,100 lines wired light, fluids, biomes, craft and mobs into
    // voxelcity, and this suite reported the SAME check count before and after:
    // not one assertion named any of them inside the sim. Each subsystem has a
    // standalone test file of its own, and every one of those passes whether or
    // not the sim ever calls it — which is exactly what "wired but inert", the
    // failure this project keeps finding, looks like from outside.
    //
    // So nothing below re-tests a subsystem's internals; those are covered. Each
    // check is about the JOIN: does the sim's single write path tell the light
    // engine, does the sim's tick drive the fluid queue, does the seed knob
    // reach the terrain, does the furnace charge the published price, does the
    // spawn loop read the same light field the mobs fight by.
    //
    // COST. Every world here is the size knob's first notch — 64 wide, 128 tall,
    // 524,288 cells — because it is the smallest world the sim will build and a
    // seam is no truer in a bigger one. Runs are hundreds of ticks, not
    // thousands, and every length was MEASURED before it was written down: water
    // crosses a three-block trench in 10 ticks, the hostile cap fills by tick 98
    // on the slowest of three seeds, a smelt is 200 ticks by publication. The
    // whole block is a few seconds against a suite already minutes long.
    {
        // One world builder, because every group below wants the same small one.
        // seed goes through on_knob, which is what rebuilds the world — setting
        // the knob value alone was the inert-control bug this sim shipped.
        auto smallCity = [](int seed, int agents, int policy) {
            auto s = std::make_unique<bench::VoxelCity>();
            s->on_knob("size",   0.f);              // 64 wide: the smallest notch
            s->on_knob("agents", float(agents));
            s->on_knob("seed",   float(seed));
            s->set_policy(policy);
            return s;
        };

        // ── LIGHT ───────────────────────────────────────────────────────────
        {
            group("voxelcity x light: the incremental field never drifts");
            using namespace bench;

            // THE assertion, and the one that would have caught the stale mob
            // light field: after a real run the incrementally maintained light
            // must equal a from-scratch rebuild, cell for cell.
            //
            // LightEngine is incremental — setBlock hands it one cell and it
            // repairs the neighbourhood — which is only correct if EVERY block
            // mutation in the sim reaches that hand-off: agents mining and
            // placing, fluids flowing, sand landing, grass spreading, crops
            // growing, leaves rotting. A single write that skips it leaves a
            // hole that is wrong forever and that nothing else notices, because
            // stale light still renders and still answers every query.
            //
            // A fresh rebuild is the oracle: it reads BlockWorld and nothing
            // else, so one differing nibble means one missed write.
            for (int seed : {3, 5}) {
                auto s = smallCity(seed, 6, 1);
                const std::string tag = "seed " + std::to_string(seed) + ": ";
                {
                    LightEngine oracle(s->world());
                    oracle.rebuild();
                    check(oracle.raw() == s->light().raw(),
                          tag + "light straight out of world generation equals a "
                          "from-scratch rebuild");
                }
                s->run_quiet(400);
                LightEngine oracle(s->world());
                oracle.rebuild();
                std::size_t bad = 0;
                const auto& want = oracle.raw();
                const auto& got  = s->light().raw();
                for (std::size_t i = 0; i < want.size() && i < got.size(); ++i)
                    if (want[i] != got[i]) ++bad;
                check(want.size() == got.size() && bad == 0,
                      tag + "after 400 ticks of a real run the incrementally maintained "
                      "light still equals a from-scratch rebuild (" + std::to_string(bad) +
                      " of " + std::to_string(want.size()) + " cells differ; any non-zero "
                      "means some block write skipped LightEngine::block_changed)");
            }

            // A cave is dark, and a torch put down through the sim's OWN write
            // path lights it. edit_block is setBlock — the same one function the
            // whole file writes blocks through — so this is the coupling, not a
            // poke at the engine.
            {
                auto s = smallCity(5, 2, 1);
                const int cx = 20, cz = 20, y = 20;      // ~40 blocks under the rock
                for (int dz = 0; dz < 6; ++dz)
                    for (int dx = 0; dx < 6; ++dx)
                        for (int dy = 0; dy < 3; ++dy)
                            s->edit_block(cx + dx, y + dy, cz + dz, std::uint8_t(Air));
                const int mx = cx + 2, my = y + 1, mz = cz + 2;
                check(s->light().sky(mx, my, mz) == 0
                      && s->light().block_light(mx, my, mz) == 0,
                      "a chamber excavated 40 blocks under the rock is dark on both "
                      "channels (sky " + std::to_string(s->light().sky(mx, my, mz)) +
                      ", block " + std::to_string(s->light().block_light(mx, my, mz)) + ")");
                check(s->light().light_allows_hostile_spawn(mx, my, mz, kSkyDarkenMidnight),
                      "and the spawn rule the sim's mob loop calls says a hostile may "
                      "appear in it");
                s->edit_block(mx, my, mz, std::uint8_t(Torch));
                check(s->light().block_light(mx, my, mz) == kTorchLight,
                      "a torch placed through the sim's write path lights its own cell to "
                      "light.hpp's published " + std::to_string(kTorchLight) + " (reads " +
                      std::to_string(s->light().block_light(mx, my, mz)) + ")");
                check(s->light().block_light(mx + 1, my, mz) == kTorchLight - 1
                      && s->light().block_light(mx + 3, my, mz) == kTorchLight - 3,
                      "and it falls off one level per block of taxicab distance (" +
                      std::to_string(s->light().block_light(mx + 1, my, mz)) + " at one, " +
                      std::to_string(s->light().block_light(mx + 3, my, mz)) + " at three)");
                check(!s->light().light_allows_hostile_spawn(mx + 1, my, mz, kSkyDarkenMidnight),
                      "so the cell beside the torch is now off limits to hostiles, which is "
                      "what putting a torch down is FOR");
                LightEngine oracle(s->world());
                oracle.rebuild();
                check(oracle.raw() == s->light().raw(),
                      "and after 108 excavated cells and a torch the incremental field "
                      "still matches a rebuild");
            }

            // The light an agent can SEE. observe() is private, but busyObs is
            // not: it is the observation an agent was looking at when it
            // committed to a multi-tick option, and it is the same vector the
            // network trains on. The three light features are the last three
            // before the four hostile ones (see observe() in voxelcity.hpp), so
            // they sit at width-7, width-6, width-5 — if that layout ever
            // changes this check fails loudly, which is the point.
            {
                auto s = smallCity(2, 6, 0);
                bool caught = false;
                std::size_t width = 0;
                float slotI = -2.f, slotB = -2.f, slotP = -2.f;
                float wantI = -1.f, wantB = -1.f, wantP = -1.f;
                int blockLight = -1;
                for (int t = 0; t < 300 && !caught; ++t) {
                    bench::VoxelCity::Agent& a0 = s->agent_for_test(0);
                    // A torch over its head, so the block-light feature carries a
                    // value no constant could be mistaken for. An agent standing
                    // in daylight reads (1, 0, 0), and a hard-coded vector of
                    // those would pass a check written without this.
                    const int tx = a0.x, ty = a0.y + 2, tz = a0.z;
                    if (s->world().at(tx, ty, tz) == Air)
                        s->edit_block(tx, ty, tz, std::uint8_t(Torch));
                    const int px = a0.x, py = a0.y, pz = a0.z;
                    const int d0 = s->sky_darken_now();
                    const int i0 = s->light().internal_light(px, py, pz, d0);
                    const int b0 = s->light().block_light(px, py, pz);
                    const int p0 = int(s->light().light_allows_hostile_spawn(px, py, pz, d0));
                    s->run_quiet(1);
                    bench::VoxelCity::Agent& a1 = s->agent_for_test(0);
                    if (a1.busyObs.empty()) continue;
                    // Only a tick on which the agent did not move and the light
                    // did not change can be checked EXACTLY: observe() ran
                    // between the two readings, so if both agree it saw these.
                    const int d1 = s->sky_darken_now();
                    if (a1.x != px || a1.y != py || a1.z != pz) continue;
                    if (s->light().internal_light(px, py, pz, d1) != i0) continue;
                    if (s->light().block_light(px, py, pz) != b0) continue;
                    if (int(s->light().light_allows_hostile_spawn(px, py, pz, d1)) != p0) continue;
                    if (a1.busyObs.size() < 7) continue;
                    caught = true;
                    width  = a1.busyObs.size();
                    slotI  = a1.busyObs[width - 7];
                    slotB  = a1.busyObs[width - 6];
                    slotP  = a1.busyObs[width - 5];
                    wantI  = float(i0) / float(kLightMax);
                    wantB  = float(b0) / float(kLightMax);
                    wantP  = float(p0);
                    blockLight = b0;
                }
                check(caught,
                      "an agent was caught mid-option standing still under steady light, "
                      "so its observation can be compared with the engine (if this fails, "
                      "no agent committed to an option in 300 ticks)");
                check(caught && blockLight > 0,
                      "the torch over its head really reaches it, so the light features are "
                      "not all zeros or all ones (block light " +
                      std::to_string(blockLight) + " of " + std::to_string(kLightMax) + ")");
                check(caught
                      && std::fabs(slotI - wantI) < 1e-6f
                      && std::fabs(slotB - wantB) < 1e-6f
                      && std::fabs(slotP - wantP) < 1e-6f,
                      "and the last three light features of the observation are the engine's "
                      "own numbers (observation " + std::to_string(slotI) + "/" +
                      std::to_string(slotB) + "/" + std::to_string(slotP) + " against engine " +
                      std::to_string(wantI) + "/" + std::to_string(wantB) + "/" +
                      std::to_string(wantP) + ", at width " + std::to_string(width) +
                      " slots " + std::to_string(width) + "-7,-6,-5)");
            }
        }

        // ── FLUIDS ──────────────────────────────────────────────────────────
        {
            group("voxelcity x fluids: a breached shore floods, and sand falls");
            using namespace bench;

            // DIRECTED EDITS, not a natural run. A town may go twenty thousand
            // ticks without ever digging into the sea, so a fluid test that waits
            // for one to happen passes by never testing anything.
            for (int seed : {1, 3, 6}) {
                auto s = smallCity(seed, 2, 1);
                const std::string tag = "seed " + std::to_string(seed) + ": ";
                const int sea = s->world().sea_level(), N = s->world().n();
                const int L = 3;                       // three cells of bank
                int bx = -1, bz = -1, dx = 0, dz = 0;
                // Open sea with a real bank beside it: three land cells in a row,
                // each solid at sea level and two below, with sky above. The
                // "open sea" requirement is deliberate — a one-cell puddle drains
                // into the trench and stops, which would test the trench and not
                // the sea.
                for (int z = 6; z < N - 8 && bx < 0; ++z)
                    for (int x = 6; x < N - 8; ++x) {
                        if (!s->fluids().is_source(x, sea, z)) continue;
                        int nearby = 0;
                        for (int b2 = -2; b2 <= 2; ++b2)
                            for (int a2 = -2; a2 <= 2; ++a2)
                                if (s->fluids().is_source(x + a2, sea, z + b2)) ++nearby;
                        if (nearby < 20) continue;           // 20 of 25: real water
                        const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
                        for (const auto& d : dirs) {
                            bool ok = true;
                            for (int k = 1; k <= L && ok; ++k) {
                                const int nx = x + d[0]*k, nz = z + d[1]*k;
                                for (int yy = sea; yy >= sea - 2 && ok; --yy)
                                    if (!block_solid(s->world().at(nx, yy, nz))) ok = false;
                                if (ok && block_solid(s->world().at(nx, sea + 1, nz))) ok = false;
                            }
                            if (ok) { bx = x; bz = z; dx = d[0]; dz = d[1]; break; }
                        }
                        if (bx >= 0) break;
                    }
                int firstFill = -1, fullAt = -1;
                long long gained = -1;
                if (bx >= 0) {
                    const std::size_t before = s->world().count(Water);
                    for (int k = 1; k <= L; ++k)
                        s->edit_block(bx + dx*k, sea, bz + dz*k, std::uint8_t(Air));
                    for (int t = 1; t <= 60; ++t) {
                        s->run_quiet(1);
                        int r = 0;
                        for (int k = 1; k <= L; ++k)
                            if (s->world().at(bx + dx*k, sea, bz + dz*k) == Water) r = k;
                        if (firstFill < 0 && r >= 1) firstFill = t;
                        if (fullAt   < 0 && r == L) fullAt   = t;
                    }
                    gained = static_cast<long long>(s->world().count(Water))
                           - static_cast<long long>(before);
                }
                check(bx >= 0, tag + "found a stretch of shore three blocks deep to breach "
                               "(if this fails the world has no coastline the test can use)");
                // fluids.hpp publishes water's tickDelay as 5: "water spreads at
                // a rate of 1 block every 5 game ticks". The sea is only allowed
                // in because the sim's own tick drives Fluids::tick, and the
                // trench is only known to it because setBlock told it.
                check(firstFill == kWater.tickDelay,
                      tag + "water enters the breach on tick " + std::to_string(firstFill) +
                      ", which is water's published tickDelay of " +
                      std::to_string(kWater.tickDelay));
                check(fullAt > 0 && fullAt <= L * kWater.tickDelay,
                      tag + "and crosses all " + std::to_string(L) + " cells within " +
                      std::to_string(L * kWater.tickDelay) + " ticks, one block per delay "
                      "(took " + std::to_string(fullAt) + ")");
                check(gained >= L,
                      tag + "and the world holds at least " + std::to_string(L) +
                      " more water blocks than before the breach (gained " +
                      std::to_string(gained) + ")");
            }

            // Gravity: take the support out from under a sand block and it must
            // leave its cell after fluids.hpp's published kGravityDelay ticks,
            // fall as an entity, and land on the first solid block below.
            {
                auto s = smallCity(4, 2, 1);
                const int sea = s->world().sea_level(), N = s->world().n();
                int sx = -1, sy = -1, sz = -1;
                for (int z = 8; z < N - 8 && sx < 0; ++z)
                    for (int x = 8; x < N - 8; ++x) {
                        const int top = s->world().surface(x, z);
                        if (top > sea + 3 && top + 6 < s->world().height()
                            && block_solid(s->world().at(x, top, z))) {
                            sx = x; sy = top; sz = z; break;
                        }
                    }
                int leftAt = -1, wasEntity = 0, landedAt = -1;
                if (sx >= 0) {
                    // A shaft with a ledge across it, and sand resting on the
                    // ledge. Nothing falls yet — that is the control.
                    for (int k = 1; k <= 3; ++k)
                        s->edit_block(sx, sy + k, sz, std::uint8_t(Air));
                    s->edit_block(sx, sy + 3, sz, std::uint8_t(Stone));   // the support
                    s->edit_block(sx, sy + 4, sz, std::uint8_t(Sand));
                    s->run_quiet(10);
                    leftAt = (s->world().at(sx, sy + 4, sz) == Sand) ? 0 : -2;
                    if (leftAt == 0) {
                        s->edit_block(sx, sy + 3, sz, std::uint8_t(Air));  // pull it out
                        leftAt = -1;
                        for (int t = 1; t <= 80; ++t) {
                            s->run_quiet(1);
                            if (leftAt < 0 && s->world().at(sx, sy + 4, sz) != Sand) leftAt = t;
                            if (!s->fluids().entities().empty()) wasEntity = 1;
                            if (landedAt < 0 && s->world().at(sx, sy + 1, sz) == Sand)
                                landedAt = t;
                        }
                    }
                }
                check(sx >= 0, "found a hilltop above sea level to drop sand down");
                check(leftAt == kGravityDelay,
                      "sand supported on a ledge stays put, and leaves its cell " +
                      std::to_string(kGravityDelay) + " ticks after the support is pulled — "
                      "fluids.hpp's published kGravityDelay (left on tick " +
                      std::to_string(leftAt) + "; 0 would mean it never sat still, -2 that "
                      "it fell while still supported)");
                check(wasEntity == 1,
                      "and it falls as a FallingBlockEntity rather than teleporting");
                check(landedAt > kGravityDelay,
                      "and lands on the first solid block below, three down (landed on tick " +
                      std::to_string(landedAt) + "; -1 means it never came to rest, so the "
                      "sim's tick is not draining the fluid queue)");
                check(sx >= 0 && s->fluids().entities().empty(),
                      "leaving no entity in flight");
            }
        }

        // ── BIOMES ──────────────────────────────────────────────────────────
        {
            group("voxelcity x biomes: the seed builds the world, and the world alone");
            using namespace bench;

            // The seed knob was inert until recently and the OLD assertion passed
            // anyway, because the same seed also seeds every agent's brain and
            // spawn point: "three seeds gave three different outcomes" was true
            // while three seeds gave one world. Everything here reads the WORLD.
            auto blockHist = [](const VoxelCity& s) {
                std::array<std::size_t, kBlocks> h{};
                for (int b = 0; b < kBlocks; ++b)
                    h[std::size_t(b)] = s.world().count(std::uint8_t(b));
                return h;
            };
            std::vector<std::unique_ptr<bench::VoxelCity>> w;
            for (int seed = 1; seed <= 3; ++seed) w.push_back(smallCity(seed, 2, 1));

            // THE SURFACE IS THE BIOME'S SURFACE.
            //
            // A mutation test found this hole: painting the filler block where
            // the surface belongs — grass replaced by the dirt underneath it —
            // left the whole suite green. Every assertion here compared seeds
            // against each other, and a bug that changes all three worlds the
            // same way is invisible to that. Comparisons catch divergence;
            // only a statement about what a column SHOULD look like catches a
            // world that is uniformly wrong.
            //
            // Checked above sea level only. At and below the waterline the
            // column is ocean floor or beach and the biome's own surface no
            // longer decides it, which is a documented divergence in the
            // generator rather than something to assert.
            {
                const auto& city = *w[0];
                const auto& bw = city.world();
                const int sea = bw.sea_level();
                std::size_t checked = 0, matched = 0;
                std::array<std::size_t, bench::kBlocks> tops{};
                for (int z = 0; z < bw.n(); ++z)
                    for (int x = 0; x < bw.n(); ++x) {
                        const int y = bw.surface(x, z);
                        if (y <= sea + 1) continue;             // shore and below
                        const std::uint8_t top = bw.at(x, y, z);
                        if (top == bench::Wood || top == bench::Leaves) continue;  // a tree
                        const auto& def = bench::biome_def(city.biomes().biome_at(x, z));
                        ++checked;
                        ++tops[std::size_t(top)];
                        if (top == def.surface) ++matched;
                    }
                check(checked > 200,
                      "there is enough land above the waterline to judge (" +
                      std::to_string(checked) + " columns)");
                // Not 100%: a column an agent has already mined or built on is
                // legitimately something else, and this runs after generation
                // only, so the tolerance is small and deliberate.
                const double frac = checked ? double(matched) / double(checked) : 0.0;
                check(frac > 0.98,
                      "every land column is topped by its own biome's surface block (" +
                      std::to_string(int(frac * 1000) / 10.0) + "% of " +
                      std::to_string(checked) + "; painting the filler here instead "
                      "would read as a world of bare dirt)");
                // And the surfaces are not all one block: a generator that paints
                // grass everywhere would satisfy the check above in a world whose
                // biomes all happen to want grass.
                int distinctTops = 0;
                for (std::size_t b = 0; b < tops.size(); ++b) if (tops[b] > 0) ++distinctTops;
                check(distinctTops >= 2,
                      "and the map shows more than one kind of surface (" +
                      std::to_string(distinctTops) + " distinct top blocks)");
            }
            for (std::size_t i = 0; i < w.size(); ++i)
                for (std::size_t j = i + 1; j < w.size(); ++j) {
                    const std::string tag = "seeds " + std::to_string(i + 1) + " and " +
                                            std::to_string(j + 1) + ": ";
                    const auto ha = blockHist(*w[i]), hb = blockHist(*w[j]);
                    int present = 0, differing = 0;
                    for (int k = 0; k < kBlocks; ++k) {
                        if (ha[std::size_t(k)] || hb[std::size_t(k)]) ++present;
                        if (ha[std::size_t(k)] != hb[std::size_t(k)]) ++differing;
                    }
                    int bio = 0;
                    for (int k = 0; k < kBiomeCount; ++k)
                        if (w[i]->biomes().count(Biome(k)) != w[j]->biomes().count(Biome(k)))
                            ++bio;
                    const auto& ra = w[i]->world().raw();
                    const auto& rb = w[j]->world().raw();
                    std::size_t cells = 0;
                    for (std::size_t k = 0; k < ra.size() && k < rb.size(); ++k)
                        if (ra[k] != rb[k]) ++cells;
                    // A HISTOGRAM, not one count. Two worlds can hold the same
                    // number of stone blocks and be nothing alike; measured, 16
                    // or 17 of the 18 block types present differ between any two
                    // of these seeds, so 10 is a floor with room in it.
                    check(differing >= 10,
                          tag + std::to_string(differing) + " of the " +
                          std::to_string(present) + " block types present have different "
                          "counts (fewer than 10 would mean the seed barely reaches the "
                          "terrain)");
                    check(bio >= 6,
                          tag + std::to_string(bio) + " of " + std::to_string(kBiomeCount) +
                          " biomes cover a different number of columns");
                    check(cells * 10 >= ra.size(),
                          tag + std::to_string(cells) + " of " + std::to_string(ra.size()) +
                          " cells hold a different block — over a tenth of the world "
                          "(measured about a quarter)");
                }

            // The same seed twice, cell for cell. This is the half the old test
            // could not have failed and the half a broken cache would break.
            {
                auto a = smallCity(2, 4, 1), b = smallCity(2, 4, 1);
                check(a->world().raw() == b->world().raw(),
                      "the same seed builds the same world, block for block");
                check(a->light().raw() == b->light().raw(),
                      "and the same light field, nibble for nibble");
                bool same = true;
                for (int z = 0; z < a->world().n() && same; ++z)
                    for (int x = 0; x < a->world().n(); ++x)
                        if (a->biomes().biome_at(x, z) != b->biomes().biome_at(x, z)) {
                            same = false; break;
                        }
                check(same, "and the same biome map, column for column");
                a->run_quiet(300); b->run_quiet(300);
                check(a->world().raw() == b->world().raw(),
                      "and 300 ticks later the two runs are still the same world, so the "
                      "seed reaches the SIMULATION and not only the generator");
            }

            // And the biome map has to be what painted it. effectiveBiome's rule
            // is documented in voxelcity.hpp — height wins at the waterline,
            // climate decides above it — and is reimplemented here from public
            // accessors so the world can be checked against the published table
            // rather than against itself.
            {
                auto s = smallCity(1, 2, 1);
                const int sea = s->world().sea_level(), N = s->world().n();
                std::size_t soil = 0, match = 0;
                std::array<bool, kBiomeCount> seen{};
                for (int z = 0; z < N; ++z)
                    for (int x = 0; x < N; ++x) {
                        const int top = s->world().surface(x, z);
                        if (top < 1) continue;
                        const std::uint8_t b = s->world().at(x, top, z);
                        // Only SOIL tops are the biome painter's business — a
                        // trunk, a leaf or an exposed ore is somebody else's.
                        if (b != Grass && b != Dirt && b != Sand && b != Stone && b != Gravel)
                            continue;
                        const Biome eb = (top <= sea)     ? Biome::Ocean
                                       : (top <= sea + 1) ? Biome::Beach
                                                          : s->biomes().biome_at(x, z);
                        seen[std::size_t(bidx(eb))] = true;
                        ++soil;
                        if (b == biome_def(eb).surface) ++match;
                    }
                int kinds = 0;
                for (bool v : seen) if (v) ++kinds;
                check(kinds >= 5,
                      "the world carries at least five different biomes (" +
                      std::to_string(kinds) + "), so what follows is not a check on one");
                check(soil > 1000 && match == soil,
                      "and every one of the " + std::to_string(soil) + " soil-topped columns "
                      "wears its biome's published surface block — sand on desert, gravel "
                      "on ocean floor, stone on mountain (" + std::to_string(soil - match) +
                      " wrong)");
            }
        }

        // ── CRAFT ───────────────────────────────────────────────────────────
        {
            group("voxelcity x craft: 200 ticks a smelt, and a pickaxe that wears out");
            using namespace bench;

            // Durability, driven directly. craft.hpp publishes wood at 59 uses
            // and stone at 131, and this sim spends one per block broken.
            // Waiting for a policy to break 59 blocks would measure the policy.
            for (int tier = 1; tier <= 2; ++tier) {
                auto s = smallCity(3, 2, 1);
                const int slot = (tier == 1) ? int(ItWoodPick) : int(ItStonePick);
                const craft::Material mat = (tier == 1) ? craft::Material::Wood
                                                        : craft::Material::Stone;
                const int uses = craft::tool_durability(mat);
                {
                    auto& a = s->agent_for_test(0);
                    a.items[std::size_t(slot)]      = 1;
                    a.durability[std::size_t(slot)] = uses;
                    a.tier = tier;
                }
                int brokeAt = -1;
                for (int k = 1; k <= uses + 2 && brokeAt < 0; ++k) {
                    s->break_one_for_test(0);
                    if (s->tools_broken() > 0) brokeAt = k;
                }
                const auto& a = s->agent_for_test(0);
                check(brokeAt == uses,
                      std::string(item_name(slot)) + " breaks on block " +
                      std::to_string(brokeAt) + ", and craft.hpp publishes " +
                      std::to_string(uses) + " uses");
                check(a.items[std::size_t(slot)] == 0 && a.tier == 0,
                      std::string("and the agent loses the ") + item_name(slot) +
                      " and drops back to bare hands, which is the only thing in this sim "
                      "that can take a rung away (holds " +
                      std::to_string(a.items[std::size_t(slot)]) + ", tier " +
                      std::to_string(a.tier) + ")");
            }

            // Smelting. craft.hpp: a furnace runs one item every 200 ticks, and
            // one coal burns for 1,600. Both are measured THROUGH the sim — the
            // agent is handed ore and coal and a furnace is placed beside it
            // through the sim's own write path, because a run that reports "0
            // smelts" cannot tell a broken furnace from an agent that never
            // found iron, and that distinction is the whole question.
            {
                auto s = smallCity(1, 4, 1);
                {
                    auto& a = s->agent_for_test(0);
                    a.blocks[std::size_t(IronOre)] = 4;
                    a.blocks[std::size_t(Coal)]    = 4;
                    a.items[std::size_t(ItStonePick)] = 1;
                    a.durability[std::size_t(ItStonePick)] =
                        craft::tool_durability(craft::Material::Stone);
                    a.tier = 2;
                    s->edit_block(a.x + 1, a.y, a.z, std::uint8_t(Furnace));
                }
                const int ingotsBefore = s->agent_for_test(0).items[std::size_t(ItIronIngot)];
                int startTick = -1, doneTick = -1, coalSpent = -1;
                long long fuelAtStart = -1;
                for (int t = 1; t <= 1200 && doneTick < 0; ++t) {
                    const int coalBefore = s->agent_for_test(0).blocks[std::size_t(Coal)];
                    s->run_quiet(1);
                    const auto& a = s->agent_for_test(0);
                    if (startTick < 0 && a.smeltLeft > 0) {
                        startTick   = t;
                        fuelAtStart = s->fuel_burned();
                        coalSpent   = coalBefore - a.blocks[std::size_t(Coal)];
                    }
                    if (startTick > 0 && s->smelts_done() > 0) doneTick = t;
                }
                check(startTick > 0,
                      "an agent standing at a furnace with ore and coal commits to a smelt "
                      "(started on tick " + std::to_string(startTick) + ")");
                check(startTick > 0 && doneTick == startTick + craft::kSmeltTicks,
                      "and the smelt takes craft.hpp's published " +
                      std::to_string(craft::kSmeltTicks) + " ticks — started " +
                      std::to_string(startTick) + ", finished " + std::to_string(doneTick) +
                      " (a difference other than " + std::to_string(craft::kSmeltTicks) +
                      " means the furnace and the clock have drifted apart)");
                check(coalSpent == 1,
                      "lighting the furnace costs exactly one coal from the pack (cost " +
                      std::to_string(coalSpent) + ")");
                check(startTick > 0 && s->fuel_burned() - fuelAtStart == craft::kSmeltTicks,
                      "and the furnace burns one tick of fuel per world tick while it cooks, "
                      "so a coal's " + std::to_string(craft::fuel_burn_ticks(craft::ItemId::Coal)) +
                      " ticks really are " +
                      std::to_string(int(craft::items_smelted(craft::ItemId::Coal))) +
                      " items (burned " +
                      std::to_string(s->fuel_burned() - fuelAtStart) + " over this one)");
                check(s->agent_for_test(0).items[std::size_t(ItIronIngot)] == ingotsBefore + 1,
                      "and the agent is holding one more iron ingot than it was");
            }

            // The rung order the metrics are indexed by. `mean rung` and `best
            // rung` are item index + 1, so the ladder only means anything if no
            // rung is reachable without the ones below it: every ITEM ingredient
            // must come earlier in the list, and every BLOCK ingredient must be
            // harvestable with a pickaxe that comes earlier still.
            {
                int outOfOrder = 0, ungated = 0;
                for (int it = 0; it < kItems; ++it) {
                    const Recipe r = recipe(it);
                    for (const auto& in : r.in) {
                        if (in.count <= 0) continue;
                        if (in.isItem) {
                            if (int(in.what) >= it) ++outOfOrder;
                            continue;
                        }
                        const int need = block_rule(in.what).tier;
                        if (need <= 0) continue;          // hand or shovel work
                        int pick = -1;
                        for (int p = 0; p < kItems; ++p) if (recipe(p).tier == need) pick = p;
                        if (pick < 0 || pick >= it) ++ungated;
                    }
                }
                check(outOfOrder == 0,
                      "every recipe's item ingredients sit lower on the ladder than the "
                      "thing they make (" + std::to_string(outOfOrder) + " do not)");
                check(ungated == 0,
                      "and every block ingredient needs a pickaxe that is itself a lower "
                      "rung, so the tree is a real constraint and `mean rung` is a real "
                      "ordering (" + std::to_string(ungated) + " are reachable out of turn)");
            }

            // And the town's own numbers have to obey that ladder. An agent
            // holding item i must be recorded at rung i+1 or higher, or the
            // headline metric is not measuring what it says.
            {
                auto s = smallCity(1, 8, 1);
                s->run_quiet(4000);
                int holds = 0, wrong = 0;
                for (const auto& a : s->agents())
                    for (int it = 0; it < kItems; ++it)
                        if (a.items[std::size_t(it)] > 0) {
                            ++holds;
                            if (a.bestRung < it + 1) ++wrong;
                        }
                check(holds > 0 && wrong == 0,
                      "after 4,000 scripted ticks the town holds " + std::to_string(holds) +
                      " kinds of item and every one of them is at or below the rung its "
                      "owner is credited with (" + std::to_string(wrong) + " are not)");
                double meanRung = -1, bestRung = -1;
                for (const auto& m : s->metrics()) {
                    if (m.name == "mean rung") meanRung = m.value;
                    if (m.name == "best rung") bestRung = m.value;
                }
                check(std::fabs(meanRung - s->mean_rung()) < 1e-9
                      && std::fabs(bestRung - double(s->best_rung())) < 1e-9,
                      "and the published metrics report that same ladder (mean rung " +
                      std::to_string(meanRung) + " against " +
                      std::to_string(s->mean_rung()) + ", best " +
                      std::to_string(bestRung) + " against " +
                      std::to_string(s->best_rung()) + ")");
            }
        }

        // ── MOBS ────────────────────────────────────────────────────────────
        {
            // ── a tape must carry the whole world it was recorded in ────
            //
            // The header carried seed/size/agents/idle and NOT roles or hunger,
            // both of which measurably change a run — roles rebuilds the town's
            // objectives, hunger scales every exhaustion cost. A replay then
            // reproduced the recording only when those knobs happened to
            // already match, which is a guarantee that holds while you are
            // testing it and fails the first time somebody moves a knob.
            //
            // Recorded under NON-default values and replayed into a sim sitting
            // at the defaults, because that is the case that was broken.
            {
                group("a possession tape carries its whole world");
                auto rec = std::make_unique<bench::VoxelCity>();
                rec->on_knob("agents", 4.f);
                rec->on_knob("size",   0.f);
                rec->on_knob("seed",   3.f);
                rec->on_knob("roles",  0.f);      // not the default
                rec->on_knob("hunger", 3.f);      // not the default
                rec->on_knob("policy", 4.f);
                for (int i = 0; i < 400; ++i) rec->step();
                const auto tape = rec->tape();
                check(tape.roles == 0 && tape.hunger > 2.9f,
                      "the tape records the knobs it was made under (roles " +
                      std::to_string(tape.roles) + ", hunger " +
                      std::to_string(tape.hunger) + ")");

                auto rep = std::make_unique<bench::VoxelCity>();   // at the defaults
                rep->possess_replay(tape);
                int gotRoles = -1; float gotHunger = -1.f;
                for (auto& k : rep->knobs()) {
                    if (k.key == "roles")  gotRoles  = int(k.value + 0.5f);
                    if (k.key == "hunger") gotHunger = k.value;
                }
                check(gotRoles == tape.roles && std::fabs(gotHunger - tape.hunger) < 1e-6f,
                      "and replaying it restores them (roles " + std::to_string(gotRoles) +
                      ", hunger " + std::to_string(gotHunger) + ")");

                // Out-of-range headers must not be obeyed. A hand-written or
                // corrupt tape once built 100,000 agents and reported a
                // possessed index of -7 as live.
                auto wide = std::make_unique<bench::VoxelCity>();
                wide->on_knob("agents", 5000.f);
                check(wide->agents().size() <= 32,
                      "the agents knob is clamped to its published maximum (asked 5000, built " +
                      std::to_string(wide->agents().size()) + ")");
                auto bad = tape; bad.index = -7;
                auto neg = std::make_unique<bench::VoxelCity>();
                neg->possess_replay(bad);
                check(neg->possessed_index() >= 0,
                      "and a negative possessed index is clamped, not obeyed (" +
                      std::to_string(neg->possessed_index()) + ")");
            }

            group("voxelcity x mobs: the light gate, the cap, and the hurt window");
            using namespace bench;

            // (1) THE LIGHT GATE, as a negative control rather than a
            // correlation. Fill every cave and the world has no cell a hostile
            // may spawn in: at noon the surface is sky light 15, which fails the
            // internal-sky <= 7 half of [W-SPAWN], and there is nothing left
            // underground. If the spawn loop is really reading the light engine,
            // spawning must stop dead — and the same seed unsealed must spawn.
            auto spawnableCells = [](const bench::VoxelCity& s) {
                std::size_t n = 0;
                const int N = s.world().n(), H = s.world().height();
                for (int y = 1; y + 1 < H; ++y)
                    for (int z = 0; z < N; ++z)
                        for (int x = 0; x < N; ++x)
                            if (s.light().hostile_can_spawn(x, y, z, s.sky_darken_now())) ++n;
                return n;
            };
            for (int seed : {1, 6}) {
                const std::string tag = "seed " + std::to_string(seed) + ": ";
                auto openWorld = smallCity(seed, 4, 1);
                openWorld->run_quiet(800);
                auto sealed = smallCity(seed, 4, 1);
                const std::size_t before = spawnableCells(*sealed);
                const int N = sealed->world().n();
                for (int z = 0; z < N; ++z)
                    for (int x = 0; x < N; ++x) {
                        const int top = sealed->world().surface(x, z);
                        for (int y = 1; y < top; ++y)
                            if (sealed->world().at(x, y, z) == Air)
                                sealed->edit_block(x, y, z, std::uint8_t(Stone));
                    }
                const std::size_t after = spawnableCells(*sealed);
                sealed->run_quiet(800);
                check(before > 0 && openWorld->mobs_spawned() > 0,
                      tag + "the open world has " + std::to_string(before) +
                      " cells the light rules allow and spawns " +
                      std::to_string(openWorld->mobs_spawned()) + " hostiles in 800 ticks");
                check(after == 0,
                      tag + "sealing every cave through the sim's write path leaves no cell "
                      "a hostile may spawn in (" + std::to_string(after) + " left)");
                check(sealed->mobs_spawned() == 0,
                      tag + "and the sealed world spawns nothing at all in the same 800 "
                      "ticks (" + std::to_string(sealed->mobs_spawned()) + " spawned — any "
                      "spawn here is a spawn the light gate did not see)");
            }

            // (2) THE CAP. mobs.hpp publishes 70 monsters per 289 spawnable
            // chunks; a 64-wide world is 16 chunks, so the cap is 3. Both halves
            // matter: a cap nothing ever reaches is untested, and a cap that is
            // exceeded is not a cap.
            {
                auto s = smallCity(3, 4, 1);
                const int chunks = std::max(1, (s->world().n() / 16) * (s->world().n() / 16));
                const int cap = mob_cap(CatMonster, chunks);
                int peak = 0, over = 0, reachedAt = -1;
                for (int t = 1; t <= 1200; ++t) {
                    s->run_quiet(1);
                    const int alive = s->mob_world().alive(CatMonster);
                    peak = std::max(peak, alive);
                    if (alive > cap) ++over;
                    if (reachedAt < 0 && alive >= cap) reachedAt = t;
                }
                check(cap > 0 && reachedAt > 0,
                      "the town fills mobs.hpp's hostile cap of " + std::to_string(cap) +
                      " for a " + std::to_string(chunks) + "-chunk world, on tick " +
                      std::to_string(reachedAt));
                check(over == 0 && peak == cap,
                      "and never exceeds it across 1,200 ticks (peak " +
                      std::to_string(peak) + ", " + std::to_string(over) +
                      " ticks above the cap)");
            }

            // (3) THE 10-TICK INVULNERABILITY WINDOW. This was wired and inert
            // once already: MobWorld::tick returned the SUM of every swing that
            // connected, and a batch cannot be gated — four zombies arrived as
            // one 12.0 and went straight through while hurtCooldown was 0.
            //
            // The agent has to be SEALED IN with them. On the surface it simply
            // walks away and nothing ever connects, which is how a broken gate
            // can look perfectly healthy for a whole run.
            {
                auto s = smallCity(6, 4, 1);
                const int cx = 32, cy = 25, cz = 32;
                // SEALED IN BEDROCK, not in stone. The shell used to be
                // whatever the world already had there, and that stopped
                // working the moment mining below the harvest tier was fixed to
                // break blocks the way the game does: a tier-0 agent simply dug
                // out. Measured on the fixture as it stood — agent placed at
                // (32,25,32), found at (32,21,37) sixty ticks later, NINE
                // BLOCKS AWAY. Only one hit landed, so the spacing assertion
                // below had nothing to measure and failed on its own
                // precondition rather than on the rule it exists to check.
                // Bedrock is the one block mineFacing() refuses outright.
                for (int dy = -1; dy <= 3; ++dy)
                    for (int dz = -2; dz <= 2; ++dz)
                        for (int dx = -2; dx <= 2; ++dx)
                            if (std::abs(dx) == 2 || std::abs(dz) == 2 || dy == -1 || dy == 3)
                                s->edit_block(cx + dx, cy + dy, cz + dz, std::uint8_t(bench::Bedrock));
                for (int dy = 0; dy < 3; ++dy)
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dx = -1; dx <= 1; ++dx)
                            s->edit_block(cx + dx, cy + dy, cz + dz, std::uint8_t(Air));
                s->edit_block(cx, cy - 1, cz, std::uint8_t(bench::Bedrock));  // unbreakable floor
                { auto& a = s->agent_for_test(0); a.x = cx; a.y = cy; a.z = cz; }
                const int swarm = 4;
                for (int i = 0; i < swarm; ++i)
                    s->spawn_mob_for_test(MobZombie, float(cx) + 0.5f, float(cy),
                                          float(cz) + 0.5f);
                std::vector<int> landedOn;
                double lastLanded = 0.0;
                int peakCooldown = 0;
                for (int t = 1; t <= 60; ++t) {
                    s->run_quiet(1);
                    if (s->mob_damage_applied() > lastLanded + 1e-9) landedOn.push_back(t);
                    lastLanded = s->mob_damage_applied();
                    peakCooldown = std::max(peakCooldown, s->agent_for_test(0).hurtCooldown);
                }
                int tooClose = 0;
                for (std::size_t i = 1; i < landedOn.size(); ++i)
                    if (landedOn[i] - landedOn[i - 1] < 10) ++tooClose;
                check(s->mob_hits() > 0 && s->mob_damage() > 0.0,
                      "four zombies sealed in a dark pocket with an agent do connect (" +
                      std::to_string(s->mob_hits()) + " swings, " +
                      std::to_string(s->mob_damage()) + " damage swung)");
                // The RULE, not the ratio.
                //
                // This asserted damage-swung == 4 x damage-landed, which is what
                // the code happens to produce with four identical zombies. It is
                // stable across seeds and it is still the wrong assertion: it
                // encodes an arithmetic coincidence of this fixture rather than
                // Java's rule, so it would have to be rewritten every time the
                // swarm size changed, and it says nothing at all about the case
                // the rule exists for. Published rule: a hit inside the
                // invulnerability window lands ONLY if it is stronger than the
                // hit the window is holding, and then only for the DIFFERENCE.
                check(s->mob_damage_applied() > 0.0
                      && s->mob_damage_applied() < s->mob_damage(),
                      "simultaneous swings are gated: less lands than is swung (" +
                      std::to_string(s->mob_damage()) + " swung, " +
                      std::to_string(s->mob_damage_applied()) + " landed; equal totals "
                      "mean the batch is bypassing the gate again)");
                check(peakCooldown == 10,
                      "a landed hit arms the game's 10-tick invulnerability window (peak "
                      "hurtCooldown " + std::to_string(peakCooldown) + ")");
                check(landedOn.size() >= 2 && tooClose == 0,
                      "and no two hits land inside one window across 60 ticks (" +
                      std::to_string(landedOn.size()) + " landed, " +
                      std::to_string(tooClose) + " of them less than 10 ticks apart)");
            }
        }
    }

    // ══ voxelcity x possession: the human baseline ══════════════════════════
    //
    // This sim exists to compare policies, and until now it had three: scripted
    // (mean rung ~5), a deep Q-network (~1), and random. Nobody had any idea
    // what a PERSON reaches, so every claim about the learner was made against a
    // bar of unknown height. Possession is a fourth policy — same action space,
    // same shaped reward, same metrics — and these are the checks that make a
    // human run a MEASUREMENT rather than an anecdote:
    //
    //   · driving actually drives                (or the control is inert)
    //   · not driving changes nothing            (or the wrapper is the effect)
    //   · a recorded run replays IDENTICALLY     (or the tape is incomplete)
    //   · the camera and the marker really move  (or you cannot see yourself)
    {
        using VC = bench::VoxelCity;

        // A fixed "person": a schedule of (tick, key) issued through the same
        // possess_command() the workbench calls. Deterministic, so the same
        // human can be run twice, which is the whole point.
        struct Press { long long at; int code; };
        auto playerPlan = [](long long ticks) {
            // The tech tree, read out loud: wood, planks and sticks, a table, a
            // wooden pick, then stone and up. What a person actually types.
            static const int loop[] = {
                VC::GoWood, VC::GoWood, VC::Craft, VC::Craft, VC::Craft,
                VC::GoWood, VC::Craft, VC::Craft, VC::GoStone, VC::GoStone,
                VC::Craft,  VC::Craft, VC::GoCoal, VC::Craft, VC::GoIron,
                VC::Craft,  VC::Eat,   VC::Ascend,
            };
            const int n = int(sizeof loop / sizeof loop[0]);
            std::vector<Press> p;
            int i = 0;
            for (long long t = 0; t < ticks; t += 250) p.push_back({t, loop[i++ % n]});
            return p;
        };
        // A deliberately UNSCRIPTED driver: dig down, cut sideways, climb out.
        // Nothing the scripted policy would ever choose, so a difference against
        // autopilot cannot be mistaken for noise.
        auto diggerPlan = [](long long ticks) {
            std::vector<Press> p;
            for (long long t = 0; t < ticks; t += 40) {
                switch ((t / 40) % 6) {
                    case 0: p.push_back({t, VC::kMoveBase + 5}); break;   // down
                    case 1: p.push_back({t, VC::Mine});          break;
                    case 2: p.push_back({t, VC::kMoveBase + 0}); break;   // -x
                    case 3: p.push_back({t, VC::Mine});          break;
                    case 4: p.push_back({t, VC::Craft});         break;
                    default: p.push_back({t, VC::Ascend});       break;
                }
            }
            return p;
        };
        auto drive = [](VC& s, const std::vector<Press>& plan, long long ticks) {
            std::size_t k = 0;
            for (long long t = 0; t < ticks; ++t) {
                while (k < plan.size() && plan[k].at == t) s.possess_command(plan[k++].code);
                s.run_quiet(1);
            }
        };
        auto town = [](int seed, int idle) {
            auto s = std::make_unique<VC>();
            s->on_knob("agents", 8.f);
            s->on_knob("size",   0.f);      // 64, so the suite stays quick
            s->on_knob("seed",   float(seed));
            s->on_knob("idle",   float(idle));
            return s;
        };
        const long long T = 4000;

        // ── 1. the tape replays to the same score, to the bit ───────────────
        //
        // THE assertion. If a replayed human run does not score identically to
        // the live one, the recording is missing something the agent depends on
        // and every human number this bench ever publishes is unreproducible.
        // Checked three ways at once, weakest to strongest: the agent's own
        // score, the town's headline metric, and a hash over every block in the
        // world and every field of every agent.
        {
            group("voxelcity x possession: a recorded human run replays identically");
            for (int seed = 1; seed <= 3; ++seed) {
                auto live = town(seed, 12000);
                live->possess(0);
                drive(*live, playerPlan(T), T);
                const auto tape = live->tape();

                auto rep = std::make_unique<VC>();
                rep->possess_replay(tape);
                rep->run_quiet(tape.ticks);

                check(live->score_of(0) == rep->score_of(0),
                      "seed " + std::to_string(seed) + ": the replayed agent scores exactly "
                      "what the live one did (rung " + std::to_string(live->score_of(0).rung) +
                      ", mined " + std::to_string(live->score_of(0).mined) + ", at (" +
                      std::to_string(live->score_of(0).x) + "," +
                      std::to_string(live->score_of(0).y) + "," +
                      std::to_string(live->score_of(0).z) + "))");
                check(live->state_hash() == rep->state_hash(),
                      "seed " + std::to_string(seed) + ": and the whole world with it — "
                      "state hash " + std::to_string(live->state_hash()) + " both times");
                check(rep->replay_fault() < 0,
                      "seed " + std::to_string(seed) + ": every decision landed on the tick "
                      "the tape recorded it on (no desync)");
                check(rep->replay_consumed() == tape.entries.size()
                      && !tape.entries.empty(),
                      "seed " + std::to_string(seed) + ": and the tape was consumed exactly, "
                      + std::to_string(rep->replay_consumed()) + " of " +
                      std::to_string(tape.entries.size()) + " decisions");
                check(std::llround(live->mean_rung() * 1e6)
                      == std::llround(rep->mean_rung() * 1e6),
                      "seed " + std::to_string(seed) + ": the town mean matches too (" +
                      std::to_string(live->mean_rung()) + "), so the other seven agents "
                      "reproduced as well");
            }
            // ── and the detector can FAIL ───────────────────────────────────
            //
            // A check that has only ever been exercised in the passing
            // direction is not evidence of anything. Two corruptions, and the
            // difference between them is the reason the tick stamp exists.
            auto live = town(2, 12000);
            live->possess(0);
            drive(*live, playerPlan(T), T);
            const auto good = live->tape();
            check(good.entries.size() > 40, "the recording is long enough to corrupt");

            // 1. THE REAL FAILURE MODE: the world under the tape did not
            //    reproduce. Nothing about the recorded actions changed — only
            //    the world they are replayed into. This is exactly the case the
            //    brief calls "the recording is not capturing everything the
            //    agent depends on", and it is caught at the tick where the two
            //    runs first stop agreeing.
            {
                auto bent = good;
                bent.seed = 3;                       // a different world, same actions
                auto rep = std::make_unique<VC>();
                rep->possess_replay(bent);
                rep->run_quiet(bent.ticks);
                check(rep->replay_fault() >= 0,
                      "the same tape replayed into a world it was not recorded in is "
                      "CAUGHT, at tick " + std::to_string(rep->replay_fault()));
                check(rep->score_of(0) != live->score_of(0),
                      "and it scores differently (rung " +
                      std::to_string(rep->score_of(0).rung) + " against " +
                      std::to_string(live->score_of(0).rung) + "), which is what an "
                      "unreproducible human baseline looks like");
            }
            // 2. A structurally damaged tape: one decision cut out of the
            //    middle, so every later decision lands one slot early.
            //
            // This is the case that justifies storing a tick at all, and it was
            // MEASURED rather than assumed. Run against the digger driver
            // (poss_tamper.cpp), cutting one decision out of 3,804 produced a
            // replay whose rung, blocks mined, final position AND full state
            // hash were all IDENTICAL to the live run — 14114998953413297904
            // both times — because a single dropped decision among thousands of
            // mostly-repeated ones changes nothing the agent does. Changing one
            // decision's CODE was likewise invisible. A replay checked only by
            // its score would have passed a damaged tape and called the human
            // baseline reproducible. The tick stamp is the only thing that sees
            // it, and it caught the cut at tick 2098.
            //
            // The assertion below is on the DETECTOR, not on the score, because
            // whether the score also moves depends on which decision was cut:
            // the player driver used here does diverge, the digger did not.
            {
                auto bent = good;
                bent.entries.erase(bent.entries.begin() + long(bent.entries.size() / 2));
                auto rep = std::make_unique<VC>();
                rep->possess_replay(bent);
                rep->run_quiet(bent.ticks);
                check(rep->replay_fault() >= 0,
                      "a tape with one decision cut out is caught by its tick stamps, at "
                      "tick " + std::to_string(rep->replay_fault()) +
                      ", even when it goes on to score " +
                      (rep->score_of(0) == live->score_of(0) ? "identically"
                                                             : "differently"));
            }
            // 3. And a tape whose stamps are simply wrong.
            {
                auto bent = good;
                for (auto& e : bent.entries) e.tick = 0;
                auto rep = std::make_unique<VC>();
                rep->possess_replay(bent);
                rep->run_quiet(bent.ticks);
                check(rep->replay_fault() >= 0,
                      "and so is a tape whose tick stamps have been flattened (tick " +
                      std::to_string(rep->replay_fault()) + ")");
            }
        }

        // ── 2. driving is not decoration ────────────────────────────────────
        //
        // The A/B this project runs on every control it ships. The arms differ
        // in ONE thing: whether commands are queued. Both are policy=possessed,
        // both go through possessedAction(), both have the same seed and the
        // same seven scripted neighbours.
        {
            group("voxelcity x possession: a driven agent differs from autopilot");
            int differed = 0, movedDifferently = 0;
            for (int seed = 1; seed <= 3; ++seed) {
                auto driven = town(seed, 12000);
                driven->possess(0);
                drive(*driven, diggerPlan(T), T);

                auto autop = town(seed, 0);      // idle 0 = handed back every tick
                autop->possess(0);
                autop->run_quiet(T);

                if (driven->state_hash() != autop->state_hash()) ++differed;
                const auto d = driven->score_of(0), u = autop->score_of(0);
                if (d.x != u.x || d.y != u.y || d.z != u.z || d.mined != u.mined)
                    ++movedDifferently;
            }
            check(differed == 3,
                  "all three seeds end in a different world when somebody is driving");
            check(movedDifferently == 3,
                  "and the driven agent ends somewhere else, having mined a different "
                  "number of blocks, in all three");
            // And the movement codes really reach the world. possessed_moves()
            // counts decisions spent on the six primitives; a driver made of
            // them must register them, and one made of none must not.
            auto mover = town(2, 12000);
            mover->possess(0);
            drive(*mover, diggerPlan(T), T);
            auto still = town(2, 12000);
            still->possess(0);
            drive(*still, playerPlan(T), T);
            check(mover->possessed_moves() > 0 && still->possessed_moves() == 0,
                  "the six movement primitives are counted separately from the shared "
                  "action space (" + std::to_string(mover->possessed_moves()) +
                  " for the digger, " + std::to_string(still->possessed_moves()) +
                  " for a driver that only issues shared actions)");
        }

        // ── 3. possession with nobody at the keyboard is EXACTLY scripted ───
        //
        // The other half of the A/B, and the stronger half. If the possessed
        // code path adds anything of its own — a stray rng draw, an extra
        // decision point, a missed commitment — the human baseline is measuring
        // that instead of the human. Bit-identical against policy=scripted is
        // the only way to rule it out.
        {
            group("voxelcity x possession: an undriven agent is the scripted policy, exactly");
            int identical = 0;
            for (int seed = 1; seed <= 3; ++seed) {
                auto autop = town(seed, 0);
                autop->possess(0);
                autop->run_quiet(T);
                auto scripted = town(seed, 12000);
                scripted->set_policy(1);
                scripted->run_quiet(T);
                if (autop->state_hash() == scripted->state_hash()) ++identical;
            }
            check(identical == 3,
                  "possessed-with-idle-0 reproduces policy=scripted bit for bit on three "
                  "seeds, so the wrapper contributes nothing and the difference measured "
                  "in the A/B above is the driving");
        }

        // ── 4. the idle timeout hands the body back, and takes it again ─────
        {
            group("voxelcity x possession: the idle timeout");
            auto s = town(1, 600);
            s->possess(0);
            s->possess_command(VC::kMoveBase + 5);
            s->run_quiet(300);
            const bool earlyAuto = s->possessed_auto();
            s->run_quiet(600);
            const bool lateAuto = s->possessed_auto();
            check(!earlyAuto && lateAuto,
                  "still yours at 300 ticks, handed back at 900 with the timeout at 600");
            const long long idleWhenAbandoned = s->possessed_idle();
            s->possess_command(VC::Mine);
            s->run_quiet(1);
            // <= 1, not == 0. possIdle_ is reset by the keystroke and then
            // incremented once by the tick that consumes it, so one tick after
            // a keypress the correct reading is 1 — "one tick since you last
            // typed". The first version of this asserted 0 and failed, which is
            // the assertion being wrong rather than the counter: demanding 0
            // would have meant the clock did not run on the tick the command
            // was spent, and an idle timeout that skips ticks is a worse bug
            // than the one it was written to catch.
            check(!s->possessed_auto() && s->possessed_idle() <= 1
                  && idleWhenAbandoned >= 600,
                  "and one keystroke takes it straight back — idle " +
                  std::to_string(idleWhenAbandoned) + " ticks, then " +
                  std::to_string(s->possessed_idle()));
            // An abandoned agent must not simply stand there. That is the whole
            // reason the timeout exists: a body left in a field scores zero and
            // would publish that zero as a human baseline.
            auto abandoned = town(2, 400);
            abandoned->possess(0);
            abandoned->run_quiet(6000);
            auto frozen = town(2, 1000000);   // a timeout that never fires
            frozen->possess(0);
            frozen->run_quiet(6000);
            check(abandoned->score_of(0).mined > frozen->score_of(0).mined,
                  "an abandoned agent goes back to work rather than standing still (" +
                  std::to_string(abandoned->score_of(0).mined) + " blocks against " +
                  std::to_string(frozen->score_of(0).mined) + " for one with no timeout)");
        }

        // ── 5. the camera follows, and you can see which one is you ─────────
        //
        // Two claims that are easy to write and easy to get wrong invisibly.
        // The camera target is checked against the renderer's own cell->cube
        // mapping; the marker is checked by COUNTING PIXELS in the sim's
        // rendered surface, because "there is a marker" is a statement about
        // the picture and not about the palette.
        {
            group("voxelcity x possession: the camera follows and the body is marked");
            auto s = std::make_unique<VC>();
            s->on_knob("agents", 8.f);
            s->on_knob("size",   0.f);
            s->on_knob("seed",   2.f);
            s->on_knob("speed",  1.f);       // one tick per step, so orders land on time
            s->possess(0);
            const auto c0 = s->camera_target();
            {
                auto plan = diggerPlan(1500);
                std::size_t k = 0;
                for (long long t = 0; t < 1500; ++t) {
                    while (k < plan.size() && plan[k].at == t)
                        s->possess_command(plan[k++].code);
                    s->step();               // step(), not run_quiet: publish() moves it
                }
            }
            const auto c1 = s->camera_target();
            const auto p1 = s->score_of(0);
            // The renderer walks a cube of side max(n, kHeight) and places cell
            // c at (c + 0.5) * 2/side - 1. size 0 is n = 64, kHeight is 128.
            const float k = 2.0f / 128.0f;
            const float ex = (float(p1.x) + 0.5f) * k - 1.f;
            const float ey = (float(p1.y) + 0.5f) * k - 1.f;
            const float ez = (float(p1.z) + 0.5f) * k - 1.f;
            const float err = std::fabs(c1[0]-ex) + std::fabs(c1[1]-ey) + std::fabs(c1[2]-ez);
            check(err < 1e-5f,
                  "the camera target sits exactly on the possessed agent, in the "
                  "renderer's own coordinates (error " + std::to_string(err) + ")");
            check(c0[0] != c1[0] || c0[1] != c1[1] || c0[2] != c1[2],
                  "and it MOVED — a follow that never updates looks identical to one "
                  "that works, until the agent walks away");

            check(s->possessed_index() == 0 && s->possession_live(), "and it is agent 0");

            // ── the marker, COUNTED IN PIXELS ───────────────────────────────
            //
            // "There is a marker" is a claim about the picture, not about the
            // palette, so it is settled by looking at the rendered surface.
            // Face shading scales a colour, it does not rotate its hue, so a
            // shaded amber cube is still a scalar multiple of the swatch.
            //
            // The fixture is CONTROLLED, and it had to be: the first version
            // drove the agent for 1500 ticks and then counted, which returned
            // 0 pixels on two seeds of three — not because the marker was
            // missing but because a driven agent digs, and a buried cube is not
            // visible from anywhere. That is a probe measuring the terrain. The
            // body is therefore stood on the highest column in the world, which
            // by construction has nothing above it and is never underwater, and
            // photographed from straight overhead.
            auto markerPixels = [](int seed) {
                auto v = std::make_unique<VC>();
                v->on_knob("agents", 8.f);
                v->on_knob("size",   0.f);
                v->on_knob("seed",   float(seed));
                v->on_knob("speed",  1.f);
                v->possess(0);
                int bx = 1, bz = 1, best = -1;
                for (int z = 1; z < 63; ++z)
                    for (int x = 1; x < 63; ++x) {
                        const int hh = v->world().surface(x, z);
                        if (hh > best) { best = hh; bx = x; bz = z; }
                    }
                const auto& pal = v->palette();
                const bench::Rgb you = pal[pal.size() - 1].colour;
                auto amber = [&](const bench::Surface* surf) {
                    int n = 0;
                    if (!surf) return n;
                    for (std::size_t i = 0; i + 3 < surf->rgba.size(); i += 4) {
                        const float r = surf->rgba[i], g = surf->rgba[i+1], b = surf->rgba[i+2];
                        if (r < 40) continue;
                        const float t = r / float(you.r);
                        if (std::fabs(g - t*float(you.g)) < 6.f
                         && std::fabs(b - t*float(you.b)) < 6.f) ++n;
                    }
                    return n;
                };
                auto stand = [&](VC& w) {
                    auto& ag = w.agent_for_test(0);
                    ag.x = bx; ag.z = bz; ag.y = best + 1; ag.alive = true; ag.health = 20.f;
                };
                stand(*v);
                v->camera_view(bench::Sim::StdView::Top);
                v->step();
                const int on = amber(v->surface());
                v->release();
                v->camera_view(bench::Sim::StdView::Top);
                stand(*v);
                v->step();
                return std::pair<int,int>{on, amber(v->surface())};
            };
            int litSeeds = 0, darkWhenOff = 0, onTotal = 0;
            for (int seed = 1; seed <= 3; ++seed) {
                const auto [on, off] = markerPixels(seed);
                onTotal += on;
                if (on > 20) ++litSeeds;
                if (off == 0) ++darkWhenOff;
            }
            check(litSeeds == 3,
                  "the possessed body is drawn in a colour nothing else in the world uses, "
                  "on three seeds (" + std::to_string(onTotal / 3) + " marker pixels each)");
            check(darkWhenOff == 3,
                  "and releasing it removes every one of them — the marker is the "
                  "possession, not a decoration that is always there");

            // The field marker too, which is what the timeline and the top-down
            // slice show: exactly one cell, and it is the possessed agent's.
            s->possess(3);
            s->step();
            const bench::Field& f = s->field();
            int youCells = 0, atAgent = 0;
            const auto p3 = s->score_of(3);
            for (int zz = 0; zz < f.h; ++zz)
                for (int xx = 0; xx < f.w; ++xx)
                    if (f.at(xx, zz) == bench::kBlocks + 1) {
                        ++youCells;
                        if (xx == p3.x && zz == p3.z) ++atAgent;
                    }
            check(youCells == 1 && atAgent == 1,
                  "the top-down view marks exactly one cell as yours, and it is agent 3's");
        }

        // ── 6. a human baseline, on the same axis as the machines ───────────
        //
        // The number this whole feature exists to produce. Agent 0's OWN rung,
        // not the town mean — possession drives one settler among seven scripted
        // neighbours, so a town mean would report a baseline that is
        // seven-eighths scripted. The claim is deliberately weak (a driven agent
        // is not worse than random) because the strong version depends on who is
        // driving, and the suite cannot type.
        {
            group("voxelcity x possession: a human run is scored like any other policy");
            const long long L = 12000;
            double scripted = 0, random = 0, player = 0;
            double hiS = 0, hiR = 0, hiP = 0;
            for (int seed = 1; seed <= 3; ++seed) {
                auto a = town(seed, 12000); a->set_policy(1); a->run_quiet(L);
                auto b = town(seed, 12000); b->set_policy(2); b->run_quiet(L);
                auto c = town(seed, 12000); c->possess(0); drive(*c, playerPlan(L), L);
                // On the RATE, the same axis every other policy is judged on
                // since the high-water mark was found not to separate them.
                // Agent 0's own, not the town's — see the note above.
                scripted += a->score_of(0).rung_per_life();
                random   += b->score_of(0).rung_per_life();
                player   += c->score_of(0).rung_per_life();
                hiS += a->score_of(0).rung; hiR += b->score_of(0).rung;
                hiP += c->score_of(0).rung;
            }
            check(player / 3 >= random / 3,
                  "a driven agent reaches at least as far up the tree as random on the "
                  "same three worlds (rung per life: player " + std::to_string(player / 3) +
                  ", scripted " + std::to_string(scripted / 3) +
                  ", random " + std::to_string(random / 3) + ", agent 0 only)");
            note("the same three runs on the lifetime high-water mark: player " +
                 std::to_string(hiP / 3) + ", scripted " + std::to_string(hiS / 3) +
                 ", random " + std::to_string(hiR / 3) + " — reported, not asserted");
            // And the metrics really do apply: the same accessors every other
            // policy is read through return live numbers under possession.
            auto c = town(2, 12000);
            c->possess(0);
            drive(*c, playerPlan(L), L);
            bool named = false;
            for (const auto& m : c->metrics()) if (m.name == std::string("mean rung")) named = true;
            check(named && c->tape().entries.size() > 100 && c->tape().ticks == L,
                  "possession publishes the same metrics as every other policy, and "
                  "records " + std::to_string(c->tape().entries.size()) +
                  " decisions over " + std::to_string(c->tape().ticks) + " ticks while it does");
            check(c->possessed_commands() == static_cast<long long>(playerPlan(L).size()),
                  "every keystroke the driver sent was accepted (" +
                  std::to_string(c->possessed_commands()) + ")");
        }

        // ── 7. the action space is a superset, not a replacement ────────────
        //
        // A human must not be handed powers the machines lack. The shared
        // eleven are exactly the shared eleven; possession adds six movement
        // primitives BELOW them, which applyPrimitive already provides to
        // Explore and to every Go* option.
        {
            group("voxelcity x possession: the codes are the shared action space plus moves");
            check(VC::kMoveBase == VC::kActions,
                  "the movement codes start where the shared action space ends");
            check(VC::kPossCodes == VC::kActions + 7,
                  "and there are exactly six of them plus a stand");
            check(!VC::poss_code_valid(-1) && !VC::poss_code_valid(VC::kPossCodes)
                  && VC::poss_code_valid(VC::kStand),
                  "an out-of-range code is rejected rather than clamped into a real action");
            // A rejected command must not become a standing order. Clamping a
            // bad key into a plausible action is how a mis-wired binding ships
            // looking like it works.
            auto s = town(1, 12000);
            s->possess(0);
            const long long before = s->possessed_commands();
            s->possess_command(-5);
            s->possess_command(VC::kPossCodes + 3);
            check(s->possessed_commands() == before,
                  "and neither of two invalid codes was accepted");
            // Every code has a name, because the readout prints one.
            bool allNamed = true;
            for (int cde = 0; cde < VC::kPossCodes; ++cde)
                if (std::string(VC::poss_code_name(cde)) == "?") allNamed = false;
            check(allNamed, "every possession code names itself for the readout");
        }

        // ── 8. the readout is about the agent, and tracks it ────────────────
        {
            group("voxelcity x possession: the readout is live");
            auto s = town(2, 12000);
            s->possess(0);
            drive(*s, playerPlan(2000), 2000);
            const auto lines = s->possession_readout();
            check(lines.size() >= 6, "the readout has a line for who, where, what it "
                                     "sees and what it carries");
            const auto p = s->score_of(0);
            const std::string where = "(" + std::to_string(p.x) + "," + std::to_string(p.y)
                                    + "," + std::to_string(p.z) + ")";
            bool found = false;
            for (const auto& l : lines) if (l.find(where) != std::string::npos) found = true;
            check(found, "and it reports the possessed agent's actual position " + where);
            const auto a = lines;
            drive(*s, diggerPlan(2000), 2000);
            const auto b2 = s->possession_readout();
            check(a != b2, "and it changes as the agent does, rather than being a caption");
        }
    }

    // ── Gen I battle mechanics ──────────────────────────────────────────────
    //
    // A rules engine that nobody checked against the rules is just a program
    // that produces numbers. The Gen I damage formula truncates at every
    // division, so this asserts a case worked through BY HAND rather than an
    // approximation — a floating-point version of the same formula gives
    // different answers, and "close enough" is how a rules engine ends up
    // quietly wrong.
    {
        group("Gen I battle: formula and type chart");
        using PB = bench::PokeBattle;
        using T  = bench::PType;

        // level 50, power 95, A = D = 85, special, no STAB, neutral:
        //   2*50/5 + 2          = 22
        //   (22 * 95 * 85/85)   = 2090
        //   2090/50 + 2         = 43
        bench::PSpecies A{"A", T::Normal, T::Count, 100,85,85,85,85, {}};
        bench::PSpecies D{"D", T::Normal, T::Count, 100,85,85,85,85, {}};
        bench::PMove   m{"Test", T::Water, 95, 100, true};       // Water on Normal: neutral
        check(PB::damage(50, m, A, D, 255, false) == 43,
              "damage at max roll equals the hand-computed 43");
        check(PB::damage(50, m, A, D, 217, false) == 43 * 217 / 255,
              "  and at min roll equals 43 * 217/255, the Gen I variance");

        // STAB is x1.5 with truncation.
        bench::PSpecies W{"W", T::Water, T::Count, 100,85,85,85,85, {}};
        check(PB::damage(50, m, W, D, 255, false) == 43 * 3 / 2,
              "same-type attack bonus is exactly 3/2 of the base damage");

        // The chart, including two things specific to Generation I.
        check(bench::type_effect(T::Water, T::Fire) == 200, "Water is super effective on Fire");
        check(bench::type_effect(T::Fire, T::Water) == 50,  "and Fire is resisted by Water");
        check(bench::type_effect(T::Electric, T::Ground) == 0, "Electric does nothing to Ground");
        check(bench::type_effect(T::Ghost, T::Psychic) == 0,
              "Ghost does NOTHING to Psychic — the Generation I chart, not a later one");
        check(bench::type_effect(T::Psychic, T::Psychic) == 50,
              "and Psychic resists itself, which later generations changed");
        check(bench::type_effect(T::Ice, T::Dragon) == 200, "Ice is super effective on Dragon");
    }

    // ── learning the payoff structure ───────────────────────────────────────
    //
    // Two baselines, and the second is the one that matters. Beating random
    // only shows the agent attacks. Beating GREEDY — always fire the highest
    // power number, ignoring type — is what shows it has learned which move
    // beats which defender.
    {
        group("battle agent beats both baselines");
        auto sim = bench::make_pokebattle();
        auto& B = *static_cast<bench::PokeBattle*>(sim.get());
        // 20000, not 6000, and the number was measured rather than picked.
        // Both claims below are margins over a baseline, and at 6000 battles
        // the worst seed of twenty cleared the required 0.02 by only 2.7
        // standard deviations of the seed-to-seed spread — close enough that
        // an unrelated change to the random stream could have flipped it. At
        // 20000 the same claims clear by 9.0 and 10.2. The fix for a thin
        // stochastic assertion is more evidence, not a looser threshold.
        B.run(20000);
        check(B.random_rate() > 0.4 && B.random_rate() < 0.7,
              "the random baseline sits near even, as it should against a fixed opponent");
        check(B.greedy_rate() > B.random_rate() + 0.02,
              "always firing the biggest number already beats random (" +
              std::to_string(int(100*B.greedy_rate())) + "% against " +
              std::to_string(int(100*B.random_rate())) + "%)");
        check(B.learner_rate() > B.greedy_rate() + 0.02,
              "and the learner beats THAT (" + std::to_string(int(100*B.learner_rate())) +
              "%) — it has learned the type chart, not merely learned to attack");
    }

    // ── the platformer NEAT plays ───────────────────────────────────────────
    //
    // Three properties, and the middle one is the whole reason the others are
    // worth having: the level must be COMPLETABLE, and it must not be
    // completable by holding one button. A task that fails either is a task
    // where an agent result means nothing.
    {
        group("platformer: solvable, and not trivially so");
        using P = bench::Platformer;

        // Physics against the closed form. Euler integration at a finite
        // timestep undershoots the continuous integral, so this checks the
        // apex is close to v^2/2g rather than equal to it.
        {
            std::vector<std::uint8_t> flat(std::size_t(P::kLevelW) * P::kLevelH, 0);
            for (int x = 0; x < P::kLevelW; ++x)
                for (int y = 13; y < P::kLevelH; ++y) flat[std::size_t(y)*P::kLevelW + x] = 1;
            P::Body b; b.x = 10; b.y = 12.5f; b.ground = true;
            float top = b.y;
            b = P::advance(b, false, false, true, flat);
            for (int i = 0; i < 80 && b.vy < 0; ++i) {
                b = P::advance(b, false, false, false, flat);
                top = std::min(top, b.y);
            }
            const float rose = 12.5f - top;
            const float closed = P::kJump * P::kJump / (2 * P::kGravity);
            check(rose > closed * 0.85f && rose < closed * 1.05f,
                  "jump apex is within a tenth of the closed form v^2/2g (" +
                  std::to_string(rose) + " against " + std::to_string(closed) + ")");
        }

        auto sim = bench::make_platformer();
        auto& G = *static_cast<P*>(sim.get());
        check(G.scripted_completes(),
              "the generated level is completable — checked at generation time, so a "
              "population that fails is failing at the task and not at an impossible level");

        // And NOT completable by mashing. This is the check that stops the
        // whole exercise being theatre: the first level I built could be
        // finished by holding right, with no jumping at all.
        {
            P::Body b = G.spawn();
            float far = b.x;
            for (int i = 0; i < 6000; ++i) {
                b = P::advance(b, false, true, true, G.tiles());
                if (P::dead(b)) break;
                far = std::max(far, b.x);
            }
            // Measured, terrain only: a masher reaches 80 of 214 and stops.
            // The earlier threshold of 25% was written against a collision bug
            // (int() truncating toward zero let the body leave the world), and
            // fixing that made jumping work properly — so the baseline moved
            // and the assertion had to move with it. What matters is unchanged:
            // mashing does not FINISH the level.
            check(far < float(P::kLevelW - 6) * 0.6f,
                  "holding right and jump plateaus at " + std::to_string(int(far)) +
                  " of " + std::to_string(P::kLevelW - 6) + " — it cannot finish");
        }

        // Several level seeds, all completable.
        {
            int ok = 0;
            for (int seed = 1; seed <= 6; ++seed) {
                auto s2 = bench::make_platformer();
                auto& G2 = *static_cast<P*>(s2.get());
                for (auto& k : G2.knobs()) if (k.key == "level") k.value = float(seed);
                s2->reset();
                ok += G2.scripted_completes() ? 1 : 0;
            }
            check(ok == 6, "every one of six level seeds is completable");
        }

        // NEAT makes real progress on it.
        // Measured with enemies in play: gen 4 reaches 80 (level with the
        // masher), gen 8 reaches 96, gen 12 finishes. The claim asserted is
        // that it BEATS the mindless baseline, which is the only comparison
        // that means anything.
        G.run_generations(10);
        check(G.best_distance() > 85.0f,
              "NEAT beats the button-mash baseline of 80 within ten generations (reached " +
              std::to_string(int(G.best_distance())) + ")");
    }

    // ── cart-pole: the published dynamics, checked by hand ──────────────────
    //
    // An environment nobody can verify is a place for bugs to live invisibly.
    // These check the physics against the equations at a state where the algebra
    // collapses to something computable on paper, so a wrong constant or a
    // transposed term cannot hide behind "the agent just did not learn".
    {
        group("cart-pole: dynamics match the 1983 paper");
        using CP = bench::CartPole;

        // Upright and still: sin(0) = 0 and cos(0) = 1, so temp = force/mass
        // exactly and both accelerations follow in closed form.
        CP::State s0;
        const CP::State n = CP::advance(s0, true);
        const float total = CP::kMassCart + CP::kMassPole;
        const float temp  = CP::kForce / total;
        const float thacc = (0.0f - temp) /
                            (CP::kLength * (4.0f/3.0f - CP::kMassPole / total));
        const float xacc  = temp - (CP::kMassPole * CP::kLength) * thacc / total;
        check(std::fabs(n.xd  - CP::kTau * xacc)  < 1e-6f,
              "cart velocity after one push matches the closed-form acceleration");
        check(std::fabs(n.thd - CP::kTau * thacc) < 1e-6f,
              "pole angular velocity likewise");
        check(n.thd < 0.0f, "and a push to the RIGHT tips the pole left, as it must");

        // Pushed the wrong way from a small lean, it must fall quickly.
        {
            CP::State s; s.th = 0.05f;
            int steps = 0;
            while (!CP::failed(s) && steps < 1000) { s = CP::advance(s, s.th < 0); ++steps; }
            check(steps < 60, "pushed the wrong way it falls within a second");
        }
        // And a hand-written policy must be able to balance it, or the task is
        // impossible and no agent result would mean anything.
        {
            CP::State s; int steps = 0;
            while (!CP::failed(s) && steps < 500) {
                s = CP::advance(s, s.th + s.thd * 0.5f > 0);
                ++steps;
            }
            check(steps == 500, "a naive hand-written policy balances the full 500 steps, "
                                "so the environment is solvable");
        }
    }

    // ── cart-pole: what discretisation costs ────────────────────────────────
    //
    // I expected the table to be capped by chopping a continuous state into
    // boxes, and for the network to win as a result. The measurement said
    // otherwise, and this records the correction: FOUR boxes per dimension is
    // the table's best setting, and finer is monotonically worse, because more
    // boxes means each is visited too rarely to converge.
    {
        group("cart-pole: the table's bin count is the whole story");
        auto run = [](int bins, int episodes) {
            auto sim = bench::make_cartpole();
            auto& C = *static_cast<bench::CartPole*>(sim.get());
            for (auto& k : C.knobs()) if (k.key == "bins") k.value = float(bins);
            sim->reset();
            C.train(episodes);
            return std::make_pair(C.mean_last(true, 20), C.best_table());
        };
        const auto coarse = run(4, 350);
        const auto fine   = run(12, 350);
        check(coarse.first > fine.first,
              "a COARSE table (4 bins) outperforms a fine one (12) — " +
              std::to_string(int(coarse.first)) + " steps against " +
              std::to_string(int(fine.first)) + ", the opposite of the obvious guess");
        check(coarse.second > 150,
              "  and the coarse table reaches " + std::to_string(coarse.second) +
              " steps, so it is not resolution that limits it");
    }

    // ── neural Q-learning, against the table ────────────────────────────────
    //
    // Swapping a Q-table for a network is not an upgrade with no cost. It is
    // the deadly triad — approximation, bootstrapping, off-policy — which has
    // no convergence guarantee, while the table's does. So the table runs on
    // the identical maze as a control, and the claims here are comparisons.
    //
    // Reliability, not a single snapshot, is the right measure: the network
    // reaches the goal at SOME checkpoints and not others, and one lucky
    // reading would misrepresent that completely.
    {
        group("neural Q-learning: measured against the tabular control");

        auto reliability = [](float replay, float target, int blocks, int perBlock) {
            auto sim = bench::make_neuralq();
            auto& N = *static_cast<bench::NeuralQ*>(sim.get());
            auto set = [&] {
                for (auto& k : N.knobs()) {
                    if (k.key == "replay") k.value = replay;
                    if (k.key == "target") k.value = target;
                }
            };
            set(); sim->reset(); set();
            int reached = 0, bestPath = 1 << 30, tableWorst = 0;
            for (int b = 0; b < blocks; ++b) {
                N.train(perBlock);
                const int p = N.greedy_path(false);
                if (p > 0) { ++reached; bestPath = std::min(bestPath, p); }
                tableWorst = std::max(tableWorst, N.greedy_path(true));
            }
            return std::make_tuple(reached, bestPath, tableWorst, N.shortest_path());
        };

        auto [reached, best, tableWorst, optimal] = reliability(1.f, 1.f, 8, 500);
        check(tableWorst == optimal,
              "the tabular control walks the optimal path at EVERY checkpoint — " +
              std::to_string(optimal) + " steps, as its convergence proof requires");
        check(reached > 0,
              "the network does learn a working policy: reached the goal at " +
              std::to_string(reached) + " of 8 checkpoints");
        check(best <= optimal + 4,
              "  and its best route is within a few steps of optimal (" +
              std::to_string(best) + " vs " + std::to_string(optimal) + ")");
        check(reached < 8,
              "  but NOT at every checkpoint, unlike the table — that gap is the deadly "
              "triad, and it is the reason the control is here");

        // Removing either of DQN's two devices should make it worse. Measured:
        // both present gives 8/12 reliability; without replay, without the
        // target network, or without either, it is 0/12 — the policy never
        // reaches the goal at all.
        auto [noReplay, nrBest, nrTable, nrOpt] = reliability(0.f, 1.f, 6, 500);
        (void)nrBest; (void)nrOpt;
        check(nrTable == nrOpt, "  the control is unaffected by the network's settings");
        check(noReplay < reached,
              "  without experience replay the network is less reliable still (" +
              std::to_string(noReplay) + " of 6): consecutive maze steps are almost the "
              "same state, and training on them in order is training on one example");
    }

    // ── transfer and catastrophic forgetting ────────────────────────────────
    //
    // The from-scratch control is the whole experiment. Without it, "the
    // transferred network reached task B in 420 epochs" is a number with
    // nothing to compare against, and would read as success. With it, the same
    // run shows transfer here is NEGATIVE — starting from A makes B slower.
    {
        group("continual learning: forgetting is measured, not asserted");

        auto run = [](int mode, int phase1) {
            auto sim = bench::make_continual();
            auto& C = *static_cast<bench::Continual*>(sim.get());
            for (auto& k : C.knobs()) {
                if (k.key == "taskB")  k.value = float(mode);
                if (k.key == "phase1") k.value = float(phase1);
            }
            sim->reset();
            C.run_to(C.phase1());
            const double aAtSwitch = C.acc_a();
            C.run_to(C.phase1() * 3);
            return std::make_tuple(aAtSwitch, C.acc_a(), C.acc_b(), C.acc_scratch(),
                                   C.epochs_to(0.9, false), C.epochs_to(0.9, true));
        };

        // The opposite function: B contradicts A on every example, so A should
        // be destroyed completely.
        {
            auto [aBefore, aAfter, b, ctrl, tEpochs, cEpochs] = run(1, 600);
            check(aBefore > 0.95, "task A is learned before the switch");
            check(b > 0.95 && ctrl > 0.95, "both networks learn task B");
            check(aAfter < 0.2,
                  "training on the OPPOSITE function destroys A completely — accuracy " +
                  std::to_string(aAfter));
            check(aAfter < aBefore - 0.5, "  a fall of more than fifty points, measured");
            check(tEpochs > cEpochs,
                  "  and transfer is NEGATIVE: the transferred net is slower to B than the "
                  "control, which is only visible because there is a control");
        }
        // An overlapping function should interfere less than the opposite one.
        // That ORDERING is the checkable claim; the exact numbers are not.
        {
            auto opp = run(1, 600);
            auto ovl = run(2, 600);
            check(std::get<1>(ovl) > std::get<1>(opp),
                  "an overlapping task B forgets A less than a contradicting one does");
        }
        // Affordance: predicting which moves a square permits, against the
        // maze's own walls as ground truth.
        {
            auto [aBefore, aAfter, b, ctrl, tEpochs, cEpochs] = run(3, 600);
            (void)aBefore; (void)tEpochs; (void)cEpochs;
            check(b > 0.7, "affordances are learnable: which moves a square permits, " +
                           std::to_string(int(100*b)) + "% correct against the maze itself");
            check(ctrl > 0.7, "  and the control learns them too");
            check(aAfter < 0.9, "  while the earlier task is partly overwritten");
        }
    }

    // ── the network diagram is a picture OF the network ─────────────────────
    {
        group("network diagram: wired to the model, layout is not");
        auto sim = bench::make_netviz();
        auto& N = *static_cast<bench::NetViz*>(sim.get());
        check(N.node_count() == 7, "2 inputs + 4 hidden + 1 output are all drawn");
        for (int i = 0; i < 200; ++i) sim->step();
        check(N.cases_correct() == 4, "the net it is drawing has actually learned XOR");

        // Dragging must move the picture and NOT the model. Layout is
        // presentation; if moving a node changed what the network computes, the
        // diagram would be an input rather than a view.
        const auto p0 = N.node_pos(3);
        const int  before = N.cases_correct();
        const float lossBefore = N.loss();
        check(sim->drag_begin(p0.first / 660.0f, p0.second / 420.0f), "a node can be grabbed");
        sim->drag_move(0.8f, 0.2f);
        sim->drag_end();
        const auto p1 = N.node_pos(3);
        check(p0.first != p1.first || p0.second != p1.second, "  and moves when dragged");
        check(N.cases_correct() == before && N.loss() == lossBefore,
              "  while the network computes exactly what it did before");
        check(!sim->drag_begin(0.02f, 0.02f),
              "empty space grabs nothing, so the drag falls through to the view");
    }

    // ── 3D life ─────────────────────────────────────────────────────────────
    //
    // A 26-neighbour stepper is easy to get subtly wrong and hard to eyeball,
    // because the picture is a projection. These pin it against outcomes that
    // are certain regardless of rule, so a counting bug cannot hide behind
    // "well, 3D rules are chaotic".
    {
        group("3D life: 26 neighbours, counted correctly");
        bench::Provenance p{"t","","","",bench::Replication::No,"",""};
        const int N = 20;
        const double vol = double(N) * N * N;
        auto frac = [&](bench::Life3D& s) { return double(s.live_voxels()) / vol; };

        // Survive on any count, born on an impossible one: nothing may change.
        {
            bench::Life3D s(p, 0, 26, 27, 27, N, 0.2f, bench::Life3D::Seed::Soup);
            const double before = frac(s);
            for (int i = 0; i < 12; ++i) s.step();
            check(std::fabs(frac(s) - before) < 1e-12,
                  "survive-anything / birth-never leaves the population untouched");
        }
        // Survive on an impossible count: everything must die in one step.
        {
            bench::Life3D s(p, 27, 27, 27, 27, N, 0.3f, bench::Life3D::Seed::Soup);
            s.step();
            check(s.live_voxels() == 0, "survive-never empties the volume in a single step");
        }
        // Born on any count: the volume must fill in one step.
        {
            bench::Life3D s(p, 0, 26, 0, 26, N, 0.05f, bench::Life3D::Seed::Soup);
            s.step();
            check(s.live_voxels() == std::size_t(vol), "birth-on-anything fills the volume at once");
        }
        // Bays' 5766 from a solid 5-cube settles to a stable structure. This is
        // the published behaviour — a rule that neither dies nor explodes — and
        // the exact count is what makes it a test rather than an impression.
        {
            auto sim = bench::make_life3d_5766(28);
            auto& L = *static_cast<bench::Life3D*>(sim.get());
            for (int i = 0; i < 60; ++i) L.step();
            const std::size_t n = L.live_voxels();
            check(n > 0, "Bays 5766 from a solid 5-cube does not die out");
            const std::size_t before = n;
            for (int i = 0; i < 25; ++i) L.step();
            check(L.live_voxels() == before, "  and has settled — population is stable");
        }
        // The projection must actually draw something, and only inside the
        // palette it published.
        {
            auto sim = bench::make_life3d_5766(28);
            for (int i = 0; i < 20; ++i) sim->step();
            std::size_t lit = 0, bad = 0;
            for (auto c : sim->field().cells) {
                if (c) ++lit;
                if (c >= sim->palette().size()) ++bad;
            }
            check(lit > 0, "the 3D projection puts pixels on the screen");
            check(bad == 0, "  and never emits an index outside its own palette");
        }
    }

    // ── the 3D camera ───────────────────────────────────────────────────────
    //
    // Sliders for yaw and pitch are not camera controls. These assert the
    // orbit camera does what an orbit camera has to: turn, pan its pivot,
    // dolly, and — the one that is easy to get wrong — REFUSE a pick that hit
    // nothing, rather than sending the view to an arbitrary point in space.
    {
        group("3D camera: orbit, pan, dolly, pick");
        auto sim = bench::make_life3d_clouds(32);
        auto& L = *static_cast<bench::Life3D*>(sim.get());
        for (int i = 0; i < 40; ++i) sim->step();

        auto hashSurface = [&] {
            std::uint64_t h = 1469598103934665603ull;
            for (auto b : sim->surface()->rgba) { h ^= b; h *= 1099511628211ull; }
            return h;
        };

        const std::uint64_t before = hashSurface();
        check(sim->camera_orbit(60.f, 20.f), "orbit is accepted");
        check(hashSurface() != before, "  and actually changes the rendered image");

        const bench::Camera c0 = L.camera();
        sim->camera_pan(50.f, -30.f);
        const bench::Camera c1 = L.camera();
        check(c0.tx != c1.tx || c0.ty != c1.ty || c0.tz != c1.tz,
              "pan moves the pivot through the world, not the image");

        const float d0 = L.camera().distance;
        sim->camera_dolly(3.f);
        check(L.camera().distance < d0, "dolly moves the eye toward the pivot");

        check(sim->camera_pick(0.5f, 0.5f), "a pick through the middle of the mass hits");
        const bench::Camera hit = L.camera();
        check(hit.tx != 0.0f || hit.ty != 0.0f || hit.tz != 0.0f,
              "  and moves the pivot off the origin, onto the voxel");

        check(!sim->camera_pick(0.02f, 0.02f), "a pick into empty space misses");
        const bench::Camera after = L.camera();
        check(after.tx == hit.tx && after.ty == hit.ty && after.tz == hit.tz,
              "  and leaves the pivot exactly where it was, rather than guessing");

        sim->camera_home();
        check(L.camera().tx == 0.f && L.camera().ty == 0.f && L.camera().tz == 0.f,
              "home returns the pivot to the origin");

        // ── the CAD conventions ────────────────────────────────────────────
        //
        // Zoom-to-cursor is the one that has to be exactly right: zooming at
        // the centre of the view must move the pivot ALONG the view axis and
        // not a millimetre across it. Measuring the raw pivot delta says
        // nothing — travelling forward changes x and z too unless the camera
        // happens to be axis-aligned. The perpendicular component is the test.
        {
            sim->camera_home();
            const bench::Camera a = L.camera();
            float f[3]; a.forward(f);
            sim->camera_dolly(4.f, 0.5f, 0.5f);
            const bench::Camera b = L.camera();
            const float d[3] = { b.tx - a.tx, b.ty - a.ty, b.tz - a.tz };
            const float along = d[0]*f[0] + d[1]*f[1] + d[2]*f[2];
            const float perp[3] = { d[0]-along*f[0], d[1]-along*f[1], d[2]-along*f[2] };
            const float pm = std::sqrt(perp[0]*perp[0] + perp[1]*perp[1] + perp[2]*perp[2]);
            check(b.distance < a.distance, "wheel forward zooms in");
            check(pm < 0.05f, "  zooming at the view centre does not slide the view sideways");
        }
        {
            sim->camera_home();
            const bench::Camera a = L.camera();
            sim->camera_dolly(4.f, 0.25f, 0.30f);
            const bench::Camera b = L.camera();
            const float moved = std::sqrt((b.tx-a.tx)*(b.tx-a.tx) + (b.ty-a.ty)*(b.ty-a.ty)
                                        + (b.tz-a.tz)*(b.tz-a.tz));
            check(moved > 0.05f, "zooming off-centre pulls the pivot toward the POINTER");
        }

        using V = bench::Sim::StdView;
        const float DEG = 180.0f / 3.14159265f;
        sim->camera_view(V::Front);
        check(std::fabs(L.camera().yaw) < 1e-4f && std::fabs(L.camera().pitch) < 1e-4f,
              "front view is square on");
        sim->camera_view(V::Right);
        check(std::fabs(L.camera().yaw * DEG - 90.0f) < 0.1f, "right view is 90 degrees round");
        sim->camera_view(V::Top);
        check(L.camera().pitch * DEG > 88.0f,
              "top view looks straight down (clamped just short of 90 to avoid degeneracy)");
        sim->camera_view(V::Iso);
        check(std::fabs(L.camera().yaw * DEG - 45.0f) < 0.1f &&
              std::fabs(L.camera().pitch * DEG - 35.26f) < 0.5f,
              "isometric is the real 45/35.26 degrees, not an approximation");

        check(!L.camera().ortho, "starts in perspective");
        sim->camera_ortho(true);
        check(L.camera().ortho && sim->camera_is_ortho(), "parallel projection can be turned on");
        check(!sim->surface()->empty(), "  and still renders");
        sim->camera_ortho(false);

        // Fit must always produce something usable, including when there is
        // nothing to frame — it is the control you reach for when the view has
        // got away from you.
        check(sim->camera_fit(), "zoom-to-fit is accepted");
        check(L.camera().distance > 0.1f && L.camera().distance < 14.0f,
              "  and leaves the camera at a sane distance");
    }

    // ── the renderer must not invent or destroy structure ───────────────────
    //
    // Zoomed out, the old renderer point-sampled: it kept one cell per
    // destination pixel and threw the rest away. That is not a cosmetic
    // difference. A one-cell-wide line rendered as a full line or as NOTHING
    // depending only on which row it sat in, and most isolated cells simply
    // disappeared. These assert the two properties that matter: nothing
    // vanishes when zoomed out, and nothing is blurred when zoomed in.
    {
        group("rendering: honest at every scale");
        const std::vector<bench::Swatch> pal{{{0,0,0},"dead"},{{255,255,255},"alive"}};
        bench::View v; v.grid = false; v.trail = 0.0f;

        auto lit_pixels = [&](const bench::Field& f, int surface) {
            bench::Raster r; r.resize(surface, surface);
            r.draw(f, pal, v);
            std::size_t n = 0;
            for (int i = 0; i < surface * surface; ++i) if (r.pixels()[i*4]) ++n;
            return n;
        };

        // The parity case, which is the one that proves it.
        const int S = 1024, SURF = 400;
        bench::Field a(S, S), b(S, S);
        for (int x = 0; x < S; ++x) { a.set(x, S/2, 1); b.set(x, S/2 + 1, 1); }
        const std::size_t la = lit_pixels(a, SURF), lb = lit_pixels(b, SURF);
        check(la > 0 && lb > 0,
              "a one-cell line is visible whichever row it is on, zoomed out 2.6x");
        check(la == lb, "  and equally visible — the row it sits on cannot change whether it exists");

        // Isolated cells must survive being zoomed out.
        bench::Field dots(S, S);
        int placed = 0;
        for (int i = 0; i < 40; ++i) { dots.set(37 + i*23, 61 + i*19, 1); ++placed; }
        check(lit_pixels(dots, SURF) >= std::size_t(placed),
              "40 isolated cells all still reach the screen (point sampling lost 37 of them)");

        // Zoomed IN, nothing may be blurred: a single live cell must produce
        // exactly the palette colour, never an average with its neighbours.
        {
            bench::Field one(8, 8);
            one.set(4, 4, 1);
            bench::Raster r; r.resize(160, 160);       // 20 px per cell
            r.draw(one, pal, v);
            bool sawPure = false, sawMuddy = false;
            for (int i = 0; i < 160*160; ++i) {
                const std::uint8_t p = r.pixels()[i*4];
                if (p == 255) sawPure = true;
                else if (p != 0) sawMuddy = true;
            }
            check(sawPure, "zoomed in, a live cell renders at full palette colour");
            check(!sawMuddy, "  and nothing in between — no filtering, so a glider stays a glider");
        }
    }

    // ── pattern I/O ─────────────────────────────────────────────────────────
    {
        group("RLE patterns round-trip and behave");

        // The strong test is not "it parsed". It is that a glider written in
        // the published notation, loaded through the reader, then simulated,
        // MOVES LIKE A GLIDER — (1,1) every four generations. That exercises
        // the header, the run lengths, the row terminators and the coordinate
        // convention together, and a mistake in any one of them breaks it.
        {
            const std::string glider = "x = 3, y = 3, rule = B3/S23\nbob$2bo$3o!\n";
            auto p = bench::parse_rle(glider, "inline");
            check(p.ok, "a glider in published RLE notation parses");
            check(p.w == 3 && p.h == 3, "  and reports 3x3");
            check(p.rule == "B3/S23", "  and carries its rule through the header");

            auto sim = bench::make_life(48);
            auto& L = as_life(sim);
            L.clear();
            const std::size_t placed = bench::paste(*L.editable(), p, 10, 10);
            check(placed == 9, "  pastes all nine of its cells");
            const Cells start = live_cells(L.field());
            check(start.size() == 5, "  five live cells, as a glider has");
            for (int i = 0; i < 4; ++i) L.step();
            check(live_cells(L.field()) == shifted(start, 1, 1),
                  "  and after loading it MOVES like a glider: (1,1) in 4 gens");
        }

        // Round-trip every sim, including the ones with more than two states —
        // Langton's Loops uses eight, and those need the two-character codes.
        for (auto& e : bench::registry()) {
            auto sim = e.make();
            for (int i = 0; i < 50; ++i) sim->step();
            const bench::Field& f = sim->field();
            const std::string rle = bench::to_rle(f, "", sim->about().title, sim->generation());
            auto back = bench::parse_rle(rle, "roundtrip");
            check(back.ok && back.w == f.w && back.h == f.h && back.cells == f.cells,
                  e.id + ": field survives an RLE round-trip byte for byte");
        }

        // Clipping. An unclipped paste is an out-of-bounds write, and this
        // project has already shipped that bug once — Wireworld's demo circuit
        // laid out for a full-size field, written into a 40x20 test grid.
        {
            bench::Field small(8, 8);
            bench::Pattern big; big.w = 20; big.h = 20;
            big.cells.assign(400, 1); big.ok = true;
            check(bench::paste(small, big, 0, 0) == 64, "an oversized pattern clips to the field");
            // 20x20 at (-5,-5) still covers the whole 8x8 field, so 64 is
            // right here — the earlier expectation of 9 was the test being
            // wrong, not the clip. A partial overlap needs a small pattern.
            check(bench::paste(small, big, -5, -5) == 64, "a large pattern at a negative offset still fills");
            bench::Pattern tiny; tiny.w = 4; tiny.h = 4; tiny.cells.assign(16, 1); tiny.ok = true;
            check(bench::paste(small, tiny, -2, -2) == 4,  "a partial overlap places only the visible quarter");
            check(bench::paste(small, tiny,  6,  6) == 4,  "and the same off the far corner");
            check(bench::paste(small, big, 100, 100) == 0, "one placed entirely outside places nothing");
            check(bench::paste(small, big, -100, 0) == 0, "and off the near edge likewise");
        }

        // Rejections and provenance honesty.
        check(!bench::parse_rle("just some text\n").ok, "rejects a file with no header");
        check(!bench::parse_rle("x = 0, y = 0\n!").ok, "rejects a zero-sized header");
        {
            auto anon = bench::parse_rle("x = 1, y = 1\no!\n");
            check(anon.ok && !anon.cited(),
                  "a pattern with no comment lines is reported as uncited, not as anonymous-but-fine");
            auto named = bench::parse_rle("#N Gosper glider gun\nx = 1, y = 1\no!\n");
            check(named.cited() && named.title() == "Gosper glider gun",
                  "and one with #N carries its name through");
        }
    }

    // ── identifying what a pattern is ───────────────────────────────────────
    //
    // Each of these has a documented period and displacement. The test is
    // whether the MEASUREMENT reproduces the catalogue — asserting the exact
    // numbers, not just that something was detected, because a classifier that
    // returns a plausible wrong period is worse than one that returns nothing.
    {
        group("identify: period and displacement match the catalogue");
        using K = bench::Identity::Kind;

        auto run = [](const char* rle) {
            auto p = bench::parse_rle(rle);
            auto sim = bench::make_life(64);
            auto& L = *static_cast<bench::LifeLike*>(sim.get());
            L.clear();
            bench::paste(*L.editable(), p, 28, 28);
            return bench::identify(*sim, 300);
        };

        auto blockId = run("x = 2, y = 2\n2o$2o!");
        check(blockId.kind == K::StillLife && blockId.period == 1,
              "block is a still life");

        auto blinker = run("x = 3, y = 1\n3o!");
        check(blinker.kind == K::Oscillator && blinker.period == 2,
              "blinker is an oscillator of period 2");

        auto toad = run("x = 4, y = 2\nb3o$3o!");
        check(toad.kind == K::Oscillator && toad.period == 2, "toad: period 2");

        auto beacon = run("x = 4, y = 4\n2o2b$2o2b$2b2o$2b2o!");
        check(beacon.kind == K::Oscillator && beacon.period == 2, "beacon: period 2");

        // The one that matters: a glider must come back as a SPACESHIP with the
        // exact displacement, which is only detectable because the hash is
        // normalised to the bounding box.
        auto glider = run("x = 3, y = 3\nbob$2bo$3o!");
        check(glider.kind == K::Spaceship, "glider is a spaceship");
        check(glider.period == 4, "  period 4");
        check(glider.dx == 1 && glider.dy == 1, "  displacing exactly (1,1) per period");
        check(glider.population == 5, "  and it is 5 cells throughout");

        auto lwss = run("x = 5, y = 4\nbo2bo$o4b$o3bo$4o!");
        check(lwss.kind == K::Spaceship && lwss.period == 4,
              "lightweight spaceship: period 4");
        check(lwss.dy == 0 && (lwss.dx == 2 || lwss.dx == -2),
              "  travelling orthogonally, exactly two cells per period");

        // The r-pentomino is the long one: over a thousand generations before
        // it settles. Inside a short budget the honest answer is that nothing
        // has repeated yet.
        auto rpent = run("x = 3, y = 3\nb2o$2ob$bob!");
        check(rpent.kind == K::Unresolved,
              "r-pentomino has not repeated within 300 generations, and says so");

        // Given room it settles into the ash of still lifes and blinkers it is
        // known for — globally an oscillator of period 2. Seeing that needs the
        // ABSOLUTE field hash: by then the pattern touches the border, and a
        // bounding-box reading alone would refuse to classify it.
        {
            auto p = bench::parse_rle("x = 3, y = 3\nb2o$2ob$bob!");
            auto sim2 = bench::make_life(64);
            auto& L2 = *static_cast<bench::LifeLike*>(sim2.get());
            L2.clear();
            bench::paste(*L2.editable(), p, 28, 28);
            const auto late = bench::identify(*sim2, 2000);
            check(late.kind == K::Oscillator && late.period == 2,
                  "  and within 2000 generations it settles to a period-2 ash");
        }

        // The border check still applies where it should: to a MOVING pattern
        // whose displacement the wrap has made meaningless.
        {
            auto p = bench::parse_rle("x = 3, y = 3\nbob$2bo$3o!");
            auto sim2 = bench::make_life(16);
            auto& L2 = *static_cast<bench::LifeLike*>(sim2.get());
            L2.clear();
            // Placed ON the edge, so the very first reading is already across
            // the seam. A glider merely CLOSE to an edge is caught cleanly at
            // generation 4, well before it wraps — measured: on a 16-cell field
            // starting at (6,6) it comes out as a spaceship (1,1), and only
            // starting at (0,0) is the displacement genuinely unreadable.
            bench::paste(*L2.editable(), p, 0, 0);
            const auto wrapped = bench::identify(*sim2, 400);
            check(wrapped.kind == K::Unreliable,
                  "a glider sitting across the seam is reported unreliable, not measured");
        }
        {
            auto p = bench::parse_rle("x = 3, y = 3\nbob$2bo$3o!");
            auto sim2 = bench::make_life(16);
            auto& L2 = *static_cast<bench::LifeLike*>(sim2.get());
            L2.clear();
            bench::paste(*L2.editable(), p, 6, 6);
            const auto clean = bench::identify(*sim2, 400);
            check(clean.kind == K::Spaceship && clean.dx == 1 && clean.dy == 1,
                  "  while the same glider clear of the edge is measured normally");
        }

        auto dead = run("x = 1, y = 1\no!");
        check(dead.kind == K::Empty, "a lone cell dies and is reported empty");
    }

    // ── rulestrings ─────────────────────────────────────────────────────────
    //
    // The parser is checked against implementations that were written by hand
    // and already verified against their published behaviour. Byte equality
    // over 200 generations from an identical seed is the whole test: matching
    // four independent implementations exactly leaves nowhere for a parser bug
    // to hide. Anything weaker (spot-checking a few cells, comparing
    // populations) would have passed the version that ran Life for every
    // Generations rule.
    {
        group("rulestrings parse to the same rule, cell for cell");

        auto identical = [](const char* rulestr, bench::SimPtr ref,
                            float dens, std::uint64_t seed, const char* name) {
            auto got = bench::make_rule(rulestr, 128, dens, seed);
            if (!got) {
                check(false, std::string(name) + ": '" + rulestr + "' failed to parse: "
                             + bench::parse_rule(rulestr).error);
                return;
            }
            for (int i = 0; i < 200; ++i) { got->step(); ref->step(); }
            check(got->field().cells == ref->field().cells,
                  std::string(name) + " == " + rulestr + " after 200 gens, cell for cell");
        };
        identical("B3/S23",       bench::make_life(128),        0.28f,  0xC0FFEEull, "Life");
        identical("23/3",         bench::make_life(128),        0.28f,  0xC0FFEEull, "Life (old notation)");
        identical("b3/s23",       bench::make_life(128),        0.28f,  0xC0FFEEull, "Life (lowercase)");
        identical("S23/B3",       bench::make_life(128),        0.28f,  0xC0FFEEull, "Life (reversed fields)");
        identical("B36/S23",      bench::make_highlife(128),    0.28f,  0xC0FFEEull, "HighLife");
        identical("B2/S",         bench::make_seeds(128),       0.004f, 0xC0FFEEull, "Seeds");
        identical("B3678/S34678", bench::make_day_night(128),   0.5f,   0xC0FFEEull, "Day & Night");
        identical("B2/S/3",       bench::make_brains_brain(128),0.12f,  0xB2A1u,     "Brian's Brain");

        // Canonical round-trip: however it was typed, it reports one form.
        check(bench::parse_rule("23/3").text == "B3/S23",   "23/3 canonicalises to B3/S23");
        check(bench::parse_rule("S23/B3").text == "B3/S23", "field order does not matter");
        check(bench::parse_rule("B2/S/3").text == "B2/S/3", "Generations keeps its state count");

        // Rejections. A rule that cannot be parsed must FAIL, never quietly
        // become a different rule — make_rule used to fall back to B3/S23, so a
        // user typing a Generations rule got a window full of Life with no
        // indication anything had gone wrong.
        check(!bench::parse_rule("B9/S23").ok,   "rejects a neighbour count above 8");
        check(!bench::parse_rule("B3/S23/x").ok, "rejects a non-numeric state count");
        check(!bench::parse_rule("").ok,         "rejects an empty rule");
        check(!bench::parse_rule("B3/S2/3/4").ok,"rejects too many fields");
        check(!bench::parse_rule("B0/S23").ok,   "rejects B0: no quiescent state, the vacuum ignites");
        check(bench::make_rule("B9/S23") == nullptr, "make_rule returns null rather than substituting");

        // Provenance honesty: a rule nobody published must not borrow a citation.
        const auto known   = bench::provenance_for(bench::parse_rule("B3/S23"));
        const auto unknown = bench::provenance_for(bench::parse_rule("B37/S1348"));
        check(known.citation.find("Gardner") != std::string::npos,
              "a recognised rulestring carries its real citation");
        check(unknown.citation.empty() && unknown.who == "user-specified",
              "an unrecognised rulestring claims no author and no citation");
        check(unknown.replication_note.find("Unknown") != std::string::npos,
              "and reports its replication verdict as unknown, not as a finding");
    }

    // ── the brush answers on every sim ──────────────────────────────────────
    //
    // This exists because it did not. The workbench drove the brush through
    // poke(), which is optional, and only four sims implemented it — so on the
    // other nine a stroke silently did nothing and the brush looked broken.
    // Nothing failed; there was simply no assertion that clicking the canvas
    // was supposed to have an effect.
    //
    // Every sim must now offer one of the two paths, and the path it offers has
    // to actually change the state that steps.
    {
        group("brush: every sim responds");
        for (auto& e : bench::registry()) {
            auto s2 = e.make();
            const auto before = s2->field().cells;

            if (bench::Field* ed = s2->editable()) {
                // The grid it hands out must be the grid it renders, or painting
                // lands somewhere the user cannot see.
                check(ed->w == s2->field().w && ed->h == s2->field().h,
                      std::string(e.id) + ": editable() matches the rendered field");
                const std::uint8_t v = s2->paint_value();
                for (int y = 0; y < std::min(4, ed->h); ++y)
                    for (int x = 0; x < std::min(4, ed->w); ++x) ed->set(x, y, v);
                check(s2->field().cells != before,
                      std::string(e.id) + ": painting changes the rendered field");
            } else {
                // No grid to paint: it must at least implement the stamp, and
                // the stamp must do something.
                const bool took = s2->poke(0.5f, 0.5f);
                check(took, std::string(e.id) + ": no editable() so it must implement poke()");
                if (took) {
                    // Particle sims publish on step, so compare after one. The
                    // comparison covers the rendered surface as well, for the
                    // same reason the trajectory hash does.
                    s2->step();
                    auto fresh = e.make(); fresh->step();
                    bool differs = s2->field().cells != fresh->field().cells;
                    if (!differs && s2->surface() && fresh->surface())
                        differs = s2->surface()->rgba != fresh->surface()->rgba;
                    check(differs, std::string(e.id) + ": the stamp changes the outcome");
                }
            }
        }
    }

    // ── every control must actually control something ───────────────────────
    //
    // A slider that moves and changes nothing is worse than no slider: it makes
    // a false claim about what the simulation is sensitive to, and you cannot
    // tell it from a working one by looking. So each knob is driven to both
    // ends and the resulting fields must differ.
    //
    // Reset-only knobs (soup density, seed, colony count) are reset in between,
    // because that is exactly what the workbench does when the drag ends —
    // testing them without the reset would assert the opposite of the truth.
    {
        group("every knob changes the outcome");
        auto run_with = [](const bench::Entry& e, const bench::Knob& spec, float v, int steps) {
            auto s2 = e.make();
            bool onReset = false;
            // Satisfy the dependency FIRST, so a knob that only bites under a
            // particular scheme is judged where it can be seen. Scaling under
            // tournament selection is not a dead control, it is an inert one,
            // and the two need telling apart.
            // Follow the chain, so a knob that needs two conditions at once
            // gets both. Bounded, because a cycle in the declarations would
            // otherwise hang the suite rather than fail it.
            std::string need = spec.requires_key;
            float needVal = spec.requires_value;
            for (int hop = 0; hop < 4 && !need.empty(); ++hop) {
                std::string nextNeed; float nextVal = 0.0f;
                for (auto& k : s2->knobs())
                    if (k.key == need) {
                        k.value = needVal;
                        if (k.on_reset) onReset = true;
                        nextNeed = k.requires_key; nextVal = k.requires_value;
                    }
                s2->on_knob(need, needVal);
                need = nextNeed; needVal = nextVal;
            }
            for (auto& k : s2->knobs())
                if (k.key == spec.key) { onReset = onReset || k.on_reset; k.value = v; }
            s2->on_knob(spec.key, v);
            if (onReset) s2->reset();
            // A sim with an epoch is measured in epochs. See trajectory_epochs.
            // Six, not three. The gridworld's discount factor cannot show up
            // before the agent has first reached the goal — until then every
            // reward is the same step cost and there is no value gradient for
            // gamma to discount. Measured: gamma 0.5 against 0.999 gives
            // identical runs at 1 and 3 episodes and diverges at 6.
            //
            // A `slow` knob gets a longer horizon rather than an exemption: the
            // platformer's complexity cap cannot bind before its population has
            // grown into it, measured at about generation 25.
            if (s2->epoch_name())
                return trajectory_epochs(*s2, spec.slow ? 25 : 6, !spec.display_only);
            return trajectory(*s2, steps);
        };
        // Sample ACROSS the range, not just at the two ends. Comparing only the
        // endpoints failed five working knobs, because both ends of a soup
        // density land in the same absorbing state: Life from an empty field
        // stays empty, and Life from a FULLY live field dies on the first step
        // (every cell has eight neighbours, and S23 kills all of them). The
        // knob was doing its job; the comparison was picking the one pair of
        // values that cannot tell.
        //
        // A genuinely dead knob gives the same field at every sample. A working
        // one differs somewhere.
        for (auto& e : bench::registry()) {
            auto probe = e.make();
            const std::size_t nk = probe->knobs().size();
            for (std::size_t ki = 0; ki < nk; ++ki) {
                auto fresh = e.make();
                const bench::Knob k = fresh->knobs()[ki];
                // Short on purpose. The trajectory hash catches a difference
                // at whatever generation it first appears, so extra steps add
                // cost and no sensitivity — and the 3D sims are 26-neighbour
                // volumes where a long sweep is billions of operations.
                const int steps = 24;
                std::vector<std::uint64_t> seen;
                bool differs = false;
                for (int t = 0; t <= 3 && !differs; ++t) {
                    const float v = k.min + (k.max - k.min) * float(t) / 3.0f;
                    const std::uint64_t out = run_with(e, k, k.quantised(v), steps);
                    // BENCH_KNOB_TRACE=<simId> prints every sample and its
                    // hash. "knob 'size' changes the outcome" failing tells you
                    // nothing about WHY — whether one sample is odd or all four
                    // are identical, and which values were even tried after
                    // quantising. Reconstructing that by hand outside the suite
                    // took longer than the fix, and got a different answer,
                    // because the sim file was being edited underneath it.
                    if (const char* tr = std::getenv("BENCH_KNOB_TRACE"); tr && e.id == tr)
                        std::printf("      TRACE %s.%s t=%d v=%.4f q=%.4f -> %016llx\n",
                                    e.id.c_str(), k.key.c_str(), t, double(v),
                                    double(k.quantised(v)), (unsigned long long)out);
                    for (auto prev : seen) if (prev != out) { differs = true; break; }
                    seen.push_back(out);
                }
                if (k.display_only) {
                    // The opposite claim, and the stronger one. A knob that
                    // only sets how fast you watch must leave the epoch
                    // outcome untouched; if it does not, the sim is quietly
                    // simulating something different at a different frame
                    // rate, which is a real bug and an easy one to miss.
                    if (probe->epoch_name())
                        check(!differs, e.id + ": display knob '" + k.key +
                                        "' does NOT change the outcome, only the rate");
                    // Without an epoch there is no frame-independent outcome
                    // to compare against, so there is nothing honest to assert.
                } else {
                    check(differs, e.id + ": knob '" + k.key + "' changes the outcome");
                }
            }
        }
    }

    // ── every rule switch must change the rule ──────────────────────────────
    //
    // Same standard as the knobs, applied to the controls that matter more.
    // These edit the transition function itself, so a dead one is a claim that
    // the rule has a clause it does not have.
    {
        group("every rule switch changes the rule");
        for (auto& e : bench::registry()) {
            auto probe = e.make();
            const std::size_t n = probe->switches().size();
            for (std::size_t si = 0; si < n; ++si) {
                auto base = e.make();
                const bench::Switch sw = base->switches()[si];
                // A rule change shows up within a few generations — it alters
                // the transition function, not a slow-building statistic. 40 is
                // ample, and 160 made the suite take minutes: there are 80 of
                // these and each one runs the sim twice.
                const int steps = 40;

                // Start from a DENSE soup covering every state, not from the
                // sim's own initial condition.
                //
                // Nine switches failed against the default start, and all for
                // the same reason: the clause was never reached. Seeds begins
                // at 0.4% density, so no cell ever has seven live neighbours;
                // Wireworld's demo circuit is thin wire, so no conductor ever
                // sees three heads at once. The rule genuinely changed and the
                // observation could not see it. A saturated field makes every
                // neighbour count reachable, which is what "does this clause do
                // anything" actually requires.
                auto run = [&](bool on) {
                    auto s2 = e.make();
                    if (bench::Field* f = s2->editable()) {
                        const std::uint8_t states = std::uint8_t(s2->palette().size());
                        std::uint64_t r = 0x9E3779B97F4A7C15ull;   // fixed: both runs identical
                        for (auto& c : f->cells) {
                            r ^= r << 13; r ^= r >> 7; r ^= r << 17;
                            c = std::uint8_t((r >> 33) % states);
                        }
                    }
                    s2->on_switch(sw.key, on);
                    return trajectory(*s2, steps);
                };
                check(run(false) != run(true),
                      e.id + ": switch '" + sw.group + " " + sw.label + "' changes the rule");
            }
        }
    }

    // ── editing a rule must not keep another rule's name ────────────────────
    //
    // The bench refuses false attribution everywhere else, and a live rule
    // editor is the easiest place to introduce it: switch B6 on and you are no
    // longer running Conway's rule, whatever the title still says.
    {
        group("editing a rule re-attributes it honestly");
        auto life = bench::make_life(128);
        check(life->about().title == "Conway's Game of Life", "starts as Conway's");

        life->on_switch("B6", true);
        check(life->about().title == "HighLife", "B3/S23 + B6 is named HighLife, not Conway's");

        auto hl = bench::make_highlife(128);
        auto l2 = bench::make_life(128);
        l2->on_switch("B6", true);
        for (int i = 0; i < 200; ++i) { hl->step(); l2->step(); }
        check(hl->field().cells == l2->field().cells,
              "and BEHAVES as HighLife, cell for cell — the name follows the rule");

        auto l3 = bench::make_life(128);
        l3->on_switch("S8", true);
        check(l3->about().who == "user-specified" && l3->about().citation.empty(),
              "an unnamed edit claims no author and no citation");
        l3->on_switch("S8", false);
        check(l3->about().title == "Conway's Game of Life",
              "switching back restores the original attribution");
    }

    // ── knob extremes must not produce non-finite state ─────────────────────
    //
    // Every knob range is reachable by dragging a slider, so every knob range
    // has to be survivable. Particle Life's friction knob goes to 0, at which
    // point velocity integrates force with no damping and a particle can leave
    // the world faster than a single-shot wrap can bring it back. Once out,
    // wrap_delta on two out-of-range values reaches inf - inf = NaN — and NaN
    // fails BOTH comparisons of a range check, so the guard that should discard
    // it passes it through and it spreads to every particle.
    {
        group("knob extremes stay finite");
        // The observable signal. splat() bounds-checks, so a particle that has
        // gone NaN or left the world simply fails to rasterise — it does not
        // corrupt anything visible. That means "the field still has particles
        // in it" is exactly the assertion with teeth: if positions go bad, the
        // screen goes empty. Checking cell indices alone would pass regardless.
        auto still_populated = [](const bench::Sim& s) {
            std::size_t live = 0;
            for (auto c : s.field().cells) if (c) ++live;
            return live > 0;
        };
        // Driving ALL knobs to one end is the wrong sweep, and the first
        // version of this test proved it by passing without the fix: setting
        // every knob to its minimum also sets Particle Life's force to 0, so
        // nothing moves and nothing can blow up. The dangerous settings are
        // MIXED — one knob at an extreme while the rest stay live. So drive
        // each knob to each end on its own, leaving the others at default.
        // Scoped to the CONTINUOUS sims on purpose. This test exists because a
        // particle can leave the world and go non-finite, which shows up as an
        // empty screen — it was written for exactly that. Applied to a lattice
        // or 3D rule it asserts something false: a rule whose survive band is
        // set to an impossible count SHOULD empty the field, and demanding
        // otherwise makes correct behaviour look like a bug.
        for (auto& e : bench::registry()) {
            if (e.id != "boids" && e.id != "particles" && e.id != "pps") continue;
            auto probe = e.make();
            const std::size_t nk = probe->knobs().size();
            if (!nk) continue;
            for (std::size_t ki = 0; ki < nk; ++ki)
                for (int end = 0; end < 2; ++end) {
                    auto s2 = e.make();
                    const std::string key = s2->knobs()[ki].key;
                    const float v = end ? s2->knobs()[ki].max : s2->knobs()[ki].min;
                    s2->on_knob(key, v);
                    for (int i = 0; i < 400; ++i) s2->step();
                    char lbl[180];
                    std::snprintf(lbl, sizeof lbl, "%s: still populated with %s=%g",
                                  e.id.c_str(), key.c_str(), double(v));
                    check(still_populated(*s2), lbl);
                }
        }
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
