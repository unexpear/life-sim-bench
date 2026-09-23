// life3d.hpp — Conway's question asked in three dimensions.
//
// Carter Bays asked what Life becomes when a cell has 26 neighbours instead of
// 8, and searched the rule space for one that behaves the way Conway's does:
// neither dying out nor filling space, and supporting a glider. He wrote rules
// as four numbers — survive between El and Eu neighbours, be born between Fl
// and Fu — and 5766 is his answer: survive on 5..7, born on exactly 6. The
// last two digits are a range with both ends at 6; 5767 would be a different
// rule, and is not the one below.
//
//   Bays, C. "Candidates for the Game of Life in Three Dimensions",
//   Complex Systems 1 (1987) 373–400.
//
// RENDERING. This renders itself, through render/voxel.hpp: a perspective
// camera, surface-voxel extraction, per-face normals, Lambertian light, ambient
// occlusion and a float depth buffer. It publishes an RGB Surface.
//
// The first version did NOT do that. It projected voxel centres to points and
// coloured them by one of six depth bands, because the Sim contract only spoke
// in palette indices and I did not want to widen it. The result looked three
// dimensional and contained no 3D rendering whatever — no perspective, no
// surfaces, no light. Widening the contract was the fix, and it was the thing
// worth doing rather than the thing worth avoiding.
//
// A coarse index Field is still published alongside, because the timeline and
// the PNG tools speak that language and there is no reason to break them.

#pragma once
#include "../sim.hpp"
#include "../rng.hpp"
#include "../parallel.hpp"
#include "../render/voxel.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace bench {

class Life3D final : public Sim {
public:
    // Bays' notation: survive in [el,eu], birth in [fl,fu].
    enum class Seed { Block, Ball, Soup };

    Life3D(Provenance about, int el, int eu, int fl, int fu, int n = 48, float density = 0.14f,
           Seed seed = Seed::Block, int block = 5)
        : about_(std::move(about)), n_(n), density_(density),
          cur_(std::size_t(n) * n * n, 0), nxt_(std::size_t(n) * n * n, 0),
          view_(kView, kView), depth_(std::size_t(kView) * kView, 0.0f) {
        vox_.resize(kRender, kRender);
        cam_.distance = 3.0f;
        buildPalette();
        buildSizeLadder();
        knobs_ = {
            // World size, and the one knob here whose cost is CUBIC. Doubling
            // the edge is eight times the work, which is why this is a short
            // ladder that stops early rather than the 256..4096 run the 2D
            // lattice family can afford. A cell costs more here too: 26
            // neighbours read instead of 8, so 128^3 at 2.1 million cells is
            // about the work of a 2D lattice with three times as many.
            //
            // Measured here on 16 workers, fastest of five, on an otherwise
            // idle machine — the last clause matters, because the same probe
            // run against a loaded one reported everything two to three times
            // higher and would have justified a much shorter ladder. Whole
            // step including the render: 40^3 (the shipped default) 2.7 ms,
            // 64^3 4.4 ms, 96^3 8.9 ms, 128^3 18 ms, 160^3 34 ms. The ladder stops
            // at 160 because 192^3 measured 57 ms — over half a second for ten
            // generations, which is where watching it stops being watching.
            // 160^3 is 64 times the volume of the shipped 40^3 and 13 times the
            // wall clock, which is the whole point of threading the sweep.
            //
            // Anything not on the ladder that a caller constructs — the
            // self-test builds 20^3 and 28^3 — is inserted into the list, so
            // the knob always names the size actually running rather than the
            // nearest one it likes.
            {"world", "world size", 0.f, float(sizes_.size() - 1), float(sizeIndex(n_)), 1.f,
             sizeLabels_, true,
             "Cells along each edge, cubed. Cost is cubic and the neighbourhood is 26 wide, so "
             "160^3 is about 34 ms a step here against under 3 for the default 40^3 — 64 times "
             "the volume for 13 times the wait. Bigger volumes are where a 3D rule has room to "
             "show structure; they are not where it runs fast."},
            // Rendering quality. A voxel volume is nothing but hard edges, so
            // every silhouette is a staircase; there is no cheaper fix than
            // more samples, because the aliasing is geometric and no blur can
            // recover what the single sample never had. Measured at 900x700 on
            // a 64-cubed shell, fastest of seven: threading it is worth about
            // 1.8x at 1x sampling and 1.8x again at 2x, and four times the
            // samples costs under three times the frame because the geometry
            // pass is shared.
            //
            // Ratios, not milliseconds. This used to quote absolute frame times
            // and name no machine, which makes a comment that is false for
            // every reader who is not on that one: the same benchmark on other
            // hardware ran two and a half times slower in absolute terms and
            // showed a LARGER speedup. The ratio is the portable half, and even
            // it moves with core count.
            //
            // The range starts at 0. The renderer's multiple is one more than
            // the setting, which is what the +1 in on_knob is for.
            {"aa", "supersampling", 0.f, 2.f, 0.f, 1.f, {"off", "2x", "3x"}, false,
             "Render oversampled and average down. 2x is the useful setting; the cost "
             "lands on rasterisation, which is the half that threads well.", true},
            {"yaw",   "view yaw (degrees)",   0.f, 360.f, 35.f, 1.f, {}, false,
             "Rotate the volume about the vertical axis. The step rule has no preferred "
             "direction, but this is not view only: it decides where a poke lands, and so "
             "every generation after one."},
            {"pitch", "view pitch (degrees)", -80.f, 80.f, 25.f, 1.f, {}, false,
             "Tilt the camera."},
            {"spin",  "auto-rotate (deg/step)", 0.f, 2.f, 0.25f, 0.05f, {}, false,
             "Rotate a little each generation. A still projection of a 3D volume is genuinely "
             "hard to read — motion is what separates the near surface from the far one."},
            {"density", "seed fill fraction", 0.05f, 1.0f, density_, 0.01f, {}, true,
             "How much of the seed region starts alive. 1.0 is solid — which is what Bays' rules "
             "are demonstrated from."},
        };
        // The rule itself, as four bounds. A 26-neighbour rule has 54 bits, so
        // a chip per count would be unreadable — Bays' own notation is ranges,
        // and these are those ranges.
        knobs_.push_back({"el", "survive: lowest neighbour count", 0.f, 26.f, float(el), 1.f, {}, false,
            "A live cell with fewer live neighbours than this dies of loneliness."});
        knobs_.push_back({"eu", "survive: highest", 0.f, 26.f, float(eu), 1.f, {}, false,
            "A live cell with more live neighbours than this dies of crowding."});
        knobs_.push_back({"fl", "birth: lowest", 1.f, 26.f, float(fl), 1.f, {}, false,
            "A dead cell needs at least this many live neighbours to be born. Keep it above 0 "
            "or the empty volume ignites."});
        knobs_.push_back({"fu", "birth: highest", 1.f, 26.f, float(fu), 1.f, {}, false,
            "And at most this many."});
        // The seed is a control, not a constant, because WHICH seed you use
        // decides whether a 3D rule shows anything at all. Measured on Bays'
        // 5766: a solid 5-cube settles into a stable 104-voxel structure, while
        // a random soup dies out only at density 0.55 and above.
        knobs_.push_back({"seed", "starting configuration", 0.f, 2.f, float(int(seed)), 1.f,
            {"solid block", "ball of noise", "random soup"}, true,
            "Solid block is how 3D rules are actually demonstrated. Random soup looks like the "
            "obvious choice and collapses under most of them."});
        knobs_.push_back({"size", "seed size", 2.f, 32.f, float(block), 1.f, {}, true,
            "Extent of the starting region: the cube's edge or the ball's diameter. Random soup "
            "ignores it and fills the whole volume — a soup inside a box would be the block seed "
            "over again. Which size you pick decides whether anything survives at all: measured "
            "on 5766 at 48^3, density 1, 150 steps, a solid 3-cube freezes at 32 voxels and a "
            "solid 5-cube at 104, while 15 of the 31 sizes in this range end empty."});
        el_ = el; eu_ = eu; fl_ = fl; fu_ = fu;
        reset();
    }

    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field&               field()   const override { return view_; }
    const Surface*             surface() const override { return &vox_.surface(); }
    std::uint64_t              generation() const override { return gen_; }
    std::vector<Knob>&         knobs()   override { return knobs_; }

    [[nodiscard]] std::string subtitle() const override {
        char b[64];
        std::snprintf(b, sizeof b, "%d%d%d%d  ·  %d^3", el_, eu_, fl_, fu_, n_);
        return b;
    }

    // ── camera ─────────────────────────────────────────────────────────────
    [[nodiscard]] bool has_camera() const override { return true; }

    bool camera_orbit(float dx, float dy) override {
        cam_.yaw   -= dx * 0.010f;
        cam_.pitch += dy * 0.010f;
        cam_.clampPitch();
        syncKnobs(); publish();
        return true;
    }
    bool camera_pan(float dx, float dy) override {
        // Move the pivot in the plane of the screen, scaled by how far away it
        // is — so a drag moves the same number of PIXELS whatever the zoom.
        float r[3], u[3]; cam_.basis(r, u);
        const float k = cam_.distance * 0.0016f;
        cam_.tx += (-dx * r[0] + dy * u[0]) * k;
        cam_.ty += (-dx * r[1] + dy * u[1]) * k;
        cam_.tz += (-dx * r[2] + dy * u[2]) * k;
        clampTarget(); publish();
        return true;
    }
    bool camera_dolly(float steps, float nx, float ny) override {
        // Zoom toward the cursor. The point under the pointer is found by ray
        // — the first solid voxel if there is one, otherwise where the ray
        // crosses the plane through the pivot — and the pivot is then moved a
        // matching fraction of the way toward it, so that point stays put on
        // screen while everything else scales around it. Zooming to the screen
        // centre instead is the thing that makes a 3D view feel like it is
        // fighting you.
        const Surface& s = vox_.surface();
        float o[3], d[3];
        screen_ray(cam_, nx * float(s.w), ny * float(s.h), s.w, s.h, o, d);

        float hx, hy, hz;
        bool have = pick_voxel(n_, [&](int x, int y, int z) { return cur_[idx(x,y,z)] != 0; },
                               cam_, nx * float(s.w), ny * float(s.h), s.w, s.h, hx, hy, hz);
        if (!have) {
            float fdir[3]; cam_.forward(fdir);
            const float denom = d[0]*fdir[0] + d[1]*fdir[1] + d[2]*fdir[2];
            if (std::fabs(denom) > 1e-5f) {
                const float t = ((cam_.tx - o[0])*fdir[0] + (cam_.ty - o[1])*fdir[1]
                               + (cam_.tz - o[2])*fdir[2]) / denom;
                hx = o[0] + d[0]*t; hy = o[1] + d[1]*t; hz = o[2] + d[2]*t;
                have = true;
            }
        }

        const float before = cam_.distance;
        cam_.distance = std::clamp(cam_.distance * std::pow(0.85f, steps), 0.15f, 14.0f);
        if (have) {
            const float k = 1.0f - cam_.distance / before;   // how far we closed in
            cam_.tx += (hx - cam_.tx) * k;
            cam_.ty += (hy - cam_.ty) * k;
            cam_.tz += (hz - cam_.tz) * k;
            clampTarget();
        }
        publish();
        return true;
    }

    bool camera_view(StdView v) override {
        const float P = 3.14159265f;
        switch (v) {
        case StdView::Front:  cam_.yaw = 0.0f;     cam_.pitch = 0.0f;    break;
        case StdView::Back:   cam_.yaw = P;        cam_.pitch = 0.0f;    break;
        case StdView::Right:  cam_.yaw = P * 0.5f; cam_.pitch = 0.0f;    break;
        case StdView::Left:   cam_.yaw = -P* 0.5f; cam_.pitch = 0.0f;    break;
        case StdView::Top:    cam_.yaw = 0.0f;     cam_.pitch =  1.5533f; break;
        case StdView::Bottom: cam_.yaw = 0.0f;     cam_.pitch = -1.5533f; break;
        case StdView::Iso:    cam_.yaw = P * 0.25f; cam_.pitch = 0.6155f; break;  // 45/35.26
        }
        syncKnobs(); publish();
        return true;
    }

    // Frame everything that is alive: centre on its bounding box and back off
    // far enough to contain it. "Zoom to fit" is the control you reach for when
    // a view has got away from you, so it must always produce something.
    bool camera_fit() override {
        int x0 = n_, y0 = n_, z0 = n_, x1 = -1, y1 = -1, z1 = -1;
        for (int z = 0; z < n_; ++z)
            for (int y = 0; y < n_; ++y)
                for (int x = 0; x < n_; ++x)
                    if (cur_[idx(x, y, z)]) {
                        x0 = std::min(x0, x); y0 = std::min(y0, y); z0 = std::min(z0, z);
                        x1 = std::max(x1, x); y1 = std::max(y1, y); z1 = std::max(z1, z);
                    }
        const float scale = 2.0f / float(n_);
        if (x1 < 0) { camera_home(); return true; }          // nothing alive
        cam_.tx = ((float(x0 + x1) * 0.5f) + 0.5f) * scale - 1.0f;
        cam_.ty = ((float(y0 + y1) * 0.5f) + 0.5f) * scale - 1.0f;
        cam_.tz = ((float(z0 + z1) * 0.5f) + 0.5f) * scale - 1.0f;
        const float radius = 0.5f * scale *
            std::sqrt(float((x1-x0)*(x1-x0) + (y1-y0)*(y1-y0) + (z1-z0)*(z1-z0)));
        cam_.distance = std::clamp((radius + scale) / std::tan(cam_.fov * 0.5f) * 1.15f,
                                   0.2f, 14.0f);
        publish();
        return true;
    }

    bool camera_ortho(bool on) override { cam_.ortho = on; publish(); return true; }
    [[nodiscard]] bool camera_is_ortho() const override { return cam_.ortho; }
    bool camera_pick(float nx, float ny) override {
        float px, py, pz;
        const Surface& s = vox_.surface();
        if (!pick_voxel(n_, [&](int x, int y, int z) { return cur_[idx(x,y,z)] != 0; },
                        cam_, nx * float(s.w), ny * float(s.h), s.w, s.h, px, py, pz))
            return false;                       // missed: leave the pivot alone
        cam_.tx = px; cam_.ty = py; cam_.tz = pz;
        publish();
        return true;
    }
    void camera_home() override {
        cam_.tx = cam_.ty = cam_.tz = 0.0f;
        cam_.distance = 3.0f;
        publish();
    }
    [[nodiscard]] const Camera& camera() const { return cam_; }

    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        if (k == "aa")    { vox_.set_supersample(int(v + 0.5f) + 1); publish(); }
        if (k == "yaw")   { cam_.yaw   = v * 3.14159265f / 180.0f; publish(); }
        if (k == "pitch") { cam_.pitch = v * 3.14159265f / 180.0f; cam_.clampPitch(); publish(); }
        if (k == "world") {
            const int i = std::clamp(int(v + 0.5f), 0, int(sizes_.size()) - 1);
            resizeWorld(sizes_[std::size_t(i)]);
        }
        if (k == "el") el_ = int(v + 0.5f);
        if (k == "eu") eu_ = int(v + 0.5f);
        if (k == "fl") fl_ = int(v + 0.5f);
        if (k == "fu") fu_ = int(v + 0.5f);
        if (k == "density") density_ = v;
    }

    void reset() override {
        rng_.reseed(0x3D11FEull);
        gen_ = 0;
        // One loop for all three seeds: `size` picks the region and `density`
        // fills it. Soup is the deliberate exception — it takes the whole
        // volume and never reads `size` — because a soup confined to a box of
        // edge k IS the block seed at a density below 1, and a seed mode that
        // duplicates another one is worse than a knob that sits out a mode.
        // The `size` help says so, so the panel is not promising an extent that
        // soup does not have.
        const int k    = std::clamp(int(knob("size") + 0.5f), 2, n_);
        const int mode = int(knob("seed") + 0.5f);
        std::fill(cur_.begin(), cur_.end(), std::uint8_t(0));

        // Integer bounds, not a float half-width. Cell centres sit on
        // half-integers when n is even, so `fabs(x - centre) < k/2` asked for a
        // 5-cube and built a 4-cube — and 5766 settles at 56 voxels from a
        // 4-cube versus 104 from a 5-cube, so the off-by-one was visible in the
        // result and nowhere else.
        const int lo = (n_ - k) / 2, hi = lo + k;
        const float c = float(n_ - 1) * 0.5f, r2 = float(k * k) * 0.25f;

        for (int z = 0; z < n_; ++z)
            for (int y = 0; y < n_; ++y)
                for (int x = 0; x < n_; ++x) {
                    bool inRegion;
                    if (mode == 1) {                            // ball of diameter k
                        const float dx = float(x)-c, dy = float(y)-c, dz = float(z)-c;
                        inRegion = dx*dx + dy*dy + dz*dz < r2;
                    } else if (mode == 2) {                     // soup: whole volume
                        inRegion = true;
                    } else {                                    // cube of edge k
                        inRegion = x >= lo && x < hi && y >= lo && y < hi && z >= lo && z < hi;
                    }
                    if (inRegion && rng_.unit() < density_) cur_[idx(x, y, z)] = 1;
                }
        publish();
    }

    void step() override {
        // Slice-parallel above a threshold, over z. Double buffered, so slices
        // are independent by construction: every read comes from cur_ and each
        // slice writes only its own span of nxt_. Nothing else is touched — in
        // particular the sweep never draws from rng_, which is the shared state
        // that would make a threaded step give a different answer than a serial
        // one. The spin, the swap and publish() below stay on one thread.
        //
        // The threshold is lower than the lattice family's 250000 cells because
        // a cell here costs more than three times as much: 26 neighbours read
        // instead of 8. Measured on this sweep alone at 16 workers, fastest of
        // five, idle machine: 32^3 is 1.46 ms serial against 0.98 threaded
        // (1.5x), while 28^3 is 0.99 serial against 1.11 threaded — an actual
        // loss. So the line goes at 32^3, and the self-test's 20^3 and 28^3
        // volumes stay serial.
        //
        // Above it the win grows with the volume: 3.6x at 48^3, 5.8x at 96^3,
        // 6.0x at 160^3 on 16 workers. Short of the 16x an ideal split would
        // give, because a 26-neighbour gather is bound by memory rather than
        // arithmetic — three planes of the volume are in flight at once.
        //
        // Timing a whole step will show less than this, and at 32^3 will show
        // nothing at all. publish() is the other half, and the voxel renderer
        // does its own threading: measured here it costs MORE on 16 workers
        // than on one below about 48^3 (0.52 ms serial against 1.76 threaded at
        // 32^3). That is render/voxel.hpp's threshold to fix, not this file's.
        const int n = n_;
        const std::size_t cells = std::size_t(n) * std::size_t(n) * std::size_t(n);
        parallel_for(std::size_t(n), [&](std::size_t zi) {
            const int z = int(zi);
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x) {
                    int c = 0;
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                if (!dx && !dy && !dz) continue;
                                c += at(x + dx, y + dy, z + dz);
                            }
                    const bool alive = cur_[idx(x, y, z)] != 0;
                    const bool live  = alive ? (c >= el_ && c <= eu_)
                                             : (c >= fl_ && c <= fu_);
                    nxt_[idx(x, y, z)] = live ? 1 : 0;
                }
        }, cells >= kParallelVoxels ? 0u : 1u);
        cur_.swap(nxt_);
        ++gen_;

        const float spin = knob("spin");
        if (spin != 0.0f) {
            cam_.yaw += spin * 3.14159265f / 180.0f;
            if (cam_.yaw > 6.2831853f) cam_.yaw -= 6.2831853f;
            syncKnobs();
        }
        publish();
    }

    std::vector<Metric> metrics() const override {
        std::size_t live = 0;
        for (auto c : cur_) if (c) ++live;
        // Neither, explicitly, rather than the Higher default. Bays was looking
        // for a rule that neither dies out nor fills the volume, so a full
        // frame is one failure and an empty one is the other — a "best so far"
        // line on this series would be reporting whichever end of the run was
        // fullest as the result. The number describes the run; it does not
        // score it.
        return { Metric{ "live fraction", double(live) / double(cur_.size()), 1.0,
                         Metric::Neither } };
    }

    // Painting into the projection would be meaningless — it is a picture of a
    // volume, not the volume. Seeding a fresh ball is the honest equivalent.
    bool poke(float nx, float ny) override {
        // The click maps onto the volume, not onto the picture: x is mirrored
        // about the centre and both axes are scaled to the volume rather than
        // the image, so only a click at dead centre lands where you clicked.
        //
        // SOLID, not a scatter. In a dense rule like 13-26/14-19 the volume is
        // already 90% full where you are aiming, so a probabilistic sprinkle can
        // leave the state untouched; a solid ball is a definite change wherever
        // it lands, which is what a stamp should be.
        // Offset from mid-depth. Measured on 13-26/14-19: a stamp in the middle
        // of a 90%-full volume adds 33 voxels and changes the picture not at
        // all, because the z-buffer correctly hides everything behind the
        // surface. The x and z components of the offset are the NEGATIVE of the
        // eye direction and only the y component follows it, so at the default
        // pitch of 25 the ball is pushed to the FAR side of the volume; it goes
        // toward the viewer only when |pitch| is above 45 degrees.
        const float r  = std::max(2.0f, float(n_) * 0.10f);
        const float mid = float(n_ - 1) * 0.5f;
        const float yaw = knob("yaw") * 3.14159265f / 180.0f;
        const float pit = knob("pitch") * 3.14159265f / 180.0f;
        const float push = mid * 0.55f;
        const float cx = std::clamp(std::clamp(nx, 0.f, 1.f) * float(n_ - 1)
                                    - std::sin(yaw) * push * std::cos(pit), 0.f, float(n_ - 1));
        const float cy = std::clamp(std::clamp(ny, 0.f, 1.f) * float(n_ - 1)
                                    + std::sin(pit) * push, 0.f, float(n_ - 1));
        const float cz = std::clamp(mid - std::cos(yaw) * push * std::cos(pit),
                                    0.f, float(n_ - 1));
        for (int z = 0; z < n_; ++z)
            for (int y = 0; y < n_; ++y)
                for (int x = 0; x < n_; ++x) {
                    const float dx = float(x)-cx, dy = float(y)-cy, dz = float(z)-cz;
                    if (dx*dx + dy*dy + dz*dz < r*r) cur_[idx(x, y, z)] = 1;
                }
        publish();
        return true;
    }

    // Seed a solid cube at the centre. This is how 3D life rules are actually
    // demonstrated.
    void seed_block(int k) {
        for (auto& kn : knobs_) {
            if (kn.key == "seed")    kn.value = 0.f;
            if (kn.key == "size")    kn.value = float(k);
            if (kn.key == "density") kn.value = 1.f;
        }
        density_ = 1.f;
        reset();
    }

    [[nodiscard]] std::size_t live_voxels() const {
        std::size_t n = 0; for (auto c : cur_) if (c) ++n; return n;
    }
    [[nodiscard]] int size() const { return n_; }
    // Test hook: one cell of the volume. A population count is not enough to
    // show that a threaded step matched a serial one — two different volumes
    // can share a count — so proving it needs the cells themselves.
    [[nodiscard]] int peek(int x, int y, int z) const { return cur_[idx(x, y, z)]; }

private:
    static constexpr int kView   = 160;  // coarse index view, for the timeline and the PNG tools
    static constexpr int kRender = 620;  // the real render
    static constexpr int kBands = 6;     // depth shades, plus index 0 for empty
    // Below this many cells the sweep stays on one thread. See step().
    static constexpr std::size_t kParallelVoxels = 32768;   // 32^3

    [[nodiscard]] std::size_t idx(int x, int y, int z) const {
        return (std::size_t(z) * n_ + y) * n_ + x;
    }
    // Toroidal in all three axes, by comparison rather than modulo — the same
    // reason Field::wrap does: this is the hot path, 26 times per cell.
    [[nodiscard]] int at(int x, int y, int z) const {
        if (x < 0) x += n_; else if (x >= n_) x -= n_;
        if (y < 0) y += n_; else if (y >= n_) y -= n_;
        if (z < 0) z += n_; else if (z >= n_) z -= n_;
        return cur_[idx(x, y, z)];
    }
    [[nodiscard]] float knob(const char* key) const {
        for (auto& kn : knobs_) if (kn.key == key) return kn.value;
        return 0.f;
    }

    // The world sizes offered, always including whatever size was actually
    // constructed. A ladder that silently omits the running size would make the
    // knob show a number the sim is not at.
    void buildSizeLadder() {
        sizes_ = {32, 40, 48, 64, 80, 96, 128, 160};
        if (std::find(sizes_.begin(), sizes_.end(), n_) == sizes_.end()) {
            sizes_.push_back(n_);
            std::sort(sizes_.begin(), sizes_.end());
        }
        sizeLabels_.clear();
        for (int s : sizes_) sizeLabels_.push_back(std::to_string(s) + "^3");
    }
    [[nodiscard]] int sizeIndex(int n) const {
        const auto it = std::find(sizes_.begin(), sizes_.end(), n);
        return it == sizes_.end() ? 0 : int(it - sizes_.begin());
    }

    // Rebuild the volume at a new edge length. reset() only refills the cells it
    // already has, so resizing has to happen here or the knob would be a no-op
    // that looks like it worked.
    void resizeWorld(int n) {
        if (n == n_ || n < 2) return;
        n_ = n;
        cur_.assign(std::size_t(n) * n * n, 0);
        nxt_.assign(std::size_t(n) * n * n, 0);
        reset();                     // refills cur_ at the new size and publishes
    }

    void buildPalette() {
        pal_.clear();
        pal_.push_back({{8, 11, 14}, "empty"});
        // Near to far. Naming the ends is what makes the depth cue readable as
        // depth rather than as an unexplained colour ramp.
        // Six bands, every one named. Ten gave smoother shading and left eight
        // blank rows in the legend — and a legend row with a colour and no
        // meaning is worse than one fewer shade.
        static const char* kNames[kBands] = {
            "nearest", "near", "mid", "mid-far", "far", "farthest"
        };
        for (int i = 0; i < kBands; ++i) {
            const float t = float(i) / float(kBands - 1);          // 0 near, 1 far
            const auto mix = [&](int nearC, int farC) {
                return std::uint8_t(float(nearC) + (float(farC) - float(nearC)) * t);
            };
            pal_.push_back({{ mix(226, 30), mix(255, 62), mix(244, 76) }, kNames[i]});
        }
    }

    // Keep the yaw/pitch sliders showing what the camera is actually doing,
    // so dragging the view and reading the numbers never disagree.
    void syncKnobs() {
        for (auto& kn : knobs_) {
            if (kn.key == "yaw")   kn.value = cam_.yaw   * 180.0f / 3.14159265f;
            if (kn.key == "pitch") kn.value = cam_.pitch * 180.0f / 3.14159265f;
        }
    }
    void clampTarget() {
        cam_.tx = std::clamp(cam_.tx, -1.5f, 1.5f);
        cam_.ty = std::clamp(cam_.ty, -1.5f, 1.5f);
        cam_.tz = std::clamp(cam_.tz, -1.5f, 1.5f);
    }

    void publish() {
        vox_.render(n_,
                    [&](int x, int y, int z) { return cur_[idx(x, y, z)] != 0; },
                    cam_, tint_, Rgb{8, 11, 14});

        // And the coarse index view the rest of the bench speaks in. Rotate,
        // orthographic-project, z-buffer — the orthographic one is this coarse
        // pass, not the perspective render above. Far voxels cannot paint over
        // near ones, which is the difference between a solid object and a cloud
        // of dots.
        view_.fill(0);
        std::fill(depth_.begin(), depth_.end(), 1e30f);

        const float yaw = knob("yaw") * 3.14159265f / 180.0f;
        const float pit = knob("pitch") * 3.14159265f / 180.0f;
        const float cy = std::cos(yaw), sy = std::sin(yaw);
        const float cp = std::cos(pit), sp = std::sin(pit);

        const float c   = float(n_ - 1) * 0.5f;
        // Fit the whole rotated cube: its longest diagonal is sqrt(3) across.
        const float fit = float(kView) / (float(n_) * 1.75f);
        const float half = float(kView) * 0.5f;
        const int   vox  = std::max(1, int(fit * 0.9f));    // splat size in pixels

        float zmin = 1e30f, zmax = -1e30f;
        for (int z = 0; z < n_; ++z)
            for (int y = 0; y < n_; ++y)
                for (int x = 0; x < n_; ++x) {
                    if (!cur_[idx(x, y, z)]) continue;
                    const float px = float(x) - c, py = float(y) - c, pz = float(z) - c;
                    const float rx =  px * cy + pz * sy;
                    const float rz = -px * sy + pz * cy;
                    const float ry =  py * cp - rz * sp;
                    const float rd =  py * sp + rz * cp;     // depth
                    if (rd < zmin) zmin = rd;
                    if (rd > zmax) zmax = rd;
                    const int sx = int(half + rx * fit), syi = int(half + ry * fit);
                    for (int oy = 0; oy < vox; ++oy)
                        for (int ox = 0; ox < vox; ++ox) {
                            const int tx = sx + ox, ty = syi + oy;
                            if (tx < 0 || ty < 0 || tx >= kView || ty >= kView) continue;
                            const std::size_t di = std::size_t(ty) * kView + tx;
                            if (rd >= depth_[di]) continue;   // something nearer is here
                            depth_[di] = rd;
                            view_.set(tx, ty, 1);             // band filled in below
                        }
                }

        // Second pass for the depth bands: the range is only known once every
        // voxel has been projected, and banding against a guessed range makes
        // a sparse frame look flat.
        if (zmax > zmin) {
            const float k = float(kBands - 1) / (zmax - zmin);
            for (std::size_t i = 0; i < view_.cells.size(); ++i) {
                if (!view_.cells[i]) continue;
                const int band = int((depth_[i] - zmin) * k + 0.5f);
                view_.cells[i] = std::uint8_t(1 + std::min(kBands - 1, std::max(0, band)));
            }
        }
    }

    Provenance          about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    std::vector<int>         sizes_;
    std::vector<std::string> sizeLabels_;
    int                 n_;
    float               density_;
    int                 el_ = 5, eu_ = 7, fl_ = 6, fu_ = 7;
    std::vector<std::uint8_t> cur_, nxt_;
    Field               view_;
    std::vector<float>  depth_;
    VoxelRenderer       vox_;
    Camera              cam_;
    Rgb                 tint_{120, 226, 210};
    Rng                 rng_{0x3D11FEull};
    std::uint64_t       gen_ = 0;
};

// ── the named rules ─────────────────────────────────────────────────────────

inline SimPtr make_life3d_5766(int n = 48) {
    Provenance p{
        "3D Life 5766", "1987", "Carter Bays",
        "Bays, C. \"Candidates for the Game of Life in Three Dimensions\", "
        "Complex Systems 1 (1987) 373-400",
        Replication::No,
        "No. Bays' criterion was a rule that behaves the way Conway's does — bounded growth and "
        "a glider — and 5766 meets it. A glider is a structure that TRANSLATES, which is not the "
        "same as one that copies itself; the distinction this bench keeps insisting on applies "
        "here too.",
        "Survive on 5 to 7 neighbours, born on exactly 6, out of 26. Bays searched the 3D rule "
        "space for one that neither dies out nor fills the volume, and this is his answer. The "
        "picture is a rendered surface: colour is a single tint shaded by light and ambient "
        "occlusion, and depth reads as perspective and occlusion rather than as hue."
    };
    // Solid 5-cube: measured to settle at 104 stable voxels.
    return std::make_unique<Life3D>(std::move(p), 5, 7, 6, 6, n, 1.0f,
                                    Life3D::Seed::Block, 5);
}

inline SimPtr make_life3d_4555(int n = 48) {
    Provenance p{
        "3D Life 4555", "1987", "Carter Bays",
        "Bays, C. \"Candidates for the Game of Life in Three Dimensions\", "
        "Complex Systems 1 (1987) 373-400",
        Replication::No,
        "No known self-replicator. NOT on the default roster: measured here at 40^3, every solid "
        "cube from 2 to 13 on a side is empty after ONE step, but the sparser seeds are not — "
        "across a 108-way sweep (three seed modes x sizes 5/10/20/32 x densities 0.10 to 0.90) 39 "
        "runs still held something at generation 250. Never more than 56 voxels out of 64000: "
        "27 still lifes, 11 period-4 oscillators and one structure that travels. Nothing "
        "grows. So it collapses rather than dying out, which is the "
        "narrower claim and the true one. That is reported rather than hidden, and the factory is "
        "kept so the rule can still be dialled up from 5766's knobs.",
        "Survive on 4 or 5 neighbours, born on exactly 5 — which is what the four digits say: "
        "El=4, Eu=5, Fl=Fu=5. One digit either way and a 3D rule usually either dies immediately "
        "or fills the cube, which is the whole reason finding a good one took a search."
    };
    return std::make_unique<Life3D>(std::move(p), 4, 5, 5, 5, n, 0.20f,
                                    Life3D::Seed::Block, 5);
}

// The symbol and the registry id still say "clouds"; the title no longer does.
// This rule was shipped as "3D Clouds 1", which is a different rule: Clouds 1
// is 13-26/13-14,17-19, a birth set with a hole in it that Bays' four-number
// notation cannot even write down. What is built here — survive 13-26, born
// 14-19 — is catalogued as "Stable Structures", and the behaviour agrees with
// that name rather than with the old one. The id is the key the workbench saves
// runs under and lives in registry.hpp, so only the claim on screen changed.
inline SimPtr make_life3d_clouds(int n = 48) {
    Provenance p{
        "3D Stable Structures 13-26/14-19", "—", "rule catalogued among Bays-notation 3D rules",
        "Rule 13-26/14-19 in Bays' (El,Eu,Fl,Fu) notation; listed as \"Stable Structures\", "
        "13-26/14-19/2/M, in Softology's 3D cellular automata rule collection "
        "(softologyblog.wordpress.com, 2019)",
        Replication::No,
        "No, and nothing else either: it stops. Measured from its own default ball with spin off, "
        "the volume erodes for 46 generations and then freezes — from generation 47 the field is "
        "bit-identical for the rest of a 400-step run, at 6946 live voxels, and the same count at "
        "32^3, 40^3 and 48^3. It is here as the far end of the rule space from 5766, not as "
        "something that lives. Note that the default 0.25 deg/step spin keeps the camera turning "
        "after the state has stopped, so a frozen run still looks like a moving one.",
        "Survive on 13 to 26, born on 14 to 19. The upper survival bound is the whole "
        "neighbourhood, so nothing ever dies of crowding and a packed interior is safe; only the "
        "thin outside dies. It sheds its ragged surface for a few dozen generations and then "
        "stops changing altogether."
    };
    // A dense ball: measured to settle at 6946 stationary voxels by generation
    // 47 — a 400-step probe with spin off, hashing the published field after
    // every step, run at 32, 40 and 48 cubed.
    return std::make_unique<Life3D>(std::move(p), 13, 26, 14, 19, n, 0.90f,
                                    Life3D::Seed::Ball, 26);
}

} // namespace bench
