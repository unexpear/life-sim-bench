// runner.cpp — bench_run: host one simulation with the whole machine.
//
// What a dedicated run is, and what it writes, is in runner.hpp. This file is
// only the command line around it.

#include "runner.hpp"
#include "projects.hpp"

#include <cstdio>
#include <iostream>

#ifndef _WIN32
#include <csignal>
#endif

namespace {

std::atomic<bool> g_interrupt{false};

#ifdef _WIN32
BOOL WINAPI on_console_event(DWORD) { g_interrupt = true; return TRUE; }
#else
void on_signal(int) { g_interrupt = true; }
#endif

void usage(FILE* f) {
    std::fputs(
        "bench_run - run one simulation with the whole machine: no window, no timeline.\n"
        "\n"
        "  bench_run <simulation> [options]\n"
        "  bench_run --list\n"
        "  bench_run --describe <simulation>\n"
        "\n"
        "<simulation> is a library id (see --list) or a path to a plugin .dll.\n"
        "\n"
        "  --set key=value      apply a setting, as Apply & restart does; repeatable.\n"
        "                       A choice takes the label the app shows, or its index.\n"
        "  --rule B3/S23        the rule, for the 'rule' simulation\n"
        "  --steps N            stop after N steps\n"
        "  --epochs N           stop after N episodes or generations (learning sims)\n"
        "  --seconds S          stop after S seconds of wall time\n"
        "  --sample N           a metrics row every N steps (default: at most 2,000 rows)\n"
        "  --frames N           a PNG every N steps (off by default; frames are uncompressed)\n"
        "  --frame-size PX      longest side of a frame (default 512)\n"
        "  --out DIR            results folder (default: runs\\<simulation>-<date>-<time>)\n"
        "  --progress-every S   seconds between progress lines (default 1)\n"
        "\n"
        "With no --steps, --epochs or --seconds it runs until stopped: press Ctrl+C,\n"
        "or create a file named 'stop' in the results folder. Either way it finishes\n"
        "cleanly and writes run.json.\n"
        "\n"
        "Writes metrics.csv, metrics-by-<epoch>.csv for learning sims, run.json and\n"
        "any frames. Exit codes: 0 done, 1 stopped early, 2 bad arguments,\n"
        "3 plugin failed to load, 4 the simulation threw, 5 out of memory.\n", f);
}

int list() {
    std::printf("%-12s %-26s %s\n", "id", "group", "title");
    for (const auto& e : bench::registry())
        std::printf("%-12s %-26s %s\n", e.id.c_str(), e.era.c_str(),
                    bench::catalog_title(e.id).c_str());
    const auto plugins = bench::plugin_files(bench::Paths::get().plugins());
    if (!plugins.empty()) {
        std::printf("\nplugins in %s:\n", bench::Paths::get().plugins().c_str());
        std::error_code ec;
        for (const auto& p : plugins)
            std::printf("  %-20s %s\n", p.name.c_str(),
                        std::filesystem::exists(p.dll, ec)
                            ? p.dll.c_str() : "(source only: build it in the workbench first)");
    }
    return 0;
}

int describe(const std::string& target) {
    bench::run::Config c;
    c.target = target;
    bench::Plugin plug;        // before the sim: a plugin's sim must die while its DLL is mapped
    bench::SimPtr sim;
    std::string kind, err;
    sim = bench::run::make_target(c, plug, kind, err);
    if (!sim) {
        std::fprintf(stderr, "bench_run: %s\n", err.c_str());
        return int(bench::run::is_dll(target) ? bench::run::Exit::LoadFailed : bench::run::Exit::BadArgs);
    }
    std::printf("%s (%s)\n", sim->about().title.c_str(), kind.c_str());
    if (const char* e = sim->epoch_name()) std::printf("learns in %ss, so --epochs applies\n", e);
    std::printf("\n%-14s %-6s %-30s %-10s %s\n", "setting", "kind", "values", "now", "what it does");
    for (const auto& k : sim->knobs()) {
        if (k.display_only) continue;
        std::string values;
        if (!k.choices.empty()) {
            for (std::size_t i = 0; i < k.choices.size(); ++i) values += (i ? "|" : "") + k.choices[i];
        } else {
            char b[64];
            std::snprintf(b, sizeof b, "%g to %g", double(k.min), double(k.max));
            values = b;
        }
        std::printf("%-14s %-6s %-30s %-10s %s\n", k.key.c_str(), k.on_reset ? "setup" : "live",
                    values.c_str(), k.shown().c_str(), k.help.c_str());
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    using namespace bench::run;
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) { usage(stderr); return int(Exit::BadArgs); }
    if (args[0] == "--help" || args[0] == "-h") { usage(stdout); return 0; }
    if (args[0] == "--list") return list();
    if(args[0]=="--export-templates") {
        if(args.size()!=2)return int(Exit::BadArgs);
        for(const auto& entry:bench::registry()) {
            bench::projects::Document document;document.model=entry.id;document.name=bench::catalog_title(entry.id);
            std::string error;const auto path=bench::path_from_utf8(args[1])/(entry.id+".benchsim");
            if(!bench::projects::save(path,document,error)){std::fprintf(stderr,"%s\n",error.c_str());return 1;}
        }
        std::printf("Exported %zu simulation templates.\n",bench::registry().size());return 0;
    }
    if (args[0] == "--describe") {
        if (args.size() != 2) { std::fprintf(stderr, "bench_run: --describe takes one simulation\n"); return int(Exit::BadArgs); }
        return describe(args[1]);
    }

    Config c;
    std::string err;
    if (!parse_args(args, c, err)) {
        std::fprintf(stderr, "bench_run: %s\n(bench_run --help lists the options)\n", err.c_str());
        return int(Exit::BadArgs);
    }
    if (c.out.empty()) c.out = default_out(c.target);

    bench::Plugin plug;        // before the sim: locals die in reverse order, and a
    bench::SimPtr sim;         // plugin's sim must be destroyed while its DLL is mapped
    std::string kind;
    sim = make_target(c, plug, kind, err);
    if (!sim) {
        std::fprintf(stderr, "bench_run: %s\n", err.c_str());
        return int(is_dll(c.target) ? Exit::LoadFailed : Exit::BadArgs);
    }
    std::vector<std::string> warnings;
    if (!apply_settings(*sim, c.settings, warnings, err)) {
        std::fprintf(stderr, "bench_run: %s\n", err.c_str());
        return int(Exit::BadArgs);
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_event, TRUE);
#else
    std::signal(SIGINT, on_signal);
#endif

    std::cout << "bench_run: " << sim->about().title << " (" << kind << "), "
              << c.settings.size() << (c.settings.size() == 1 ? " setting, " : " settings, ")
              << describe_stop(c, *sim) << "\n"
              << "results: " << c.out << std::endl;
    for (const auto& w : warnings) std::cout << "warning: " << w << "\n";

    Outcome o = execute(*sim, c, std::cout, &g_interrupt);
    o.warnings.insert(o.warnings.begin(), warnings.begin(), warnings.end());

    const std::string summary = (std::filesystem::path(c.out) / "run.json").string();
    if (!write_summary(summary, c, o, *sim, kind))
        std::cerr << "bench_run: could not write " << summary << "\n";

    const std::string unit = (c.epochs > 0 && sim->epoch_name()) ? sim->epoch_name() : "step";
    std::cout << "finished: " << o.reason << " after " << count_of(o.units, unit)
              << " in " << duration(o.seconds) << " (exit " << int(o.exit) << ")" << std::endl;
    if (!o.error.empty()) std::cout << "error: " << o.error << std::endl;
    for (const auto& n : o.lateMetrics)
        std::cout << "note: metric '" << n << "' first appeared after the columns were fixed; "
                     "it is not in metrics.csv\n";
    return int(o.exit);
}
