// runner.hpp — one simulation, the whole machine, and nothing else.
//
// The workbench shows a world while it runs, and everything it does to stay
// responsive costs that world something: a step batch is cut off after eight
// milliseconds so the window can paint, the timeline copies the field every
// frame, and a single step longer than a frame freezes the window because
// nothing can pre-empt it. That is the right trade for exploring. It is the
// wrong one for a run you have already configured and simply want to be big,
// long, or both.
//
// This hosts exactly one simulation — a library entry by id, the rulestring
// entry with its rule, or your own plugin DLL — with no window, no timeline and
// no frame clock, and writes what it measured to disk as it goes:
//
//   <out>/metrics.csv              a row every N units, streamed and flushed
//   <out>/metrics-by-<epoch>.csv   a row per completed epoch (learning sims)
//   <out>/frame-<n>.png            optional pictures, every N units
//   <out>/run.json                 the configuration, the outcome, final metrics
//
// STREAMED, not kept in History's trace ring. The ring holds only the most
// recent samples, so a long run would silently lose its own beginning, and it
// rate-limits by wall-clock time — the property that made the plugin test
// flaky. A dedicated run samples by unit count and keeps everything it records.
//
// Settings are applied the way Apply & restart applies them — on_knob for each,
// then one reset — and read the way the panel reads typed input, so a value the
// shipped program cannot be in is REFUSED, never clamped into something else.

#pragma once
#include "registry.hpp"
#include "plugin.hpp"
#include "paths.hpp"
#include "workflows.hpp"
#include "render/raster.hpp"
#include "render/png.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace bench::run {

struct Setting { std::string key, text; };

struct Config {
    std::string target;              // library id, or a path ending in .dll
    std::string rule;                // rulestring, for the "rule" entry only
    std::vector<Setting> settings;   // --set key=value, in the order given
    long long steps   = 0;           // stop after this many steps      (0 = no limit)
    long long epochs  = 0;           // stop after this many epochs     (0 = step mode)
    double    seconds = 0.0;         // stop after this much wall time  (0 = no limit)
    long long sample  = 0;           // a metrics row every N units     (0 = automatic)
    long long frames  = 0;           // a PNG every N units             (0 = off)
    int       frameSize = 512;       // longest side of a frame, in pixels
    std::string out;                 // results folder
    double    progressEvery = 1.0;   // seconds between progress lines
};

enum class Exit : int {
    Done        = 0,   // reached the steps, epochs or time asked for
    Stopped     = 1,   // asked to stop, or the run could not advance any further
    BadArgs     = 2,
    LoadFailed  = 3,
    StepFailed  = 4,
    OutOfMemory = 5,
};

struct Outcome {
    Exit        exit = Exit::Done;
    std::string reason;      // steps|epochs|seconds|stop-file|interrupted|stalled|error|out-of-memory
    std::string error;
    long long   units = 0;           // steps in step mode, epochs in epoch mode
    long long   steps = 0, epochs = 0;
    long long   rows = 0, epochRows = 0, framesWritten = 0;
    long long   firstMetricsAt = -1; // the unit at which the sim first reported anything
    double      seconds = 0.0;
    std::vector<std::string> warnings;
    std::set<std::string>    lateMetrics;   // names that appeared after the columns were fixed
};

// ── formatting ──────────────────────────────────────────────────────────────

inline std::string thousands(long long n) {
    const std::string s = std::to_string(n < 0 ? -n : n);
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i && (s.size() - i) % 3 == 0) o += ',';
        o += s[i];
    }
    return (n < 0 ? "-" : "") + o;
}

inline std::string duration(double s) {
    char b[40];
    if (s < 60.0)        std::snprintf(b, sizeof b, "%.1fs", s);
    else if (s < 3600.0) std::snprintf(b, sizeof b, "%dm %02ds", int(s) / 60, int(s) % 60);
    else                 std::snprintf(b, sizeof b, "%dh %02dm", int(s) / 3600, (int(s) % 3600) / 60);
    return b;
}

// "1,000 episodes", "3 lives" — and for an epoch name that is already a
// quantity ("100 epochs", "tree rung"), "1,000 x 100 epochs" rather than the
// "1,000 100 epochss" a blind plural would make of it. ASCII on purpose: this
// also goes to a console, whose code page is not the app's.
inline std::string count_of(long long n, const std::string& unit) {
    if (unit.find(' ') != std::string::npos) return thousands(n) + " x " + unit;
    if (n == 1) return "1 " + unit;
    return thousands(n) + " " + (unit == "life" ? std::string("lives") : unit + "s");
}

// A blank cell for a value that is not a number: every CSV reader treats blank
// as missing, and "nan" or "inf" in a numeric column breaks half of them.
inline std::string number(double v) {
    if (!std::isfinite(v)) return {};
    char b[32];
    std::snprintf(b, sizeof b, "%.10g", v);
    return b;
}

// Metric names are free text and plugins choose them, so quote anything a
// spreadsheet would otherwise split into two columns.
inline std::string csv_field(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string q = "\"";
    for (char ch : s) { if (ch == '"') q += '"'; q += ch; }
    return q + "\"";
}

inline std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (unsigned char ch : s) {
        switch (ch) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (ch < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
                else o += char(ch);
        }
    }
    return o + "\"";
}

inline std::string json_num(double v) {
    if (!std::isfinite(v)) return "null";
    char b[32];
    std::snprintf(b, sizeof b, "%.10g", v);
    return b;
}

// ── reading the command line ────────────────────────────────────────────────

// A count of steps or epochs. "1e6" is accepted because people type it; a
// fraction, a negative or trailing junk is refused rather than truncated.
inline bool parse_count(const std::string& s, long long& v) {
    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(d) || d < 0.0
        || d != std::floor(d) || d > 9.0e15) return false;
    v = static_cast<long long>(d);
    return true;
}

inline bool parse_seconds(const std::string& s, double& v) {
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0' && std::isfinite(v) && v >= 0.0;
}

inline bool is_dll(const std::string& t) {
    if (t.size() < 5) return false;
    std::string ext = t.substr(t.size() - 4);
    for (auto& ch : ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return ext == ".dll";
}

inline bool parse_args(const std::vector<std::string>& a, Config& c, std::string& err) {
    auto value = [&](std::size_t& i, std::string& out) {
        if (i + 1 >= a.size()) { err = a[i] + " needs a value"; return false; }
        out = a[++i];
        return true;
    };
    for (std::size_t i = 0; i < a.size(); ++i) {
        const std::string s = a[i];
        std::string v;
        if (s == "--set") {
            if (!value(i, v)) return false;
            const auto eq = v.find('=');
            if (eq == std::string::npos || eq == 0) {
                err = "--set expects key=value, got '" + v + "'";
                return false;
            }
            c.settings.push_back({v.substr(0, eq), v.substr(eq + 1)});
        } else if (s == "--rule") {
            if (!value(i, c.rule)) return false;
        } else if (s == "--out") {
            if (!value(i, c.out)) return false;
        } else if (s == "--steps" || s == "--epochs" || s == "--sample" || s == "--frames") {
            if (!value(i, v)) return false;
            long long n = 0;
            if (!parse_count(v, n)) { err = s + " expects a whole number, got '" + v + "'"; return false; }
            if (s == "--steps") c.steps = n;
            else if (s == "--epochs") c.epochs = n;
            else if (s == "--sample") c.sample = n;
            else c.frames = n;
        } else if (s == "--seconds") {
            if (!value(i, v)) return false;
            if (!parse_seconds(v, c.seconds)) { err = "--seconds expects a number, got '" + v + "'"; return false; }
        } else if (s == "--progress-every") {
            if (!value(i, v)) return false;
            if (!parse_seconds(v, c.progressEvery) || c.progressEvery <= 0.0) {
                err = "--progress-every expects a positive number of seconds, got '" + v + "'";
                return false;
            }
        } else if (s == "--frame-size") {
            if (!value(i, v)) return false;
            long long n = 0;
            if (!parse_count(v, n) || n < 16 || n > 8192) {
                err = "--frame-size expects 16 to 8192 pixels, got '" + v + "'";
                return false;
            }
            c.frameSize = int(n);
        } else if (s.size() > 1 && s[0] == '-') {
            err = "unknown option '" + s + "'";
            return false;
        } else if (c.target.empty()) {
            c.target = s;
        } else {
            err = "unexpected '" + s + "': the simulation is already '" + c.target + "'";
            return false;
        }
    }
    if (c.target.empty()) { err = "no simulation given"; return false; }
    if (c.steps > 0 && c.epochs > 0) {
        err = "--steps and --epochs count different things; give one of them";
        return false;
    }
    if (!c.rule.empty() && c.target != "rule") {
        err = "--rule only applies to the 'rule' simulation";
        return false;
    }
    return true;
}

// ── making the one simulation ───────────────────────────────────────────────

// Null with `err` set on failure. `kind` is "library", "rule" or "plugin".
// The rulestring entry is built exactly as the workbench's rule box builds it,
// so a dedicated run of a typed rule starts where the window showed it starting.
inline SimPtr make_target(const Config& c, Plugin& plug, std::string& kind, std::string& err) {
    if (is_dll(c.target)) {
        kind = "plugin";
        std::error_code ec;
        if (!std::filesystem::exists(c.target, ec)) { err = "no such plugin: " + c.target; return nullptr; }
        if (!plug.load(c.target)) { err = "could not load " + c.target + ": " + plug.error(); return nullptr; }
        SimPtr sim = plug.make();
        if (!sim) err = c.target + " loaded, but its bench_create_sim returned nothing";
        return sim;
    }
    for (const auto& e : registry()) {
        if (e.id != c.target) continue;
        if (e.id == "rule" && !c.rule.empty()) {
            kind = "rule";
            const RuleSpec r = parse_rule(c.rule);
            if (!r.ok) { err = "rule '" + c.rule + "' does not parse: " + r.error; return nullptr; }
            SimPtr sim = make_rule_workspace(c.rule);
            if (!sim) err = "could not build rule '" + c.rule + "'";
            return sim;
        }
        kind = "library";
        return e.make();
    }
    err = "no simulation called '" + c.target + "'. Run bench_run --list to see them.";
    return nullptr;
}

// What a typed setting means for this knob. A choice accepts its label as the
// app shows it ("4096") — label first, so a numeric label is never mistaken for
// an index — or its index. Anything else is read exactly as the panel reads a
// typed number: finite, inside the knob's range, quantised to its grid.
inline bool setting_value(const Knob& k, const std::string& text, float& v, std::string& err) {
    for (std::size_t i = 0; i < k.choices.size(); ++i)
        if (k.choices[i] == text) { v = k.min + float(i); return true; }
    if (parse_control_value(text, k, v)) return true;
    std::ostringstream m;
    m << "'" << text << "' is not a value '" << k.key << "' can take: ";
    if (!k.choices.empty()) {
        m << "one of";
        for (const auto& ch : k.choices) m << " '" << ch << "'";
        m << ", or an index from " << k.min << " to " << k.max;
    } else {
        m << "a number from " << k.min << " to " << k.max;
    }
    err = m.str();
    return false;
}

// Apply every setting through on_knob, then reset once if anything was set —
// the Apply & restart sequence, and nothing more. A sim opened with no settings
// is NOT reset, because opening a sim in the window does not reset it either,
// and a reset that advances an RNG would make the two start differently.
inline bool apply_settings(Sim& sim, const std::vector<Setting>& settings,
                           std::vector<std::string>& warnings, std::string& err) {
    for (const auto& s : settings) {
        const auto& knobs = sim.knobs();
        const auto it = std::find_if(knobs.begin(), knobs.end(),
                                     [&](const Knob& k) { return k.key == s.key; });
        if (it == knobs.end()) {
            err = "this simulation has no setting '" + s.key + "'. It has:";
            for (const auto& k : knobs) err += " " + k.key;
            if (knobs.empty()) err += " (none)";
            return false;
        }
        float v = 0.0f;
        if (!setting_value(*it, s.text, v, err)) return false;
        const std::string key = it->key;      // on_knob may rebuild the knob list
        const float before = it->value;
        sim.on_knob(key, v);
        for (auto& k : sim.knobs()) {
            if (k.key != key || k.value == v) continue;
            if (k.value == before && before != v) {
                // The host contract is that on_knob records the value, and every
                // library entry does. A plugin that forgets would run on v while
                // reporting the old value, so record it rather than let run.json
                // describe a configuration the run was not in.
                k.value = v;
                warnings.push_back("the simulation did not record '" + key +
                                   "'; the runner recorded the value it was given");
            } else {
                // The sim chose a different value itself — life3d clamps its seed
                // size to the world, for one. Its answer stands; say so.
                warnings.push_back("asked for '" + key + "' = " + s.text + "; the simulation set " +
                                   number(k.value));
            }
        }
    }
    if (!settings.empty()) sim.reset();
    return true;
}

// ── writing what happened ───────────────────────────────────────────────────

struct Table {
    std::string path, clock;
    std::ofstream out;
    std::vector<std::string> names;   // fixed by the first sample that had any
    bool open = false;
    long long rows = 0;
};

// The columns are fixed by the first non-empty sample. Some sims report nothing
// until they have something true to say — the movement lab publishes no fitness
// until a generation completes, rather than seed its curve with a fictional 0 —
// so an empty sample neither opens the file nor fixes the columns. A metric that
// appears later is recorded as late rather than silently dropped.
inline void add_row(Table& t, long long unit, std::uint64_t gen, double secs,
                    const std::vector<Metric>& ms, Outcome& o) {
    if (!t.open) {
        if (ms.empty()) return;
        t.out.open(t.path);
        if (!t.out) throw std::runtime_error("cannot write " + t.path);
        t.out << t.clock << ",generation,elapsed_s";
        for (const auto& m : ms)
            if (std::find(t.names.begin(), t.names.end(), m.name) == t.names.end()) {
                t.names.push_back(m.name);
                t.out << ',' << csv_field(m.name);
            }
        t.out << '\n';
        t.open = true;
    }
    t.out << unit << ',' << gen << ',' << number(secs);
    for (const auto& n : t.names) {
        t.out << ',';
        for (const auto& m : ms) if (m.name == n) { t.out << number(m.value); break; }
    }
    for (const auto& m : ms)
        if (std::find(t.names.begin(), t.names.end(), m.name) == t.names.end())
            o.lateMetrics.insert(m.name);
    t.out << '\n';
    t.out.flush();            // a run that is killed keeps every row it wrote
    ++t.rows;
}

// Drawn through the same Raster the window and the contact sheet use: the sim's
// Surface if it publishes one, its Field and palette otherwise.
inline bool write_frame(Sim& sim, const std::string& path, int longest) {
    const Surface* s = sim.surface();
    const int sw = s ? s->w : sim.field().w;
    const int sh = s ? s->h : sim.field().h;
    if (sw <= 0 || sh <= 0) return false;
    int fw = longest, fh = longest;
    if (sw >= sh) fh = std::max(1, int(std::lround(double(longest) * sh / sw)));
    else          fw = std::max(1, int(std::lround(double(longest) * sw / sh)));
    Raster r;
    r.resize(fw, fh);
    r.clear_accumulator();
    View v;
    v.grid = false;
    if (s) r.draw(*s, v);
    else   r.draw(sim.field(), sim.palette(), v);
    return write_png(path.c_str(), fw, fh, r.pixels());
}

inline std::string progress_line(const std::string& unit, long long done, long long target,
                                 double secs, const std::vector<Metric>& ms) {
    std::ostringstream p;
    p << "progress " << unit << ' ' << thousands(done);
    if (target > 0) p << " of " << thousands(target);
    p << " | " << duration(secs);
    if (done > 0) {
        const double per = secs / double(done);
        if (per >= 1.0) p << " | " << duration(per) << '/' << unit;
        else {
            char b[48];
            std::snprintf(b, sizeof b, " | %.3g ms/%s", per * 1000.0, unit.c_str());
            p << b;
        }
        if (target > done) p << " | about " << duration(per * double(target - done)) << " left";
    }
    int shown = 0;
    for (const auto& m : ms) {
        if (shown++ == 3) break;
        p << " | " << m.name << ' ' << number(m.value);
    }
    return p.str();
}

inline std::string describe_stop(const Config& c, const Sim& sim) {
    std::string s;
    if (c.epochs > 0) s = "until " + count_of(c.epochs, sim.epoch_name());
    else if (c.steps > 0) s = "until " + count_of(c.steps, "step");
    if (c.seconds > 0.0) s += (s.empty() ? "for " : ", at most ") + duration(c.seconds);
    if (s.empty()) s = "until stopped (Ctrl+C, or create a file named 'stop' in the results folder)";
    return s;
}

inline std::string timestamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
    return b;
}

// runs\<life>-<date>-<time>, with a suffix when two runs land in the same second
// so that neither overwrites the other's results.
inline std::string default_out(const std::string& target) {
    namespace fs = std::filesystem;
    const fs::path base = fs::path(Paths::get().root()) / "runs";
    const std::string stem = fs::path(target).stem().string() + "-" + timestamp();
    fs::path p = base / stem;
    std::error_code ec;
    for (int n = 2; fs::exists(p, ec); ++n) p = base / (stem + "-" + std::to_string(n));
    return p.string();
}

// ── the run ─────────────────────────────────────────────────────────────────

inline Outcome execute(Sim& sim, const Config& c, std::ostream& log,
                       const std::atomic<bool>* interrupt = nullptr) {
    namespace fs = std::filesystem;
    using Clock = std::chrono::steady_clock;
    Outcome o;
    std::error_code ec;
    fs::create_directories(c.out, ec);
    if (!fs::is_directory(c.out, ec)) {
        o.exit = Exit::BadArgs; o.reason = "error";
        o.error = "cannot create the results folder " + c.out;
        return o;
    }
    const bool byEpoch = c.epochs > 0;
    if (byEpoch && !sim.epoch_name()) {
        o.exit = Exit::BadArgs; o.reason = "error";
        o.error = "this simulation does not learn in epochs; use --steps or --seconds";
        return o;
    }
    const std::string unit = byEpoch ? sim.epoch_name() : "step";
    const long long target = byEpoch ? c.epochs : c.steps;
    const long long sample = c.sample > 0 ? c.sample
                           : byEpoch     ? 1
                           : c.steps > 0 ? std::max<long long>(1, c.steps / 2000)
                                         : 100;

    const fs::path dir(c.out);
    const std::string stopFile = (dir / "stop").string();
    fs::remove(stopFile, ec);        // a request left by an earlier run must not end this one
    // Said only AFTER the old request is gone. A host that wants to stop the run
    // waits for this line before writing its own stop file — otherwise a Stop
    // pressed in the first instant is deleted here as "left over", and the run
    // it was meant to end carries on.
    log << "running" << std::endl;

    Table perUnit;
    perUnit.path = (dir / "metrics.csv").string();
    perUnit.clock = unit;
    Table perEpoch;
    const bool hasEpochs = sim.epoch_name() != nullptr;
    if (hasEpochs) {
        perEpoch.path = export_path(c.out, std::string("metrics-by-") + sim.epoch_name() + ".csv");
        perEpoch.clock = sim.epoch_name();
    }

    const auto t0 = Clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(Clock::now() - t0).count(); };
    const std::uint64_t gen0 = sim.generation();
    const int epoch0 = sim.epoch_count();
    int lastEpoch = epoch0;
    long long units = 0, lastRow = -1;
    double nextProgress = c.progressEvery, nextStopCheck = 0.0;
    bool frameFailed = false;

    auto row = [&](double secs) {
        add_row(perUnit, units, sim.generation(), secs, sim.metrics(), o);
        if (o.firstMetricsAt < 0 && perUnit.open) o.firstMetricsAt = units;
        lastRow = units;
    };
    auto frame = [&](const std::string& name) {
        if (write_frame(sim, (dir / name).string(), c.frameSize)) ++o.framesWritten;
        else if (!frameFailed) { frameFailed = true; o.warnings.push_back("could not write " + name); }
    };

    try {
        row(0.0);                                   // the state the run started from
        if (c.frames > 0) frame("frame-0.png");
        for (;;) {
            if (interrupt && interrupt->load()) { o.exit = Exit::Stopped; o.reason = "interrupted"; break; }
            if (target > 0 && units >= target) { o.reason = byEpoch ? "epochs" : "steps"; break; }
            const double now = elapsed();
            if (c.seconds > 0.0 && now >= c.seconds) { o.reason = "seconds"; break; }
            if (now >= nextStopCheck) {
                nextStopCheck = now + std::min(c.progressEvery, 0.25);
                if (fs::exists(stopFile, ec)) { o.exit = Exit::Stopped; o.reason = "stop-file"; break; }
            }
            if (byEpoch) {
                // An epoch that does not complete is not an epoch — the same rule
                // the window applies, for the same reason: a learner that cannot
                // reach its next milestone would otherwise spin here forever.
                const int before = sim.epoch_count();
                const bool ok = sim.advance_epoch();
                if (sim.epoch_count() == before) {
                    o.exit = Exit::Stopped; o.reason = "stalled";
                    o.error = std::string("a ") + sim.epoch_name() + " did not complete" +
                              (ok ? "" : ": the simulation declined to start one");
                    break;
                }
            } else {
                sim.step();
            }
            ++units;
            const double secs = elapsed();
            if (units % sample == 0) row(secs);
            if (hasEpochs && sim.epoch_count() != lastEpoch) {
                lastEpoch = sim.epoch_count();
                add_row(perEpoch, lastEpoch, sim.generation(), secs, sim.metrics(), o);
            }
            if (c.frames > 0 && units % c.frames == 0) frame("frame-" + std::to_string(units) + ".png");
            if (secs >= nextProgress) {
                nextProgress = secs + c.progressEvery;
                log << progress_line(unit, units, target, secs, sim.metrics()) << std::endl;
            }
        }
    } catch (const std::bad_alloc&) {
        o.exit = Exit::OutOfMemory; o.reason = "out-of-memory";
        o.error = "out of memory during " + unit + " " + thousands(units + 1);
    } catch (const std::exception& e) {
        o.exit = Exit::StepFailed; o.reason = "error";
        o.error = std::string(e.what()) + " (during " + unit + " " + thousands(units + 1) + ")";
    }

    // End every table on the state the run actually ended in — but not after a
    // failure, when the sim is in whatever state threw and asking it to report
    // or render is asking it to fail again. What was written before stands.
    if (o.exit == Exit::Done || o.exit == Exit::Stopped) {
        try {
            if (lastRow != units) row(elapsed());
            if (c.frames > 0) frame("frame-final.png");
        } catch (const std::exception& e) {
            o.warnings.push_back(std::string("could not record the final state: ") + e.what());
        }
    }
    o.units = units;
    o.steps = byEpoch ? static_cast<long long>(sim.generation() - gen0) : units;
    o.epochs = sim.epoch_count() - epoch0;
    o.rows = perUnit.rows;
    o.epochRows = perEpoch.rows;
    o.seconds = elapsed();
    return o;
}

inline bool write_summary(const std::string& path, const Config& c, const Outcome& o,
                          Sim& sim, const std::string& kind) {
    std::ofstream j(path);
    if (!j) return false;
    const bool byEpoch = c.epochs > 0;
    j << "{\n";
    j << "  \"target\": " << json_str(c.target) << ",\n";
    j << "  \"kind\": " << json_str(kind) << ",\n";
    j << "  \"title\": " << json_str(sim.about().title) << ",\n";
    if (!c.rule.empty()) j << "  \"rule\": " << json_str(c.rule) << ",\n";
    j << "  \"exit_code\": " << int(o.exit) << ",\n";
    j << "  \"reason\": " << json_str(o.reason) << ",\n";
    j << "  \"error\": " << json_str(o.error) << ",\n";
    j << "  \"unit\": " << json_str(byEpoch ? sim.epoch_name() : "step") << ",\n";
    j << "  \"requested\": {\"steps\": " << c.steps << ", \"epochs\": " << c.epochs
      << ", \"seconds\": " << json_num(c.seconds) << "},\n";
    j << "  \"units\": " << o.units << ",\n";
    j << "  \"steps\": " << o.steps << ",\n";
    j << "  \"epochs\": " << o.epochs << ",\n";
    j << "  \"generation\": " << sim.generation() << ",\n";
    j << "  \"seconds\": " << json_num(o.seconds) << ",\n";
    j << "  \"ms_per_unit\": " << json_num(o.units > 0 ? 1000.0 * o.seconds / double(o.units) : 0.0) << ",\n";
    j << "  \"rows\": " << o.rows << ",\n";
    j << "  \"epoch_rows\": " << o.epochRows << ",\n";
    j << "  \"frames\": " << o.framesWritten << ",\n";
    j << "  \"first_metrics_at\": " << o.firstMetricsAt << ",\n";
    j << "  \"settings\": [";
    for (std::size_t i = 0; i < c.settings.size(); ++i)
        j << (i ? ", " : "") << "{\"key\": " << json_str(c.settings[i].key)
          << ", \"value\": " << json_str(c.settings[i].text) << "}";
    j << "],\n";
    // Every knob's final value, not just the ones that were set: a run is only
    // reproducible if the values nobody touched are on the record too.
    j << "  \"knobs\": {";
    bool first = true;
    for (const auto& k : sim.knobs()) {
        if (k.display_only) continue;
        j << (first ? "" : ", ") << json_str(k.key) << ": {\"value\": " << json_num(k.value)
          << ", \"shown\": " << json_str(k.shown()) << "}";
        first = false;
    }
    j << "},\n";
    j << "  \"metrics\": {";
    first = true;
    if (o.exit == Exit::Done || o.exit == Exit::Stopped)
        for (const auto& m : sim.metrics()) {
            j << (first ? "" : ", ") << json_str(m.name) << ": " << json_num(m.value);
            first = false;
        }
    j << "},\n";
    j << "  \"warnings\": [";
    for (std::size_t i = 0; i < o.warnings.size(); ++i) j << (i ? ", " : "") << json_str(o.warnings[i]);
    j << "],\n";
    j << "  \"late_metrics\": [";
    first = true;
    for (const auto& n : o.lateMetrics) { j << (first ? "" : ", ") << json_str(n); first = false; }
    j << "]\n";
    j << "}\n";
    return bool(j);
}

} // namespace bench::run
