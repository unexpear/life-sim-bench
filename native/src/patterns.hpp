// patterns.hpp — read and write the format everything is published in.
//
// The bench can simulate anything and, until now, could only be handed an
// initial condition by someone editing C++. Every pattern anyone has ever
// published — every glider gun, every spaceship, every Wireworld circuit — is
// distributed as RLE. Reading it connects this program to that entire corpus;
// writing it is what makes something found in the workbench shareable.
//
// LICENCE NOTE. RLE is a documented format, and a format is a specification,
// not someone's code. This is written from the spec. Crucially the bench ships
// NO pattern files: you point it at your own. That keeps "no strings" intact
// regardless of how any particular pattern collection is licensed.
//
// Supported:
//   #C / #c / #N / #O / #r  comment lines — PRESERVED, because they carry the
//                           pattern's name and attribution
//   #CXRLE Pos=x,y          the extended-RLE origin
//   x = W, y = H, rule = R  header
//   <n>b <n>o <n>$ !        run-length body, '.' and 'A' also accepted
//   pA..yX                  two-character codes for states above 24, so
//                           Wireworld and Langton's Loops round-trip

#pragma once
#include "field.hpp"
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cctype>

namespace bench {

struct Pattern {
    int w = 0, h = 0;
    std::vector<std::uint8_t> cells;      // row-major, w*h
    std::string rule;                     // as written in the header, may be empty
    std::vector<std::string> comments;    // the '#' lines, verbatim and in order
    std::string source;                   // where it came from, for display
    int  origin_x = 0, origin_y = 0;      // from #CXRLE Pos=, if present
    bool ok = false;
    std::string error;

    [[nodiscard]] std::uint8_t at(int x, int y) const {
        return cells[std::size_t(y) * w + x];
    }
    // A pattern with no comment lines carries no attribution. The bench says so
    // rather than presenting an anonymous blob as though it were sourced.
    [[nodiscard]] bool cited() const {
        for (const auto& c : comments)
            if (c.size() > 2 && (c[1] == 'N' || c[1] == 'O' || c[1] == 'C' || c[1] == 'c'))
                return true;
        return false;
    }
    [[nodiscard]] std::string title() const {
        for (const auto& c : comments)
            if (c.size() > 3 && c[1] == 'N') return trimmed(c.substr(2));
        return "(untitled pattern)";
    }
private:
    static std::string trimmed(std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
        return s;
    }
};

// ── state <-> character ─────────────────────────────────────────────────────
//
// State 0 is 'b' (dead). 1..24 are 'A'..'X'. Above that the codes are two
// characters: 'p'..'y' selects a block of 24, then 'A'..'X' within it. Without
// this, any rule with more than two states silently truncates on save.
inline void state_to_rle(std::uint8_t s, std::string& out) {
    if (s == 0) { out += 'b'; return; }
    if (s == 1) { out += 'o'; return; }                 // conventional for 2-state
    const int v = s - 1;
    if (v < 24) { out += char('A' + v); return; }
    out += char('p' + (v / 24) - 1);
    out += char('A' + (v % 24));
}

// ── read ────────────────────────────────────────────────────────────────────
inline Pattern parse_rle(const std::string& text, const std::string& source = "") {
    Pattern p; p.source = source;
    auto fail = [&](const std::string& why) { p.ok = false; p.error = why; return p; };

    std::istringstream in(text);
    std::string line, body;
    bool haveHeader = false;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (line[0] == '#') {
            p.comments.push_back(line);
            // #CXRLE Pos=-11,-4  — the origin, so a pattern lands where it was authored
            const auto pos = line.find("Pos=");
            if (pos != std::string::npos) {
                int px = 0, py = 0; char comma = 0;
                std::istringstream ps(line.substr(pos + 4));
                if (ps >> px >> comma >> py && comma == ',') { p.origin_x = px; p.origin_y = py; }
            }
            continue;
        }
        if (!haveHeader && (line[0] == 'x' || line[0] == 'X')) {
            // x = 3, y = 3, rule = B3/S23
            haveHeader = true;
            std::string t = line;
            for (char& c : t) if (c == ',') c = ' ';
            std::istringstream hs(t);
            std::string tok;
            while (hs >> tok) {
                auto eq = tok.find('=');
                std::string key = tok, val;
                if (eq != std::string::npos) {
                    key = tok.substr(0, eq); val = tok.substr(eq + 1);
                    if (val.empty() && !(hs >> val)) break;
                }
                else if (!(hs >> val)) break;
                if (val == "=") { if (!(hs >> val)) break; }
                if (!val.empty() && val[0] == '=') val.erase(val.begin());
                if (val.empty()) continue;
                if (key == "x" || key == "X") p.w = std::atoi(val.c_str());
                else if (key == "y" || key == "Y") p.h = std::atoi(val.c_str());
                else if (key == "rule" || key == "Rule") p.rule = val;
            }
            continue;
        }
        body += line;
    }
    if (!haveHeader) return fail("no 'x = ..., y = ...' header — is this an RLE file?");
    if (p.w <= 0 || p.h <= 0) return fail("header gives a non-positive size");
    if (std::size_t(p.w) * std::size_t(p.h) > 64u * 1024u * 1024u)
        return fail("pattern larger than the 64 M cell ceiling");

    p.cells.assign(std::size_t(p.w) * p.h, 0);

    int x = 0, y = 0, run = 0;
    for (std::size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        if (std::isdigit(static_cast<unsigned char>(c))) { run = run * 10 + (c - '0'); continue; }
        const int n = run ? run : 1; run = 0;

        if (c == '!') break;
        if (c == '$') { y += n; x = 0; continue; }

        std::uint8_t state = 0;
        if (c == 'b' || c == '.') state = 0;
        else if (c == 'o')        state = 1;
        else if (c >= 'A' && c <= 'X') state = std::uint8_t(c - 'A' + 1);
        else if (c >= 'p' && c <= 'y') {
            // two-character code: this letter selects the block, the next the offset
            if (i + 1 >= body.size()) return fail("truncated multi-state code at end of file");
            const char c2 = body[++i];
            if (c2 < 'A' || c2 > 'X') return fail("bad second character in a multi-state code");
            state = std::uint8_t((c - 'p' + 1) * 24 + (c2 - 'A') + 1);
        } else continue;   // unknown character: skip rather than abort

        // Clip rather than write out of bounds. A header that under-reports its
        // own size is common in hand-edited files, and an unguarded write here
        // is the Wireworld out-of-bounds bug repeated in a new place.
        for (int k = 0; k < n; ++k, ++x)
            if (state && x >= 0 && x < p.w && y >= 0 && y < p.h)
                p.cells[std::size_t(y) * p.w + x] = state;
    }
    p.ok = true;
    return p;
}

// Plaintext '.'/'O' with '!' comment lines. Read only — RLE is what anyone
// should be writing.
inline Pattern parse_cells(const std::string& text, const std::string& source = "") {
    Pattern p; p.source = source;
    std::istringstream in(text);
    std::string line;
    std::vector<std::string> rows;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '!') { p.comments.push_back("#C" + line.substr(1)); continue; }
        rows.push_back(line);
    }
    while (!rows.empty() && rows.back().empty()) rows.pop_back();
    if (rows.empty()) { p.error = "no cell rows"; return p; }
    for (const auto& r : rows) p.w = std::max(p.w, int(r.size()));
    p.h = int(rows.size());
    p.cells.assign(std::size_t(p.w) * p.h, 0);
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < int(rows[std::size_t(y)].size()); ++x) {
            const char c = rows[std::size_t(y)][std::size_t(x)];
            if (c == 'O' || c == 'o' || c == '*') p.cells[std::size_t(y) * p.w + x] = 1;
        }
    p.ok = true;
    return p;
}

inline Pattern load_pattern(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { Pattern p; p.error = "could not open " + path; return p; }
    std::stringstream ss; ss << f.rdbuf();
    const std::string text = ss.str();
    const auto dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    return (ext == "cells") ? parse_cells(text, path) : parse_rle(text, path);
}

// ── write ───────────────────────────────────────────────────────────────────
//
// Emits the provenance as #C lines first. A field saved out of the workbench
// should say what rule produced it, or it is just a grid of numbers.
inline std::string to_rle(const Field& f, const std::string& rule,
                          const std::string& title, std::uint64_t generation) {
    std::string out;
    if (!title.empty()) out += "#N " + title + "\n";
    out += "#C written by life-sim-bench at generation " + std::to_string(generation) + "\n";
    if (!rule.empty()) out += "#C rule " + rule + "\n";
    out += "x = " + std::to_string(f.w) + ", y = " + std::to_string(f.h);
    if (!rule.empty()) out += ", rule = " + rule;
    out += "\n";

    std::string line, tok;
    // RLE lines are conventionally wrapped at 70 columns, and a run token must
    // never be split across the wrap or it decodes as two different runs.
    auto emit = [&](const std::string& t) {
        if (line.size() + t.size() > 68) { out += line; out += "\n"; line.clear(); }
        line += t;
    };
    auto flush_run = [&](std::uint8_t s, int n) {
        if (n <= 0) return;
        tok.clear();
        if (n > 1) tok += std::to_string(n);
        state_to_rle(s, tok);
        emit(tok);
    };
    auto end_row = [&](int n) { emit(n > 1 ? std::to_string(n) + "$" : std::string("$")); };

    for (int y = 0; y < f.h; ++y) {
        // Trailing dead cells carry no information; the row terminator implies them.
        int last = -1;
        for (int x = 0; x < f.w; ++x) if (f.at(x, y)) last = x;
        int run = 0; std::uint8_t cur = 0;
        for (int x = 0; x <= last; ++x) {
            const std::uint8_t s = f.at(x, y);
            if (run && s != cur) { flush_run(cur, run); run = 0; }
            cur = s; ++run;
        }
        if (run) flush_run(cur, run);
        if (y + 1 < f.h) end_row(1);
    }
    out += line;
    out += "!\n";
    return out;
}

// ── paste ───────────────────────────────────────────────────────────────────
//
// CLIPS. It does not wrap and it does not write out of bounds. `Field::at` is a
// raw index with no check and `Field::wrap` is only valid within one field
// width, so an unclipped paste is an out-of-bounds write — the exact bug this
// project already hit once with Wireworld's oversized demo circuit.
//
// Returns how many cells actually landed, so a paste that fell entirely outside
// can be reported instead of silently doing nothing.
inline std::size_t paste(Field& f, const Pattern& p, int ox, int oy, bool clearFirst = false) {
    if (clearFirst) f.fill(0);
    std::size_t placed = 0;
    for (int y = 0; y < p.h; ++y) {
        const int ty = oy + y;
        if (ty < 0 || ty >= f.h) continue;
        for (int x = 0; x < p.w; ++x) {
            const int tx = ox + x;
            if (tx < 0 || tx >= f.w) continue;
            f.set(tx, ty, p.at(x, y));
            ++placed;
        }
    }
    return placed;
}

} // namespace bench
