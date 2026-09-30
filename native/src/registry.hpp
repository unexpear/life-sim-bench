// registry.hpp — the roster.
//
// One list. The bench builds its index from this, the self-test walks it, and
// adding a simulation means adding one line. Order is the order they appear.

#pragma once
#include "sim.hpp"
#include "sims/life_like.hpp"
#include "sims/von_neumann.hpp"
#include "sims/small_lattice.hpp"
#include "sims/continuous.hpp"
#include "sims/collision.hpp"
#include "sims/satisfy.hpp"
#include "sims/satisfy_more.hpp"
#include "sims/sand.hpp"
#include "sims/machines.hpp"
#include "sims/fly_arena.hpp"
#include "sims/pps.hpp"
#include "sims/langton_loops.hpp"
#include "sims/spatial_pd.hpp"
#include "sims/my_sim.hpp"
#include "sims/rulespec.hpp"
#include "sims/life3d.hpp"
#include "sims/gridworld.hpp"
#include "sims/netviz.hpp"
#include "sims/continual.hpp"
#include "sims/neuralq.hpp"
#include "sims/cartpole.hpp"
#include "sims/platformer.hpp"
#include "sims/pokebattle.hpp"
#include "sims/voxelcraft.hpp"
#include "sims/voxelcity.hpp"
#include "sims/gasim.hpp"
#include "sims/lifeengine.hpp"
#include "sims/locomotion.hpp"
#include "sims/kinesis.hpp"
#include "sims/hexplanetview.hpp"
#include "sims/cutemold.hpp"
#include "sims/traffic_idm.hpp"
#include "sims/traffic.hpp"
#include "sims/sorting.hpp"

#include <functional>
#include <string>
#include <vector>

namespace bench {

struct Entry {
    std::string            id;
    std::string            era;      // grouping in the index
    std::string            source;   // where the code lives, so the bench can show it
    std::function<SimPtr()> make;
    int                    warm = 200;  // steps before this one is worth looking at
};

inline const std::vector<Entry>& registry() {
    static const std::vector<Entry> r = {
        // Where it started, and what it was a simplification of.
        {"life",      "Where it started",     "sims/life_like.hpp",    []{ return make_life(); },            200},
        {"vonneumann","Where it started",     "sims/von_neumann.hpp",  []{ return make_von_neumann(56, 32); }, 260},

        // The lattice lineage.
        {"loops",     "The lattice lineage",  "sims/langton_loops.hpp",[]{ return make_langton_loops(); },  1800},
        {"highlife",  "The lattice lineage",  "sims/life_like.hpp",    []{ return make_highlife(); },        200},
        {"wireworld", "The lattice lineage",  "sims/small_lattice.hpp",[]{ return make_wireworld(); },       120},
        {"brain",     "The lattice lineage",  "sims/small_lattice.hpp",[]{ return make_brains_brain(); },    200},
        {"seeds",     "The lattice lineage",  "sims/life_like.hpp",    []{ return make_seeds(); },           120},
        {"daynight",  "The lattice lineage",  "sims/life_like.hpp",    []{ return make_day_night(); },       200},
        {"ant",       "The lattice lineage",  "sims/small_lattice.hpp",[]{ return make_langton_ant(); },    1200},

        // Three dimensions. Same question, 26 neighbours.
        {"life3d",    "Three dimensions",     "sims/life3d.hpp",       []{ return make_life3d_5766(40); },    90},
        {"clouds3d",  "Three dimensions",     "sims/life3d.hpp",       []{ return make_life3d_clouds(40); },  40},

        // A different game entirely.
        {"nowakmay",  "Game theory on a lattice","sims/spatial_pd.hpp",  []{ return make_spatial_pd(); },      600},

        // No lattice at all.
        {"boids",     "Continuous space",     "sims/continuous.hpp",   []{ return make_boids(); },           900},
        {"particles", "Continuous space",     "sims/continuous.hpp",   []{ return make_particle_life(); },  1200},
        {"collision2d","Continuous space",    "sims/collision.hpp",    []{ return make_collision(); },         30},
        {"satisfy-grow",   "Satisfying 2D", "sims/satisfy.hpp", []{ return make_satisfy_grow(); },    20},
        {"satisfy-blocks", "Satisfying 2D", "sims/satisfy.hpp", []{ return make_satisfy_blocks(); },  40},
        {"satisfy-bowl",   "Satisfying 2D", "sims/satisfy.hpp", []{ return make_satisfy_bowl(); },    30},
        {"satisfy-fill",   "Satisfying 2D", "sims/satisfy.hpp", []{ return make_satisfy_fill(); },    30},
        {"satisfy-spiral", "Satisfying 2D", "sims/satisfy.hpp", []{ return make_satisfy_spiral(); },  30},
        {"satisfy-water",  "Satisfying 2D", "sims/satisfy.hpp", []{ return make_satisfy_water(); },   40},
        {"satisfy-columns","Satisfying 2D", "sims/satisfy_more.hpp", []{ return make_satisfy_columns(); }, 30},
        {"satisfy-sides",  "Satisfying 2D", "sims/satisfy_more.hpp", []{ return make_satisfy_sides(); },   20},
        {"satisfy-square", "Satisfying 2D", "sims/satisfy_more.hpp", []{ return make_satisfy_square(); },  30},
        {"satisfy-claim",  "Satisfying 2D", "sims/satisfy_more.hpp", []{ return make_satisfy_claim(); },   40},
        {"satisfy-glass",  "Satisfying 2D", "sims/satisfy_more.hpp", []{ return make_satisfy_glass(); },   20},
        {"satisfy-beat",   "Satisfying 2D", "sims/satisfy_more.hpp", []{ return make_satisfy_beat(); },    20},
        {"satisfy-shrink", "Satisfying 2D", "sims/satisfy_more.hpp", []{ return make_satisfy_shrink(); },  20},
        {"satisfy-kaleido","Satisfying 2D", "sims/satisfy_more.hpp", []{ return make_satisfy_kaleido(); }, 30},
        {"satisfy-escape", "Satisfying 2D", "sims/satisfy_more.hpp", []{ return make_satisfy_escape(); },  40},
        {"sand-pour",      "Sand", "sims/sand.hpp", []{ return make_sand_pour(); },      20},
        {"sand-hourglass", "Sand", "sims/sand.hpp", []{ return make_sand_hourglass(); }, 30},
        {"sand-ball",      "Sand", "sims/sand.hpp", []{ return make_sand_ball(); },      20},
        {"machine-shred",  "Machines", "sims/machines.hpp", []{ return make_machine_shredder(); }, 25},
        {"machine-press",  "Machines", "sims/machines.hpp", []{ return make_machine_press(); },    20},
        {"machine-rollers","Machines", "sims/machines.hpp", []{ return make_machine_rollers(); },  25},
        {"flyarena",  "Continuous space",    "sims/fly_arena.hpp",    []{ return make_fly_arena(); },        120},
        {"pps",       "Continuous space",     "sims/pps.hpp",          []{ return make_pps(); },             900},

        // Something that learns.
        {"gridworld", "Learning agents",     "sims/gridworld.hpp",    []{ return make_gridworld(); },       400},
        {"netviz",    "Learning agents",     "sims/netviz.hpp",       []{ return make_netviz(); },          200},
        {"continual", "Learning agents",     "sims/continual.hpp",    []{ return make_continual(); },        60},
        {"neuralq",   "Learning agents",     "sims/neuralq.hpp",      []{ return make_neuralq(); },          80},
        {"cartpole",  "Learning agents",     "sims/cartpole.hpp",     []{ return make_cartpole(); },        120},
        {"platformer","Learning agents",     "sims/platformer.hpp",   []{ return make_platformer(); },       60},
        {"pokebattle","Learning agents",     "sims/pokebattle.hpp",   []{ return make_pokebattle(); },       40},
        {"voxelcraft","Learning agents",     "sims/voxelcraft.hpp",   []{ return make_voxelcraft(); },      200},
        {"voxelcity", "Learning agents",     "sims/voxelcity.hpp",    []{ return make_voxelcity(); },        60},
        {"ga",        "Learning agents",     "sims/gasim.hpp",        []{ return make_gasim(); },           200},
        {"locomotion","Evolving bodies",     "sims/locomotion.hpp",   []{ return make_locomotion(); },       180},
        {"lifeengine","Evolving bodies",     "sims/lifeengine.hpp",   []{ return make_lifeengine(); },     2000},
        {"kinesis",   "Evolving bodies",     "sims/kinesis.hpp",      []{ return make_kinesis(); },         600},
        {"cutemold",  "Evolving bodies",     "sims/cutemold.hpp",     []{ return make_cutemold(); },        400},
        {"hexplanet", "Worlds",              "sims/hexplanetview.hpp",[]{ return make_hexplanet(); },         1},
        {"traffic",   "Worlds",              "sims/traffic.hpp",      []{ return make_traffic(); },         400},
        {"trafficidm","Worlds",              "sims/traffic_idm.hpp",  []{ return make_traffic_idm(); },     400},

        {"sorting2d", "Algorithms", "sims/sorting.hpp", []{ return make_sorting(); }, 30},
        {"sorting3d", "Algorithms", "sims/sorting.hpp", []{ return make_sorting(SortingSim::Columns); }, 30},
        {"sortingsphere", "Algorithms", "sims/sorting.hpp", []{ return make_sorting(SortingSim::Sphere); }, 30},

        // Yours.
        // The rulestring entry is a template in the strongest sense: it is not
        // one rule, it is every outer-totalistic and Generations rule, typed in
        // at runtime. Starts on Conway so it shows something familiar.
        {"rule",      "Your own",             "sims/rulespec.hpp",     []{ return make_rule("B3/S23"); },    200},
        {"mysim",     "Your own",             "sims/my_sim.hpp",       []{ return make_my_sim(); },          200},
    };
    return r;
}

} // namespace bench
