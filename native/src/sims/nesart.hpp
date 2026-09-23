// SPDX-License-Identifier: GPL-3.0-or-later
// Original workbench robot, machinery and energy-cell pixel art (2026).
// Created from geometric shapes for this project; no game ROM or sprite sheet.
// Historical function names are retained to preserve the platformer's drawing API.
#pragma once
#include "../field.hpp"
#include <cstdint>
#include <vector>

namespace bench::nes {
inline Rgb colour(char c) {
    switch (c) {
        case '.': return Rgb{0,0,0};
        case 'S': return Rgb{92,148,252};
        case 'k': return Rgb{14,24,29};
        case 'w': return Rgb{224,237,240};
        case 'r': return Rgb{216,40,0};
        case 'R': return Rgb{136,20,0};
        case 's': return Rgb{252,188,176};
        case 'h': return Rgb{97,67,52};
        case 'b': return Rgb{32,56,236};
        case 'B': return Rgb{16,24,140};
        case 'o': return Rgb{192,111,56};
        case 'O': return Rgb{126,71,43};
        case 'y': return Rgb{252,224,60};
        case 'Y': return Rgb{180,140,0};
        case 'g': return Rgb{88,216,84};
        case 'G': return Rgb{0,120,0};
        case 'm': return Rgb{158,179,186};
        case 'd': return Rgb{59,79,89};
        case 'c': return Rgb{71,216,196};
        case 'C': return Rgb{36,108,102};
        case 'a': return Rgb{255,184,77};
        default: return Rgb{255,0,255};
    }
}

inline const char* const* tile_ground() {
    static const char* pixels[16] = {
        "cccccccccccccccc",
        "cccccccccccccccc",
        "cccccccccccccccc",
        "CCCCCCCCCCCCCCCC",
        "CCCCCCCCCCCCCCCC",
        "dddddddddddddddd",
        "dmmmmmmmmmmmmmmd",
        "dmddmmmmmmmmddmd",
        "dmddmmmmmmmmddmd",
        "dmmmmmmmmmmmmmmd",
        "dmmmmmmmmmmmmmmd",
        "dmmhmmmmhmmmmhmd",
        "dmmhmmmmhmmmmhmd",
        "dmhmmmmhmmmmhmmd",
        "dddddddddddddddd",
        "dddddddddddddddd",
    };
    return pixels;
}

inline const char* const* tile_brick() {
    static const char* pixels[16] = {
        "kkkkkkkkkkkkkkkk",
        "kddddddddddddddk",
        "kdmmmmmmmmmmmmdk",
        "kdmwwwwwwwwwwmdk",
        "kdmwwwwwwwwwwmdk",
        "kdmwwwwwwwwwwmdk",
        "kdmmmmmmmmmmmmdk",
        "kdmmmmmmmmmmmmdk",
        "kdmCCCCCCCCCCmdk",
        "kdmccCCCCCCccmdk",
        "kdmccCCCCCCccmdk",
        "kdmCCCCCCCCCCmdk",
        "kdmmmmmmmmmmmmdk",
        "kddddddddddddddk",
        "kdCCCCCCCCCCCCdk",
        "kkkkkkkkkkkkkkkk",
    };
    return pixels;
}

inline const char* const* tile_query() {
    static const char* pixels[16] = {
        "kkkkkkkkkkkkkkkk",
        "kmmCCCCCCCCCCmmk",
        "kmmddddddddddmmk",
        "kCddccccccccddCk",
        "kCddckkkkkkcddCk",
        "kCddckkaakkcddCk",
        "kCddckaaaakcddCk",
        "kCddckaaaakcddCk",
        "kCddckkaakkcddCk",
        "kCddckkaakkcddCk",
        "kCddckkaakkcddCk",
        "kCddckkkkkkcddCk",
        "kCddccccccccddCk",
        "kmmddddddddddmmk",
        "kmmCCCCCCCCCCmmk",
        "kkkkkkkkkkkkkkkk",
    };
    return pixels;
}

inline const char* const* tile_pipe_top() {
    static const char* pixels[16] = {
        "................",
        ".mmmmmmmmmmmmmm.",
        ".mdaaaddddaaadm.",
        ".mddddddddddddm.",
        ".mmmmmmmmmmmmmm.",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
    };
    return pixels;
}

inline const char* const* tile_pipe_body() {
    static const char* pixels[16] = {
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kmaammmmaamk..",
        "..kmaammmmaamk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
        "..kddCCccCCddk..",
    };
    return pixels;
}

inline const char* const* sprite_run_a() {
    static const char* pixels[16] = {
        ".......aa.......",
        ".......cc.......",
        ".......cc.......",
        "....CCCCCCCC....",
        "...CkaakkaakC...",
        "...CkkkkkkkkC...",
        "...CCCCCCCCCC...",
        "....CCCCCCCCmm..",
        "..mm.cccccc.mm..",
        "..mm.cCaaCc.mm..",
        "..mm.cCaaCc.mm..",
        "..mm.cCCCCc.....",
        ".....cccccmm....",
        ".....mm...mm....",
        ".....mm...mm....",
        "...CCCC...CCCC..",
    };
    return pixels;
}

inline const char* const* sprite_run_b() {
    static const char* pixels[16] = {
        ".......aa.......",
        ".......cc.......",
        ".......cc.......",
        "....CCCCCCCC....",
        "...CkaakkaakC...",
        "...CkkkkkkkkC...",
        "...CCCCCCCCCC...",
        "..mmCCCCCCCC....",
        "..mm.cccccc.mm..",
        "..mm.cCaaCc.mm..",
        "..mm.cCaaCc.mm..",
        ".....cCCCCc.mm..",
        "....mmccccc.....",
        "....mm...mm.....",
        "....mm...mm.....",
        "..CCCC...CCCC...",
    };
    return pixels;
}

inline const char* const* sprite_jump() {
    static const char* pixels[16] = {
        ".......aa.......",
        ".......cc.......",
        ".......cc.......",
        ".cc.CCCCCCCC.cc.",
        ".ccCkaakkaakCcc.",
        "...mkkkkkkkkCm..",
        "...mCCCCCCCCm...",
        "....mCCCCCCCm...",
        "....mccccccm....",
        ".....cCaaCc.....",
        ".....cCaaCc.....",
        ".....cCCCCc.....",
        ".....cccccc.....",
        "....mmm..mmm....",
        "..CCCmm..mmCCC..",
        "................",
    };
    return pixels;
}

inline const char* const* sprite_walker() {
    static const char* pixels[16] = {
        "................",
        "................",
        "................",
        ".......aa.......",
        ".......aa.......",
        ".......aa.......",
        "....hhhhhhhh....",
        "...ookkaakkoo...",
        "...ookkkkkkoo...",
        "...oooooooooo...",
        "...oooooooooo...",
        "..dddddddddddd..",
        "..dddddddddddd..",
        "..mkm.mkm.mkm...",
        "..mmm.mmm.mmm...",
        "................",
    };
    return pixels;
}

inline const char* const* sprite_coin() {
    static const char* pixels[16] = {
        "................",
        "................",
        "......mmmm......",
        "......mmmm......",
        "....CCCCCCCC....",
        "....CccccccC....",
        "....CckkkkcC....",
        "....CckaakcC....",
        "....CckaakcC....",
        "....CckaakcC....",
        "....CckkkkcC....",
        "....CccccccC....",
        "....CCCCCCCC....",
        ".....mmmmmm.....",
        "................",
        "................",
    };
    return pixels;
}

} // namespace bench::nes
