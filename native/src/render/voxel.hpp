// voxel.hpp — an actual 3D renderer. Perspective camera, triangle rasteriser,
// depth buffer, per-face lighting.
//
// The previous 3D path projected voxel centres to points and coloured them by
// one of six depth bands. That produces a 3D-looking picture without doing any
// 3D rendering: no perspective, no surfaces, no light, and a depth resolution
// of six. This does the real thing:
//
//   · perspective projection from a camera at a distance, so near voxels are
//     genuinely larger than far ones
//   · only SURFACE voxels are drawn — one whose six face-neighbours are all
//     alive cannot be seen from anywhere, and skipping it is both correct and
//     the single biggest saving
//   · only the up-to-three faces pointing at the camera are drawn, each as two
//     triangles with a real normal
//   · Lambertian shading from a fixed light, plus ambient occlusion from how
//     enclosed the voxel is, so a hollow looks like a hollow
//   · a float depth buffer, so occlusion is per-pixel rather than per-voxel
//
// Output is RGB, not palette indices, because shading has more than 256 values
// and pretending otherwise was the original mistake.

#pragma once
#include "../field.hpp"
#include "../parallel.hpp"
#include <algorithm>
#include <cmath>
#include <type_traits>
#include <cstdint>
#include <vector>

namespace bench {

// An RGB image a sim can render itself into, for the cases where a grid of
// palette indices genuinely cannot express the picture.
struct Surface {
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgba;

    void resize(int width, int height) {
        if (w == width && h == height) return;
        w = width; h = height;
        rgba.assign(std::size_t(w) * h * 4, 0);
    }
    [[nodiscard]] bool empty() const { return w <= 0 || h <= 0; }
};

// An orbit camera: it circles a TARGET rather than the origin, so you can
// choose what you are looking at. Everything is in the normalised [-1,1] cube
// the volume is mapped into, so the same camera works for any n.
struct Camera {
    float yaw = 0.6f, pitch = 0.4f;   // radians, around the target
    float distance = 3.2f;            // target -> eye
    float fov = 1.05f;                // vertical field of view, radians
    float tx = 0.0f, ty = 0.0f, tz = 0.0f;   // the point being orbited

    // CAD defaults to a parallel projection, because perspective makes it
    // impossible to compare two features by eye — equal things must measure
    // equal on screen. Perspective is better for reading a shape, so both are
    // here and the toggle keeps the framing.
    bool ortho = false;

    // The vertical extent an orthographic view covers, derived from distance
    // and fov so switching projection does not jump the framing.
    [[nodiscard]] float orthoHeight() const {
        return 2.0f * distance * std::tan(fov * 0.5f);
    }

    void eye(float* out) const {
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        out[0] = tx + distance * cp * std::sin(yaw);
        out[1] = ty + distance * sp;
        out[2] = tz + distance * cp * std::cos(yaw);
    }
    // Right and up vectors of the view plane — what panning moves along.
    void basis(float* right, float* up) const {
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        const float f[3] = { -cp * std::sin(yaw), -sp, -cp * std::cos(yaw) };
        right[0] =  std::cos(yaw); right[1] = 0.0f; right[2] = -std::sin(yaw);
        up[0] = right[1]*f[2] - right[2]*f[1];
        up[1] = right[2]*f[0] - right[0]*f[2];
        up[2] = right[0]*f[1] - right[1]*f[0];
    }
    // Direction from the eye toward the target.
    void forward(float* out) const {
        float e[3]; eye(out); (void)e;
        out[0] = tx - out[0]; out[1] = ty - out[1]; out[2] = tz - out[2];
        const float m = std::sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
        if (m > 1e-6f) { out[0]/=m; out[1]/=m; out[2]/=m; }
    }
    void clampPitch() {
        const float lim = 1.5533f;               // just under 90 degrees
        if (pitch >  lim) pitch =  lim;
        if (pitch < -lim) pitch = -lim;
    }
};

class VoxelRenderer {
public:
    void resize(int w, int h) {
        outW_ = w; outH_ = h;
        surf_.resize(w, h);
        allocWork();
    }

    // ── supersampling ──────────────────────────────────────────────────────
    //
    // Render at ss times the resolution and box-filter down. A voxel volume is
    // nothing but hard edges — every silhouette is a staircase, and at a
    // distance the thin faces alias into a shimmer that reads as noise in the
    // data rather than as a limit of the renderer. There is no cheaper fix
    // available here: the edges are geometric, so no post-process blur can
    // recover what the single sample never had.
    //
    // 2 is the useful setting: four samples per output pixel, and the cost lands
    // almost entirely on rasterisation, which is the half that threads well. The
    // geometry pass is unchanged, so it is far cheaper than 4x the frame.
    void set_supersample(int ss) {
        ss = std::max(1, std::min(3, ss));
        if (ss == ss_) return;
        ss_ = ss;
        allocWork();
    }
    [[nodiscard]] int supersample() const { return ss_; }

    // 0 means "ask the machine". Set it to 1 to render single-threaded, which
    // is what the self-test compares the threaded output against.
    void set_threads(unsigned n) { threads_ = n; }
    [[nodiscard]] unsigned threads() const {
        return threads_ ? threads_ : default_workers();
    }
    [[nodiscard]] const Surface& surface() const { return surf_; }

    // `live(x,y,z)` decides occupancy; `tint` gives one colour for everything.
    template <class LiveFn>
    void render(int n, LiveFn live, const Camera& cam, Rgb tint, Rgb background) {
        render(n, live, [tint](int,int,int) { return tint; }, cam, background);
    }

    // Per-voxel colour, for worlds where blocks are not all the same material.
    template <class LiveFn, class ColourFn>
    void render(int n, LiveFn live, ColourFn colourOf, const Camera& cam, Rgb background) {
        // Everything below draws into the WORK surface, which is the output at
        // ss = 1 and larger otherwise. dst_ is what tri() writes through.
        dst_ = (ss_ == 1) ? &surf_ : &work_;
        const int W = dst_->w, H = dst_->h;
        if (W <= 0 || H <= 0) return;

        for (std::size_t i = 0; i < depth_.size(); ++i) depth_[i] = 1e30f;
        for (int i = 0; i < W * H; ++i) {
            std::uint8_t* p = &dst_->rgba[std::size_t(i) * 4];
            p[0] = background.r; p[1] = background.g; p[2] = background.b; p[3] = 255;
        }

        // Camera basis. The volume is normalised to [-1,1] so the same camera
        // works for any n.
        float eye[3]; cam.eye(eye);

        // Forward points at the TARGET, not at the origin — that is the whole
        // difference between orbiting the middle of the volume and orbiting
        // whatever you decided to look at.
        const float fwd[3] = { cam.tx - eye[0], cam.ty - eye[1], cam.tz - eye[2] };
        const float fl = std::sqrt(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
        const float f[3] = { fwd[0]/fl, fwd[1]/fl, fwd[2]/fl };
        float r[3] = { f[2], 0.0f, -f[0] };
        const float rl = std::sqrt(r[0]*r[0] + r[2]*r[2]);
        if (rl > 1e-6f) { r[0] /= rl; r[2] /= rl; }
        const float u[3] = { r[1]*f[2] - r[2]*f[1],
                             r[2]*f[0] - r[0]*f[2],
                             r[0]*f[1] - r[1]*f[0] };

        const float focal = float(H) * 0.5f / std::tan(cam.fov * 0.5f);
        const float scale = 2.0f / float(n);      // cell -> normalised
        const float hv    = scale * 0.5f;         // half a voxel

        // A light fixed to the world, not the camera: rotating the volume then
        // moves the highlight across it, which is most of what makes a still
        // frame read as solid.
        float L[3] = { 0.45f, 0.78f, 0.44f };
        { const float m = std::sqrt(L[0]*L[0]+L[1]*L[1]+L[2]*L[2]);
          L[0]/=m; L[1]/=m; L[2]/=m; }

        static const int FACE_N[6][3] = {
            {  1, 0, 0 }, { -1, 0, 0 }, { 0,  1, 0 },
            {  0,-1, 0 }, {  0, 0, 1 }, { 0,  0,-1 }
        };

        // Orthographic divides by a constant instead of by depth, so parallel
        // edges stay parallel and equal features measure equal on screen.
        const float orthoK = cam.ortho ? (float(H) / cam.orthoHeight()) : 0.0f;
        auto project = [&](float px, float py, float pz, float& sx, float& sy, float& sz) {
            const float dx = px - eye[0], dy = py - eye[1], dz = pz - eye[2];
            const float vz = dx*f[0] + dy*f[1] + dz*f[2];      // depth along view
            if (vz <= 0.05f) { sz = -1.0f; sx = sy = 0.0f; return; }
            const float vx = dx*r[0] + dy*r[1] + dz*r[2];
            const float vy = dx*u[0] + dy*u[1] + dz*u[2];
            const float k  = cam.ortho ? orthoK : (focal / vz);
            sx = float(W) * 0.5f + vx * k;
            sy = float(H) * 0.5f - vy * k;
            sz = vz;
        };

        // ── pass 1: which faces are visible, and where do they land ────────
        //
        // Split from rasterisation so both halves can be threaded. The geometry
        // work — the 26-neighbour occlusion count above all — is a real share of
        // the cost on a dense volume, and it writes nothing shared: each worker
        // fills its own slice of the face list, indexed by z, so the list comes
        // out in the same order however many workers there are. That ordering is
        // the whole reason the parallel image is identical to the serial one and
        // not merely similar.
        const unsigned workers = std::max(1u, threads_ ? threads_ : default_workers());
        // resize + clear, NOT assign. assign() destroys every slab's capacity
        // and reallocates it next frame — at sixty frames a second on a shell
        // of seventy thousand faces that is the allocator doing more work than
        // the renderer. clear() keeps the capacity, so after the first frame
        // the face lists never allocate again.
        if (slabs_.size() != std::size_t(n)) slabs_.resize(std::size_t(n));
        for (auto& sl : slabs_) sl.clear();
        parallel_for(std::size_t(n), [&](std::size_t zi) {
            const int z = int(zi);
            auto& out = slabs_[zi];
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x) {
                    if (!live(x, y, z)) continue;

                    // Interior voxels are invisible from every direction. This
                    // is the correctness point AND the speed one: a solid mass
                    // draws only its shell.
                    int openFaces = 0, open[6];
                    for (int fi = 0; fi < 6; ++fi) {
                        const int nx = x + FACE_N[fi][0], ny = y + FACE_N[fi][1],
                                  nz = z + FACE_N[fi][2];
                        const bool outside = nx < 0 || ny < 0 || nz < 0 ||
                                             nx >= n || ny >= n || nz >= n;
                        if (outside || !live(nx, ny, nz)) open[openFaces++] = fi;
                    }
                    if (openFaces == 0) continue;

                    // Ambient occlusion: how enclosed this voxel is, over the
                    // full 26-neighbourhood. A voxel in a crevice goes darker.
                    int around = 0;
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                if (!dx && !dy && !dz) continue;
                                const int nx = x+dx, ny = y+dy, nz = z+dz;
                                if (nx < 0 || ny < 0 || nz < 0 || nx >= n || ny >= n || nz >= n)
                                    continue;
                                if (live(nx, ny, nz)) ++around;
                            }
                    const float ao = 1.0f - 0.55f * (float(around) / 26.0f);

                    const float bx = (float(x) + 0.5f) * scale - 1.0f;
                    const float by = (float(y) + 0.5f) * scale - 1.0f;
                    const float bz = (float(z) + 0.5f) * scale - 1.0f;

                    for (int k = 0; k < openFaces; ++k) {
                        const int fi = open[k];
                        const float nx = float(FACE_N[fi][0]),
                                    ny = float(FACE_N[fi][1]),
                                    nz = float(FACE_N[fi][2]);
                        // Back-face cull: skip faces pointing away from the eye.
                        const float ox = bx + nx*hv - eye[0];
                        const float oy = by + ny*hv - eye[1];
                        const float oz = bz + nz*hv - eye[2];
                        if (nx*ox + ny*oy + nz*oz >= 0.0f) continue;

                        // Lambert + ambient, modulated by occlusion.
                        const float lam = std::max(0.0f, nx*L[0] + ny*L[1] + nz*L[2]);
                        const float shade = (0.28f + 0.72f * lam) * ao;

                        float cx[4], cyv[4], cz[4];
                        faceCorners(fi, bx, by, bz, hv, cx, cyv, cz);
                        Face fa;
                        bool ok = true;
                        for (int c = 0; c < 4; ++c) {
                            project(cx[c], cyv[c], cz[c], fa.x[c], fa.y[c], fa.z[c]);
                            if (fa.z[c] < 0.0f) { ok = false; break; }
                        }
                        if (!ok) continue;

                        // Let the colour function see WHICH face this is, when
                        // it wants to. A grass block is green on top and brown
                        // down the sides, and a world where every face of a
                        // block is the same colour reads as coloured cubes
                        // rather than as ground. Faces are +x -x +y -y +z -z, so
                        // 2 is up and 3 is down.
                        //
                        // Detected rather than required, so every sim already
                        // passing a three-argument colour function keeps working
                        // untouched — widening this signature for all of them
                        // would have been a much bigger change for one sim's
                        // benefit.
                        Rgb base;
                        if constexpr (std::is_invocable_r_v<Rgb, ColourFn, int, int, int, int>)
                            base = colourOf(x, y, z, fi);
                        else
                            base = colourOf(x, y, z);
                        fa.col = Rgb{
                            std::uint8_t(std::min(255.0f, float(base.r) * shade)),
                            std::uint8_t(std::min(255.0f, float(base.g) * shade)),
                            std::uint8_t(std::min(255.0f, float(base.b) * shade)) };
                        out.push_back(fa);
                    }
                }
        }, workers);

        // ── pass 2: rasterise, one horizontal band per worker ──────────────
        //
        // Every worker walks every face and clips to its own rows. That is more
        // total clipping work than splitting the faces would be, and it is the
        // only split that needs no locking on the depth buffer at all — a face
        // can land anywhere on the screen, so a face-parallel split would have
        // several threads depth-testing the same pixel and the winner would
        // depend on timing.
        const int bandRows = (H + int(workers) - 1) / int(workers);
        parallel_for(std::size_t(workers), [&](std::size_t wi) {
            const int y0 = int(wi) * bandRows;
            const int y1 = std::min(H, y0 + bandRows);
            if (y0 >= y1) return;
            for (const auto& slab : slabs_)
                for (const auto& fa : slab) {
                    tri(fa.x[0],fa.y[0],fa.z[0], fa.x[1],fa.y[1],fa.z[1],
                        fa.x[2],fa.y[2],fa.z[2], fa.col, y0, y1);
                    tri(fa.x[0],fa.y[0],fa.z[0], fa.x[2],fa.y[2],fa.z[2],
                        fa.x[3],fa.y[3],fa.z[3], fa.col, y0, y1);
                }
        }, workers);

        if (ss_ > 1) resolve(workers);
    }

private:
    void allocWork() {
        const int w = outW_ * ss_, h = outH_ * ss_;
        if (ss_ > 1) work_.resize(w, h);
        else         work_.resize(0, 0);
        depth_.assign(std::size_t(w) * std::size_t(h), 1e30f);
    }

    // Box-filter the oversampled frame down to the output.
    //
    // A plain box average, not a weighted kernel. The samples are on a regular
    // grid inside one output pixel, so a box is the correct area average; a
    // Gaussian here would just blur a picture whose whole subject is hard edges.
    void resolve(unsigned workers) {
        const int ss = ss_, W = outW_, H = outH_;
        const int SW = work_.w;
        const int area = ss * ss;
        parallel_for(std::size_t(H), [&](std::size_t yi) {
            const int y = int(yi);
            for (int x = 0; x < W; ++x) {
                int acc[3] = {0, 0, 0};
                for (int sy = 0; sy < ss; ++sy) {
                    const std::uint8_t* row =
                        &work_.rgba[(std::size_t(y * ss + sy) * std::size_t(SW) +
                                     std::size_t(x * ss)) * 4];
                    for (int sx = 0; sx < ss; ++sx) {
                        acc[0] += row[sx * 4 + 0];
                        acc[1] += row[sx * 4 + 1];
                        acc[2] += row[sx * 4 + 2];
                    }
                }
                std::uint8_t* p = &surf_.rgba[(std::size_t(y) * std::size_t(W) + std::size_t(x)) * 4];
                p[0] = std::uint8_t(acc[0] / area);
                p[1] = std::uint8_t(acc[1] / area);
                p[2] = std::uint8_t(acc[2] / area);
                p[3] = 255;
            }
        }, workers);
    }

    static void faceCorners(int fi, float bx, float by, float bz, float h,
                            float* cx, float* cy, float* cz) {
        // Corner offsets per face, wound consistently.
        static const float O[6][4][3] = {
            {{ 1,-1,-1},{ 1, 1,-1},{ 1, 1, 1},{ 1,-1, 1}},   // +x
            {{-1,-1, 1},{-1, 1, 1},{-1, 1,-1},{-1,-1,-1}},   // -x
            {{-1, 1,-1},{ 1, 1,-1},{ 1, 1, 1},{-1, 1, 1}},   // +y
            {{-1,-1, 1},{ 1,-1, 1},{ 1,-1,-1},{-1,-1,-1}},   // -y
            {{-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}},   // +z
            {{-1, 1,-1},{ 1, 1,-1},{ 1,-1,-1},{-1,-1,-1}},   // -z
        };
        for (int c = 0; c < 4; ++c) {
            cx[c] = bx + O[fi][c][0] * h;
            cy[c] = by + O[fi][c][1] * h;
            cz[c] = bz + O[fi][c][2] * h;
        }
    }

    // Flat-shaded triangle with a depth buffer. Barycentric fill: no clipping
    // beyond the screen rectangle, which is all this needs because the camera
    // never enters the volume.
    // Rasterise into rows [bandY0, bandY1). The band is how this is threaded:
    // a worker owns a horizontal strip of the frame and writes only inside it,
    // so no two threads ever touch the same pixel or the same depth value and
    // no lock is needed anywhere. Every worker walks the whole face list, which
    // is cheap — the clip below rejects a face outside the band in four
    // comparisons — and it is what keeps the result identical to the
    // single-threaded one.
    void tri(float x0, float y0, float z0, float x1, float y1, float z1,
             float x2, float y2, float z2, Rgb col, int bandY0, int bandY1) {
        const int W = dst_->w, H = dst_->h;
        int minX = int(std::floor(std::min({x0, x1, x2})));
        int maxX = int(std::ceil (std::max({x0, x1, x2})));
        int minY = int(std::floor(std::min({y0, y1, y2})));
        int maxY = int(std::ceil (std::max({y0, y1, y2})));
        minX = std::max(minX, 0); minY = std::max(minY, bandY0);
        maxX = std::min(maxX, W - 1); maxY = std::min(maxY, std::min(H, bandY1) - 1);
        if (minX > maxX || minY > maxY) return;

        const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (std::fabs(area) < 1e-7f) return;
        const float inv = 1.0f / area;

        for (int py = minY; py <= maxY; ++py)
            for (int px = minX; px <= maxX; ++px) {
                const float fx = float(px) + 0.5f, fy = float(py) + 0.5f;
                float w0 = ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) * inv;
                float w1 = ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) * inv;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                const float zz = w0 * z0 + w1 * z1 + w2 * z2;
                const std::size_t di = std::size_t(py) * W + px;
                if (zz >= depth_[di]) continue;
                depth_[di] = zz;
                std::uint8_t* p = &dst_->rgba[di * 4];
                p[0] = col.r; p[1] = col.g; p[2] = col.b; p[3] = 255;
            }
    }

    // One visible face, already projected. Collected in pass 1 and rasterised
    // in pass 2 — see render().
    struct Face { float x[4], y[4], z[4]; Rgb col; };

    Surface            surf_;      // what callers see, at the requested size
    Surface            work_;      // the oversampled frame, when ss_ > 1
    Surface*           dst_ = &surf_;
    std::vector<float> depth_;
    std::vector<std::vector<Face>> slabs_;   // one per z, so the order is fixed
    unsigned           threads_ = 0;
    int                outW_ = 0, outH_ = 0, ss_ = 1;
};

// Ray-march from the camera through a screen point and return the first solid
// voxel. This is what makes "set the pivot" mean a place in the volume rather
// than a place on the screen: you click a voxel and the camera orbits THAT.
//
// Returns false when the ray misses everything, in which case the caller should
// leave the pivot alone rather than sending the camera somewhere arbitrary.
// The world-space ray through a screen point. Zoom-to-cursor needs it whether
// or not anything solid is under the pointer.
inline void screen_ray(const Camera& cam, float sx, float sy, int W, int H,
                       float* origin, float* dir) {
    float eye[3]; cam.eye(eye);
    const float fwd[3] = { cam.tx - eye[0], cam.ty - eye[1], cam.tz - eye[2] };
    const float fl = std::sqrt(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
    const float f[3] = { fwd[0]/fl, fwd[1]/fl, fwd[2]/fl };
    float r[3] = { f[2], 0.0f, -f[0] };
    const float rl = std::sqrt(r[0]*r[0] + r[2]*r[2]);
    if (rl > 1e-6f) { r[0] /= rl; r[2] /= rl; }
    const float u[3] = { r[1]*f[2] - r[2]*f[1], r[2]*f[0] - r[0]*f[2], r[0]*f[1] - r[1]*f[0] };
    const float focal = float(H) * 0.5f / std::tan(cam.fov * 0.5f);

    if (cam.ortho) {
        const float k = cam.orthoHeight() / float(H);
        const float ox = (sx - float(W)*0.5f) * k, oy = -(sy - float(H)*0.5f) * k;
        origin[0] = eye[0] + r[0]*ox + u[0]*oy;
        origin[1] = eye[1] + r[1]*ox + u[1]*oy;
        origin[2] = eye[2] + r[2]*ox + u[2]*oy;
        dir[0] = f[0]; dir[1] = f[1]; dir[2] = f[2];
    } else {
        const float ndx = (sx - float(W)*0.5f) / focal, ndy = -(sy - float(H)*0.5f) / focal;
        origin[0] = eye[0]; origin[1] = eye[1]; origin[2] = eye[2];
        dir[0] = f[0] + r[0]*ndx + u[0]*ndy;
        dir[1] = f[1] + r[1]*ndx + u[1]*ndy;
        dir[2] = f[2] + r[2]*ndx + u[2]*ndy;
        const float m = std::sqrt(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
        dir[0]/=m; dir[1]/=m; dir[2]/=m;
    }
}

template <class LiveFn>
inline bool pick_voxel(int n, LiveFn live, const Camera& cam,
                       float sx, float sy, int W, int H,
                       float& outX, float& outY, float& outZ) {
    if (W <= 0 || H <= 0) return false;
    float eye[3]; cam.eye(eye);
    const float fwd[3] = { cam.tx - eye[0], cam.ty - eye[1], cam.tz - eye[2] };
    const float fl = std::sqrt(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
    if (fl < 1e-6f) return false;
    const float f[3] = { fwd[0]/fl, fwd[1]/fl, fwd[2]/fl };
    float r[3] = { f[2], 0.0f, -f[0] };
    const float rl = std::sqrt(r[0]*r[0] + r[2]*r[2]);
    if (rl > 1e-6f) { r[0] /= rl; r[2] /= rl; }
    const float u[3] = { r[1]*f[2] - r[2]*f[1], r[2]*f[0] - r[0]*f[2], r[0]*f[1] - r[1]*f[0] };

    const float focal = float(H) * 0.5f / std::tan(cam.fov * 0.5f);
    float d[3];
    if (cam.ortho) {
        // Parallel rays, offset across the view plane. Using the perspective
        // ray here would make clicks land off-target in orthographic mode.
        const float k = cam.orthoHeight() / float(H);
        const float ox = (sx - float(W) * 0.5f) * k, oy = -(sy - float(H) * 0.5f) * k;
        eye[0] += r[0]*ox + u[0]*oy;
        eye[1] += r[1]*ox + u[1]*oy;
        eye[2] += r[2]*ox + u[2]*oy;
        d[0] = f[0]; d[1] = f[1]; d[2] = f[2];
    } else {
        const float ndx = (sx - float(W) * 0.5f) / focal;
        const float ndy = -(sy - float(H) * 0.5f) / focal;
        d[0] = f[0] + r[0]*ndx + u[0]*ndy;
        d[1] = f[1] + r[1]*ndx + u[1]*ndy;
        d[2] = f[2] + r[2]*ndx + u[2]*ndy;
        const float m = std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]); d[0]/=m; d[1]/=m; d[2]/=m;
    }

    // Step at half a voxel so nothing thin is stepped over.
    const float scale = 2.0f / float(n);
    const float step  = scale * 0.5f;
    const float maxT  = cam.distance + 2.0f * 1.74f;      // through the far corner
    for (float t = 0.0f; t < maxT; t += step) {
        const float px = eye[0] + d[0]*t, py = eye[1] + d[1]*t, pz = eye[2] + d[2]*t;
        const int vx = int((px + 1.0f) / scale), vy = int((py + 1.0f) / scale),
                  vz = int((pz + 1.0f) / scale);
        if (vx < 0 || vy < 0 || vz < 0 || vx >= n || vy >= n || vz >= n) continue;
        if (!live(vx, vy, vz)) continue;
        outX = (float(vx) + 0.5f) * scale - 1.0f;
        outY = (float(vy) + 0.5f) * scale - 1.0f;
        outZ = (float(vz) + 0.5f) * scale - 1.0f;
        return true;
    }
    return false;
}

} // namespace bench
