// sweep.cpp — parameter sweeps, headless.
//
// Turns "what does this knob actually do" from a guess into a picture and a
// table. Run one sim across a range of one or two knobs, render every result
// into a grid, and print the metrics for each cell so you get numbers as well
// as an image.
//
// This is the same method that found the lambda landscape earlier in this
// project's life: sweep, measure, and let the shape of the data decide the
// story rather than assuming it.
//
// ── three things it did not do ─────────────────────────────────────────────
//
// It never said whether the sweep MOVED anything. A knob with no effect prints
// a tidy column of identical numbers over a grid of identical pictures, and
// that reads as "a flat response over this range" — a result — when it is the
// signature of the defect this project hits most. Now the run ends with a
// verdict naming every metric that never moved, and says so when the tiles are
// byte-identical.
//
// It set knob values the interface cannot produce. `k.value = v` skipped
// Knob::quantised(), so sweeping a six-position choice knob in eight steps set
// it to 0.714 and 1.43 — settings no user can reach, whose behaviour is
// therefore not a fact about the sim as shipped. Values now go through the same
// quantiser the panel uses, and the table prints what was actually set.
//
// And it drew the wrong picture for every sim that publishes a Surface: those
// render their own finished image, and their Field is a coarse stand-in.
//
// Usage:
//   sweep <simId> <knob> <min> <max> <steps> [warmScale] [out.png]
//   sweep <simId> <knobX> <minX> <maxX> <stepsX> x <knobY> <minY> <maxY> <stepsY> [warm] [out]
//
// Examples:
//   sweep boids cohesion 0 4 8
//   sweep pps alpha 90 270 6 x beta 0 40 5
//   sweep mysim density 0.1 0.9 9 1.0 anneal-density.png

#include "registry.hpp"
#include "sims/rulespec.hpp"
#include "render/raster.hpp"
#include "render/png.hpp"
#include "parallel.hpp"
#include "tool_freshness.hpp"
#include <cmath>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const bench::Entry* find(const std::string& id) {
    for (const auto& e : bench::registry()) if (e.id == id) return &e;
    return nullptr;
}

void usage() {
    std::printf(
      "usage: sweep <simId> <knob> <min> <max> <steps> [warmScale] [out.png]\n"
      "       sweep <simId> <kx> <minx> <maxx> <nx> x <ky> <miny> <maxy> <ny> [warm] [out]\n\n"
      "sims:");
    std::string era;
    for (const auto& e : bench::registry()) {
        if (e.era != era) { era = e.era; std::printf("\n  [%s]\n   ", era.c_str()); }
        std::printf(" %s", e.id.c_str());
    }
    std::printf("\n\nknobs: load a sim and it prints its own knob names.\n");
}

// Set a knob the way the PANEL would, and report the value that actually
// landed. A knob with a step lands on its grid; asking for 0.714 on a
// six-choice knob is asking about a configuration the shipped program cannot
// be in, and answering that question is worse than refusing it.
bool set_knob(bench::Sim& s, const std::string& key, float v, std::string* err,
              float* used = nullptr) {
    for (auto& k : s.knobs())
        if (k.key == key) {
            const float q = k.quantised(v);
            k.value = q; s.on_knob(k.key, q);
            if (used) *used = q;
            return true;
        }
    if (err) {
        *err = "sim has no knob '" + key + "'. it has:";
        for (auto& k : s.knobs()) *err += " " + k.key;
        if (s.knobs().empty()) *err += " (none)";
    }
    return false;
}

// Draw whatever the sim actually renders — its Surface if it has one, its Field
// otherwise. See the header: a Field published alongside a Surface is a
// stand-in, and photographing the stand-in is photographing nothing.
void draw_sim(bench::Raster& r, const bench::Sim& s, const bench::View& v) {
    if (const bench::Surface* sf = s.surface()) r.draw(*sf, v);
    else                                        r.draw(s.field(), s.palette(), v);
}

std::uint64_t tile_hash(const bench::Raster& r, int cell) {
    std::uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < cell; ++y) {
        const std::uint8_t* row = r.pixels() + std::size_t(y) * r.pitch();
        for (std::size_t i = 0; i < std::size_t(cell) * 4; ++i) { h ^= row[i]; h *= 1099511628211ull; }
    }
    return h;
}

// Did the sweep move anything? Collected across cells, reported at the end.
//
// This is the whole reason the tool exists and it was the one thing it never
// said. A column of identical numbers under a grid of identical pictures is not
// a flat response; it is a control doing nothing, and the two are
// indistinguishable unless something checks.
struct Response {
    std::vector<std::string> names;
    std::vector<double>      lo, hi;
    std::vector<std::uint64_t> tiles;
    std::vector<std::string> labels;      // what each cell was, for naming collisions
    int cells = 0;

    void observe(const std::vector<bench::Metric>& ms, std::uint64_t tile,
                 const std::string& label = {}) {
        ++cells;
        tiles.push_back(tile);
        labels.push_back(label);
        for (const auto& m : ms) {
            std::size_t i = 0;
            while (i < names.size() && names[i] != m.name) ++i;
            if (i == names.size()) { names.push_back(m.name); lo.push_back(m.value); hi.push_back(m.value); }
            else { lo[i] = std::min(lo[i], m.value); hi[i] = std::max(hi[i], m.value); }
        }
    }

    // Returns true if the sweep demonstrably changed something.
    bool report(const std::string& knob) const {
        if (cells < 2) return true;                 // one cell is not a sweep
        bool sameTiles = true;
        for (std::size_t i = 1; i < tiles.size(); ++i) if (tiles[i] != tiles[0]) sameTiles = false;
        int flat = 0;
        std::string flatNames;
        for (std::size_t i = 0; i < names.size(); ++i)
            if (!(hi[i] > lo[i] || lo[i] > hi[i])) {     // NaN-safe "did not move"
                ++flat;
                if (flat <= 6) flatNames += (flatNames.empty() ? "" : ", ") + names[i];
            }
        std::printf("\n");
        if (sameTiles && flat == int(names.size())) {
            std::printf("NOTHING MOVED. All %d tiles are byte-identical and all %zu metrics are\n"
                        "constant across the range. Over this interval '%s' is not a control.\n",
                        cells, names.size(), knob.c_str());
            return false;
        }
        if (sameTiles)
            std::printf("All %d tiles are byte-identical — '%s' changed the numbers but not the\n"
                        "picture, so the image above carries no information.\n", cells, knob.c_str());
        if (flat)
            std::printf("%d of %zu metrics never moved across the whole range: %s%s\n",
                        flat, names.size(), flatNames.c_str(),
                        flat > 6 ? ", ..." : "");
        if (!sameTiles && !flat)
            std::printf("Every metric moved and the tiles differ — '%s' is a live control here.\n",
                        knob.c_str());
        // Collisions between individual cells, which "are all the tiles the
        // same" cannot see. For a rule sweep this is the interesting one: two
        // rulestrings that render identically are one rule under two names, and
        // a rule-space picture with a hidden duplicate in it is a picture with
        // a wrong caption. Reported, never failed — a sweep is allowed to
        // revisit a setting.
        int collisions = 0;
        for (std::size_t i = 1; i < tiles.size() && !sameTiles; ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (tiles[i] == tiles[j]) {
                    if (++collisions <= 6 && !labels[i].empty())
                        std::printf("  %s and %s render byte-identically\n",
                                    labels[j].c_str(), labels[i].c_str());
                    break;
                }
        if (collisions > 6) std::printf("  ...and %d more identical pairs\n", collisions - 6);
        return true;
    }
};

} // namespace

// Sweep a list of rulestrings instead of a knob. Every tile is a different
// RULE, which is a thing the float-axis sweep structurally could not express.
int sweep_rules(int argc, char** argv) {
    std::vector<std::string> rules;
    {
        std::string cur;
        for (const char* p = argv[2]; *p; ++p) {
            if (*p == ',') { if (!cur.empty()) rules.push_back(cur); cur.clear(); }
            else if (!std::isspace(static_cast<unsigned char>(*p))) cur += *p;
        }
        if (!cur.empty()) rules.push_back(cur);
    }
    if (rules.empty()) { std::printf("no rules given\n"); return 1; }

    // Parse everything BEFORE rendering anything. Failing halfway through a
    // 64-tile sweep after two minutes of work is a waste of the user's time,
    // and a rule that cannot be parsed must never silently become another rule.
    std::vector<bench::RuleSpec> specs;
    bool bad = false;
    for (const auto& t : rules) {
        auto r = bench::parse_rule(t);
        if (!r.ok) { std::printf("  %-16s cannot parse: %s\n", t.c_str(), r.error.c_str()); bad = true; }
        specs.push_back(std::move(r));
    }
    if (bad) return 1;

    const int steps = (argc > 3) ? std::max(1, std::atoi(argv[3])) : 300;
    const std::string out = (argc > 4) ? argv[4] : "rules.png";

    // Square-ish grid, so 64 rules is 8x8 rather than a 64-wide strip.
    const int nx = std::max(1, int(std::ceil(std::sqrt(double(specs.size())))));
    const int ny = int((specs.size() + nx - 1) / nx);
    const int cell = 260;
    const int W = nx * cell, H = ny * cell;
    std::vector<std::uint8_t> sheet(std::size_t(W) * H * 4, 0);
    bench::Raster r; r.resize(cell, cell);

    std::printf("sweeping %zu rules, %d steps each -> %dx%d\n\n", specs.size(), steps, nx, ny);
    Response resp;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        auto sim = bench::make_rule(specs[i].text, 256,
                                    specs[i].generations() ? 0.12f : 0.28f,
                                    specs[i].generations() ? 0xB2A1ull : 0xC0FFEEull);
        bench::View v; v.trail = 0.88f;
        r.clear_accumulator();
        for (int s = 0; s < steps; ++s) {
            sim->step();
            if (s > steps - 40) draw_sim(r, *sim, v);
        }
        draw_sim(r, *sim, v);
        resp.observe(sim->metrics(), tile_hash(r, cell), specs[i].text);

        const int tx = int(i % nx) * cell, ty = int(i / nx) * cell;
        for (int y = 0; y < cell; ++y)
            std::memcpy(&sheet[(std::size_t(ty + y) * W + tx) * 4],
                        r.pixels() + std::size_t(y) * r.pitch(), std::size_t(cell) * 4);

        std::printf("  %-16s", specs[i].text.c_str());
        for (const auto& m : sim->metrics()) std::printf("  %s=%.4g", m.name.c_str(), m.value);
        std::printf("\n");
    }
    if (!bench::write_png(out.c_str(), W, H, sheet.data())) {
        std::printf("could not write %s\n", out.c_str()); return 1;
    }
    std::printf("\nwrote %s\n", out.c_str());
    // Two rulestrings that render identically are the same rule under two
    // names, which is worth knowing before publishing a rule-space picture.
    return resp.report("the rule") ? 0 : 1;
}

// Sweep a knob on a LEARNING sim, over epochs, with repeats.
//
// The frame sweep above is the wrong instrument for these. It runs one seed for
// a fixed number of frames and prints the final metric, and every one of those
// three choices is wrong for an agent that learns: a frame is not the unit, one
// seed is not a measurement, and the last value is not the result.
//
// This project produced four throwaway harnesses answering questions this
// should have answered, and every one of them was dominated by variance —
// six seeds of the platformer at one setting spanned 71 to 214 of a 214-wide
// level. So the output is mean, worst and best across seeds, not a number.
//
// Usage: sweep --learn <sim> <knob> <min> <max> <steps> <epochs> [repeats] [metric]
int sweep_learn(int argc, char** argv) {
    if (argc < 8) {
        std::printf("usage: sweep --learn <sim> <knob> <min> <max> <steps> <epochs> "
                    "[repeats] [metric]\n\nlearning sims:");
        for (const auto& e : bench::registry()) {
            auto s = e.make();
            if (s->epoch_name()) std::printf("  %s(%s)", e.id.c_str(), s->epoch_name());
        }
        std::printf("\n");
        return 1;
    }
    const std::string simId = argv[2], key = argv[3];
    const float v0 = float(std::atof(argv[4])), v1 = float(std::atof(argv[5]));
    const int n       = std::max(1, std::atoi(argv[6]));
    const int epochs  = std::max(1, std::atoi(argv[7]));
    const int repeats = (argc > 8) ? std::max(1, std::atoi(argv[8])) : 5;
    const std::string want = (argc > 9) ? argv[9] : "";

    const bench::Entry* entry = find(simId);
    if (!entry) { std::printf("no sim called '%s'\n", simId.c_str()); return 1; }
    std::string unit, metricName;
    bool hasSeed = false;
    {
        auto probe = entry->make();
        if (!probe->epoch_name()) {
            std::printf("'%s' has no epoch — it does not learn, so use the plain sweep\n",
                        simId.c_str());
            return 1;
        }
        unit = probe->epoch_name();
        std::string err;
        if (!set_knob(*probe, key, v0, &err)) { std::printf("%s\n", err.c_str()); return 1; }
        for (auto& k : probe->knobs()) if (k.key == "seed") hasSeed = true;
        // Pick the metric: the named one, else the first that has a direction.
        for (const auto& m : probe->metrics()) {
            if (!want.empty()) { if (m.name == want) { metricName = m.name; break; } }
            else if (m.better != bench::Metric::Neither) { metricName = m.name; break; }
        }
        if (metricName.empty()) {
            std::printf("no such metric '%s'. it reports:", want.c_str());
            for (const auto& m : probe->metrics()) std::printf("  \"%s\"", m.name.c_str());
            std::printf("\n");
            return 1;
        }
    }
    if (!hasSeed && repeats > 1)
        std::printf("note: '%s' has no seed knob, so all %d repeats are the SAME run\n",
                    simId.c_str(), repeats);

    std::printf("%s: %s %g..%g in %d, %d %ss each, %d seed%s\nmeasuring \"%s\"\n\n",
                simId.c_str(), key.c_str(), double(v0), double(v1), n, epochs,
                unit.c_str(), repeats, repeats == 1 ? "" : "s", metricName.c_str());
    std::printf("%-12s  %8s  %8s  %8s  %8s\n", key.c_str(), "mean", "worst", "best", "spread");

    std::vector<double> means;
    for (int i = 0; i < n; ++i) {
        const float v = (n == 1) ? v0 : v0 + (v1 - v0) * float(i) / float(n - 1);
        // Repeats run in parallel. Every seed is an entirely separate Sim that
        // shares nothing — which is exactly why several seeds are needed at all
        // — so this is the one place in the bench where threading is free of
        // any ordering question. Results are collected BY INDEX, never by
        // completion order, so the table is identical however many cores run it.
        // The value that LANDS, not the one that was asked for. The frame sweep
        // already marks these; a learning sweep that prints 3.33333 for a knob
        // quantised to 3 is reporting a setting the program was never in.
        float landed = v;
        { auto probe = entry->make(); set_knob(*probe, key, v, nullptr, &landed); }
        struct Rep { double best; bool have; bool higher; };
        auto reps = bench::parallel_map<Rep>(std::size_t(repeats), [&](std::size_t r) {
            auto sim = entry->make();
            if (hasSeed) set_knob(*sim, "seed", float(int(r) + 1), nullptr);
            set_knob(*sim, key, v, nullptr);
            sim->reset();          // both knobs in effect from the first epoch
            // Best-so-far in the metric's own direction, which is the result of
            // a training run — not the last value, which is one noisy sample.
            Rep out{0.0, false, true};
            for (int e = 0; e < epochs; ++e) {
                sim->advance_epoch();
                for (const auto& m : sim->metrics()) {
                    if (m.name != metricName) continue;
                    out.higher = (m.better != bench::Metric::Lower);
                    if (!out.have) { out.best = m.value; out.have = true; }
                    else out.best = out.higher ? std::max(out.best, m.value)
                                               : std::min(out.best, m.value);
                }
            }
            return out;
        });
        std::vector<double> results;
        bool higher = true;
        for (const auto& r : reps) if (r.have) { results.push_back(r.best); higher = r.higher; }
        if (results.empty()) { std::printf("  %-10g  (no result)\n", double(landed)); continue; }
        double sum = 0, lo = results[0], hi = results[0];
        for (double d : results) { sum += d; lo = std::min(lo, d); hi = std::max(hi, d); }
        const double worst = higher ? lo : hi, bestOf = higher ? hi : lo;
        means.push_back(sum / double(results.size()));
        std::printf("  %-9g%s  %8.4g  %8.4g  %8.4g  %8.4g\n",
                    double(landed), (landed != v) ? "*" : " ",
                    sum / double(results.size()), worst, bestOf, hi - lo);
    }
    // Said plainly, because the spread column is the one that decides whether
    // any difference between rows means anything at all.
    std::printf("\nspread is worst-to-best across seeds at one setting. A difference between\n"
                "rows smaller than the spread within a row is not a result.\n");
    // And the question the table cannot answer by being read: did the knob do
    // anything at all? Identical means over every setting is not a flat
    // response curve, it is a dead control, and the two look the same in print.
    bool flat = true;
    for (std::size_t i = 1; i < means.size(); ++i) if (means[i] != means[0]) flat = false;
    if (means.size() > 1 && flat) {
        std::printf("\nNOTHING MOVED. \"%s\" is identical to %.6g at every one of the %zu settings\n"
                    "of %s. Over this range it is not a control.\n",
                    metricName.c_str(), means[0], means.size(), key.c_str());
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    bench::gate("sweep");
    if (argc > 2 && std::strcmp(argv[1], "--rules") == 0) return sweep_rules(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "--learn") == 0) return sweep_learn(argc, argv);
    if (argc < 6) { usage(); return 1; }

    const std::string simId = argv[1];
    const bench::Entry* entry = find(simId);
    if (!entry) { std::printf("no sim called '%s'\n\n", simId.c_str()); usage(); return 1; }

    const std::string kx = argv[2];
    const float x0 = float(std::atof(argv[3])), x1 = float(std::atof(argv[4]));
    const int   nx = std::max(1, std::atoi(argv[5]));

    std::string ky; float y0 = 0, y1 = 0; int ny = 1;
    int argi = 6;
    if (argc > 10 && std::strcmp(argv[6], "x") == 0) {
        ky = argv[7];
        y0 = float(std::atof(argv[8])); y1 = float(std::atof(argv[9]));
        ny = std::max(1, std::atoi(argv[10]));
        argi = 11;
    }
    const float warmScale = (argc > argi) ? float(std::atof(argv[argi])) : 1.0f;
    const std::string out = (argc > argi + 1) ? argv[argi + 1] : "sweep.png";

    // Validate the knob names once, and show what's available if wrong.
    {
        auto probe = entry->make();
        std::string err;
        if (!set_knob(*probe, kx, x0, &err)) { std::printf("%s\n", err.c_str()); return 1; }
        if (!ky.empty() && !set_knob(*probe, ky, y0, &err)) { std::printf("%s\n", err.c_str()); return 1; }
    }

    const int cell = 260;
    const int W = nx * cell, H = ny * cell;
    std::vector<std::uint8_t> sheet(std::size_t(W) * H * 4, 0);
    bench::Raster r;
    r.resize(cell, cell);

    const int warm = std::max(1, int(entry->warm * warmScale));
    std::printf("sweeping %s: %s %g..%g in %d", simId.c_str(), kx.c_str(),
                double(x0), double(x1), nx);
    if (!ky.empty()) std::printf("  x  %s %g..%g in %d", ky.c_str(), double(y0), double(y1), ny);
    std::printf("   (%d steps each)\n\n", warm);

    Response resp;
    std::vector<float> distinctX;
    for (int iy = 0; iy < ny; ++iy) {
        const float vy = (ny == 1) ? y0 : y0 + (y1 - y0) * float(iy) / float(ny - 1);
        for (int ix = 0; ix < nx; ++ix) {
            const float vx = (nx == 1) ? x0 : x0 + (x1 - x0) * float(ix) / float(nx - 1);

            auto sim = entry->make();
            float gotX = vx, gotY = vy;
            set_knob(*sim, kx, vx, nullptr, &gotX);
            if (!ky.empty()) set_knob(*sim, ky, vy, nullptr, &gotY);
            sim->reset();                       // so the knob is in effect from step 0
            if (iy == 0 &&
                std::find(distinctX.begin(), distinctX.end(), gotX) == distinctX.end())
                distinctX.push_back(gotX);

            bench::View v; v.trail = 0.88f;
            r.clear_accumulator();
            for (int s = 0; s < warm; ++s) {
                sim->step();
                if (s > warm - 40) draw_sim(r, *sim, v);
            }
            draw_sim(r, *sim, v);
            resp.observe(sim->metrics(), tile_hash(r, cell));

            const int tx = ix * cell, ty = iy * cell;
            for (int y = 0; y < cell; ++y)
                std::memcpy(&sheet[(std::size_t(ty + y) * W + tx) * 4],
                            r.pixels() + std::size_t(y) * r.pitch(), std::size_t(cell) * 4);

            // the numbers, which are the half of this that survives being looked at.
            // The value printed is the one that LANDED, marked when the quantiser
            // moved it, so nobody reads a row as evidence about a setting the
            // program was never actually in.
            std::printf("  %s=%-8g%s", kx.c_str(), double(gotX), (gotX != vx) ? "*" : " ");
            if (!ky.empty())
                std::printf("%s=%-8g%s", ky.c_str(), double(gotY), (gotY != vy) ? "*" : " ");
            for (const auto& m : sim->metrics())
                std::printf("  %s=%.4g", m.name.c_str(), m.value);
            std::printf("\n");
        }
    }

    if (!bench::write_png(out.c_str(), W, H, sheet.data())) {
        std::printf("could not write %s\n", out.c_str());
        return 1;
    }
    std::printf("\nwrote %s (%dx%d)  — left-to-right is %s%s\n",
                out.c_str(), W, H, kx.c_str(),
                ky.empty() ? "" : (", top-to-bottom is " + ky).c_str());
    // A sweep whose columns collapse onto the same setting is not the sweep
    // that was asked for, and repeating a tile is not evidence about a range.
    if (int(distinctX.size()) < nx)
        std::printf("\nNOTE: %d columns but only %zu distinct settings of %s — the rest are\n"
                    "duplicates the quantiser folded together. Ask for %zu steps, or a wider range.\n",
                    nx, distinctX.size(), kx.c_str(), distinctX.size());
    const bool moved = resp.report(kx);
    // Nonzero when the swept knob demonstrably did nothing, so this can be used
    // as a check and not only as a picture.
    return moved ? 0 : 1;
}
