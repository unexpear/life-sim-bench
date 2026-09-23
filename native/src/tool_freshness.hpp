// tool_freshness.hpp — a tool refuses to report results from a stale binary.
//
// Twice today a measurement was quoted from an executable built before the
// change it was supposed to be measuring. Nothing about the output says so: the
// numbers are well-formed, the tool exits 0, and the reader has no way to tell
// a fresh result from last night's. That is worse than a crash, because a crash
// is not quotable.
//
// So every standalone tool in this folder calls gate() first. It compares its
// OWN write time against the newest of the files it is actually built from:
// every header under src/, recursively, plus its own .cpp. That set is exact,
// not a guess — a tool that walks the registry pulls in nearly every header
// here, and headers are the only thing it can pull in.
//
// The first version scanned .cpp files too, and it cried wolf within the hour:
// a sibling editing self_test.cpp made every tool refuse, though not one of
// them compiles a line of it. A gate that fires on an unrelated edit teaches
// people to pass the override, which is worse than no gate. The one tool that
// genuinely depends on another .cpp — fragile.cpp reads self_test.cpp at run
// time — checks it by content instead, which is stronger than a timestamp.
//
// BENCH_ALLOW_STALE=1 overrides, and says so loudly on stdout so that an
// overridden run cannot be pasted into a report as a clean one.

#pragma once
#include "paths.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace bench {

struct Freshness {
    bool        known    = false;   // false if the source tree could not be read
    bool        stale    = false;
    long long   behindS  = 0;       // seconds the binary is older than the newest source
    std::string newest;             // the file that is newer
    std::string exe;
};

inline Freshness check_freshness(const char* tool) {
    namespace fs = std::filesystem;
    Freshness f;
    std::error_code ec;

    // The exe's own path, not argv[0]: argv[0] is whatever the shell typed and
    // may be a bare name with no directory at all.
    std::string exePath;
#ifdef _WIN32
    {
        char buf[MAX_PATH]{};
        const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
        if (n > 0) exePath.assign(buf, buf + n);
    }
#endif
    if (exePath.empty()) return f;
    f.exe = exePath;

    const auto exeTime = fs::last_write_time(exePath, ec);
    if (ec) return f;

    const std::string root = Paths::get().src();
    fs::file_time_type newest{};
    bool any = false;
    auto consider = [&](const fs::path& p) {
        std::error_code tec;
        const auto t = fs::last_write_time(p, tec);
        if (tec) return;
        if (!any || t > newest) { newest = t; f.newest = p.filename().string(); any = true; }
    };
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = it->path().extension().string();
        if (ext != ".hpp" && ext != ".h") continue;    // headers are the whole include graph
        consider(it->path());
    }
    // ...and the tool's own translation unit, which is the one .cpp it compiles.
    {
        const fs::path own = fs::path(root) / (std::string(tool) + ".cpp");
        std::error_code oec;
        if (fs::exists(own, oec) && !oec) consider(own);
    }
    if (!any) return f;                 // no source tree found — say nothing rather than lie

    f.known = true;
    if (newest > exeTime) {
        f.stale = true;
        f.behindS = std::chrono::duration_cast<std::chrono::seconds>(newest - exeTime).count();
    }
    return f;
}

// Call this as the first statement of main(). Exits 2 rather than returning,
// because a stale tool must not produce a single line a reader could quote.
// `tool` must be the basename of the tool's own .cpp under src/, e.g. "inert"
// for src/inert.cpp — it names both the message and the translation unit whose
// timestamp is checked.
inline void gate(const char* tool) {
    const Freshness f = check_freshness(tool);
    if (!f.known) {
        std::printf("%s: cannot find src/ next to the binary — freshness UNVERIFIED\n", tool);
        return;
    }
    if (!f.stale) return;

    const long long mins = f.behindS / 60;
    std::printf("%s: STALE BINARY — refusing to run.\n", tool);
    std::printf("   %s was modified %lld min %llds after this exe was linked.\n",
                f.newest.c_str(), mins, f.behindS % 60);
    std::printf("   Anything printed now would describe code that is no longer here.\n");
    if (const char* ok = std::getenv("BENCH_ALLOW_STALE"); ok && ok[0] == '1') {
        std::printf("   BENCH_ALLOW_STALE=1 — running anyway. EVERY NUMBER BELOW IS SUSPECT.\n\n");
        return;
    }
    std::printf("   Rebuild it. (BENCH_ALLOW_STALE=1 overrides, and labels the output.)\n");
    std::exit(2);
}

} // namespace bench
