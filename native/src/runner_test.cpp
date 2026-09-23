// runner_test.cpp — the dedicated runner, checked in-process.
//
// The property that matters most is the first one: a dedicated run must be the
// SAME simulation as stepping it by hand. Everything the runner adds — settings,
// sampling, CSV, frames, the summary — has to be observation only. A runner that
// quietly changed what it was running would produce big, long, confident
// results about a different experiment.
//
// Usage: bench_runner_test [folder holding the example plugin DLL]
// The folder is optional; when given, the plugin path is exercised end to end.

#include "runner.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

namespace {

int checks = 0, failed = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) { ++failed; std::printf("FAIL %s\n", what.c_str()); }
}

namespace fs = std::filesystem;
using namespace bench::run;

// Nothing in it but the behaviours the runner has to survive.
class Fake : public bench::Sim {
public:
    int  quietFor = 0;       // report no metrics while generation < quietFor
    int  lateAt   = -1;      // add a metric once generation >= lateAt
    int  throwAt  = -1;      // throw from step() when generation == throwAt
    bool records  = true;    // on_knob records the value, as the host contract says
    int  resets   = 0;
    std::function<void(std::uint64_t)> onStep;

    Fake() : field_(8, 8) {
        knobs_.push_back({"density", "density", 0.f, 1.f, 0.5f, 0.25f, {}, true});
        knobs_.push_back({"size", "size", 0.f, 2.f, 0.f, 1.f,
                          std::vector<std::string>{"small", "medium", "large"}, true});
        knobs_.push_back({"clamped", "clamped", 0.f, 10.f, 1.f, 1.f, {}, true});
    }
    const bench::Provenance& about() const override {
        static const bench::Provenance p = [] { bench::Provenance q; q.title = "Fake"; return q; }();
        return p;
    }
    const std::vector<bench::Swatch>& palette() const override {
        static const std::vector<bench::Swatch> s{{{0, 0, 0}, "off"}, {{255, 255, 255}, "on"}};
        return s;
    }
    const bench::Field& field() const override { return field_; }
    void reset() override { gen_ = 0; ++resets; }
    void step() override {
        if (throwAt >= 0 && int(gen_) == throwAt) throw std::runtime_error("fake failure");
        ++gen_;
        field_.cells[gen_ % field_.cells.size()] ^= 1;
        if (onStep) onStep(gen_);
    }
    std::uint64_t generation() const override { return gen_; }
    std::vector<bench::Knob>& knobs() override { return knobs_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) {
            if (kn.key != k) continue;
            if (k == "clamped") kn.value = std::min(v, 5.0f);   // corrects, the way life3d does
            else if (records)   kn.value = v;
        }
    }
    std::vector<bench::Metric> metrics() const override {
        if (int(gen_) < quietFor) return {};
        std::vector<bench::Metric> m{{"gen", double(gen_)}, {"with, comma", 1.0}};
        if (lateAt >= 0 && int(gen_) >= lateAt) m.push_back({"late", 2.0});
        return m;
    }

private:
    bench::Field field_;
    std::vector<bench::Knob> knobs_;
    std::uint64_t gen_ = 0;
};

// A unique folder per case, and only the files the runner documents writing are
// removed afterwards — never a recursive delete, the same rule the workflow
// test keeps.
struct Scratch {
    fs::path dir;
    explicit Scratch(const std::string& name) {
        dir = fs::temp_directory_path() /
              ("bench runner test " + name + " " +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    }
    ~Scratch() {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string n = e.path().filename().string();
            if (n == "metrics.csv" || n == "run.json" || n == "stop" ||
                n.rfind("metrics-by-", 0) == 0 || n.rfind("frame-", 0) == 0)
                fs::remove(e.path(), ec);
        }
        fs::remove(dir, ec);
    }
    [[nodiscard]] std::string str() const { return dir.string(); }
};

std::vector<std::string> lines_of(const fs::path& p) {
    std::ifstream in(p);
    std::vector<std::string> out;
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s; s << in.rdbuf();
    return s.str();
}

bool png_size(const fs::path& p, int& w, int& h) {
    const std::string b = slurp(p);
    if (b.size() < 24 || std::memcmp(b.data(), "\x89PNG\r\n\x1a\n", 8) != 0) return false;
    auto be = [&](std::size_t o) {
        return (int(std::uint8_t(b[o])) << 24) | (int(std::uint8_t(b[o + 1])) << 16) |
               (int(std::uint8_t(b[o + 2])) << 8) | int(std::uint8_t(b[o + 3]));
    };
    w = be(16); h = be(20);
    return true;
}

bench::SimPtr library(const std::string& id) {
    for (const auto& e : bench::registry()) if (e.id == id) return e.make();
    return nullptr;
}

Config quiet(const std::string& out) {
    Config c; c.out = out; c.progressEvery = 1e9;   // no progress lines in the test log
    return c;
}

} // namespace

int main(int argc, char** argv) {
    std::ostringstream sink;   // progress lines go here, not to the test output

    // ── the command line ────────────────────────────────────────────────────
    {
        Config c; std::string err;
        check(parse_args({"life", "--set", "size=4096", "--steps", "1e3", "--out", "x"}, c, err)
              && c.target == "life" && c.steps == 1000 && c.settings.size() == 1
              && c.settings[0].key == "size" && c.settings[0].text == "4096" && c.out == "x",
              "a well-formed command line parses, and 1e3 is a thousand steps");
        const std::vector<std::vector<std::string>> bad = {
            {}, {"--steps", "1.5", "life"}, {"life", "--set", "size"}, {"life", "--set", "=3"},
            {"life", "--steps", "10", "--epochs", "2"}, {"life", "--bogus"}, {"life", "extra"},
            {"life", "--rule", "B3/S23"}, {"life", "--steps"}, {"life", "--steps", "-4"},
            {"life", "--frame-size", "4"}, {"life", "--progress-every", "0"}};
        for (const auto& b : bad) {
            Config d; std::string e;
            std::string shown; for (const auto& s : b) shown += s + " ";
            check(!parse_args(b, d, e) && !e.empty(), "refused with a reason: " + shown);
        }
    }

    // ── the formatting the progress line and the files depend on ───────────
    check(thousands(1234567) == "1,234,567" && thousands(0) == "0" && thousands(-1234) == "-1,234",
          "thousands separators");
    check(count_of(1000, "episode") == "1,000 episodes" && count_of(3, "life") == "3 lives"
          && count_of(1, "step") == "1 step" && count_of(1000, "100 epochs") == "1,000 x 100 epochs",
          "counts read as English, including epoch names that are already quantities");
    check(csv_field("plain") == "plain" && csv_field("with, comma") == "\"with, comma\"" &&
          csv_field("q\"x") == "\"q\"\"x\"", "CSV quotes a metric name that would split a column");
    check(json_str("a\"b\\c\n") == "\"a\\\"b\\\\c\\n\"", "JSON escapes quotes, backslashes, newlines");
    check(json_num(std::nan("")) == "null" && number(std::nan("")).empty(),
          "a non-number is null in JSON and blank in CSV, never 'nan'");
    {
        const std::string p = progress_line("step", 500, 1000, 5.0, {{"gen", 500}});
        check(p.rfind("progress step 500 of 1,000", 0) == 0 && p.find("about 5.0s left") != std::string::npos
              && p.find("gen 500") != std::string::npos, "progress line: position, estimate, a metric");
    }

    // ── making the one simulation ───────────────────────────────────────────
    {
        bench::Plugin plug; std::string kind, err;
        Config c; c.target = "life";
        check(make_target(c, plug, kind, err) != nullptr && kind == "library", "a library id makes that simulation");
        c.target = "nope";
        check(!make_target(c, plug, kind, err) && err.find("no simulation called") != std::string::npos,
              "an unknown id is refused, naming the way to list them");
        c.target = "missing-plugin.dll";
        check(!make_target(c, plug, kind, err) && err.find("no such plugin") != std::string::npos,
              "a missing DLL is refused before any attempt to load it");
        c.target = "rule"; c.rule = "B36/S23";
        check(make_target(c, plug, kind, err) != nullptr && kind == "rule", "the rule entry takes a typed rule");
        c.rule = "not a rule";
        check(!make_target(c, plug, kind, err) && !err.empty(), "a rule that does not parse is refused, not replaced");
    }

    // ── settings: validated as typed input, applied as Apply & restart ──────
    {
        Fake f; std::vector<std::string> warn; std::string err;
        auto apply = [&](bench::Sim& sim, std::vector<Setting> list) {
            return apply_settings(sim, list, warn, err);
        };
        // One setting, built by name rather than as a braced list. At every braced
        // call here GCC 16 reports a dangling pointer into the initializer_list's
        // backing array. There is none, but this build keeps zero warnings, so the
        // list is simply never formed.
        auto one = [](const char* key, const char* value) {
            std::vector<Setting> list;
            list.push_back(Setting{key, value});
            return list;
        };
        check(apply(f, one("size", "large")) && f.knobs()[1].value == 2.0f,
              "a choice accepts its label");
        check(apply(f, one("size", "1")) && f.knobs()[1].value == 1.0f,
              "a choice accepts its index");
        check(apply(f, one("density", "0.3")) && f.knobs()[0].value == 0.25f,
              "a number lands on the knob's grid, as the panel quantises it");
        check(!apply(f, one("size", "9")) && err.find("large") != std::string::npos,
              "an out-of-range choice is refused, listing the choices");
        check(!apply(f, one("density", "abc")), "a non-number is refused");
        check(!apply(f, one("density", "2")) && f.knobs()[0].value == 0.25f,
              "out of range is refused, NOT clamped, and the old value stands");
        check(!apply(f, one("speed", "1")) &&
              err.find("density size clamped") != std::string::npos,
              "an unknown setting is refused, listing the ones that exist");

        Fake g2; warn.clear();
        check(apply(g2, one("clamped", "8")) && g2.knobs()[2].value == 5.0f
              && !warn.empty() && warn[0].find("set 5") != std::string::npos,
              "a value the simulation corrects itself is kept, and reported, not overwritten");

        Fake g3; g3.records = false; warn.clear();
        check(apply(g3, one("density", "0.75")) && g3.knobs()[0].value == 0.75f
              && !warn.empty() && warn[0].find("did not record") != std::string::npos,
              "a simulation that does not record its value is caught and recorded");

        Fake r0; warn.clear();
        apply(r0, {});
        Fake r1;
        apply(r1, one("density", "0.5"));
        check(r0.resets == 0 && r1.resets == 1,
              "reset once when setup changed, never when it did not — as opening a sim does not");
    }

    // ── THE property: a dedicated run is the same simulation ────────────────
    for (const bool withSettings : {true, false}) {
        Scratch s(withSettings ? "same-settings" : "same-plain");
        auto a = library("life");
        auto b = library("life");
        Config c = quiet(s.str()); c.target = "life"; c.steps = 300; c.sample = 50;
        if (withSettings) c.settings = {{"density", "0.35"}};
        std::vector<std::string> warn; std::string err;
        apply_settings(*a, c.settings, warn, err);
        const Outcome o = execute(*a, c, sink);
        if (withSettings) {
            for (const auto& k : b->knobs()) if (k.key == "density") b->on_knob(k.key, k.quantised(0.35f));
            b->reset();
        }
        for (int i = 0; i < 300; ++i) b->step();
        const std::string tag = withSettings ? " (with a setting)" : " (no settings)";
        check(o.exit == Exit::Done && o.reason == "steps" && o.units == 300, "life runs its 300 steps" + tag);
        check(a->generation() == b->generation() && a->field().cells == b->field().cells,
              "the dedicated run's world is cell-for-cell the world stepped by hand" + tag);
        const auto ma = a->metrics(), mb = b->metrics();
        bool same = ma.size() == mb.size();
        for (std::size_t i = 0; same && i < ma.size(); ++i) same = ma[i].name == mb[i].name && ma[i].value == mb[i].value;
        check(same, "and every metric agrees exactly" + tag);

        const auto rows = lines_of(s.dir / "metrics.csv");
        check(!rows.empty() && rows[0].rfind("step,generation,elapsed_s,", 0) == 0,
              "metrics.csv names its clock first" + tag);
        check(rows.size() == 1 + 7 && rows.back().rfind("300,", 0) == 0,
              "a row at 0, every 50 steps, ending on the final state" + tag);
        check(o.rows == 7, "the outcome counts the rows it wrote" + tag);
    }

    // ── learning sims: epochs are their own clock ───────────────────────────
    {
        Scratch s("epochs");
        auto sim = library("gridworld");
        check(sim && sim->epoch_name(), "gridworld learns in epochs");
        if (sim && sim->epoch_name()) {
            Config c = quiet(s.str()); c.target = "gridworld"; c.epochs = 3;
            const int before = sim->epoch_count();
            const Outcome o = execute(*sim, c, sink);
            check(o.exit == Exit::Done && o.reason == "epochs" && o.units == 3 && o.epochs == 3
                  && sim->epoch_count() == before + 3, "--epochs 3 completes exactly three");
            const auto rows = lines_of(bench::export_path(s.str(), std::string("metrics-by-") + sim->epoch_name() + ".csv"));
            check(rows.size() == 1 + 3 && rows[0].rfind(std::string(sim->epoch_name()) + ",", 0) == 0,
                  "one row per completed epoch, its clock named after the epoch");
        }
        Scratch s2("epochs-refused");
        auto life = library("life");
        Config c = quiet(s2.str()); c.epochs = 2;
        check(execute(*life, c, sink).exit == Exit::BadArgs, "--epochs on a sim that does not learn is refused");
    }

    // ── a sim that reports late, and a name with a comma in it ──────────────
    {
        Scratch s("late");
        Fake f; f.quietFor = 5; f.lateAt = 20;
        Config c = quiet(s.str()); c.steps = 30; c.sample = 10;
        const Outcome o = execute(f, c, sink);
        const auto rows = lines_of(s.dir / "metrics.csv");
        check(o.firstMetricsAt == 10 && rows.size() == 1 + 3,
              "no columns until the sim reports something, then rows from there (10, 20, 30)");
        check(!rows.empty() && rows[0] == "step,generation,elapsed_s,gen,\"with, comma\"",
              "the header is the first report's names, quoted where they must be");
        check(o.lateMetrics.count("late") == 1, "a metric that turns up later is recorded as late, not lost");
    }

    // ── a sim that throws keeps what it wrote ───────────────────────────────
    {
        Scratch s("throws");
        Fake f; f.throwAt = 7;
        Config c = quiet(s.str()); c.steps = 100; c.sample = 1;
        const Outcome o = execute(f, c, sink);
        check(o.exit == Exit::StepFailed && o.error.find("fake failure") != std::string::npos,
              "a throwing step ends the run as a failure, naming the exception");
        check(o.units == 7 && o.rows == 8 && lines_of(s.dir / "metrics.csv").size() == 1 + 8,
              "every row written before the failure is on disk (steps 0 to 7)");
    }

    // ── stopping ────────────────────────────────────────────────────────────
    {
        Scratch s("stale-stop");
        fs::create_directories(s.dir);
        std::ofstream(s.dir / "stop") << "left over";
        Fake f;
        Config c = quiet(s.str()); c.steps = 10;
        check(execute(f, c, sink).reason == "steps", "a stop file left by an earlier run does not end this one");

        Scratch t("stop");
        Fake g2;
        const fs::path stop = t.dir / "stop";
        g2.onStep = [&](std::uint64_t gen) { if (gen == 50) std::ofstream(stop) << "stop"; };
        Config d = quiet(t.str()); d.progressEvery = 0.01;
        const Outcome o = execute(g2, d, sink);
        check(o.exit == Exit::Stopped && o.reason == "stop-file" && o.units >= 50,
              "a stop file created mid-run ends an unbounded run cleanly");

        Scratch u("interrupt");
        Fake h; std::atomic<bool> flag{true};
        Config e = quiet(u.str());
        const Outcome oi = execute(h, e, sink, &flag);
        check(oi.exit == Exit::Stopped && oi.reason == "interrupted" && oi.units == 0, "Ctrl+C stops before the next step");

        Scratch v("seconds");
        Fake k;
        Config t2 = quiet(v.str()); t2.seconds = 0.2;
        const Outcome os = execute(k, t2, sink);
        check(os.exit == Exit::Done && os.reason == "seconds" && os.seconds >= 0.2 && os.seconds < 5.0,
              "--seconds ends the run on wall time");
    }

    // ── frames ──────────────────────────────────────────────────────────────
    {
        Scratch s("frames");
        auto life = library("life");
        Config c = quiet(s.str()); c.steps = 20; c.frames = 10; c.frameSize = 64;
        const Outcome o = execute(*life, c, sink);
        int w = 0, h = 0;
        bool all = true;
        for (const char* n : {"frame-0.png", "frame-10.png", "frame-20.png", "frame-final.png"})
            all = all && png_size(s.dir / n, w, h) && w == 64 && h == 64;
        check(o.framesWritten == 4 && all, "frames at 0, 10, 20 and the final state, as 64x64 PNGs");
    }

    // ── the summary ─────────────────────────────────────────────────────────
    {
        Scratch s("summary");
        auto life = library("life");
        Config c = quiet(s.str()); c.target = "life"; c.steps = 40;
        c.settings = {{"density", "0.35"}};
        std::vector<std::string> warn; std::string err;
        apply_settings(*life, c.settings, warn, err);
        const Outcome o = execute(*life, c, sink);
        const std::string path = (s.dir / "run.json").string();
        check(write_summary(path, c, o, *life, "library"), "run.json is written");
        const std::string j = slurp(path);
        check(j.find("\"reason\": \"steps\"") != std::string::npos && j.find("\"exit_code\": 0") != std::string::npos
              && j.find("\"units\": 40") != std::string::npos, "run.json records why and how far it ran");
        check(j.find("\"density\": {\"value\": ") != std::string::npos && j.find("\"shape\"") != std::string::npos,
              "run.json records every knob, including the ones nobody set");
    }

    // ── a plugin, end to end ────────────────────────────────────────────────
    if (argc > 1) {
        Scratch s("plugin");
        // Whatever the build named it — CMake calls it example_plugin.dll, the
        // plugins folder example_rule.dll — found the way the workbench finds
        // plugins, which also skips shadow copies left by an earlier load.
        const auto found = bench::Plugin::scan(argv[1]);
        const std::string dll = found.empty() ? (fs::path(argv[1]) / "example_rule.dll").string()
                                              : found.front();
        bench::Plugin plug;          // before the sim: it must die while the DLL is mapped
        bench::SimPtr sim;
        std::string kind, err;
        Config c = quiet(s.str()); c.target = dll; c.steps = 30;
        sim = make_target(c, plug, kind, err);
        check(sim != nullptr && kind == "plugin", "the example plugin loads through the runner: " + err);
        if (sim) {
            const Outcome o = execute(*sim, c, sink);
            check(o.exit == Exit::Done && o.units == 30, "and runs its 30 steps");
        }
    }

    std::printf("%d runner checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
