// hexplanetview.hpp — the Goldberg planet, on the bench, turnable.
//
// The geometry and its proofs live in hexplanet.hpp. This is the viewer: it
// renders the sphere, generates terrain on it, and runs the climate model —
// and it exists so the twelve pentagons can be LOOKED at, because "exactly
// twelve, always" is the kind of claim that is much more convincing when you
// can spin the thing and count them.
//
// Three things here are only possible because the world is a sphere rather
// than a plane pretending to be one, and each is the honest argument for the
// extra difficulty:
//
//   Noise      is sampled in 3D at the face centre. There is no projection, so
//              there is no seam to hide and no pole to special-case. A height
//              field on a flat map has to be made to agree with itself at the
//              edges; this one cannot disagree.
//   Ocean      distance is a breadth-first walk over the face adjacency graph,
//   distance   which is exact on the sphere and has no wrap-around cases. The
//              climate model wants "how far to the sea" and the mesh already
//              knows who its neighbours are.
//   Latitude   is just the y component of a unit vector. No cylindrical
//              distortion, because nothing was projected.
//
// The terrain follows PART of PlanetSmith's described approach: a Life-like
// land growth where a cell surrounded by land is likelier to be land, so
// continents come out connected rather than speckled, and then temperature and
// humidity from latitude and distance to water. His version is recursive, each
// pass doubling the resolution so continents are built coarse-to-fine; these
// passes run over one fixed mesh and only smooth a map that already exists.
// hexplanet.hpp lists in full what is and is not borrowed, including the
// vertical dimension — cliffs, overhangs, caves — that this file does not have.

#pragma once
#include "hexplanet.hpp"
#include "../sim.hpp"
#include "../rng.hpp"
#include "../render/voxel.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <vector>

namespace bench {

class HexPlanetView final : public Sim {
public:
    HexPlanetView() {
        about_ = Provenance{
            "Hex planet: twelve pentagons, no more",
            "1937",
            "Michael Goldberg, who classified these solids; the world generation follows part "
            "of Kenneth Ward's described approach for PlanetSmith (Incandescent Games) — "
            "hexplanet.hpp lists which parts, and which are described there but absent here",
            "Goldberg, M. 'A class of multi-symmetric polyhedra', Tohoku Mathematical "
            "Journal 43, 1937; PlanetSmith dev logs, 2020-2026",
            Replication::No,
            "No. It is a shape and a terrain generator, and it is on the bench because the "
            "central claim is checkable rather than because anything here copies itself: a "
            "sphere cannot be tiled with hexagons alone, and Euler's formula forces exactly "
            "twelve pentagons however far you subdivide.",
            "An icosahedron subdivided and dualised. Every vertex of the subdivided solid "
            "becomes a face here; the twelve original corners have five neighbours and become "
            "pentagons, everything else has six. Faces = 10n^2 + 2, twelve of them pentagons, "
            "always. Terrain is 3D noise sampled on the sphere itself, so there is no "
            "projection seam anywhere, and the climate model walks the face adjacency graph "
            "to find the distance to open water."
        };
        pal_ = {
            {{ 16, 26, 46}, "deep ocean"},
            {{ 28, 58, 96}, "shallow sea"},
            {{214,198,148}, "shore"},
            {{ 96,140, 72}, "grass"},
            {{ 54,102, 60}, "forest"},
            {{182,166,120}, "desert"},
            {{120,124,128}, "rock"},
            {{236,240,246}, "ice"},
            {{236, 92,120}, "a pentagon"},
        };
        knobs_ = {
            {"seed", "run seed", 1.f, 40.f, 1.f, 1.f, {}, true,
             "Which planet. The same seed reproduces the identical world — the generator is "
             "deterministic, so a seed IS a planet."},
            {"subdiv", "subdivisions", 4.f, 40.f, 18.f, 1.f, {}, true,
             "How many times each icosahedron edge is cut. Tiles = 10n^2 + 2, of which twelve "
             "are pentagons at every single value — that is Euler's formula, not a setting."},
            {"sea", "sea level", 0.f, 1.f, 0.52f, 0.01f, {}, true,
             "Where the water line sits in the height field. This is the single control with "
             "the most say over whether you get continents, islands or a waterworld."},
            {"grow", "continent growth passes", 0.f, 6.f, 3.f, 1.f, {}, true,
             "Passes of the Life-like smoothing that makes land prefer to sit next to land. "
             "The first pass does nearly all of it — 1005 tiles across forty planets, against "
             "15 at the third and 1 at the fourth; from pass 3 up the map has already reached "
             "a fixed point."},
            // Renamed from "relief", which read as an amplitude control it has
            // never been: the height field is generated by noise3() before this
            // knob is read at all. It scales the altitude term of the
            // temperature model, and that is a lapse rate.
            {"lapse", "lapse rate", 0.f, 1.f, 0.5f, 0.05f, {}, true,
             "How much altitude counts against temperature. The land itself does not move — "
             "coastlines and land fraction are identical at every setting, because the height "
             "field is fixed by the seed. What moves is the snow line: at zero only latitude "
             "is cold, and turning it up carries ice a little further from the poles. Measured "
             "at subdiv 18 over every setting of this knob against all forty seeds, the lowest "
             "ice tile anywhere sits at |y| 0.54; at seed 1 it goes from |y| 0.70 at zero to "
             "0.63 at one."},
            {"pents", "mark the pentagons", 0.f, 1.f, 0.f, 1.f, {"no", "yes"}, false,
             "Paint the twelve pentagons in a colour of their own. Spin the planet and count "
             "them: there are twelve at four subdivisions and twelve at forty."},
            {"spin", "spin", 0.f, 1.f, 0.15f, 0.01f, {}, false,
             "Degrees of rotation per frame. Display only — the planet does not change."},
        };
        surf_.resize(kW, kH);
        view_ = Field(64, 48);
        cam_.distance = 3.1f;
        cam_.pitch = 0.35f;
        reset();
    }

    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field&               field()   const override { return view_; }
    const Surface*             surface() const override { return &surf_; }
    std::vector<Knob>&         knobs()   override { return knobs_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        if (k == "pents") render();
    }

    [[nodiscard]] std::string subtitle() const override {
        char b[192];
        std::snprintf(b, sizeof b,
                      "%zu tiles  ·  %zu hexagons + %zu pentagons  ·  %.0f%% land  ·  n=%d",
                      P_.face.size(), P_.hexagons(), P_.pentagons(),
                      100.0 * landFraction(), P_.subdivisions);
        return b;
    }

    void reset() override {
        // salt_ IS the seed. An Rng member used to be reseeded here from the
        // knob, and the terrain never drew a single value out of it — the seed
        // knob was inert and every "different" world was the same world. The
        // generator has been deleted rather than left sitting in the class
        // looking load-bearing; salt_ carries the seed into noise3(), and
        // poke() offsets it from there.
        salt_ = int(knob("seed") + 0.5f) * 7919;
        P_ = goldberg::build(std::max(2, int(knob("subdiv") + 0.5f)));
        generate();
        gen_ = 0;
        render();
    }

    void step() override {
        cam_.yaw += knob("spin") * 0.0174532925f;
        ++gen_;
        render();
    }

    [[nodiscard]] std::uint64_t generation() const override { return gen_; }

    std::vector<Metric> metrics() const override {
        return {
            Metric{ "land fraction", landFraction(), 1.0, Metric::Neither },
            Metric{ "tiles", double(P_.face.size()), 0.0, Metric::Neither },
            // Twelve. Always. Plotted so it is visibly a constant and not a
            // number that happens to be twelve today.
            Metric{ "pentagons", double(P_.pentagons()), 0.0, Metric::Neither },
            Metric{ "hexagon area spread", double(hexSpread()), 0.0, Metric::Lower },
        };
    }

    // ── camera ─────────────────────────────────────────────────────────────
    [[nodiscard]] bool has_camera() const override { return true; }
    bool camera_orbit(float dx, float dy) override {
        cam_.yaw   -= dx * 0.008f;
        cam_.pitch -= dy * 0.008f;
        cam_.clampPitch();
        render(); return true;
    }
    bool camera_pan(float, float) override { return false; }   // a planet has a centre
    bool camera_dolly(float steps, float, float) override {
        cam_.distance = std::clamp(cam_.distance * (steps > 0 ? 0.9f : 1.0f/0.9f), 1.25f, 12.0f);
        render(); return true;
    }
    bool camera_view(StdView v) override {
        switch (v) {
            case StdView::Front:  cam_.yaw = 0;            cam_.pitch = 0;      break;
            case StdView::Back:   cam_.yaw = 3.14159265f;  cam_.pitch = 0;      break;
            case StdView::Left:   cam_.yaw = 1.57079633f;  cam_.pitch = 0;      break;
            case StdView::Right:  cam_.yaw = -1.57079633f; cam_.pitch = 0;      break;
            case StdView::Top:    cam_.pitch = 1.4f;                            break;
            case StdView::Bottom: cam_.pitch = -1.4f;                           break;
            case StdView::Iso:    cam_.yaw = 0.7853982f;   cam_.pitch = 0.6154797f; break;
        }
        cam_.clampPitch(); render(); return true;
    }
    bool camera_fit() override { cam_.distance = 3.1f; render(); return true; }
    void camera_home() override { cam_.yaw = 0; cam_.pitch = 0.35f; cam_.distance = 3.1f; render(); }

    // Re-roll the planet without touching the seed knob, so the same settings
    // can be seen on several worlds.
    bool poke(float, float) override {
        salt_ += 7919;
        generate();
        render();
        return true;
    }
    Field* editable() override { return nullptr; }

    [[nodiscard]] const HexPlanet& mesh() const { return P_; }
    [[nodiscard]] double land_fraction() const { return landFraction(); }

private:
    static constexpr int kW = 520, kH = 520;

    [[nodiscard]] float knob(const char* key) const {
        for (const auto& k : knobs_) if (k.key == key) return k.value;
        return 0.0f;
    }

    // Not value noise, whatever the octaves and the decaying amplitude suggest.
    // There is no lattice here, no hashed value stored at one, and nothing
    // interpolated between them: it is a closed-form sum of products of sines
    // and cosines, which is periodic and band-limited where value noise is
    // neither. The name below is kept because this is what the terrain USES as
    // noise, but the statistics are a different animal and the comment should
    // not claim a technique the function does not implement.
    //
    // The part that earns its keep is the argument, not the body: it is
    // evaluated at a point in 3D on the sphere, so nothing is projected and
    // there is no seam to hide.
    [[nodiscard]] float noise3(float x, float y, float z, int octaves) const {
        float sum = 0.0f, amp = 1.0f, freq = 1.6f, norm = 0.0f;
        for (int o = 0; o < octaves; ++o) {
            const float s = std::sin(x*freq*1.7f + float(o)*2.1f + float(salt_)*0.013f)
                          * std::cos(y*freq*1.3f - float(o)*1.7f)
                          + std::sin(z*freq*2.1f + float(o)*0.9f)
                          * std::cos(x*freq*0.9f + y*freq*1.1f + float(salt_)*0.007f);
            sum  += amp * s * 0.5f;
            norm += amp;
            amp  *= 0.55f; freq *= 2.03f;
        }
        return 0.5f + 0.5f * std::clamp(sum / std::max(0.0001f, norm), -1.0f, 1.0f);
    }

    void generate() {
        const std::size_t n = P_.face.size();
        height_.assign(n, 0.0f);
        land_.assign(n, 0);
        temp_.assign(n, 0.0f);
        wet_.assign(n, 0.0f);

        for (std::size_t i = 0; i < n; ++i) {
            const auto& c = P_.face[i].centre;
            height_[i] = noise3(c.x, c.y, c.z, 5);
        }

        const float sea = knob("sea");
        for (std::size_t i = 0; i < n; ++i) land_[i] = height_[i] > sea ? 1 : 0;

        // Land prefers to sit next to land, and it is done on the adjacency
        // graph rather than with a blur. One pass does nearly all of it, and
        // the map is at a fixed point by the third.
        const int passes = int(knob("grow") + 0.5f);
        for (int p = 0; p < passes; ++p) {
            std::vector<std::uint8_t> next = land_;
            for (std::size_t i = 0; i < n; ++i) {
                int around = 0;
                for (int g : P_.face[i].neigh) around += land_[std::size_t(g)];
                const int deg = int(P_.face[i].neigh.size());
                if (around >= deg - 1)      next[i] = 1;
                else if (around <= 1)       next[i] = 0;
            }
            land_.swap(next);
        }

        // Distance to open water, by breadth-first walk over the mesh. Exact on
        // the sphere, and with no wrap-around case to get wrong.
        std::vector<int> dist(n, -1);
        std::deque<int> q;
        for (std::size_t i = 0; i < n; ++i) if (!land_[i]) { dist[i] = 0; q.push_back(int(i)); }
        while (!q.empty()) {
            const int i = q.front(); q.pop_front();
            for (int g : P_.face[std::size_t(i)].neigh)
                if (dist[std::size_t(g)] < 0) { dist[std::size_t(g)] = dist[std::size_t(i)] + 1;
                                                q.push_back(g); }
        }
        int far = 1;
        for (int d : dist) far = std::max(far, d);

        const float lapse = knob("lapse");
        for (std::size_t i = 0; i < n; ++i) {
            const auto& c = P_.face[i].centre;
            // Y, not Z. The camera's up axis is Y — its right vector is built
            // with a zero Y component — so taking latitude from Z put the ice
            // caps on the horizon instead of at the poles, and the planet came
            // out with a glacier down one side of its equator.
            const float lat = std::fabs(c.y);                       // 0 equator, 1 pole
            // The altitude term was x3.0 and the lapse rate 0.9, which put the
            // snow line below the mid-latitudes. Land reaches 0.31 above sea
            // level at the very widest — measured over all forty seeds at
            // subdiv 18 and the default sea level; 0.21 at seed 1 — so the
            // multiplier has to be small enough that a mountain is colder than a
            // plain without being colder than a pole.
            const float alt = land_[i] ? (height_[i] - sea) * lapse * 1.2f : 0.0f;
            temp_[i] = std::clamp(1.0f - lat * 1.15f - alt * 0.55f, 0.0f, 1.0f);
            // Humid near water, dry deep inland — and drier where it is cold,
            // because cold air holds less.
            const float inland = float(dist[i]) / float(far);
            wet_[i] = std::clamp((1.0f - inland * 1.6f) * (0.35f + 0.65f * temp_[i]), 0.0f, 1.0f);
        }
    }

    [[nodiscard]] double landFraction() const {
        if (land_.empty()) return 0.0;
        std::size_t l = 0;
        for (auto v : land_) l += v;
        return double(l) / double(land_.size());
    }
    [[nodiscard]] float hexSpread() const {
        float lo = 1e9f, hi = 0.0f;
        for (const auto& f : P_.face)
            if (f.corners.size() == 6) { lo = std::min(lo, f.area); hi = std::max(hi, f.area); }
        return (lo < 1e8f && lo > 0) ? hi / lo : 0.0f;
    }

    [[nodiscard]] Rgb biome(std::size_t i) const {
        if (knob("pents") > 0.5f && P_.face[i].corners.size() == 5) return pal_[8].colour;
        if (!land_[i]) return height_[i] < knob("sea") - 0.08f ? pal_[0].colour : pal_[1].colour;
        const float t = temp_[i], w = wet_[i];
        // Cold first, then high. The rock threshold used to shrink as it got
        // colder — sea + 0.22*t — so at the poles almost every scrap of land
        // cleared it and turned to bare rock before it could ever be ice. A
        // polar continent came out uniformly grey.
        if (t < 0.20f) return pal_[7].colour;                       // ice
        if (height_[i] > knob("sea") + 0.20f) return pal_[6].colour; // bare rock, by altitude
        if (w < 0.20f) return pal_[5].colour;                       // desert
        if (w > 0.62f && t > 0.45f) return pal_[4].colour;          // forest
        if (height_[i] < knob("sea") + 0.02f) return pal_[2].colour; // shore
        return pal_[3].colour;                                       // grass
    }

    // ── rendering ──────────────────────────────────────────────────────────
    void render() {
        for (int i = 0; i < kW * kH; ++i) {
            std::uint8_t* p = &surf_.rgba[std::size_t(i) * 4];
            p[0] = 6; p[1] = 8; p[2] = 14; p[3] = 255;
        }
        depth_.assign(std::size_t(kW) * kH, 1e30f);

        float eye[3]; cam_.eye(eye);
        const HexPlanet::V3 E{eye[0], eye[1], eye[2]};
        const HexPlanet::V3 f = goldberg::norm(HexPlanet::V3{-E.x, -E.y, -E.z});
        HexPlanet::V3 r{f.z, 0.0f, -f.x};
        const float rl = std::sqrt(r.x*r.x + r.z*r.z);
        if (rl > 1e-6f) { r.x /= rl; r.z /= rl; }
        const HexPlanet::V3 u = goldberg::cross(r, f);
        const float focal = float(kH) * 0.5f / std::tan(cam_.fov * 0.5f);

        auto project = [&](HexPlanet::V3 p, float& sx, float& sy, float& sz) {
            const HexPlanet::V3 d{p.x - E.x, p.y - E.y, p.z - E.z};
            sz = goldberg::dot(d, f);
            if (sz <= 0.05f) { sx = sy = 0; sz = -1; return; }
            sx = float(kW) * 0.5f + goldberg::dot(d, r) * focal / sz;
            sy = float(kH) * 0.5f - goldberg::dot(d, u) * focal / sz;
        };

        // A light fixed to the world. The spin knob turns the camera, not the
        // planet, so the terminator never moves across the surface — what the
        // world-fixed light buys is that it moves across the VIEW as you orbit,
        // instead of staying pinned to the screen the way an eye-fixed light
        // would.
        const HexPlanet::V3 L = goldberg::norm({0.55f, 0.62f, 0.56f});

        for (std::size_t i = 0; i < P_.face.size(); ++i) {
            const auto& F = P_.face[i];
            // Back-face cull against the eye: on a sphere the face normal IS
            // its centre, which is one of the small pleasures of this shape.
            const HexPlanet::V3 toEye = goldberg::norm(goldberg::sub(E, F.centre));
            if (goldberg::dot(F.centre, toEye) <= 0.02f) continue;

            const float lam = std::max(0.0f, goldberg::dot(F.centre, L));
            const float shade = 0.30f + 0.70f * lam;
            const Rgb base = biome(i);
            const Rgb col{ std::uint8_t(std::min(255.0f, float(base.r) * shade)),
                           std::uint8_t(std::min(255.0f, float(base.g) * shade)),
                           std::uint8_t(std::min(255.0f, float(base.b) * shade)) };

            float cx, cy, cz;
            project(F.centre, cx, cy, cz);
            if (cz < 0) continue;
            const std::size_t nc = F.corners.size();
            for (std::size_t k = 0; k < nc; ++k) {
                float ax, ay, az, bx, by, bz;
                project(P_.corner[std::size_t(F.corners[k])], ax, ay, az);
                project(P_.corner[std::size_t(F.corners[(k + 1) % nc])], bx, by, bz);
                if (az < 0 || bz < 0) continue;
                tri(cx, cy, cz, ax, ay, az, bx, by, bz, col);
            }
        }

        // The index view is a coarse equirectangular sketch, only so the
        // timeline has something to store — and the notes say so rather than
        // letting it look like the real picture.
        for (int y = 0; y < view_.h; ++y)
            for (int x = 0; x < view_.w; ++x) {
                const float lon = (float(x) / float(view_.w)) * 6.2831853f;
                const float lat = (0.5f - float(y) / float(view_.h)) * 3.14159265f;
                const HexPlanet::V3 d{ std::cos(lat) * std::cos(lon), std::sin(lat),
                                       std::cos(lat) * std::sin(lon) };
                const int fi = P_.nearest_face(d);
                view_.set(x, y, std::uint8_t(land_[std::size_t(fi)] ? 3 : 0));
            }
    }

    void tri(float x0, float y0, float z0, float x1, float y1, float z1,
             float x2, float y2, float z2, Rgb col) {
        int minX = int(std::floor(std::min({x0, x1, x2}))), maxX = int(std::ceil(std::max({x0, x1, x2})));
        int minY = int(std::floor(std::min({y0, y1, y2}))), maxY = int(std::ceil(std::max({y0, y1, y2})));
        minX = std::max(minX, 0); minY = std::max(minY, 0);
        maxX = std::min(maxX, kW - 1); maxY = std::min(maxY, kH - 1);
        if (minX > maxX || minY > maxY) return;
        const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (std::fabs(area) < 1e-7f) return;
        const float inv = 1.0f / area;
        for (int py = minY; py <= maxY; ++py)
            for (int px = minX; px <= maxX; ++px) {
                const float fx = float(px) + 0.5f, fy = float(py) + 0.5f;
                const float w0 = ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) * inv;
                const float w1 = ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) * inv;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                const float zz = w0 * z0 + w1 * z1 + w2 * z2;
                const std::size_t di = std::size_t(py) * kW + px;
                if (zz >= depth_[di]) continue;
                depth_[di] = zz;
                std::uint8_t* p = &surf_.rgba[di * 4];
                p[0] = col.r; p[1] = col.g; p[2] = col.b; p[3] = 255;
            }
    }

    Provenance          about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    Surface             surf_;
    Field               view_;
    std::vector<float>  depth_;
    HexPlanet           P_;
    Camera              cam_;
    std::vector<float>  height_, temp_, wet_;
    std::vector<std::uint8_t> land_;
    int                 salt_ = 0;
    std::uint64_t       gen_ = 0;
};

inline SimPtr make_hexplanet() { return std::make_unique<HexPlanetView>(); }

} // namespace bench
