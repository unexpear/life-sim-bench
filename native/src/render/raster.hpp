// raster.hpp — the rendering core. No windowing library, no GPU, no dependencies.
//
// Everything that decides what a frame LOOKS like lives here, so it can be
// tested without a window: palette mapping, the accumulation buffer, zoom, pan,
// letterbox fitting, cell sampling and filtered images. SDL3 (or anything else) only
// has to hand this an RGBA buffer and put it on screen.
//
// The accumulation buffer is the centrepiece and the reason this is not just a
// blit. A still frame of a cellular automaton is close to meaningless: what
// matters is what MOVED, and clearing every frame throws exactly that away.
// Decay the previous frame instead, and take the brighter of decayed-vs-current
// per channel. Gliders acquire trails, wavefronts acquire direction, and a
// field of dots becomes something with history.

#pragma once
#include "../field.hpp"
#include "../parallel.hpp"
#include "voxel.hpp"
#include "surface_filter.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace bench {

struct View {
    float zoom  = 1.0f;
    float pan_x = 0.0f;
    float pan_y = 0.0f;
    float trail = 0.0f;   // 0 = off; 0.90 is a long wake; above ~0.97 never fades
    float gamma = 1.0f;   // <1 lifts dim cells out of the background
    bool  grid  = true;   // cell gridlines, drawn only when zoomed far enough to help
};

class Raster {
public:
    // Below this many cells a frame is not worth threading — the launch costs
    // more than the work. Measured on this machine at a few hundred
    // microseconds for the pool, against a pass that runs at roughly a
    // gigacell a second.
    static constexpr std::size_t kParallelCells = 250000;

    // 0 asks the machine; 1 forces single-threaded, which is what the
    // self-test compares the threaded output against.
    void set_threads(unsigned n) { threads_ = n; }

    // Target surface size in pixels.
    void resize(int w, int h) {
        if (w == w_ && h == h_) return;
        w_ = w; h_ = h;
        rgba_.assign(std::size_t(w_) * h_ * 4, 0);
    }
    [[nodiscard]] int width()  const { return w_; }
    [[nodiscard]] int height() const { return h_; }
    [[nodiscard]] const std::uint8_t* pixels() const { return rgba_.data(); }
    [[nodiscard]] std::size_t pitch() const { return std::size_t(w_) * 4; }

    void clear_accumulator() { std::fill(acc_.begin(), acc_.end(), 0.0f); }

    // Where a source cell lands on the surface, and back again. Kept public
    // because input handling needs the inverse and duplicating the maths is how
    // clicks end up landing somewhere other than where the cursor is.
    struct Fit { float scale; float ox; float oy; };
    [[nodiscard]] Fit fit_of(int fw, int fh, const View& v) const {
        const float s = std::min(float(w_) / float(fw), float(h_) / float(fh)) * v.zoom;
        return Fit{ s, (float(w_) - fw * s) * 0.5f + v.pan_x,
                       (float(h_) - fh * s) * 0.5f + v.pan_y };
    }
    [[nodiscard]] std::pair<float,float> to_cell(float px, float py, const Fit& f) const {
        return { (px - f.ox) / f.scale, (py - f.oy) / f.scale };
    }

    // Draw a pre-rendered RGB image. Same fit, zoom, pan and gamma as the
    // index path — a sim that renders itself still lives inside the same
    // viewport machinery.
    void draw(const Surface& s, const View& v) {
        if (w_ <= 0 || h_ <= 0 || s.empty()) return;
        std::fill(rgba_.begin(), rgba_.end(), std::uint8_t(0));

        const bool doGamma = (v.gamma > 0.0f && std::fabs(v.gamma - 1.0f) > 0.001f);
        if (doGamma && std::fabs(v.gamma - lutGamma_) > 0.0001f) {
            lutGamma_ = v.gamma;
            for (int i = 0; i < 256; ++i)
                lut_[i] = std::uint8_t(255.0f * std::pow(float(i) / 255.0f, v.gamma));
        }
        const Fit fit = fit_of(s.w, s.h, v);
        surfaceFilter_.draw(s,rgba_.data(),w_,h_,fit.scale,fit.ox,fit.oy,doGamma?lut_:nullptr);
    }

    void draw(const Field& f, const std::vector<Swatch>& pal, const View& v) {
        if (w_ <= 0 || h_ <= 0 || f.w <= 0 || f.h <= 0) return;
        const std::size_t n = std::size_t(f.w) * f.h;
        if (acc_.size() != n * 3) { acc_.assign(n * 3, 0.0f); }

        // 1 — resolve indices to colour, into the accumulator.
        //
        // The palette is flattened to a 256-entry table first. It used to be a
        // bounds-checked lookup per CELL, which on a 2048^2 field is four
        // million branches a frame to answer a question with 256 possible
        // answers.
        float palf[256 * 3];
        for (int i = 0; i < 256; ++i) {
            const Rgb c = colour_of(pal, std::uint8_t(i));
            palf[i*3+0] = float(c.r); palf[i*3+1] = float(c.g); palf[i*3+2] = float(c.b);
        }
        // Row-parallel. Every cell's accumulator entry depends only on itself
        // and on that same entry's previous value, so rows are independent and
        // the result cannot depend on how the work was split. On a 2048-square
        // field this pass is four million iterations a frame.
        //
        // Threaded only above a size threshold: below it, launching threads
        // costs more than the work. Measured, not guessed — see the numbers in
        // the parallel section of STATUS.
        const unsigned rw = (threads_ == 1u || n < kParallelCells) ? 1u : threads_;
        if (v.trail > 0.0f) {
            const float d = std::min(v.trail, 0.999f);
            parallel_for(std::size_t(f.h), [&](std::size_t row) {
                const std::size_t i0 = row * std::size_t(f.w);
                for (std::size_t i = i0; i < i0 + std::size_t(f.w); ++i) {
                    const float* c = &palf[std::size_t(f.cells[i]) * 3];
                    float* a = &acc_[i * 3];
                    a[0] = std::max(a[0] * d, c[0]);
                    a[1] = std::max(a[1] * d, c[1]);
                    a[2] = std::max(a[2] * d, c[2]);
                }
            }, rw);
        } else {
            parallel_for(std::size_t(f.h), [&](std::size_t row) {
                const std::size_t i0 = row * std::size_t(f.w);
                for (std::size_t i = i0; i < i0 + std::size_t(f.w); ++i) {
                    const float* c = &palf[std::size_t(f.cells[i]) * 3];
                    float* a = &acc_[i * 3];
                    a[0] = c[0]; a[1] = c[1]; a[2] = c[2];
                }
            }, rw);
        }

        // 2 — composite to the surface with nearest-neighbour sampling. Nearest
        // is the default on purpose: these are single-cell structures and any
        // filtering turns a glider into a smudge.
        const Fit fit = fit_of(f.w, f.h, v);
        std::fill(rgba_.begin(), rgba_.end(), std::uint8_t(0));

        // Gamma below 1 lifts dim cells clear of the background without
        // touching the simulation. Precomputed, because a pow() per pixel per
        // frame is not worth it.
        const bool doGamma = (v.gamma > 0.0f && std::fabs(v.gamma - 1.0f) > 0.001f);
        if (doGamma && std::fabs(v.gamma - lutGamma_) > 0.0001f) {
            lutGamma_ = v.gamma;
            for (int i = 0; i < 256; ++i)
                lut_[i] = std::uint8_t(255.0f * std::pow(float(i) / 255.0f, v.gamma));
        }

        // Two sampling paths, chosen by scale, because they are answering
        // different questions.
        //
        // ZOOMED IN (>= 1 pixel per cell): nearest neighbour. These are
        // single-cell structures and any filtering turns a glider into a
        // smudge.
        //
        // ZOOMED OUT (< 1 pixel per cell): average the cells covering each
        // pixel. Point sampling here does not merely look worse, it shows a
        // picture that is not there — measured on a 1024^2 field in a 400px
        // view, 40 isolated cells rendered as 3, and a full-width one-cell line
        // rendered as 400 pixels or as NOTHING AT ALL depending only on which
        // row it happened to sit in. Averaging costs one pass over the source
        // cells and cannot drop anything: a lone cell in a 3x3 block comes out
        // at a ninth of full brightness, dim but present, and the gamma control
        // is there to lift it.
        if (fit.scale >= 1.0f) {
            // Which source column each destination column samples, computed
            // once and reused for every row. This was a float divide per pixel
            // — a million of them a frame at a typical window size — to answer
            // a question that only depends on x.
            colMap_.resize(std::size_t(w_));
            int xBegin = w_, xEnd = 0;
            const float inv = 1.0f / fit.scale;
            for (int x = 0; x < w_; ++x) {
                const float sx = (float(x) - fit.ox) * inv;
                const int cx = (sx < 0.0f) ? -1 : (sx >= float(f.w) ? -1 : int(sx));
                colMap_[std::size_t(x)] = cx;
                if (cx >= 0) { if (x < xBegin) xBegin = x; if (x + 1 > xEnd) xEnd = x + 1; }
            }
            parallel_for(std::size_t(h_), [&](std::size_t yi) {
                const int y = int(yi);
                const float sy = (float(y) - fit.oy) * inv;
                if (sy < 0.0f || sy >= float(f.h)) return;
                const std::size_t rowBase = std::size_t(int(sy)) * f.w;
                std::uint8_t* prow = &rgba_[std::size_t(y) * w_ * 4];
                for (int x = xBegin; x < xEnd; ++x) {
                    const int cx = colMap_[std::size_t(x)];
                    if (cx < 0) continue;
                    const float* a = &acc_[(rowBase + std::size_t(cx)) * 3];
                    std::uint8_t* p = prow + std::size_t(x) * 4;
                    if (doGamma) { p[0]=lut_[u8(a[0])]; p[1]=lut_[u8(a[1])]; p[2]=lut_[u8(a[2])]; }
                    else         { p[0]=u8(a[0]);       p[1]=u8(a[1]);       p[2]=u8(a[2]);       }
                    p[3] = 255;
                }
            }, rw);
        } else {
            const float inv = 1.0f / fit.scale;
            // The source-column span of each destination column depends only on
            // x, and was being recomputed for every row: two floors, two ceils
            // and four clamps per pixel per frame to answer a question that is
            // the same all the way down the column.
            spanLo_.resize(std::size_t(w_)); spanHi_.resize(std::size_t(w_));
            for (int x = 0; x < w_; ++x) {
                int x0 = int(std::floor((float(x)     - fit.ox) * inv));
                int x1 = int(std::ceil ((float(x + 1) - fit.ox) * inv));
                x0 = std::max(x0, 0); x1 = std::min(x1, f.w);
                spanLo_[std::size_t(x)] = x0;
                spanHi_[std::size_t(x)] = (x1 > x0) ? x1 : x0;   // empty span marks itself
            }
            parallel_for(std::size_t(h_), [&](std::size_t yi) {
                const int y = int(yi);
                // Source rows covered by this destination row.
                int y0 = int(std::floor((float(y)     - fit.oy) * inv));
                int y1 = int(std::ceil ((float(y + 1) - fit.oy) * inv));
                if (y1 <= 0 || y0 >= f.h) return;
                y0 = std::max(y0, 0); y1 = std::min(y1, f.h);
                if (y1 <= y0) return;
                std::uint8_t* prow = &rgba_[std::size_t(y) * w_ * 4];
                for (int x = 0; x < w_; ++x) {
                    const int x0 = spanLo_[std::size_t(x)], x1 = spanHi_[std::size_t(x)];
                    if (x1 <= x0) continue;

                    float r = 0.0f, g = 0.0f, b = 0.0f;
                    for (int cy = y0; cy < y1; ++cy) {
                        const float* a = &acc_[(std::size_t(cy) * f.w + x0) * 3];
                        for (int cx = x0; cx < x1; ++cx, a += 3) { r += a[0]; g += a[1]; b += a[2]; }
                    }
                    const float k = 1.0f / float((x1 - x0) * (y1 - y0));
                    r *= k; g *= k; b *= k;
                    std::uint8_t* p = prow + std::size_t(x) * 4;
                    if (doGamma) { p[0]=lut_[u8(r)]; p[1]=lut_[u8(g)]; p[2]=lut_[u8(b)]; }
                    else         { p[0]=u8(r);       p[1]=u8(g);       p[2]=u8(b);       }
                    p[3] = 255;
                }
            }, rw);
        }

        // 3 — cell gridlines, but only once a cell is big enough that they help
        // rather than swallow the picture. Below ~8px they would be most of it.
        if (v.grid && fit.scale >= 8.0f) drawGrid(f.w, f.h, fit);
    }

    // Which cell is under a surface pixel, or {-1,-1} if outside the field.
    [[nodiscard]] std::pair<int,int> cell_at(float px, float py, int fw, int fh,
                                             const View& v) const {
        const Fit fit = fit_of(fw, fh, v);
        const auto c = to_cell(px, py, fit);
        const int cx = int(std::floor(c.first)), cy = int(std::floor(c.second));
        if (cx < 0 || cy < 0 || cx >= fw || cy >= fh) return {-1,-1};
        return {cx, cy};
    }

    // Outline the whole field, so the edge of the world is visible.
    void outline_field(int fw, int fh, const View& v,
                       std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        const Fit fit = fit_of(fw, fh, v);
        const int x0 = int(fit.ox) - 1, y0 = int(fit.oy) - 1;
        const int x1 = int(fit.ox + fw * fit.scale), y1 = int(fit.oy + fh * fit.scale);
        auto put = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
            std::uint8_t* p = &rgba_[(std::size_t(y) * w_ + x) * 4];
            p[0]=r; p[1]=g; p[2]=b; p[3]=255;
        };
        for (int x = x0; x <= x1; ++x) { put(x, y0); put(x, y1); }
        for (int y = y0; y <= y1; ++y) { put(x0, y); put(x1, y); }
    }

    // Outline one cell on the surface — the cursor readout's anchor.
    void highlight(int cx, int cy, int fw, int fh, const View& v, std::uint8_t r,
                   std::uint8_t g, std::uint8_t b) {
        if (cx < 0 || cy < 0 || cx >= fw || cy >= fh) return;
        const Fit fit = fit_of(fw, fh, v);
        const int x0 = int(fit.ox + cx * fit.scale), y0 = int(fit.oy + cy * fit.scale);
        const int x1 = int(fit.ox + (cx+1) * fit.scale), y1 = int(fit.oy + (cy+1) * fit.scale);
        auto put = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
            std::uint8_t* p = &rgba_[(std::size_t(y) * w_ + x) * 4];
            p[0]=r; p[1]=g; p[2]=b; p[3]=255;
        };
        for (int x = x0; x <= x1; ++x) { put(x, y0); put(x, y1); }
        for (int y = y0; y <= y1; ++y) { put(x0, y); put(x1, y); }
    }

private:
    // Darken, rather than paint a colour: a fixed grey looks wrong over every
    // palette, and this reads as a seam on all of them.
    void drawGrid(int fw, int fh, const Fit& fit) {
        auto dim = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
            std::uint8_t* p = &rgba_[(std::size_t(y) * w_ + x) * 4];
            p[0] = std::uint8_t(p[0] * 6 / 10);
            p[1] = std::uint8_t(p[1] * 6 / 10);
            p[2] = std::uint8_t(p[2] * 6 / 10);
        };
        for (int cx = 0; cx <= fw; ++cx) {
            const int x = int(fit.ox + cx * fit.scale);
            if (x < 0 || x >= w_) continue;
            for (int y = 0; y < h_; ++y) dim(x, y);
        }
        for (int cy = 0; cy <= fh; ++cy) {
            const int y = int(fit.oy + cy * fit.scale);
            if (y < 0 || y >= h_) continue;
            for (int x = 0; x < w_; ++x) dim(x, y);
        }
    }
    static std::uint8_t u8(float v) {
        return std::uint8_t(v < 0.0f ? 0 : (v > 255.0f ? 255 : v));
    }
    std::uint8_t lut_[256]{};
    float        lutGamma_ = -1.0f;
    static Rgb colour_of(const std::vector<Swatch>& pal, std::uint8_t i) {
        return i < pal.size() ? pal[i].colour : (pal.empty() ? Rgb{0,0,0} : pal[0].colour);
    }
    int                       w_ = 0, h_ = 0;
    std::vector<std::uint8_t> rgba_;
    // destination column -> source column, rebuilt per frame, reused per row
    unsigned                  threads_ = 0;
    std::vector<int>          colMap_;
    std::vector<int> spanLo_, spanHi_;      // source-column span per output column
    std::vector<float>        acc_;
    SurfaceFilter             surfaceFilter_;
};

} // namespace bench
