// sim.hpp — what every simulation on the bench must provide.
//
// The interface is small on purpose. A rule that needs more than this is
// probably smuggling presentation into the model.

#pragma once
#include "field.hpp"
#include "render/voxel.hpp"
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace bench {

// Whether a system genuinely makes copies of itself, merely organises itself
// into structure, or does neither. This is the single most over-claimed
// property in artificial life, so the bench makes every sim state it.
enum class Replication { Yes, Disputed, No };

struct Knob {
    std::string key;
    std::string label;
    float       min   = 0.0f;
    float       max   = 1.0f;
    float       value = 0.0f;

    // Not every parameter is a continuous float, and pretending otherwise
    // produces controls that lie: a "steps per tick" slider that lands on 12.7,
    // or a rule selector where the values between the options mean nothing.
    float step = 0.0f;                  // 0 = continuous; >0 quantises to a grid

    // When set, `value` indexes this list and the slider shows the name rather
    // than the number. For genuinely discrete choices.
    std::vector<std::string> choices;

    // Some parameters only mean anything at reset — an initial soup density
    // cannot be changed halfway through without rewriting history. Marking them
    // lets the workbench re-seed on release instead of silently doing nothing,
    // which is what a dead-looking slider actually is.
    bool on_reset = false;

    // This knob changes how fast you WATCH, not what happens.
    //
    // "Steps per tick" sets how much simulation happens per frame. Measured in
    // frames it looks like the most powerful knob on the sim; measured in the
    // sim's own epochs it changes nothing at all, because an episode runs to
    // its boundary however fast the frames go past. Both readings are correct
    // and they mean opposite things, so the knob has to say which kind it is —
    // and then a display knob that DOES change the outcome is a real bug, which
    // is the stronger claim and the one worth testing.
    bool display_only = false;

    // This knob's effect needs a longer run than a handful of epochs to appear.
    //
    // The platformer's complexity cap is the case: with the paper's addNode
    // rate its population does not reach even two hidden nodes until about
    // generation fifteen, so a cap of any size is genuinely inert before then.
    // The dead-knob test gives these a longer horizon rather than skipping
    // them — an exemption would be a place for a truly dead knob to hide.
    bool slow = false;

    // This knob does nothing unless another one is set a particular way.
    //
    // Fitness scaling is the case, and it is not a quirk of this code: uniform,
    // tournament and truncation selection all COMPARE weights, so they are
    // invariant to any monotone rescaling of fitness and the scaling choice
    // changes literally nothing. Only roulette, which uses the weights
    // proportionally, can see it. Measured on rastrigin over 30 generations:
    // under tournament every scaling gives cost 3.645 exactly; under roulette
    // they give 17.03, 5.19, 10.75, 9.17 and 27.61.
    //
    // Dependencies CHAIN. The scaling constant needs two things at once: a
    // selection scheme that reads weights proportionally, and a scaling scheme
    // that has a constant to read — raw and rank both ignore it entirely. So
    // "scaling constant" requires scaling = sigma, and "scaling" in turn
    // requires selection = roulette. Following the chain satisfies both without
    // needing a list, and expresses the real structure: each requirement is a
    // fact about one knob, not a bundle attached to another.
    //
    // Naming the dependency lets the panel say a control is inert instead of
    // leaving it to look broken, and lets the dead-knob test set the dependency
    // before deciding the knob does nothing.
    std::string requires_key;
    float       requires_value = 0.0f;

    std::string help;                   // one sentence, shown on hover

    // A constructor rather than aggregate initialisation, so the sims that only
    // need the first five fields do not each trip -Wmissing-field-initializers.
    Knob() = default;
    Knob(std::string k, std::string l, float mn, float mx, float v,
         float st = 0.0f, std::vector<std::string> ch = {}, bool onReset = false,
         std::string h = {}, bool displayOnly = false, bool isSlow = false,
         std::string reqKey = {}, float reqValue = 0.0f)
        : key(std::move(k)), label(std::move(l)), min(mn), max(mx), value(v),
          step(st), choices(std::move(ch)), on_reset(onReset),
          display_only(displayOnly), slow(isSlow),
          requires_key(std::move(reqKey)), requires_value(reqValue),
          help(std::move(h)) {}

    [[nodiscard]] float quantised(float v) const {
        v = v < min ? min : (v > max ? max : v);
        if (step > 0.0f) v = min + step * std::round((v - min) / step);
        return v < min ? min : (v > max ? max : v);
    }
    // What to print next to the label.
    [[nodiscard]] std::string shown() const {
        if (!choices.empty()) {
            // Indexed from `min`, not from zero.
            //
            // This used to index with the raw value, which is only correct when
            // a choice knob starts at 0 — and one did not. voxelcraft's
            // supersampling knob runs 1..3, so every position was labelled one
            // off: value 1 displayed "2x" while the renderer was at 1x, and
            // value 3 fell off the end of the list, dropped through to the
            // snprintf fallback and printed a bare number instead of a name.
            // The contract is now the one the field name implies: a choice
            // knob's positions are its range, whatever it starts at.
            const int i = int(value - min + 0.5f);
            if (i >= 0 && i < int(choices.size())) return choices[std::size_t(i)];
        }
        char b[32];
        std::snprintf(b, sizeof b, (step >= 1.0f) ? "%.0f" : "%.3g", double(value));
        return b;
    }
};

// A control that is ON or OFF, for the parts of a rule that are genuinely a set
// rather than a magnitude.
//
// This exists because a slider cannot express a transition rule. "Birth on
// exactly 3 neighbours" is not a point on a continuum — it is one bit in a
// nine-bit set, and the interesting edits are turning individual bits on and
// off while the thing is running. Density and seed sliders change the soup you
// start with; these change what the rule DOES.
//
// `group` lets the bench lay related switches out as one row (all the birth
// conditions together, all the survival conditions together) instead of a
// column of eighteen unrelated checkboxes.
struct Switch {
    std::string key;
    std::string label;      // short: shown on the chip itself, e.g. "3"
    std::string group;      // e.g. "birth" / "survive"
    bool        value = false;
    // What this bit DOES, in a sentence. Written by the sim, because the UI has
    // no idea what "3" means and should not be inventing an explanation.
    std::string help;

    Switch() = default;
    Switch(std::string k, std::string l, std::string g, bool v, std::string h = {})
        : key(std::move(k)), label(std::move(l)), group(std::move(g)), value(v),
          help(std::move(h)) {}
};

// A named scalar the sim measures about itself. The workbench records these into
// ring buffers and plots them, which is the difference between watching a
// simulation and benching one.
//
// Rule: a metric may only report a property the simulation ACTUALLY HAS.
// Nothing derived for effect, nothing smoothed to look better than it is.
struct Metric {
    std::string name;
    double      value = 0.0;
    // Optional fixed scale for the plot. Leave at 0 to autoscale.
    double      max   = 0.0;

    // Which direction counts as better.
    //
    // Not cosmetic: the workbench reports a "best so far" for every series,
    // and without this it reported the running MAXIMUM of "steps to goal" —
    // the agent's single worst episode, labelled as its best result. It also
    // decides when a training run has stopped improving, and which way to word
    // a comparison against a kept run.
    //
    // Neither is a real third case, not a default. A species count and an
    // exploration rate describe a run rather than scoring it, and forcing them
    // to pick a direction makes the panel claim "7 better" about a number that
    // cannot be better. Those series get no best line and no comparison.
    enum Direction { Higher, Lower, Neither };
    Direction   better = Higher;
};

struct Provenance {
    std::string title;      // "Conway's Game of Life"
    std::string year;       // "1970"
    std::string who;        // author, and where it was published
    std::string citation;   // the actual reference
    Replication replicates = Replication::No;
    std::string replication_note;  // why — never just the verdict
    std::string blurb;
};

class Sim {
public:
    virtual ~Sim() = default;

    virtual const Provenance&           about()   const = 0;
    virtual const std::vector<Swatch>&  palette() const = 0;
    virtual const Field&                field()   const = 0;

    virtual void reset()  = 0;
    virtual void step()   = 0;

    [[nodiscard]] virtual std::uint64_t generation() const = 0;

    // Optional: a knob the bench exposes as a slider. Default: none.
    virtual std::vector<Knob>& knobs() { static std::vector<Knob> none; return none; }
    virtual void on_knob(const std::string&, float) {}

    // Optional: a fully rendered RGB image, for sims a grid of palette indices
    // genuinely cannot describe.
    //
    // This exists because refusing to add it produced a fake. A 3D volume needs
    // per-face lighting and a real depth buffer, and shading has far more than
    // 256 distinguishable values — encoding it as palette indices gave a
    // 3D-looking picture with no 3D rendering in it. Sims that return a surface
    // are drawn from it directly; everything else keeps publishing indices and
    // is unaffected.
    [[nodiscard]] virtual const Surface* surface() const { return nullptr; }

    // ── camera, for sims that render a space rather than a plane ────────────
    //
    // A 3D view needs to be flown, not dialled. Sliders for yaw and pitch are
    // not camera controls: you cannot orbit something you are looking at, and
    // you certainly cannot choose WHAT you are orbiting.
    //
    // dx/dy are in pixels of drag. Returning true means the view changed and
    // the sim has re-rendered.
    // Whether this sim has a camera at all. A network diagram renders itself
    // and has nothing to orbit; a voxel volume does.
    [[nodiscard]] virtual bool has_camera() const { return false; }

    virtual bool camera_orbit(float /*dx*/, float /*dy*/) { return false; }
    virtual bool camera_pan  (float /*dx*/, float /*dy*/) { return false; }
    // steps>0 zooms in. nx/ny are the cursor in 0..1 of the rendered image:
    // CAD zooms toward the POINTER, not the screen centre, and that single
    // behaviour is most of what makes a 3D view feel controllable.
    virtual bool camera_dolly(float /*steps*/, float /*nx*/ = 0.5f, float /*ny*/ = 0.5f) {
        return false;
    }
    // Standard views, the way every CAD package has them.
    enum class StdView { Front, Back, Left, Right, Top, Bottom, Iso };
    virtual bool camera_view(StdView) { return false; }
    virtual bool camera_fit()         { return false; }   // frame everything
    virtual bool camera_ortho(bool)   { return false; }   // parallel projection
    [[nodiscard]] virtual bool camera_is_ortho() const { return false; }
    // Set the orbit pivot to whatever solid thing is under this point of the
    // rendered image, in 0..1 surface coordinates. False if the ray missed.
    virtual bool camera_pick (float /*nx*/, float /*ny*/) { return false; }
    virtual void camera_home () {}

    // ── epochs: generations and episodes ───────────────────────────────────
    //
    // step() advances a FRAME, which is the wrong unit for anything that
    // learns. A NEAT generation is 80 genomes each playing to death; an RL
    // episode ends when the agent does. Watching those go past at 30 frames a
    // second and trying to catch the boundary by eye is not a control.
    //
    // A sim that has a natural epoch says so, runs exactly one when asked, and
    // names it — so the workbench can offer "+1 generation" instead of only
    // "+1 frame", and can stop cleanly on the boundary.
    [[nodiscard]] virtual const char* epoch_name() const { return nullptr; }
    virtual bool advance_epoch() { return false; }
    [[nodiscard]] virtual int epoch_count() const { return 0; }

    // ── direct manipulation ────────────────────────────────────────────────
    //
    // For sims whose picture has parts you can take hold of. drag_begin returns
    // true if it grabbed something, which is also how the workbench decides
    // whether a drag belongs to the sim or to the camera — a 3D volume has
    // nothing to grab and says so by returning false.
    //
    // Coordinates are 0..1 within the sim's own rendered image.
    virtual bool drag_begin(float /*nx*/, float /*ny*/) { return false; }
    virtual void drag_move (float /*nx*/, float /*ny*/) {}
    virtual void drag_end  () {}

    // Optional: a short line naming what is running RIGHT NOW, as distinct
    // from what the sim is. For an editable rule that is the rulestring — you
    // need to see "B36/S23" while clicking rule bits, and the title alone
    // cannot tell you which unnamed rule you have landed on.
    [[nodiscard]] virtual std::string subtitle() const { return {}; }

    // Optional: on/off controls, for rule membership rather than magnitude.
    virtual std::vector<Switch>& switches() { static std::vector<Switch> none; return none; }
    virtual void on_switch(const std::string&, bool) {}

    // Optional: what this sim measures about itself, sampled by the workbench
    // and plotted over time. Default: population of each non-zero state, which
    // is meaningful for every lattice rule and harmless for the rest.
    virtual std::vector<Metric> metrics() const {
        std::vector<Metric> out;
        const Field& f = field();
        const auto&  p = palette();
        std::vector<std::size_t> n(p.size(), 0);
        for (auto c : f.cells) if (c < n.size()) ++n[c];
        const double total = double(f.cells.size());
        // Neither, not the struct default of Higher.
        //
        // This fallback is what every sim without a metrics() override
        // publishes, and a population share DESCRIBES a run rather than
        // scoring it. Left at Higher, the panel drew a best-so-far line and
        // reported the densest field a run ever reached as its best result —
        // for Life that is the generation-1 peak of 0.32 against a settled
        // 0.038, labelled "best". Every hand-written share elsewhere in the
        // project already declares Neither; this one was inheriting a default.
        for (std::size_t i = 1; i < p.size(); ++i)
            out.push_back(Metric{ p[i].label, double(n[i]) / total, 1.0, Metric::Neither });
        return out;
    }

    // Optional: seed something at a normalised position. This is the "stamp"
    // action — plant a loop, drop a patch of defectors, scatter particles.
    // Sim-specific and deliberately coarse.
    virtual bool poke(float /*nx*/, float /*ny*/) { return false; }

    // Optional: the grid the brush may paint into directly.
    //
    // Return the field ONLY if it is the sim's real state. Several sims here
    // render a derived view — von Neumann collapses 29 states into 8 render
    // categories, the particle sims rasterise agents onto a grid — and painting
    // into those would be erased on the next step, which looks exactly like a
    // broken brush. Those sims return nullptr and implement poke() instead.
    virtual Field* editable() { return nullptr; }

    // What a brush stroke should write, and what it should erase to. Defaults
    // suit a two-state lattice; override if 0 is not "empty" for you.
    [[nodiscard]] virtual std::uint8_t paint_value() const { return 1; }
    [[nodiscard]] virtual std::uint8_t erase_value() const { return 0; }
};

using SimPtr = std::unique_ptr<Sim>;

} // namespace bench
