// identify.hpp — work out what a pattern IS, rather than just showing it.
//
// A still life, an oscillator and a spaceship are the three things a lattice
// pattern can settle into, and telling them apart by eye is exactly the kind of
// counting a computer should be doing. This runs a pattern forward and reports
// its period and its displacement per period — so a glider comes back as
// "spaceship, period 4, moves (1,1)", which is a measurement, not a label.
//
// The method: hash each generation's cells RELATIVE TO THEIR BOUNDING BOX. A
// spaceship is a pattern whose normalised form repeats while its bounding box
// moves, and normalising is what makes that detectable at all — an absolute
// hash of a glider never repeats, because it is somewhere new every time.
//
// What it will NOT do is guess. A pattern that has not repeated inside the
// budget is reported as unresolved, and one touching the field border is
// reported as unreliable rather than being silently classified from a bounding
// box that the wrap has made meaningless.

#pragma once
#include "sim.hpp"
#include <string>
#include <unordered_map>
#include <cstdio>

namespace bench {

struct Identity {
    enum class Kind {
        Empty,        // nothing left alive
        StillLife,    // period 1, no displacement
        Oscillator,   // period > 1, no displacement
        Spaceship,    // period > 1, displaces
        Unresolved,   // no repeat inside the budget
        Unreliable    // touched the border; wrap makes the bounding box a lie
    };

    Kind        kind      = Kind::Unresolved;
    int         period    = 0;
    int         dx = 0, dy = 0;      // displacement per period
    std::size_t population = 0;
    int         checked   = 0;       // generations actually run

    [[nodiscard]] std::string describe() const {
        char b[160];
        switch (kind) {
        case Kind::Empty:
            return "empty — nothing survived";
        case Kind::StillLife:
            std::snprintf(b, sizeof b, "still life · %zu cells", population);
            return b;
        case Kind::Oscillator:
            std::snprintf(b, sizeof b, "oscillator · period %d · %zu cells", period, population);
            return b;
        case Kind::Spaceship:
            std::snprintf(b, sizeof b, "spaceship · period %d · moves (%d,%d) · %zu cells",
                          period, dx, dy, population);
            return b;
        case Kind::Unreliable:
            return "moving, but it has wrapped the edge — displacement cannot be trusted";
        default:
            std::snprintf(b, sizeof b, "no period within %d generations", checked);
            return b;
        }
    }
};

namespace detail {

struct Norm {
    std::uint64_t hash = 0;
    int  ox = 0, oy = 0;
    std::size_t population = 0;
    bool touchesBorder = false;
};

// Hash the live cells relative to their own bounding box, so two translated
// copies of the same shape hash equal. Without this a spaceship is invisible to
// period detection.
inline Norm normalise(const Field& f) {
    Norm n;
    int x0 = f.w, y0 = f.h, x1 = -1, y1 = -1;
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x)
            if (f.at(x, y)) {
                ++n.population;
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x > x1) x1 = x;
                if (y > y1) y1 = y;
            }
    if (x1 < 0) return n;                       // empty
    n.ox = x0; n.oy = y0;
    n.touchesBorder = (x0 == 0 || y0 == 0 || x1 == f.w - 1 || y1 == f.h - 1);

    std::uint64_t h = 1469598103934665603ull;   // FNV-1a
    auto mix = [&](std::uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix(std::uint64_t(x1 - x0 + 1));
    mix(std::uint64_t(y1 - y0 + 1));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) mix(f.at(x, y));
    n.hash = h;
    return n;
}

} // namespace detail

// Runs the sim forward, so it MUTATES it. Callers that need the state back
// should copy the field first — the workbench does.
inline Identity identify(Sim& sim, int budget = 240) {
    Identity id;
    // Two indexes, because they answer two different questions.
    //
    // ABSOLUTE: the whole field, unnormalised. A repeat here means the state
    // came back exactly where it was — a still life or an oscillator — and it
    // is trustworthy even when the pattern fills the field or crosses the seam,
    // because no bounding box is involved.
    //
    // NORMALISED: cells relative to their bounding box. A repeat here with the
    // box in a new place is a spaceship. THAT is the reading wrapping can ruin,
    // so the border only disqualifies this one.
    //
    // Keying everything off the normalised hash and refusing outright when the
    // pattern touched an edge made the whole tool useless on the sims you would
    // actually point it at: a random soup fills the field on generation zero,
    // so every one of them came back "unreliable".
    std::unordered_map<std::uint64_t, int> absolute;
    std::unordered_map<std::uint64_t, std::pair<int, std::pair<int,int>>> shape;
    bool everTouchedBorder = false;

    for (int gen = 0; gen <= budget; ++gen) {
        const Field& f = sim.field();
        const detail::Norm n = detail::normalise(f);
        id.checked = gen;
        id.population = n.population;
        if (n.population == 0) { id.kind = Identity::Kind::Empty; return id; }
        everTouchedBorder = everTouchedBorder || n.touchesBorder;

        std::uint64_t abs = 1469598103934665603ull;
        for (auto c : f.cells) { abs ^= c; abs *= 1099511628211ull; }

        if (auto it = absolute.find(abs); it != absolute.end()) {
            id.period = gen - it->second;
            id.dx = id.dy = 0;
            id.kind = (id.period == 1) ? Identity::Kind::StillLife
                                       : Identity::Kind::Oscillator;
            return id;
        }
        if (auto it = shape.find(n.hash); it != shape.end()) {
            const int dx = n.ox - it->second.second.first;
            const int dy = n.oy - it->second.second.second;
            if (dx != 0 || dy != 0) {
                if (everTouchedBorder) { id.kind = Identity::Kind::Unreliable; return id; }
                id.period = gen - it->second.first;
                id.dx = dx; id.dy = dy;
                id.kind = Identity::Kind::Spaceship;
                return id;
            }
        }
        absolute.emplace(abs, gen);
        shape.emplace(n.hash, std::make_pair(gen, std::make_pair(n.ox, n.oy)));
        sim.step();
    }
    id.kind = Identity::Kind::Unresolved;
    return id;
}

} // namespace bench
