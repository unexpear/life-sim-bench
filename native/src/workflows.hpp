#pragma once
#include "file_paths.hpp"
#include "sim.hpp"
#include <algorithm>
#include <filesystem>
#include <cstdlib>
#include <string>
#include <vector>

namespace bench {

// Presentation metadata only. Keys refer to the simulation's existing controls;
// the UI never substitutes a different rule or retunes a default.
struct Workflow {
    std::string id, title, hint, action;
    std::vector<std::string> primary;
    bool placed = false;
};
inline const std::vector<Workflow>& workflows() {
    static const std::vector<Workflow> rows = {
        {"sorting2d", "Watch an algorithm find order", "Run or Step through the sort. Set Events per step to 1 for individual operations. Reset replays the same data; New data changes the seed.", "New data", {"view","algorithm","size","pattern","speed","direction"}},
        {"sorting3d", "Explore sorting in three dimensions", "Drag to orbit the columns. Switch View without losing progress. Rows follow array order from front to back. Reset replays the same data.", "New data", {"view","algorithm","size","pattern","speed","direction"}},
        {"sortingsphere", "See how far each item is from home", "Items spiral from the top of the sphere to the bottom. The further an item is from its sorted place, the nearer it sits to the centre; a finished sort is a full sphere. Drag to orbit.", "New data", {"view","algorithm","size","pattern","speed","direction"}},
        {"locomotion", "Build a creature. Evolve its movement.", "Run to watch the population, or use Train to finish generations. Edit the body to build your own mover.", "", {"task","population","seconds","speed"}},
        {"life", "Explore cellular patterns", "Paint cells, change the birth and survival rule, or drop an RLE file onto the canvas.", "Seed patch", {"density","size","shape"}, true},
        {"vonneumann", "Program a constructor", "Choose a tape program. Increase the world width to give the tape and construction room.", "Place wire", {"program","code","size"}, true},
        {"loops", "Grow a colony", "Set the number of starting colonies, then watch their growth fronts meet.", "Plant loop", {"colonies","size"}, true},
        {"highlife", "Explore HighLife", "Use the rule chips to experiment with B36/S23. Paint or import a pattern to explore its evolution.", "Seed patch", {"density","size","shape"}, true},
        {"wireworld", "Build a circuit", "Paint conductors, heads and tails. Adjust the two clock widths to compare their signals.", "", {"clockA","clockB","size"}},
        {"brain", "Follow firing waves", "Paint firing and refractory cells, then single-step to inspect the wave front.", "", {"density","size"}},
        {"seeds", "Explore a birth-only rule", "Start sparsely and use single steps to see how new cells replace their neighbours.", "Seed patch", {"density","size"}, true},
        {"daynight", "Explore complementary patterns", "Change the starting density, paint a patch, and compare the resulting boundaries.", "Seed patch", {"density","shape","size"}, true},
        {"ant", "Follow the turning rule", "Choose a named turn sequence. The rate control changes how many ant moves you see per tick.", "", {"turns","speed","size"}},
        {"life3d", "Sculpt a living volume", "Orbit by dragging; use the camera views below. Set the seed and neighbour-count ranges separately.", "Seed volume", {"world","seed","size","density","el","eu","fl","fu"}, true},
        {"clouds3d", "Explore stable structures", "Orbit the volume to inspect its interior surfaces. Adjust birth and survival ranges while it runs.", "Seed volume", {"world","seed","density","el","eu","fl","fu"}, true},
        {"nowakmay", "Watch cooperation spread", "Adjust the temptation to defect and follow the cooperator fraction in Measurements.", "Add defectors", {"b","size"}, true},
        {"boids", "Balance flocking forces", "Compare separation, alignment and cohesion. Measurements shows order and spacing together.", "Disturb flock", {"size","separation","alignment","cohesion","radius"}, true},
        {"particles", "Explore particle interactions", "Tune the interaction radius, core repulsion and damping; use trails to follow the motion.", "Disturb particles", {"size","rmax","beta","friction","force"}, true},
        {"collision2d", "Watch disks collide", "Choose a scene. Restitution 1 is the elastic case the oracle checks; lower values keep less of the closing speed. Saving stores these settings, not live positions.", "Disturb disks", {"preset","restitution","seed"}, true},
        {"flyarena", "Watch the fly walk", "Choose a scene and controller. Click to move the odor or light source. Rock count applies to the obstacles scene.", "Move the source", {"scene","controller","speed"}, true},
        {"satisfy-grow", "Watch the ball become the circle", "It grows on every bounce. Speed and growth apply while it runs. Click to kick it.", "Kick the ball", {"speed","growth"}, true},
        {"satisfy-blocks", "Watch the rings fall inward", "The striker peels squares from the outside in. They stack under the circle. Click to knock one loose.", "Knock a block", {"rate"}, true},
        {"satisfy-bowl", "Watch the bowl close", "Capsules drop in and the ring shrinks toward the fill target. Click to drop one more.", "Drop a capsule", {"interval","target"}, true},
        {"satisfy-fill", "Watch the trails cover the disc", "Gravity bends the path. The run ends when the painted trails fill the circle. Click to nudge the ball.", "Nudge the ball", {"speed","width"}, true},
        {"satisfy-spiral", "Watch it speed up toward the center", "The bead accelerates along the spiral. The ticks are the path so far. Click to boost it.", "Boost", {"accel"}, true},
        {"satisfy-water", "Watch each fall add a water ball", "The drop runs the sawtooth spiral. Every time it falls off the inner end, the pool gains one ball. Click to add one.", "Add a water ball", {"speed","target"}, true},
        {"satisfy-columns", "See which column fills first", "Balls fall through the pegs into thirteen columns. Spread moves where a drop starts. Click to drop one.", "Drop a ball", {"rate","spread"}, true},
        {"satisfy-sides", "Watch the polygon become a circle", "It starts as a triangle. Every bounce adds a side, and 48 sides is the circle. Click to add a side.", "Add a side", {"speed","seed"}, true},
        {"satisfy-square", "Watch the square grow", "The square grows each time it hits the frame, and leaves its outline behind. Click to kick it.", "Kick the square", {"speed","growth"}, true},
        {"satisfy-claim", "Watch the two colours claim dots", "A dot takes the colour of the mover that touches it. Click to claim the nearest dot.", "Claim a dot", {"speed","seed"}, true},
        {"satisfy-glass", "Watch the ball speed up", "Every tile it breaks adds speed. The outer frame does not. Click to boost it.", "Boost", {"speed","accel"}, true},
        {"satisfy-beat", "Watch it land on the beat", "The ball hops from note to note. A taller note is a higher jump. Click to skip ahead.", "Skip a note", {"tempo","seed"}, true},
        {"satisfy-shrink", "Watch the walls close in", "All four walls step inward on every bounce. Click to kick the ball.", "Kick the ball", {"speed","shrink"}, true},
        {"satisfy-kaleido", "Watch one ball fill the circle", "The picture is that single path, copied around the centre. Click to nudge it.", "Nudge the ball", {"speed","folds"}, true},
        {"satisfy-escape", "Watch one escape become three", "A ball that leaves through the gap is replaced by three inside. Click to add three.", "Spawn three", {"speed","gap"}, true},
        {"sand-pour", "Watch sand pile up", "Grains fall from the spout and settle into a slope. Click to dump a handful.", "Dump sand", {"rate","spread","flow"}, true},
        {"sand-hourglass", "Watch the hourglass drain", "Sand starts in the upper bulb. The neck width changes how fast it falls. Click to add sand.", "Add sand", {"gap","flow"}, true},
        {"sand-ball", "Watch a linked sand ball slump", "Neighbours start linked. A link breaks when it stretches, and the ball becomes a pile. Click to tear it.", "Tear links", {"break","range"}, true},
        {"machine-shred", "Watch blocks get shredded", "Blocks fall into moving teeth and come out as colored sand. Click to break off a little sand.", "Break off sand", {"speed","teeth"}, true},
        {"machine-press", "Watch the plate crush a block", "The plate grinds down through the block and the crumbs spill out the sides. Click to break off a little sand.", "Break off sand", {"speed","bite"}, true},
        {"machine-rollers", "Watch the rollers grind", "Blocks drop through the nip and fall out as sand. Click to break off a little sand.", "Break off sand", {"speed","gap"}, true},
        {"pps", "Explore local turning", "Change fixed turn, neighbour-dependent turn and movement speed independently.", "Disturb particles", {"size","alpha","beta","v","r"}, true},
        {"gridworld", "Teach an agent a route", "Train by episode. Compare route length and reward; keep a run before changing exploration.", "", {"epsilon","alpha","gamma","size"}},
        {"netviz", "Look inside a learning network", "Drag nodes to rearrange the diagram. Select an XOR example and follow its activations.", "New weights", {"showcase","hidden","lr"}},
        {"continual", "Compare learning and forgetting", "Choose task B and when to switch. Measurements tracks both tasks as training continues.", "New weights", {"taskB","phase1","lr"}},
        {"neuralq", "Compare a network with a table", "Train by episode. Toggle replay and the target network, then compare the learning curves.", "", {"replay","target","lr","epsilon"}},
        {"cartpole", "Balance a pole", "Compare the network and table over episodes. Change table resolution or disturb the pole.", "Shove pole", {"bins","epsilon","lr"}},
        {"platformer", "Evolve a player", "Train by generation. Choose enemy visibility and parent selection; compare progress across runs.", "Jump", {"seeenemies","population","parentsel","maxnodes"}},
        {"pokebattle", "Compare battle policies", "A training batch contains 100 battles. Follow win rates against the greedy and random baselines.", "New battle run", {"epsilon","alpha"}},
        {"voxelcraft", "Climb the crafting tree", "Choose an agent, advance one tree rung, and orbit the world to inspect its route.", "New world", {"agent","size","seed"}},
        {"voxelcity", "Run a town or drive an agent", "Compare policies and roles, or take control of one agent using the command pad.", "", {"size","policy","agents","roles","share","hunger"}},
        {"ga", "Compare evolutionary search", "Choose a problem and selection method. Keep a curve before changing mutation or crossover.", "Add immigrants", {"problem","selection","population","mutrate","crossover"}},
        {"lifeengine", "Evolve bodies", "Balance food production, upkeep and reproduction. Follow population changes by turnover.", "Add organism", {"size","produce","upkeep","mutate","food"}, true},
        {"kinesis", "Explore aggregation", "Compare slowing and turning responses. Scatter the agents to repeat the observation on the same terrain.", "Scatter agents", {"size","slow","turn","patches","agents"}},
        {"cutemold", "Grow and disperse molds", "Tune upkeep, mutation and the spore threshold. Place a founder to disturb the population.", "Plant mold", {"size","upkeep","mutate","spore","light"}, true},
        {"hexplanet", "Explore a generated planet", "Drag to orbit. Compare sea level and continent growth; mark the twelve pentagons for orientation.", "New planet", {"sea","subdiv","grow","pents"}},
        {"traffic", "Create and inspect a traffic jam", "Compare density and random braking. Place a disturbance on the road and follow its propagation.", "Brake traffic", {"length","density","rule","p","vmax"}, true},
        {"trafficidm", "Compare driving and lane changes", "Set traffic density, headway and passing rules. Use Measurements to compare flow and lane changes.", "Brake traffic", {"length","density","lanes","T","v0","rules","polite"}, true},
        {"rule", "Explore your own cellular rules", "Choose an example or paste a rulestring. Apply to start a new run with your world settings.", "", {"density","size","seed"}},
        {"mysim", "Experiment with annealing", "Change the starting density, paint a boundary and inspect how it settles.", "", {"density","size"}},
    };
    return rows;
}
inline const Workflow& workflow(const std::string& id) {
    for (const auto& w : workflows()) if (w.id == id) return w;
    static const Workflow fallback{"", "Your simulation", "Edit the source, build it, and inspect its controls and measurements.", "", {}};
    return fallback;
}

// Library labels are metadata: listing simulations must never instantiate them.
inline std::string catalog_title(const std::string& id) {
    if (id == "sorting2d") return "Sorting lab: 2D";
    if (id == "sorting3d") return "Sorting lab: 3D";
    if (id == "sortingsphere") return "Sorting lab: disparity sphere";
    if (id == "locomotion") return "Creature evolution: movement lab";
    if (id == "life") return "Conway's Game of Life";
    if (id == "vonneumann") return "Von Neumann's 29-state automaton";
    if (id == "loops") return "Langton's Loops";
    if (id == "highlife") return "HighLife";
    if (id == "wireworld") return "Wireworld";
    if (id == "brain") return "Brian's Brain";
    if (id == "seeds") return "Seeds";
    if (id == "daynight") return "Day & Night";
    if (id == "ant") return "Langton's Ant";
    if (id == "life3d") return "3D Life 5766";
    if (id == "clouds3d") return "3D Stable Structures 13-26/14-19";
    if (id == "nowakmay") return "Spatial prisoner's dilemma";
    if (id == "boids") return "Boids";
    if (id == "particles") return "Particle Life";
    if (id == "collision2d") return "Particle Collision Lab";
    if (id == "flyarena") return "Fly Arena (reactive stub)";
    if (id == "satisfy-grow") return "Ball grows into the circle";
    if (id == "satisfy-blocks") return "Blocks fall toward the center";
    if (id == "satisfy-bowl") return "Bowl closes as it fills";
    if (id == "satisfy-fill") return "Trails fill the circle";
    if (id == "satisfy-spiral") return "Spiral speeds to the center";
    if (id == "satisfy-water") return "Water balls on a spiral";
    if (id == "satisfy-columns") return "Which column fills first";
    if (id == "satisfy-sides") return "Every bounce adds a side";
    if (id == "satisfy-square") return "The square grows on each bounce";
    if (id == "satisfy-claim") return "Dots join the side they touch";
    if (id == "satisfy-glass") return "Shattering glass speeds the ball";
    if (id == "satisfy-beat") return "Lands on every beat";
    if (id == "satisfy-shrink") return "Walls close in on each bounce";
    if (id == "satisfy-kaleido") return "One ball, mirrored";
    if (id == "satisfy-escape") return "Each escape spawns three balls";
    if (id == "sand-pour") return "Sand pour";
    if (id == "sand-hourglass") return "Sand hourglass";
    if (id == "sand-ball") return "Linked sand ball";
    if (id == "machine-shred") return "Block shredder";
    if (id == "machine-press") return "Block press";
    if (id == "machine-rollers") return "Roller mill";
    if (id == "pps") return "Primordial Particle System";
    if (id == "gridworld") return "Q-learning gridworld";
    if (id == "netviz") return "Neural network, live";
    if (id == "continual") return "Transfer & catastrophic forgetting";
    if (id == "neuralq") return "Neural Q-learning (vs a table)";
    if (id == "cartpole") return "Cart-pole (vs a discretised table)";
    if (id == "platformer") return "Platformer evolved by NEAT";
    if (id == "pokebattle") return "Gen I battle system, learned";
    if (id == "voxelcraft") return "Block world: the diamond problem";
    if (id == "voxelcity") return "Block world: a town";
    if (id == "ga") return "Genetic algorithm, population visible";
    if (id == "lifeengine") return "The Life Engine";
    if (id == "kinesis") return "Kinesis: aggregation without direction";
    if (id == "cutemold") return "Cute Mold";
    if (id == "hexplanet") return "Hex planet: twelve pentagons, no more";
    if (id == "traffic") return "Nagel-Schreckenberg freeway traffic";
    if (id == "trafficidm") return "Intelligent Driver Model + MOBIL";
    if (id == "rule") return "Rule lab · custom cellular automata";
    if (id == "mysim") return "Anneal";
    return id;
}

inline std::string export_path(const std::string& directory, std::string filename) {
    for (char& c : filename)
        if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '-';
    return (std::filesystem::path(directory) / filename).string();
}

inline bool parse_control_value(const std::string& text, const Knob& knob, float& result) {
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(value) || value < knob.min || value > knob.max) return false;
    result = knob.quantised(value);
    return true;
}

struct PluginFile { std::string source, dll, name; };
inline std::vector<PluginFile> plugin_files(const std::string& directory) {
    namespace fs = std::filesystem;
    std::vector<PluginFile> out;
    std::error_code ec;
    for (fs::directory_iterator it(bench::path_from_utf8(directory), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        auto p = it->path();
        const auto ext = p.extension().string();
        if (ext != ".cpp" && ext != ".dll") continue;
        const auto name = path_text(p.stem());
        if (name.ends_with(".loaded")||name.find(".loaded-")!=std::string::npos) continue;
        if (std::any_of(out.begin(), out.end(), [&](const PluginFile& f) { return f.name == name; })) continue;
        auto src = p; src.replace_extension(".cpp");
        auto dll = p; dll.replace_extension(".dll");
        out.push_back({path_text(src), path_text(dll), name});
    }
    std::sort(out.begin(), out.end(), [](const PluginFile& a, const PluginFile& b) { return a.name < b.name; });
    return out;
}
} // namespace bench
