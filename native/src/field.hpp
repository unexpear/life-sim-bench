// field.hpp — the contract between a simulation and the renderer.
//
// Every simulation, however different its internals, publishes the same thing:
// a grid of small integers plus a palette to interpret them. The renderer never
// knows what a "confluent state" or a "defector" is; it uploads indices and lets
// a lookup table colour them.
//
// This is deliberate. It keeps the sims dependency-free and testable, and it
// means adding a new rule never touches rendering code.

#pragma once
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <array>

namespace bench {

// ── things every continuous sim needs and nobody should rewrite ─────────────

// Minimum-image convention: the shortest offset between two points on a torus.
// Getting this wrong is the single most common bug in a wrapped particle sim —
// forces suddenly reverse across the seam.
inline float wrap_delta(float d, float extent) noexcept {
    const float half = extent * 0.5f;
    if (d >  half) d -= extent;
    if (d < -half) d += extent;
    return d;
}

// Keep a position inside [0, extent).
//
// HARDENING, not a fix for an observed failure — stated plainly so nobody
// later assumes this was load-bearing.
//
// The single-shot form (two ifs) is correct only while one step moves less than
// a full world width. That constraint was real and undocumented. The predicted
// way to break it was Particle Life's friction knob at 0, where velocity
// integrates force with no damping; the danger is that once a particle leaves
// the world, wrap_delta on two out-of-range values can reach inf - inf = NaN,
// and NaN fails BOTH comparisons of a range check — so the guard that should
// discard it passes it through and it spreads to every particle.
//
// Measured: it does not happen. At friction 0 and force at maximum, 1185 of
// 1200 particles are still alive after 3000 steps, because the pairwise forces
// change sign and random-walk rather than accumulating. So this is defensive:
// fmod costs nothing on the fast path and removes an unstated precondition.
// The cold path is a separate non-inlined function on purpose. Folding fmod and
// isfinite into the body cost 11-18% on PPS, boids and Particle Life — measured,
// not guessed — because it stopped the common case inlining to a few compares.
// Splitting it restores the original speed exactly while still handling the
// arbitrary case.
[[gnu::noinline, gnu::cold]]
inline float wrap_pos_far(float p, float extent) noexcept {
    if (!std::isfinite(p)) return extent * 0.5f;            // never propagate NaN/inf
    p = std::fmod(p, extent);
    if (p < 0.0f) p += extent;
    return p;
}

inline float wrap_pos(float p, float extent) noexcept {
    if (p >= 0.0f && p < extent)      return p;             // inside already
    if (p < 0.0f  && p >= -extent)    return p + extent;    // one width low
    if (p >= extent && p < 2*extent)  return p - extent;    // one width high
    return wrap_pos_far(p, extent);                         // anything wilder
}

struct Rgb { std::uint8_t r, g, b; };

// A palette entry carries its label so the legend is generated from the same
// data the renderer uses. A key that can drift out of sync with the colours is
// worse than no key at all.
struct Swatch {
    Rgb          colour;
    std::string  label;
};

// Indices into the palette. One byte per cell: every rule here has < 256 states,
// and byte grids upload straight to an R8 texture with no conversion.
struct Field {
    int                     w = 0;
    int                     h = 0;
    std::vector<std::uint8_t> cells;

    Field() = default;
    Field(int width, int height) : w(width), h(height), cells(std::size_t(width) * height, 0) {}

    [[nodiscard]] std::size_t index(int x, int y) const noexcept {
        return std::size_t(y) * w + x;
    }
    [[nodiscard]] std::uint8_t at(int x, int y) const noexcept { return cells[index(x, y)]; }
    void set(int x, int y, std::uint8_t v) noexcept { cells[index(x, y)] = v; }

    // Toroidal read. Neighbour access is the hottest path in the whole bench —
    // eight of these per cell per step — and integer division is expensive, so
    // this wraps by comparison rather than by modulo. Valid for any offset
    // within one field width, which is every neighbourhood anyone uses. The
    // modulo version measured ~3x slower on a 256^2 Life grid.
    [[nodiscard]] std::uint8_t wrap(int x, int y) const noexcept {
        int xx = x; if (xx < 0) xx += w; else if (xx >= w) xx -= w;
        int yy = y; if (yy < 0) yy += h; else if (yy >= h) yy -= h;
        return cells[std::size_t(yy) * w + xx];
    }
    // For offsets that may exceed one width (rare; poke/brush paths).
    [[nodiscard]] std::uint8_t wrapAny(int x, int y) const noexcept {
        int xx = x % w; if (xx < 0) xx += w;
        int yy = y % h; if (yy < 0) yy += h;
        return cells[std::size_t(yy) * w + xx];
    }
    void fill(std::uint8_t v) noexcept { std::fill(cells.begin(), cells.end(), v); }
    [[nodiscard]] std::size_t count(std::uint8_t v) const noexcept {
        std::size_t n = 0;
        for (auto c : cells) if (c == v) ++n;
        return n;
    }
};

} // namespace bench
