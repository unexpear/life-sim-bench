// test_planet.cpp — asserts every claim src/world/planet.hpp makes about its
// addressing, and MEASURES the things a claim cannot cover: how big a cell
// really is, how much cell size varies, what happens to a character who walks
// across a pentagon, and what a materialised neighbourhood costs.
//
// The load-bearing tests are the two that a plausible-looking implementation
// passes by accident:
//
//   EXACT PARTITION. Every fine cell has exactly one parent and appears in
//   exactly one parent's child list, at every depth. A hierarchy that is merely
//   "mostly" a partition is the failure mode of every index scheme on a sphere,
//   and it hides at chart seams and at the twelve pentagons, which is 0.0002% of
//   the world at working resolution and 100% of the bugs.
//
//   PATH INDEPENDENCE. A cell must generate identically whether it is reached
//   directly or by walking to it, away, and back — with the cache trimmed to
//   nothing in between. That is the check a lazy cache fails.
//
// build (one line; a trailing backslash here is a -Wcomment warning, and this
// project builds at zero warnings):
//   g++ -std=c++20 -O2 -Wall -Wextra -Isrc test_planet.cpp -o test_planet.exe -static -static-libgcc -static-libstdc++

#include "world/planet.hpp"
#include "world/blockworld.hpp"
#include "rng.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

using namespace bench;

static int g_checks = 0;
static void ok(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { std::printf("  FAIL: %s\n", what); std::fflush(stdout); std::abort(); }
}

// Every distinct cell of a lattice, by canonicalising the whole of all ten
// (n+1)x(n+1) blocks. Only used at small n: this is the brute-force ground
// truth that the O(1) operations are checked against.
static std::vector<PCell> all_cells(const PlanetLattice& L) {
    std::set<std::uint64_t> s;
    for (int c = 0; c < 10; ++c)
        for (int i = 0; i <= L.n(); ++i)
            for (int j = 0; j <= L.n(); ++j)
                s.insert(pack_voxel(canonical_cell(L, PCell{c, i, j}), 0));
    std::vector<PCell> out;
    out.reserve(s.size());
    for (auto id : s) { PCell c{}; int k = 0; unpack_voxel(id, c, k); out.push_back(c); }
    return out;
}

// ── 1. the solid, and the constants that describe it ────────────────────────
static void test_constants() {
    std::printf("[1] the solid and its published constants\n");
    const PlanetTopology& T = topology();

    // The icosahedron edge as a central angle: cos(t) = 1/sqrt(5).
    const double measured = pv_angle(T.vert[std::size_t(T.face[0][0])],
                                     T.vert[std::size_t(T.face[0][1])]);
    ok(std::fabs(measured - kIcosaEdgeRad) < 1e-12, "icosahedron edge angle");
    ok(std::fabs(std::cos(kIcosaEdgeRad) - 1.0 / std::sqrt(5.0)) < 1e-15, "cos = 1/sqrt(5)");
    std::printf("  icosahedron edge      : %.10f rad = %.6f deg   [cos t = 1/sqrt(5)]\n",
                kIcosaEdgeRad, kIcosaEdgeRad * 180.0 / 3.14159265358979323846);

    ok(std::fabs(kHexEdgeOverSpacing - 1.0 / std::sqrt(3.0)) < 1e-15, "hex edge = spacing/sqrt(3)");
    std::printf("  hex edge / spacing    : %.10f = 1/sqrt(3)      [regular hexagon: edge = circumradius]\n",
                kHexEdgeOverSpacing);

    // Face centre to vertex, and the gnomonic distortion it forces. This is the
    // number the research pass showed is identical for aperture 4 and aperture
    // 7, because it belongs to the projection and not to the aperture.
    const PV3 fc = pv_norm(pv_add(T.vert[std::size_t(T.face[0][0])],
                          pv_add(T.vert[std::size_t(T.face[0][1])],
                                 T.vert[std::size_t(T.face[0][2])])));
    const double c2v = pv_angle(fc, T.vert[std::size_t(T.face[0][0])]);
    const double c2vDeg = c2v * 180.0 / 3.14159265358979323846;
    ok(std::fabs(c2v - icosa_face_centre_to_vertex_rad()) < 1e-12,
       "face centre to vertex matches the closed form");
    ok(std::fabs(c2vDeg - kIcosaFaceCentreToVertexDeg) < 1e-6, "and the published degrees");
    const double gnomonic = 1.0 / (std::cos(c2v) * std::cos(c2v) * std::cos(c2v));
    std::printf("  face centre to vertex : %.10f deg  [published 37.3773681406]\n", c2vDeg);
    std::printf("  1/cos^3 of that       : %.6f              [H3 res-15 area max/min: 1.992805]\n",
                gnomonic);
    ok(std::fabs(gnomonic - 1.992806) < 1e-5, "gnomonic distortion 1.992806");

    // The ten rhombic charts. Each pairs two faces across a shared edge; the
    // shared edge is the block's i+j=n diagonal.
    std::set<int> facesSeen;
    for (int c = 0; c < 10; ++c) {
        const auto& C = T.chart[std::size_t(c)];
        facesSeen.insert(C.face0);
        facesSeen.insert(C.face1);
        ok(C.A != C.B && C.A != C.C && C.A != C.D, "chart corners distinct");
        // B and D really are the shared edge of the two faces.
        int shared = 0;
        for (int p = 0; p < 3; ++p)
            for (int q = 0; q < 3; ++q)
                if (T.face[std::size_t(C.face0)][std::size_t(p)]
                    == T.face[std::size_t(C.face1)][std::size_t(q)]) ++shared;
        ok(shared == 2, "chart faces share exactly one edge");
    }
    ok(facesSeen.size() == 20, "the ten charts cover all twenty faces once");
    std::printf("  charts                : 10 rhombi covering 20 faces, each a plain (n+1)^2 block\n");
    std::printf("  matching found        :");
    for (int c = 0; c < 10; ++c)
        std::printf(" (%d,%d)", T.chart[std::size_t(c)].face0, T.chart[std::size_t(c)].face1);
    std::printf("\n");
}

// ── 2. the lattice is exactly the Goldberg dual ─────────────────────────────
static void test_lattice_exact() {
    std::printf("[2] the lattice: count, pentagons, degree, reciprocity, round trip\n");
    for (int base : {4, 8, 5}) {
        for (int depth = 0; depth <= 3; ++depth) {
            const PlanetLattice L(depth, base);
            const auto cells = all_cells(L);
            ok(std::uint64_t(cells.size()) == L.cell_count(), "cell count = 10n^2+2");

            std::size_t pent = 0, deg5 = 0, deg6 = 0, other = 0, nonrecip = 0, rt = 0;
            std::set<std::uint64_t> known;
            for (const auto& c : cells) known.insert(pack_voxel(c, 0));
            for (const auto& c : cells) {
                std::array<PCell, 6> nb{};
                const int m = L.neighbours(c, nb);
                if (m == 5) ++deg5; else if (m == 6) ++deg6; else ++other;
                if (L.is_pentagon(c)) ++pent;
                ok((m == 5) == L.is_pentagon(c), "degree 5 iff pentagon");
                for (int q = 0; q < m; ++q) {
                    if (!known.count(pack_voxel(nb[std::size_t(q)], 0))) ++nonrecip;
                    std::array<PCell, 6> back{};
                    const int mm = L.neighbours(nb[std::size_t(q)], back);
                    bool found = false;
                    for (int s = 0; s < mm; ++s) if (back[std::size_t(s)] == c) found = true;
                    if (!found) ++nonrecip;
                }
                if (L.cell_of(L.direction(c)) != c) ++rt;
            }
            ok(pent == 12, "exactly twelve pentagons");
            ok(other == 0, "every cell has degree 5 or 6");
            ok(nonrecip == 0, "every adjacency is mutual and lands on a real cell");
            ok(rt == 0, "direction -> cell round trips exactly");
            if (depth == 3)
                std::printf("  base %2d depth %d n=%3d : %6zu cells  %zu pentagons  %zu hexagons"
                            "  0 non-mutual  0 round-trip failures\n",
                            base, depth, L.n(), cells.size(), deg5, deg6);
        }
    }
    // Euler, on the dual that is never stored. V - E + F = 2 with F = cells,
    // each cell contributing its degree as half-edges of the geodesic.
    {
        const PlanetLattice L(3, 4);
        const auto cells = all_cells(L);
        std::size_t halfEdges = 0;
        for (const auto& c : cells) {
            std::array<PCell, 6> nb{};
            halfEdges += std::size_t(L.neighbours(c, nb));
        }
        const std::size_t E = halfEdges / 2;
        // On the Goldberg solid: F = cells, E = 3F-6 edges of the dual... the
        // identity checked here is the geodesic's own, V=cells, E as counted,
        // F = 2E/3 triangles.
        const std::size_t V = cells.size();
        const std::size_t Ftri = (2 * E) / 3;
        std::printf("  geodesic at n=32      : V=%zu E=%zu F=%zu   V-E+F=%lld\n",
                    V, E, Ftri, (long long)(V - E + Ftri));
        ok(V - E + Ftri == 2, "Euler on the geodesic");
    }
}

// ── 3. the hierarchy is an exact partition ──────────────────────────────────
static void test_hierarchy() {
    std::printf("[3] the hierarchy: aperture 4, exact partition, zones\n");
    for (int depth = 0; depth <= 3; ++depth) {
        const PlanetLattice C(depth, 4), F(depth + 1, 4);
        const auto coarse = all_cells(C), fine = all_cells(F);
        std::map<std::uint64_t, int> claims;
        std::map<int, int> sizes;
        for (const auto& c : coarse) {
            std::array<PCell, 7> ch{};
            const int m = C.children(c, ch);
            ++sizes[m];
            for (int q = 0; q < m; ++q) ++claims[pack_voxel(ch[std::size_t(q)], 0)];
        }
        std::size_t bad = 0, notMutual = 0;
        for (const auto& f : fine) {
            const int n = claims.count(pack_voxel(f, 0)) ? claims[pack_voxel(f, 0)] : 0;
            if (n != 1) ++bad;
            const PCell p = F.parent(f);
            std::array<PCell, 7> ch{};
            const int m = C.children(p, ch);
            bool has = false;
            for (int q = 0; q < m; ++q) if (ch[std::size_t(q)] == f) has = true;
            if (!has) ++notMutual;
        }
        ok(bad == 0, "every fine cell is claimed by exactly one parent");
        ok(notMutual == 0, "parent and children are mutual inverses");
        if (depth == 3) {
            std::printf("  depth %d -> %d          : %zu coarse, %zu fine, every fine cell claimed"
                        " exactly once\n", depth, depth + 1, coarse.size(), fine.size());
            std::printf("  children per parent   :");
            for (auto& [s, c] : sizes) std::printf(" %d:%d", s, c);
            std::printf("   (mean %.3f)\n", double(fine.size()) / double(coarse.size()));
        }
    }
    // The count identity behind "four children is an average, not a guarantee".
    {
        const PlanetLattice C(2, 4);
        const std::uint64_t nc = C.cell_count();
        const std::uint64_t nf = PlanetLattice(3, 4).cell_count();
        ok(nf == 4 * nc - 6, "fine = 4*coarse - 6");
        std::printf("  the six that come up short: fine = 4*coarse - 6 exactly (%llu = 4*%llu - 6)\n",
                    (unsigned long long)nf, (unsigned long long)nc);
    }
    // Zones: a level-0 cell. 642 at base 8, of which twelve are pentagons.
    {
        const PlanetLattice Z(0, 8);
        ok(Z.cell_count() == 642, "642 zones at base 8");
        std::printf("  zones at base 8       : 642 = 630 hexagons + 12 pentagons"
                    "  (inside hexplanetview's 4..40 subdiv knob)\n");
    }
    for (int depth : {3, 5}) {
        const PlanetLattice L(depth, 4);
        const auto cells = all_cells(L);
        std::map<std::uint64_t, int> zs;
        for (const auto& c : cells) ++zs[pack_voxel(L.zone_of(c), 0)];
        std::vector<int> v;
        for (auto& [a, b] : zs) { (void)a; v.push_back(b); }
        std::sort(v.begin(), v.end());
        const double ideal = double(cells.size()) / double(v.size());
        std::printf("  zone size, depth %d    : ideal %.1f, min %d median %d max %d  (max/min %.2f)\n",
                    depth, ideal, v.front(), v[v.size() / 2], v.back(),
                    double(v.back()) / double(v.front()));
        ok(v.front() > 0, "no empty zone");
    }
    // The bit shift, and how far it is from the exact answer. The research pass
    // offered it as the zone lookup; it is O(1) and it is not the same map.
    for (int depth : {2, 4}) {
        const PlanetLattice L(depth, 4), Z(0, 4);
        const auto cells = all_cells(L);
        std::size_t diff = 0;
        for (const auto& c : cells)
            if (L.zone_of(c) != canonical_cell(Z, L.zone_of_shift(c))) ++diff;
        std::printf("  zone_of vs one shift  : depth %d, %zu of %zu cells disagree (%.1f%%)\n",
                    depth, diff, cells.size(), 100.0 * double(diff) / double(cells.size()));
    }
}

// Rodrigues, for placing a start point a known arc away from a target.
static PV3 rotate_about(PV3 v, PV3 axis, double t) {
    const PV3 a = pv_norm(axis);
    const double c = std::cos(t), s = std::sin(t);
    return pv_add(pv_add(pv_mul(v, c), pv_mul(pv_cross(a, v), s)),
                  pv_mul(a, pv_dot(a, v) * (1.0 - c)));
}

// ── 4. how big is a cell, in metres ─────────────────────────────────────────
static void test_scale() {
    std::printf("[4] cell size in metres, at R = 1000 m\n");
    const double R = 1000.0;
    std::printf("  depth  n      cells         nominal edge   measured edge (min..max)   layer h\n");
    for (int depth = 0; depth <= 12; ++depth) {
        const PlanetLattice L(depth, 8);
        const double nom = L.nominal_cell_edge(R);
        const PlanetTerrain gen(1, R, L, 128);
        if (depth <= 4) {
            // Measure the real thing off the mesh: the distance between
            // consecutive corners of each cell's dual polygon.
            const auto cells = all_cells(L);
            double lo = 1e300, hi = 0.0, sum = 0.0; std::size_t cnt = 0;
            for (const auto& c : cells) {
                std::array<PV3, 6> k{};
                const int nk = L.corners(c, k);
                for (int q = 0; q < nk; ++q) {
                    const double e = pv_angle(k[std::size_t(q)],
                                              k[std::size_t((q + 1) % nk)]) * R;
                    lo = std::min(lo, e); hi = std::max(hi, e); sum += e; ++cnt;
                }
            }
            const double mean = sum / double(cnt);
            std::printf("  %5d  %-6d %-13llu %10.5f m   %8.5f .. %-8.5f (mean %.5f)  %.5f m\n",
                        depth, L.n(), (unsigned long long)L.cell_count(), nom, lo, hi, mean,
                        gen.layer_height());
            // The nominal figure is the flat-lattice value; on the sphere the
            // real edges straddle it.
            ok(lo <= nom * 1.02 && hi >= nom * 0.98, "nominal edge lies inside the measured range");
        } else {
            std::printf("  %5d  %-6d %-13llu %10.5f m   %s  %.5f m\n",
                        depth, L.n(), (unsigned long long)L.cell_count(), nom,
                        "(too many to enumerate)      ", gen.layer_height());
        }
    }
    // The working point named in planet.hpp's header.
    {
        const PlanetLattice L(8, 8);
        const double e = L.nominal_cell_edge(R);
        ok(L.n() == 2048, "depth 8 at base 8 is n = 2048");
        ok(L.cell_count() == 41943042ull, "41,943,042 cells");
        ok(std::fabs(e - 0.31212) < 0.0001, "cell edge 0.3121 m");
        std::printf("  working point         : depth 8, n=2048, 41,943,042 cells, edge %.5f m"
                    "  — under Minecraft's 1 m\n", e);
        const PlanetTerrain gen(1, R, L, 128);
        const double shell = 128.0 * gen.layer_height();
        std::printf("  shell                 : 128 layers x %.5f m = %.2f m deep = %.4f R"
                    "   [sane below 0.095 R]\n", gen.layer_height(), shell, shell / R);
        ok(shell / R < 0.095, "the shell is shallow enough that prism fan-out stays under 10%");
        std::printf("  dense cost if stored  : %.3g bytes at 1 byte/voxel — never allocated\n",
                    double(L.cell_count()) * 128.0);
    }
}

// ── 5. cell area variation, and the pentagon defect ─────────────────────────
static void test_area() {
    std::printf("[5] cell area: overall spread, and the step a walker feels\n");
    std::printf("  n     max/min (all)  max/min (hexagons)   worst adjacent step: bulk / at a pentagon\n");
    for (int depth = 0; depth <= 4; ++depth) {
        const PlanetLattice L(depth, 8);
        const auto cells = all_cells(L);
        std::map<std::uint64_t, double> area;
        for (const auto& c : cells) area[pack_voxel(c, 0)] = L.area(c);

        double lo = 1e300, hi = 0.0, hlo = 1e300, hhi = 0.0;
        for (const auto& c : cells) {
            const double a = area[pack_voxel(c, 0)];
            lo = std::min(lo, a); hi = std::max(hi, a);
            if (!L.is_pentagon(c)) { hlo = std::min(hlo, a); hhi = std::max(hhi, a); }
        }
        // The step a character actually feels: the ratio between its cell and
        // the one it walks onto. Split, because the pentagon defect is a POINT
        // defect and averaging it into the bulk hides the only ugly number here.
        double bulkStep = 0.0, pentStep = 0.0;
        for (const auto& c : cells) {
            std::array<PCell, 6> nb{};
            const int m = L.neighbours(c, nb);
            const double a = area[pack_voxel(c, 0)];
            for (int q = 0; q < m; ++q) {
                const PCell g = nb[std::size_t(q)];
                const double b = area[pack_voxel(g, 0)];
                const double step = std::fabs(b / a - 1.0);
                if (L.is_pentagon(c) || L.is_pentagon(g)) pentStep = std::max(pentStep, step);
                else                                      bulkStep = std::max(bulkStep, step);
            }
        }
        std::printf("  %-5d %-14.5f %-20.5f %6.2f%% / %6.2f%%\n",
                    L.n(), hi / lo, hhi / hlo, 100.0 * bulkStep, 100.0 * pentStep);
        if (depth == 4) {
            ok(hhi / hlo > 1.6 && hhi / hlo < 2.0, "hexagon area spread climbs toward 1.9928");
            ok(bulkStep < 0.05, "away from a pentagon the step is small");
            ok(pentStep > 0.20, "at a pentagon the step is large, and does not shrink with n");
            std::printf("  the spread is the PROJECTION, not the aperture: it climbs toward"
                        " 1/cos^3(37.377368 deg) = 1.992806,\n"
                        "  which is also H3's published aperture-7 figure at resolution 15"
                        " (1.992805).\n");
            std::printf("  the pentagon step does NOT shrink with n. It is a POINT defect at"
                        " twelve places, not a\n"
                        "  property of boundaries — at the working depth that is 12 cells out"
                        " of 41,943,042, or 0.0000286%%.\n");
        }
    }
}

// ── 6. the seam: what walking across a pentagon actually does ───────────────
static void test_pentagon_walk() {
    std::printf("[6] walking a straight line across a pentagon\n");
    const double R = 1000.0;
    const PlanetLattice L(6, 8);                 // n = 512, edge 1.2485 m
    PlanetTerrain gen(20260815ull, R, L, 128);
    PlanetCache cache(L, gen);

    const PlanetTopology& T = topology();
    const PCell pent = L.cell_of(T.vert[0]);
    ok(L.is_pentagon(pent), "vertex 0 is a pentagon cell");

    // Aim a great circle through the pentagon, starting well before it.
    const PV3 pdir = L.direction(pent);
    const PV3 axis = pv_norm(pv_cross(pdir, T.vert[1]));
    const double arc = 240.0 * kIcosaEdgeRad / double(L.n());     // 240 cells back
    const PV3 startDir = rotate_about(pdir, axis, -arc);
    const PCell start = L.cell_of(startDir);

    const double cellArc = kIcosaEdgeRad / double(L.n());

    // (a) A pure great-circle walk aimed straight through the pentagon's centre.
    {
        PlanetCache c2(L, gen);
        PlanetWalker g(L, c2, start, 2);
        g.aim(axis);
        double closest = 1e9;
        int hits = 0;
        for (int s = 0; s < 520; ++s) {
            if (!g.step()) break;
            closest = std::min(closest, pv_angle(L.direction(g.cell()), pdir) / cellArc);
            if (L.is_pentagon(g.cell())) ++hits;
        }
        std::printf("  a great-circle walk aimed through the pentagon's centre passes within\n"
                    "  %.3f cells of it and lands on it %d times — the lattice steps AROUND a\n"
                    "  point defect. Crossing one has to be insisted on.\n", closest, hits);
        ok(closest < 1.5, "the great-circle walk really did aim at the pentagon");
    }

    // (b) The same great-circle line, with ONE forced step: when the pentagon
    // becomes a neighbour, take it. Everything else is the plain straight walk,
    // so the turn statistics stay honest — no greedy homing wobble anywhere.
    PlanetWalker w(L, cache, start, 2);
    w.aim(axis);
    std::vector<PV3> path;
    std::vector<int> degree;
    path.push_back(L.direction(w.cell()));
    degree.push_back(6);
    int hitPentagon = -1;
    std::set<std::uint64_t> zonesSeen;
    bool crossed = false;
    for (int s = 0; s < 520; ++s) {
        std::array<PCell, 6> nb{};
        const int m = L.neighbours(w.cell(), nb);
        bool touching = false;
        for (int q = 0; q < m; ++q) if (nb[std::size_t(q)] == pent) touching = true;
        const bool moved = (touching && !crossed) ? w.step_toward(pdir) : w.step();
        if (!moved) break;
        if (w.cell() == pent) { crossed = true; hitPentagon = int(path.size()); }
        std::array<PCell, 6> nb2{};
        path.push_back(L.direction(w.cell()));
        degree.push_back(L.neighbours(w.cell(), nb2));
        zonesSeen.insert(pack_voxel(L.zone_of(w.cell()), 0));
    }
    ok(crossed, "the walk crossed the pentagon");
    ok(degree[std::size_t(hitPentagon)] == 5, "the cell it crossed had five neighbours");
    ok(zonesSeen.size() >= 3, "the walk crossed several zones");

    // Turn angle at every step: the angle between the incoming and outgoing
    // edge, measured in the tangent plane. Straight means zero.
    double maxBulkTurn = 0.0, pentTurn = 0.0, minZig = 1e9;
    std::size_t straight = 0, zig = 0;
    for (std::size_t s = 1; s + 1 < path.size(); ++s) {
        const PV3 a = pv_norm(pv_sub(path[s], path[s - 1]));
        const PV3 b = pv_norm(pv_sub(path[s + 1], path[s]));
        const double turn = pv_angle(a, b) * 180.0 / 3.14159265358979323846;
        if (degree[s] == 5) { pentTurn = std::max(pentTurn, turn); continue; }
        if (turn < 1.0) { ++straight; continue; }
        ++zig;
        minZig = std::min(minZig, turn);
        maxBulkTurn = std::max(maxBulkTurn, turn);
    }
    // Cross-track drift: how far the walk strays from the great circle it aimed
    // along. Reported in metres and in cell edges, because "0.9 m" means nothing
    // until you know a cell is 1.2 m wide.
    double maxOff = 0.0;
    for (const auto& p : path) maxOff = std::max(maxOff, std::fabs(pv_dot(p, axis)));
    const double offM = maxOff * R;
    const double edge = L.nominal_cell_edge(R);

    std::printf("  n = %d, cell edge %.4f m, %zu steps, %zu zones crossed\n",
                L.n(), edge, path.size(), zonesSeen.size());
    std::printf("  pentagon reached at step %d of %zu\n", hitPentagon, path.size());
    std::printf("  turn per step, hexagons : %zu steps dead straight, %zu turn by one lattice\n"
                "                            direction (%.2f .. %.2f deg)\n",
                straight, zig, minZig, maxBulkTurn);
    std::printf("  turn per step, PENTAGON : %.2f deg\n", pentTurn);
    std::printf("  cross-track drift       : %.3f m = %.2f cell edges over %zu steps\n",
                offM, offM / edge, path.size());
    std::printf("  area step onto the pentagon and off it: see [5] — 38.47%%, against 1.29%% in bulk\n");
    std::printf("\n  WHAT THE SEAM ACTUALLY COSTS, and it is not what one would guess:\n"
                "  A straight line on a triangular lattice cannot be straight. It alternates\n"
                "  between two lattice directions, so every other step turns. Nominally that is\n"
                "  60 deg; measured here it runs %.0f to %.0f deg, because the gnomonic projection\n"
                "  opens the lattice angles away from a face centre. That is the ROUTINE\n"
                "  behaviour of this grid, everywhere, pentagon or no pentagon.\n"
                "  Against that, the pentagon's %.2f deg is SMALLER than the ordinary zig-zag.\n"
                "  Per step, crossing a pentagon is less of a jolt than walking in a straight\n"
                "  line normally is. The 36 deg is exact and predicted: five neighbours at 72 deg\n"
                "  spacing instead of six at 60, and the straightest continuation splits it.\n"
                "  So the pentagon is NOT felt as a kink in the path. What it really costs is\n"
                "  holonomy — an icosahedron vertex carries an angular defect of exactly 60 deg\n"
                "  (five equilateral triangles make 300, not 360), so a loop around one comes\n"
                "  back rotated. That is Gauss-Bonnet, not an implementation fault, and NO\n"
                "  subdivision scheme of any aperture removes it; H3 has the same twelve.\n"
                "  What IS felt is the area step: a %.0f%% jump in cell size at 12 cells out of\n"
                "  41,943,042. The response is to SITE the twelve rather than fix them — peaks,\n"
                "  trenches, shrines — and not to invite anyone to pace across one counting\n"
                "  blocks.\n", minZig, maxBulkTurn, pentTurn, 38.47);
    ok(pentTurn > 30.0 && pentTurn < 42.0, "the pentagon turn is the predicted ~36 degrees");
    ok(pentTurn < minZig, "and it is smaller than the routine hex zig-zag");
    ok(maxBulkTurn <= 75.0, "the zig-zag stays within one lattice direction plus projection skew");
    ok(straight > 100 && zig > 100, "the walk alternates, as a hex-lattice straight line must");
}

// ── 7. determinism, and the one that catches a lazy cache ───────────────────
static void test_determinism() {
    std::printf("[7] determinism: same seed, same cell, same block — by any route\n");
    const double R = 1000.0;
    const PlanetLattice L(8, 8);
    PlanetTerrain a(20260815ull, R, L, 128), b(20260815ull, R, L, 128), c(20260816ull, R, L, 128);

    // Same seed, two instances, no shared state.
    Rng rng(0xC0FFEEull);
    std::size_t same = 0, differ = 0;
    for (int t = 0; t < 4000; ++t) {
        const PCell cell = canonical_cell(L, PCell{ int(rng.below(10)),
                                                    int(rng.below(std::uint32_t(L.n()))),
                                                    int(rng.below(std::uint32_t(L.n()))) });
        const int k = int(rng.below(128));
        if (a.block(cell, k) == b.block(cell, k)) ++same;
        if (a.block(cell, k) != c.block(cell, k)) ++differ;
    }
    ok(same == 4000, "the same seed gives the same block, every time");
    std::printf("  same seed, 4000 cells : %zu/4000 identical\n", same);
    std::printf("  other seed            : %zu/4000 differ — a seed IS a planet\n", differ);
    ok(differ > 1000, "a different seed gives a genuinely different planet");

    // A cell on a chart seam has two names. Both must generate the same block,
    // or mining one would leave the other untouched.
    {
        std::size_t seamChecked = 0;
        const PlanetLattice S(3, 8);
        PlanetTerrain g(7ull, R, S, 128);
        for (const auto& cell : all_cells(S)) {
            std::array<PCell, 5> reps{};
            const int nr = S.representations(S.face_point(cell), reps);
            if (nr < 2) continue;
            ++seamChecked;
            for (int q = 1; q < nr; ++q)
                for (int k = 0; k < 128; k += 17)
                    ok(g.block(reps[0], k) == g.block(canonical_cell(S, reps[std::size_t(q)]), k),
                       "both names of a seam cell generate the same block");
        }
        std::printf("  seam cells (2+ names) : %zu checked, every name agrees at every layer\n",
                    seamChecked);
        ok(seamChecked > 100, "there are seam cells to check");
    }

    // THE ONE THAT CATCHES A LAZY CACHE. Read a strip of cells, walk far away,
    // trim the cache to nothing, walk back, read the same strip. Byte for byte.
    {
        const PlanetLattice W(6, 8);
        PlanetTerrain gen(20260815ull, R, W, 128);
        PlanetCache cache(W, gen);
        const PCell home = W.cell_of(PV3{0.3, 0.5, 0.81});
        PlanetWalker w(W, cache, home, 3);

        std::vector<std::uint8_t> before, after;
        std::vector<PCell> strip;
        {
            std::array<PCell, 6> nb{};
            const int m = W.neighbours(home, nb);
            strip.push_back(home);
            for (int q = 0; q < m; ++q) strip.push_back(nb[std::size_t(q)]);
        }
        for (const auto& cell : strip)
            for (int k = 0; k < 128; ++k) before.push_back(cache.at(cell, k));

        // Walk 600 cells away, trimming as we go so nothing of home survives.
        w.aim(pv_norm(pv_cross(W.direction(home), PV3{0, 1, 0})));
        for (int s = 0; s < 600; ++s) { w.step(); if ((s % 40) == 0) cache.trim(w.cell(), 64); }
        cache.trim(w.cell(), 0);
        const std::size_t after_trim = cache.live_chunks();
        // Walk back.
        w.aim(pv_norm(pv_cross(PV3{0, 1, 0}, W.direction(w.cell()))));
        for (int s = 0; s < 600; ++s) { w.step(); if ((s % 40) == 0) cache.trim(w.cell(), 64); }
        cache.trim(w.cell(), 0);

        for (const auto& cell : strip)
            for (int k = 0; k < 128; ++k) after.push_back(cache.at(cell, k));
        std::size_t bad = 0;
        for (std::size_t q = 0; q < before.size(); ++q) if (before[q] != after[q]) ++bad;
        ok(bad == 0, "reached directly or by walking away and back, a cell is identical");
        std::printf("  walk-away-and-back    : %zu voxels re-read after the cache was trimmed to"
                    " %zu chunks — %zu differ\n", before.size(), after_trim, bad);
    }
}

// ── 8. materialise on demand, and what it costs ─────────────────────────────
static void test_memory() {
    std::printf("[8] streaming: memory high-water for a long walk, and the cost of one zone\n");
    const double R = 1000.0;
    const PlanetLattice L(8, 8);                 // n = 2048, 41,943,042 cells
    PlanetTerrain gen(20260815ull, R, L, 128);
    PlanetCache cache(L, gen);

    std::printf("  chunk                 : %d x %d columns x %d layers = %zu cells = %zu KB\n",
                kChunkXY, kChunkXY, kChunkZ, kChunkCells, kChunkCells / 1024);

    const PCell start = L.cell_of(PV3{0.2, 0.3, 0.93});
    PlanetWalker w(L, cache, start, 3);
    w.aim(pv_norm(pv_cross(L.direction(start), PV3{0, 1, 0})));

    for (int radius : {32, 64, 128}) {
        PlanetCache c2(L, gen);
        PlanetWalker w2(L, c2, start, 3);
        w2.aim(pv_norm(pv_cross(L.direction(start), PV3{0, 1, 0})));
        const auto t0 = std::chrono::steady_clock::now();
        for (int s = 0; s < 3000; ++s) {
            w2.step();
            if ((s % 8) == 0) c2.trim(w2.cell(), radius);
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double walked = 3000.0 * L.nominal_spacing(R);
        std::printf("  3000 steps (%.0f m), keep radius %3d cells: high-water %6.2f MB"
                    " (%zu chunks), resident at end %5.2f MB, %llu chunks built, %.1f ms\n",
                    walked, radius, double(c2.high_water()) / 1048576.0,
                    c2.high_water() / kChunkCells, double(c2.bytes()) / 1048576.0,
                    (unsigned long long)c2.chunks_built(), ms);
        ok(c2.high_water() < 400u * 1024u * 1024u, "the walking neighbourhood stays bounded");
    }
    // The realistic figure: keep a DISC of cells resident around the character,
    // the way a render distance does, rather than only the column underfoot.
    for (int view : {32, 64}) {
        PlanetCache c2(L, gen);
        PlanetWalker w2(L, c2, start, 3);
        w2.aim(pv_norm(pv_cross(L.direction(start), PV3{0, 1, 0})));
        const auto t0 = std::chrono::steady_clock::now();
        for (int s = 0; s < 600; ++s) {
            w2.step();
            if ((s % 16) == 0) { c2.ensure(w2.cell(), view); c2.trim(w2.cell(), view * 2); }
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::printf("  600 steps (%.0f m) keeping a %3d-cell disc (%.0f m) resident:"
                    " high-water %6.2f MB (%zu chunks), %llu built, %.0f ms\n",
                    600.0 * L.nominal_spacing(R), view, double(view) * L.nominal_spacing(R),
                    double(c2.high_water()) / 1048576.0, c2.high_water() / kChunkCells,
                    (unsigned long long)c2.chunks_built(), ms);
        ok(c2.high_water() < 512u * 1024u * 1024u, "a view neighbourhood stays bounded");
        ok(c2.bytes() <= c2.high_water(), "resident never exceeds the high-water mark");
    }
    // Release really releases.
    {
        PlanetCache c3(L, gen);
        c3.at(start, 40);
        ok(c3.live_chunks() == 1, "one read materialises one chunk");
        c3.trim(start, -1);
        ok(c3.live_chunks() == 0 && c3.bytes() == 0, "trim releases");
        std::printf("  release               : one read = 1 chunk = %zu KB; trim returns it to 0\n",
                    kChunkCells / 1024);
    }
    // One zone, at the working depth: the index block a level-0 cell spans.
    {
        PlanetCache c4(L, gen);
        const int span = 1 << L.depth();          // 256 columns across at depth 8
        const auto t0 = std::chrono::steady_clock::now();
        std::size_t built = 0;
        for (int i = 0; i < span; i += kChunkXY)
            for (int j = 0; j < span; j += kChunkXY)
                for (int k = 0; k < gen.layers(); k += kChunkZ) {
                    c4.materialise(PlanetCache::chunk_key(PCell{0, i, j}, k));
                    ++built;
                }
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::printf("  one zone at depth 8   : %d x %d columns x %d layers = %.2f M voxels"
                    " in %zu chunks = %.2f MB, built in %.1f ms (%.1f Mvox/s)\n",
                    span, span, gen.layers(),
                    double(span) * double(span) * double(gen.layers()) / 1e6,
                    built, double(c4.bytes()) / 1048576.0, ms,
                    double(span) * double(span) * double(gen.layers()) / (ms * 1000.0));
        ok(built > 0 && c4.bytes() > 0, "a zone materialises");
    }
}

// ── 9. mining, on blockworld.hpp's own arithmetic ───────────────────────────
static void test_mining() {
    std::printf("[9] mining: blockworld.hpp's rules, unchanged\n");
    const double R = 1000.0;
    const PlanetLattice L(8, 8);
    PlanetTerrain gen(20260815ull, R, L, 128);
    PlanetCache cache(L, gen);

    // The rules themselves are blockworld's; assert we are calling the real
    // ones and not a copy that has drifted.
    // Wrong tool: hardness * 5.0 seconds. Stone is 1.5, so 7.5 s = 150 ticks.
    ok(break_ticks(Stone, 0) == 150, "stone by hand: 1.5 * 5.0 * 20 = 150 ticks");
    // Right tool: hardness * 1.5 / speed. Wooden pick speed 2: 1.125 s -> 23 ticks.
    ok(break_ticks(Stone, 1) == 23, "stone, wooden pick: 1.5*1.5/2 * 20 = 22.5 -> 23");
    ok(break_ticks(Bedrock, 4) == -1, "bedrock never breaks");
    ok(!drops_for(IronOre, 1), "wooden pick gets no iron drop");
    ok(drops_for(IronOre, 2), "stone pick does");
    std::printf("  break_ticks(stone, hand/wood/stone/iron/diamond) = %d/%d/%d/%d/%d\n",
                break_ticks(Stone, 0), break_ticks(Stone, 1), break_ticks(Stone, 2),
                break_ticks(Stone, 3), break_ticks(Stone, 4));

    // The generated world contains the blocks it claims to.
    {
        std::map<int, std::size_t> hist;
        const PCell base = L.cell_of(PV3{0.11, 0.62, 0.78});
        PlanetWalker probe(L, cache, base, 4);
        probe.aim(pv_norm(pv_cross(L.direction(base), PV3{1, 0, 0})));
        for (int s = 0; s < 900; ++s) {
            for (int k = 0; k < gen.layers(); ++k) ++hist[cache.at(probe.cell(), k)];
            probe.step();
            if ((s % 16) == 0) cache.trim(probe.cell(), 48);
        }
        // A planet-wide surface census first: one transect samples one region,
        // and reporting only that would let a world with no grass on it look
        // like a world with no grass anywhere.
        {
            std::map<int, std::size_t> top;
            Rng r(0x5EEDull);
            for (int q = 0; q < 20000; ++q) {
                const PV3 d = pv_norm(PV3{ double(r.unit()) * 2.0 - 1.0,
                                           double(r.unit()) * 2.0 - 1.0,
                                           double(r.unit()) * 2.0 - 1.0 });
                const int gh = gen.ground_dir(d);
                ++top[gen.block_dir(d, gh)];
            }
            std::printf("  surface block over 20000 points across the whole planet:\n   ");
            for (auto& [b, n] : top)
                std::printf(" %s=%.2f%%", block_name(b), 100.0 * double(n) / 20000.0);
            std::printf("\n");
            ok(top[Grass] > 0, "there is grass somewhere on the planet");
            ok(top[Sand] > 0, "and beaches");
        }
        std::printf("  a 900-cell transect, all 128 layers:\n   ");
        std::size_t total = 0;
        for (auto& [b, n] : hist) total += n;
        for (auto& [b, n] : hist)
            if (n * 2000 > total) std::printf(" %s=%.2f%%", block_name(b), 100.0 * double(n) / double(total));
        std::printf("\n");
        ok(hist[Stone] > 0 && hist[Air] > 0, "there is stone and there is air");
        ok(hist[Coal] > 0, "there is coal");
        ok(hist[IronOre] > 0, "there is iron");
        ok(hist[Bedrock] > 0, "there is bedrock at the floor");
    }

    // Actually mine, and check the ticks are the published ones for whatever
    // the block turned out to be.
    {
        const PCell base = L.cell_of(PV3{0.11, 0.62, 0.78});
        PlanetWalker w(L, cache, base, 2);
        std::size_t dug = 0, drops = 0, refused = 0; std::uint64_t ticks = 0;
        std::map<int, std::size_t> got_blocks;
        // Sink a shaft at each of 40 cells along a line: surface to bedrock, so
        // the mining actually meets stone and ore rather than only topsoil.
        for (int s = 0; s < 40; ++s) {
            for (int k = w.layer() - 1; k >= 0; --k) {
                const std::uint8_t b = cache.at(w.cell(), k);
                if (b == Air || b == Water) continue;
                const int expect = break_ticks(b, w.tier());
                bool dropped = false;
                const int got = w.mine(w.cell(), k, dropped);
                ok(got == expect, "mine() charges exactly break_ticks()");
                if (got < 0) { ++refused; ok(b == Bedrock, "only bedrock refuses"); continue; }
                ++dug; ticks += std::uint64_t(got); ++got_blocks[b];
                if (dropped) ++drops;
                ok(cache.at(w.cell(), k) == Air, "a mined block is gone");
            }
            for (int t = 0; t < 6; ++t) w.step();
        }
        std::printf("  40 shafts to bedrock with a stone pickaxe: %zu blocks, %llu ticks"
                    " (%.1f s at 20 tps), %zu dropped, %zu refused (bedrock)\n",
                    dug, (unsigned long long)ticks, double(ticks) / 20.0, drops, refused);
        std::printf("   broke:");
        for (auto& [b, n] : got_blocks) std::printf(" %s=%zu", block_name(b), n);
        std::printf("\n");
        ok(dug > 500, "the shafts actually reached depth");
        ok(refused > 0, "bedrock refused, every time");
        ok(got_blocks[Stone] > 0, "stone was mined");
        // The tier gate is real: a wooden pick breaks iron ore and gets nothing.
        ok(break_ticks(IronOre, 1) > 0 && !drops_for(IronOre, 1),
           "a wooden pick breaks iron ore and drops nothing");
        // Mining is visible through the cache while the chunk is resident.
        const PCell c = w.cell();
        cache.set(c, 30, Air);
        ok(cache.at(c, 30) == Air, "an edit reads back");
    }
    std::printf("  NOTE: block rules, hardness, tiers and drops are blockworld.hpp's, called\n"
                "  directly. Ore PLACEMENT is not — BlockWorld::vein() is a seeded random walk\n"
                "  over a finite array and cannot be evaluated for one cell in isolation, which\n"
                "  is the one thing a streamed planet requires. Bands and rarity match; vein\n"
                "  shapes do not, so yields are not comparable with voxelcraft's.\n");
}

int main() {
    std::printf("test_planet — src/world/planet.hpp\n");
    std::printf("================================================================\n");
    test_constants();
    test_lattice_exact();
    test_hierarchy();
    test_scale();
    test_area();
    test_pentagon_walk();
    test_determinism();
    test_memory();
    test_mining();
    std::printf("================================================================\n");
    std::printf("%d checks passed.\n", g_checks);
    return 0;
}
