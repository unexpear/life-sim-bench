// contact_sheet.cpp — render every sim in the registry to a PNG, headless, and
// MEASURE every tile.
//
// The point: you can look at the whole roster without a window, a GPU, or SDL.
// It also means the rendering core is exercised on every rule in CI.
//
// ── what was wrong with the old version ────────────────────────────────────
//
// Its own header claimed "a rule that blows up or renders black is caught by
// looking rather than by trusting". Nothing looked. It wrote an 8-megabyte PNG
// and printed a generation counter per sim, and a tile that came out uniformly
// black produced exactly the same output as one full of gliders. The claim was
// true only of a human who opened the file and scrolled, which is the kind of
// verification this project exists to replace. So each tile is now measured:
// how much of it is not background, how many distinct colours it contains, and
// whether it is byte-identical to another tile.
//
// Worse, it rendered the wrong thing for a third of the roster. Eleven sims
// publish a fully rendered image through Sim::surface() — the voxel worlds, the
// planet, netviz, the learning sims with their own diagrams — and this drew
// sim->field() for all of them. netviz's own header says the Field it publishes
// is a coarse stand-in for the layout; life3d's is a slice. Those tiles were
// pictures of the placeholder, presented as pictures of the sim.
//
// Writes a single PNG. No image library — PNG is emitted directly with stored
// (uncompressed) deflate blocks, which is a dozen lines and avoids a dependency
// for something written once and viewed in any image viewer.

#include "registry.hpp"
#include "render/raster.hpp"
#include "render/png.hpp"
#include "tool_freshness.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace {

struct TileStat {
    double        ink = 0.0;      // fraction of pixels that are not the modal colour
    std::size_t   colours = 0;    // distinct RGB values
    std::uint64_t hash = 0;
};

// Modal colour, distinct-colour count and a content hash, in one pass over the
// tile. "Ink" is measured against the MODE rather than against black: several
// palettes here have a light background, and counting non-black pixels called
// those tiles 100% full whatever they contained.
TileStat measure(const std::uint8_t* px, std::size_t pitch, int cell) {
    std::map<std::uint32_t, std::size_t> hist;
    TileStat t;
    std::uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < cell; ++y) {
        const std::uint8_t* row = px + std::size_t(y) * pitch;
        for (int x = 0; x < cell; ++x) {
            const std::uint32_t c = std::uint32_t(row[x*4+0]) | (std::uint32_t(row[x*4+1]) << 8)
                                  | (std::uint32_t(row[x*4+2]) << 16);
            ++hist[c];
            for (int b = 0; b < 3; ++b) { h ^= row[x*4+b]; h *= 1099511628211ull; }
        }
    }
    std::size_t mode = 0;
    for (const auto& [c, n] : hist) { (void)c; mode = std::max(mode, n); }
    const double total = double(cell) * double(cell);
    t.ink = 1.0 - double(mode) / total;
    t.colours = hist.size();
    t.hash = h;
    return t;
}

} // namespace

int main(int argc, char** argv) {
    bench::gate("contact_sheet");

    const int  cell   = 300;                       // per-tile pixels
    const int  cols   = 4;
    const float wscale = (argc > 1) ? float(std::atof(argv[1])) : 1.0f;  // multiply per-sim warm-up
    const char* out   = (argc > 2) ? argv[2] : "contact-sheet.png";

    const auto& reg = bench::registry();
    const int rows = int((reg.size() + cols - 1) / cols);
    const int W = cols * cell, H = rows * cell;

    std::vector<std::uint8_t> sheet(std::size_t(W) * H * 4, 0);
    bench::Raster r;
    r.resize(cell, cell);

    // A tile that is one flat colour is a sim that rendered nothing. Stated
    // rather than tuned: below one percent of the tile differing from its modal
    // colour there is nothing on screen to look at, which is the condition this
    // file's header always claimed to catch.
    constexpr double kBlank = 0.01;

    std::printf("rendering %zu sims (warm-up scale %.2f)\n\n", reg.size(), double(wscale));
    std::printf("  %-11s %-22s %6s %7s %7s %9s  %s\n",
                "sim", "title", "src", "ink %", "colours", "peak", "");

    std::vector<TileStat> stats(reg.size());
    std::vector<std::string> flags(reg.size());
    int blank = 0, dup = 0;

    for (std::size_t i = 0; i < reg.size(); ++i) {
        auto sim = reg[i].make();
        bench::View v;
        v.trail = 0.88f;                           // show motion, not just state
        r.clear_accumulator();
        const int warm = std::max(1, int(reg[i].warm * wscale));

        // Draw what the sim actually renders. A sim that publishes a Surface is
        // publishing its finished picture; its Field is whatever coarse thing it
        // could fit into a grid of palette indices, and for several of them that
        // is a stand-in with no relation to what the user sees.
        const bool hasSurface = sim->surface() != nullptr;
        for (int s = 0; s < warm; ++s) {
            sim->step();
            if (s > warm - 40) {                   // build the wake
                if (hasSurface) r.draw(*sim->surface(), v);
                else            r.draw(sim->field(), sim->palette(), v);
            }
        }
        if (hasSurface) r.draw(*sim->surface(), v);
        else            r.draw(sim->field(), sim->palette(), v);

        stats[i] = measure(r.pixels(), r.pitch(), cell);

        const int tx = int(i % cols) * cell, ty = int(i / cols) * cell;
        for (int y = 0; y < cell; ++y)
            std::memcpy(&sheet[(std::size_t(ty + y) * W + tx) * 4],
                        r.pixels() + std::size_t(y) * r.pitch(), std::size_t(cell) * 4);

        // Two independent ways to be empty, and the second needs no threshold.
        //
        // Ink alone was not enough. life3d's tile came out at 1.3% — above the
        // blank cut, because the voxel renderer still draws the empty box — while
        // its own "live fraction" metric read exactly 0: 3D Life 5766 dies out
        // completely within about ten steps at every starting density, so the
        // shipped 90-step warm-up photographs an empty room. When every number a
        // sim publishes about itself is zero, the sim is saying there is nothing
        // there, and no tuned percentage is needed to hear it.
        double peak = 0.0; bool anyMetric = false;
        for (const auto& m : sim->metrics()) { anyMetric = true; peak = std::max(peak, std::fabs(m.value)); }
        if (stats[i].ink < kBlank) { flags[i] = "BLANK — nothing rendered"; ++blank; }
        else if (anyMetric && peak == 0.0) {
            flags[i] = "EMPTY — every metric this sim publishes reads 0";
            ++blank;
        }
        for (std::size_t j = 0; j < i; ++j)
            if (stats[j].hash == stats[i].hash) {
                flags[i] = "IDENTICAL to " + reg[j].id + " — two entries, one picture";
                ++dup;
            }

        std::printf("  %-11s %-22s %6s %6.1f%% %7zu %9.4g  %s\n",
                    reg[i].id.c_str(), sim->about().title.c_str(),
                    hasSurface ? "surf" : "field",
                    stats[i].ink * 100.0, stats[i].colours, peak, flags[i].c_str());
    }

    if (!bench::write_png(out, W, H, sheet.data())) {
        std::printf("could not write %s\n", out);
        return 1;
    }
    std::printf("\nwrote %s (%dx%d)\n", out, W, H);
    std::printf("%zu tiles: %d blank, %d duplicated.\n", reg.size(), blank, dup);
    if (dup)
        std::printf("A duplicate is reported, not failed: the 'rule' entry deliberately starts on\n"
                    "B3/S23 so it opens on something familiar, and it is therefore the same\n"
                    "picture as 'life' by design. Worth seeing on the sheet; not worth a red CI.\n");
    std::printf("ink %% is the share of the tile that differs from its most common colour —\n"
                "measured against the MODE, not against black, because several palettes here\n"
                "have a light background and counting non-black pixels calls those tiles full\n"
                "whatever is in them.\n");
    // Exit nonzero on a BLANK tile only. This runs in CI, and a rule that
    // renders nothing must fail something rather than merely be printed — but a
    // duplicate is a fact about the roster, not a defect, and failing on one
    // would make the first CI run red for a reason nobody intends to fix.
    return blank ? 1 : 0;
}
