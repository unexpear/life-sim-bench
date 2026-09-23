// planet.hpp — a walkable planet at sub-metre voxel scale, addressed by
// recursive subdivision, materialised on demand.
//
// ── the one idea ────────────────────────────────────────────────────────────
//
// DO NOT STORE HEXAGONS. Store the geodesic VERTEX LATTICE. Every geodesic
// vertex is one cell; its hexagon (or pentagon) is the DUAL and is computed only
// when something wants to draw it. Nothing here ever subdivides a hexagon, so it
// never matters that hexagons do not nest.
//
// That single move dissolves the problem the research pass was sent to
// investigate. Regular hexagons are irrep-infinity — Wikipedia's rep-tile
// article: among regular polygons only the triangle and the square dissect into
// equal copies of themselves — and H3's own docs concede that "hexagons do not
// cleanly subdivide into seven finer hexagons", approximating it by rotating
// every resolution ~19.1 degrees against the last. That rotation is exactly
// atan(sqrt(3)/5) = 19.106605 deg, the angle of the index-7 sublattice vector
// 2a1+a2, |2a1+a2| = sqrt(7). H3 pays for it with chirality: Class II / Class III
// alternation and per-level orientation state.
//
// Here the recursion is APERTURE 4 on the triangle lattice: GP(m,0) -> GP(2m,0),
// T x4, Class I to Class I, no rotation, no chirality. The dual of the triangle
// lattice at level r IS the Goldberg solid GP(n0*2^r, 0), so the hexagons are
// still there, free, and nothing stores them.
//
// WHAT THIS COSTS, stated up front because the research pass was explicit that
// the usual reason given for aperture 4 is wrong. Aperture 4 does NOT buy back
// uniform cell area. Area spread on an icosahedral grid is a property of the
// GNOMONIC PROJECTION, not of the aperture: the icosahedron's face-centre-to-
// vertex angle is 37.377368 deg and 1/cos^3(37.377368 deg) = 1.992806, which is
// H3's own published resolution-15 max/min hexagon area ratio of 1.992805 to six
// figures. Both apertures land on the same number. What aperture 4 actually buys
// is the addressing: parent is a bit shift, children are four shifts, and there
// is no rotation to track. Aperture 7's genuine advantage — 92.84% of a parent's
// area covered by its children, against 62.51% for aperture 4 — is a geometric
// containment property this file does not need, because a coarse cell's
// footprint is DEFINED as the union of its children.
//
// ── the layout ──────────────────────────────────────────────────────────────
//
// 20 icosahedron faces, paired across shared edges into 10 RHOMBIC CHARTS. A
// chart is a plain (N+1)x(N+1) integer block: chunking, iteration and
// serialisation are all Minecraft-shaped. The diagonal i+j=N of the block is the
// icosahedron edge the two faces share, and the axial 6-neighbour rule is
// uniform across it — proved in the test, not asserted here.
//
// A lattice point on a chart boundary has more than one (chart,i,j) name. The
// canonical name is the lexicographically smallest, which is a total rule that
// needs no lookup table and no pole special case, and which makes the cell count
// come out at exactly 10N^2+2 by itself.
//
// The research pass recommended instead a HALF-OPEN [0,N)x[0,N) block per chart
// plus two singleton poles, which would make every chart exactly N^2 cells. That
// layout is IMPOSSIBLE and the file says so rather than quietly doing something
// else: half-open ownership means a chart owns exactly the two boundary edges
// incident to its (0,0) corner, so covering all 20 boundary icosahedron edges
// once each would need every icosahedron vertex to carry an ODD number of
// rhombus diagonals. A vertex has 5 faces in a cycle, so at most two consecutive
// pairs can be matched and the diagonal count at a vertex is 0, 1 or 2 — for all
// twelve to be odd every one must be exactly 1, summing to 12, when the ten
// diagonals have 20 endpoints and force a sum of 20. 12 != 20, so no perfect
// matching of any kind admits the half-open rule. Lexicographic ownership costs
// a comparison over at most five representations and has no such obstruction.
//
// ── scale ───────────────────────────────────────────────────────────────────
//
// Cell edge at level r: the icosahedron edge subtends 63.434949 deg =
// 1.1071487178 rad (the dihedral-complement angle acos(1/sqrt(5))), so the
// geodesic edge is L = 1.1071487178*R/N and the dual hexagon's edge is L/sqrt(3)
// = 0.6392205...*R/N, because a regular hexagon's edge equals its circumradius
// and the dual hexagon's circumradius is the triangle lattice spacing over
// sqrt(3). At R = 1000 m and level 8 (N = 2048) that is 0.3121 m — under
// Minecraft's 1 m — with 10*2048^2+2 = 41,943,042 surface cells.
//
// Nothing that size is stored. Terrain is a PURE FUNCTION of (seed, cell, layer)
// and chunks are materialised around whoever is looking. The research pass put
// an Earth at ~1e16 surface columns and 3.66e5 TiB dense at one byte a voxel,
// and — the constraint that actually binds — showed that even the smallest
// planet with a playable 128-layer shell already exceeds a dense budget, because
// a radial prism shell of depth D at radius R only stays under 10% linear
// fan-out when D <= 0.095 R.
//
// ── what is reused ──────────────────────────────────────────────────────────
//
// Blocks, hardness, tool tiers, break times and drop rules all come from
// blockworld.hpp unchanged: Block, block_rule, break_ticks, drops_for,
// block_solid, tool_speed. This file invents no mining rules.
//
// It does NOT reuse BlockWorld's ore placement, and the reason is a hard
// constraint rather than taste: BlockWorld::vein() is a random walk seeded once
// and run across a finite array, which is not a function of a cell and cannot be
// evaluated for one cell in isolation. Ore here is a threshold on a positional
// noise field. Same blocks, same depth bands, same rules — different placement,
// so vein SHAPES will not match voxelcraft's and any yield comparison between
// the two is a comparison of two generators.

#pragma once
#include "blockworld.hpp"
#include "../rng.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace bench {

// ── vectors, in double ──────────────────────────────────────────────────────
//
// Double, not float. At level 12 the lattice has N = 32768 steps along an
// icosahedron edge and the inverse map has to land on an exact integer; float
// carries 24 bits of mantissa and the barycentric solve would be resolving one
// part in 3.3e4 out of a quantity near 1, which leaves under three decimal
// digits of margin. Double leaves eleven.
struct PV3 {
    double x = 0.0, y = 0.0, z = 0.0;
};
[[nodiscard]] inline PV3 pv_add(PV3 a, PV3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
[[nodiscard]] inline PV3 pv_sub(PV3 a, PV3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
[[nodiscard]] inline PV3 pv_mul(PV3 a, double s) { return {a.x*s, a.y*s, a.z*s}; }
[[nodiscard]] inline double pv_dot(PV3 a, PV3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
[[nodiscard]] inline PV3 pv_cross(PV3 a, PV3 b) {
    return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
[[nodiscard]] inline double pv_len(PV3 a) { return std::sqrt(pv_dot(a, a)); }
[[nodiscard]] inline PV3 pv_norm(PV3 a) {
    const double m = pv_len(a);
    return m > 0.0 ? PV3{a.x/m, a.y/m, a.z/m} : a;
}
// Great-circle angle. acos loses precision for small angles, which is the only
// regime that matters here — adjacent cells at level 12 are 6e-4 rad apart — so
// this uses the atan2 form, which is conditioned well everywhere.
[[nodiscard]] inline double pv_angle(PV3 a, PV3 b) {
    return std::atan2(pv_len(pv_cross(a, b)), pv_dot(a, b));
}

// ── published constants, each with the arithmetic that produces it ──────────

// The icosahedron edge, as a central angle. Two adjacent vertices of a regular
// icosahedron satisfy cos(t) = 1/sqrt(5), so t = 63.4349488229 deg.
inline constexpr double kIcosaEdgeRad = 1.10714871779409050301706546017855;
// Hexagon edge over triangle-lattice spacing. A regular hexagon's edge equals
// its circumradius, and the dual hexagon's circumradius is spacing/sqrt(3).
inline constexpr double kHexEdgeOverSpacing = 0.57735026918962576450914878050196;  // 1/sqrt(3)
// Icosahedron face centre to its vertices, as a central angle. Derived rather
// than transcribed: a face centre is the normalised sum of its three vertices,
// so cos(t) = (1 + 2cos(e)) / sqrt(3 + 6cos(e)) with cos(e) = 1/sqrt(5). The
// literal it replaced was right to six figures and wrong at the seventh, which
// is exactly the kind of decay a derived constant cannot suffer.
//
// The gnomonic area distortion across that span is 1/cos^3 of it = 1.992806 —
// the asymptotic area spread of this grid, and of H3's aperture-7 grid alike.
[[nodiscard]] inline double icosa_face_centre_to_vertex_rad() {
    const double c = 1.0 / std::sqrt(5.0);
    return std::acos((1.0 + 2.0 * c) / std::sqrt(3.0 + 6.0 * c));
}
// The published figure, for citation: 37.3773681406 degrees.
inline constexpr double kIcosaFaceCentreToVertexDeg = 37.3773681406;

// ── a cell name ─────────────────────────────────────────────────────────────
//
// (chart, i, j) on the surface; k is the radial layer. chart is 0..9, i and j
// are 0..N inclusive — inclusive because a chart boundary point is shared, and
// which chart carries it is decided by the lexicographic rule rather than by
// trimming the block.
struct PCell {
    int chart = 0;
    int i = 0;
    int j = 0;

    [[nodiscard]] bool operator==(const PCell& o) const {
        return chart == o.chart && i == o.i && j == o.j;
    }
    [[nodiscard]] bool operator!=(const PCell& o) const { return !(*this == o); }
    // Lexicographic, which is also the canonicalisation rule itself.
    [[nodiscard]] bool operator<(const PCell& o) const {
        if (chart != o.chart) return chart < o.chart;
        if (i != o.i) return i < o.i;
        return j < o.j;
    }
};

// Packed 64-bit voxel id: chart 4 | i 24 | j 24 | k 12.
//
// 24 bits of i and j reach N = 16,777,215, which at R = 1000 m is a cell edge of
// 3.81e-5 m. The packing is never the limit on how fine this can go; memory is.
[[nodiscard]] inline std::uint64_t pack_voxel(PCell c, int k) {
    return (std::uint64_t(c.chart & 0xF) << 60)
         | (std::uint64_t(std::uint32_t(c.i) & 0xFFFFFFu) << 36)
         | (std::uint64_t(std::uint32_t(c.j) & 0xFFFFFFu) << 12)
         | (std::uint64_t(std::uint32_t(k) & 0xFFFu));
}
inline void unpack_voxel(std::uint64_t id, PCell& c, int& k) {
    c.chart = int((id >> 60) & 0xF);
    c.i     = int((id >> 36) & 0xFFFFFFu);
    c.j     = int((id >> 12) & 0xFFFFFFu);
    k       = int(id & 0xFFFu);
}

// ── the fixed topology: icosahedron, faces, charts ──────────────────────────
//
// Level-independent. Built once; every depth shares it.
struct PlanetTopology {
    std::array<PV3, 12> vert{};
    std::array<std::array<int, 3>, 20> face{};

    // acrossFace[f][m] — the face sharing the edge OPPOSITE vertex m of face f.
    // m is the local index 0..2, so the shared edge is the other two vertices.
    std::array<std::array<int, 3>, 20> acrossFace{};

    // The five faces meeting at each icosahedron vertex.
    std::array<std::array<int, 5>, 12> vertFaces{};

    // A chart is two faces sharing an edge. Corners in cyclic order:
    // A = (0,0), B = (N,0), C = (N,N), D = (0,N). A and C are the apexes;
    // B and D are the shared edge, so the block's diagonal i+j=N IS that edge
    // and each half of the block is one real icosahedron face.
    struct Chart {
        int face0 = 0;   // the triangle (A,B,D), the i+j <= N half
        int face1 = 0;   // the triangle (C,D,B), the i+j >= N half
        int A = 0, B = 0, C = 0, D = 0;
    };
    std::array<Chart, 10> chart{};
    std::array<int, 20> chartOfFace{};

    // Inverse of the matrix [V0 V1 V2] for each face, so a direction inverts to
    // barycentric coordinates in closed form. This is what replaces
    // hexplanet.hpp's nearest_face(), an O(F) dot-product scan over every face —
    // at 41,943,042 cells that scan is not slow, it is unusable.
    std::array<std::array<double, 9>, 20> faceInv{};
};

namespace planet_detail {

// The icosahedron from the golden ratio — the same vertex and face lists
// hexplanet.hpp uses, so the two files describe the same solid and a
// measurement taken on one is a statement about the other.
inline void icosahedron(std::array<PV3, 12>& v, std::array<std::array<int, 3>, 20>& f) {
    const double t = (1.0 + std::sqrt(5.0)) * 0.5;
    const std::array<PV3, 12> raw = {
        PV3{-1, t, 0}, PV3{ 1, t, 0}, PV3{-1,-t, 0}, PV3{ 1,-t, 0},
        PV3{ 0,-1, t}, PV3{ 0, 1, t}, PV3{ 0,-1,-t}, PV3{ 0, 1,-t},
        PV3{ t, 0,-1}, PV3{ t, 0, 1}, PV3{-t, 0,-1}, PV3{-t, 0, 1} };
    for (std::size_t i = 0; i < 12; ++i) v[i] = pv_norm(raw[i]);
    f = std::array<std::array<int, 3>, 20>{
        std::array<int,3>{0,11,5}, {0,5,1}, {0,1,7}, {0,7,10}, {0,10,11},
        {1,5,9}, {5,11,4}, {11,10,2}, {10,7,6}, {7,1,8},
        {3,9,4}, {3,4,2}, {3,2,6}, {3,6,8}, {3,8,9},
        {4,9,5}, {2,4,11}, {6,2,10}, {8,6,7}, {9,8,1} };
}

// 3x3 inverse by cofactors. The matrix is three unit vectors that are never
// coplanar (they are the corners of a real face), so it is always invertible.
inline void invert3(const PV3& a, const PV3& b, const PV3& c, std::array<double, 9>& out) {
    const double m[9] = { a.x, b.x, c.x,
                          a.y, b.y, c.y,
                          a.z, b.z, c.z };
    const double d = m[0]*(m[4]*m[8]-m[5]*m[7])
                   - m[1]*(m[3]*m[8]-m[5]*m[6])
                   + m[2]*(m[3]*m[7]-m[4]*m[6]);
    const double s = 1.0 / d;
    out[0] = (m[4]*m[8]-m[5]*m[7])*s;  out[1] = (m[2]*m[7]-m[1]*m[8])*s;  out[2] = (m[1]*m[5]-m[2]*m[4])*s;
    out[3] = (m[5]*m[6]-m[3]*m[8])*s;  out[4] = (m[0]*m[8]-m[2]*m[6])*s;  out[5] = (m[2]*m[3]-m[0]*m[5])*s;
    out[6] = (m[3]*m[7]-m[4]*m[6])*s;  out[7] = (m[1]*m[6]-m[0]*m[7])*s;  out[8] = (m[0]*m[4]-m[1]*m[3])*s;
}

} // namespace planet_detail

// Build the topology. Deterministic, no randomness, no clock.
[[nodiscard]] inline PlanetTopology build_topology() {
    using namespace planet_detail;
    PlanetTopology T;
    icosahedron(T.vert, T.face);

    // Faces across each edge: the face sharing the two vertices other than m.
    for (int f = 0; f < 20; ++f)
        for (int m = 0; m < 3; ++m) {
            const int u = T.face[std::size_t(f)][std::size_t((m + 1) % 3)];
            const int w = T.face[std::size_t(f)][std::size_t((m + 2) % 3)];
            int found = -1;
            for (int g = 0; g < 20 && found < 0; ++g) {
                if (g == f) continue;
                bool hu = false, hw = false;
                for (int q = 0; q < 3; ++q) {
                    if (T.face[std::size_t(g)][std::size_t(q)] == u) hu = true;
                    if (T.face[std::size_t(g)][std::size_t(q)] == w) hw = true;
                }
                if (hu && hw) found = g;
            }
            T.acrossFace[std::size_t(f)][std::size_t(m)] = found;
        }

    // The five faces at each vertex, in the order they are found. Order does not
    // matter: everything downstream dedupes or takes a minimum.
    for (int v = 0; v < 12; ++v) {
        int n = 0;
        for (int f = 0; f < 20; ++f)
            for (int q = 0; q < 3; ++q)
                if (T.face[std::size_t(f)][std::size_t(q)] == v && n < 5)
                    { T.vertFaces[std::size_t(v)][std::size_t(n)] = f; ++n; break; }
    }

    // Pair the 20 faces into 10 rhombi. Any perfect matching of the face
    // adjacency graph works — that graph is the dodecahedral graph, cubic and
    // bridgeless, so Petersen's theorem guarantees one exists. The search below
    // takes the first in index order, which makes the pairing a fact about this
    // file rather than a magic constant pasted in.
    std::array<int, 20> partner{};
    for (auto& p : partner) p = -1;
    {
        std::array<bool, 20> used{};
        for (auto& u : used) u = false;
        // Depth-first over the lowest unmatched face. Bounded and deterministic.
        struct Search {
            const PlanetTopology& T;
            std::array<int, 20>& partner;
            std::array<bool, 20>& used;
            bool adjacent(int a, int b) const {
                int shared = 0;
                for (int p = 0; p < 3; ++p)
                    for (int q = 0; q < 3; ++q)
                        if (T.face[std::size_t(a)][std::size_t(p)]
                            == T.face[std::size_t(b)][std::size_t(q)]) ++shared;
                return shared == 2;
            }
            bool go() {
                int a = -1;
                for (int f = 0; f < 20; ++f) if (!used[std::size_t(f)]) { a = f; break; }
                if (a < 0) return true;
                used[std::size_t(a)] = true;
                for (int b = a + 1; b < 20; ++b) {
                    if (used[std::size_t(b)] || !adjacent(a, b)) continue;
                    used[std::size_t(b)] = true;
                    partner[std::size_t(a)] = b;
                    partner[std::size_t(b)] = a;
                    if (go()) return true;
                    partner[std::size_t(a)] = -1;
                    partner[std::size_t(b)] = -1;
                    used[std::size_t(b)] = false;
                }
                used[std::size_t(a)] = false;
                return false;
            }
        } s{T, partner, used};
        const bool okMatch = s.go();
        // A cubic bridgeless graph always has one; if this ever fired the
        // icosahedron table above would be wrong, which is worth knowing loudly.
        if (!okMatch) { std::fprintf(stderr, "planet.hpp: no face matching\n"); std::abort(); }
    }

    int nc = 0;
    for (int f = 0; f < 20; ++f) {
        const int g = partner[std::size_t(f)];
        if (g < f) continue;                       // take each pair once
        PlanetTopology::Chart& C = T.chart[std::size_t(nc)];
        C.face0 = f; C.face1 = g;
        // The shared edge is B,D; the apexes are A (in f) and C (in g).
        int shared[2] = {-1, -1}; int ns = 0;
        for (int p = 0; p < 3; ++p) {
            const int vv = T.face[std::size_t(f)][std::size_t(p)];
            for (int q = 0; q < 3; ++q)
                if (T.face[std::size_t(g)][std::size_t(q)] == vv && ns < 2) shared[ns++] = vv;
        }
        int apexF = -1, apexG = -1;
        for (int p = 0; p < 3; ++p) {
            const int a = T.face[std::size_t(f)][std::size_t(p)];
            if (a != shared[0] && a != shared[1]) apexF = a;
            const int b = T.face[std::size_t(g)][std::size_t(p)];
            if (b != shared[0] && b != shared[1]) apexG = b;
        }
        C.A = apexF; C.C = apexG;
        // Orient B and D so that (A,B,D) winds counter-clockwise seen from
        // outside. Every chart then has the same handedness, which is what lets
        // one axial neighbour rule serve all ten without a chirality flag —
        // the thing aperture 7 cannot have.
        const PV3 pa = T.vert[std::size_t(C.A)];
        const PV3 p0 = T.vert[std::size_t(shared[0])];
        const PV3 p1 = T.vert[std::size_t(shared[1])];
        const double orient = pv_dot(pv_cross(pv_sub(p0, pa), pv_sub(p1, pa)), pa);
        C.B = orient > 0.0 ? shared[0] : shared[1];
        C.D = orient > 0.0 ? shared[1] : shared[0];
        T.chartOfFace[std::size_t(f)] = nc;
        T.chartOfFace[std::size_t(g)] = nc;
        ++nc;
    }

    for (int f = 0; f < 20; ++f)
        invert3(T.vert[std::size_t(T.face[std::size_t(f)][0])],
                T.vert[std::size_t(T.face[std::size_t(f)][1])],
                T.vert[std::size_t(T.face[std::size_t(f)][2])],
                T.faceInv[std::size_t(f)]);
    return T;
}

// The one shared copy. It is 20 faces of arithmetic and it never changes.
[[nodiscard]] inline const PlanetTopology& topology() {
    static const PlanetTopology T = build_topology();
    return T;
}

// ── a point in face coordinates ─────────────────────────────────────────────
//
// Integer barycentric weights against the face's own three vertices, summing to
// N. This is the internal working form because it is ORDER-FREE: the same point
// has the same weights whichever face is asked, once you know which weight
// belongs to which icosahedron vertex. All six neighbour steps are a +1/-1 pair
// on two of the three weights, so an edge crossing shows up as exactly one
// weight going to -1 and nothing else can happen.
struct FacePt {
    int face = 0;
    std::array<int, 3> w{{0, 0, 0}};   // aligned to topology().face[face], sum = N
};

// ── the lattice at one depth ────────────────────────────────────────────────
class PlanetLattice {
public:
    // base is the level-0 subdivision, so a zone is a level-0 cell. 8 gives
    // 10*64+2 = 642 zones — 630 hexagons and 12 pentagons — which sits inside
    // hexplanetview.hpp's existing 4..40 subdivision knob, so the zone layer of
    // this planet is a mesh that sim can already draw.
    explicit PlanetLattice(int depth = 0, int base = 8)
        : base_(std::max(1, base)), depth_(std::max(0, depth)) {
        n_ = base_ << depth_;
    }

    [[nodiscard]] int base() const { return base_; }
    [[nodiscard]] int depth() const { return depth_; }
    [[nodiscard]] int n() const { return n_; }
    // Faces of the Goldberg dual = cells here = 10T+2 with T = n^2.
    [[nodiscard]] std::uint64_t cell_count() const {
        return 10ull * std::uint64_t(n_) * std::uint64_t(n_) + 2ull;
    }

    // ── naming ──────────────────────────────────────────────────────────────

    // Every (chart,i,j) representation of a point, canonical one first.
    // At most five: a face interior has one, an icosahedron edge has two, an
    // icosahedron vertex has five faces which collapse to three, four or five
    // distinct charts.
    int representations(FacePt p, std::array<PCell, 5>& out) const {
        const PlanetTopology& T = topology();
        std::array<int, 5> faces{};
        int nf = 0;
        // Which weights are zero decides how many faces share this point.
        int zeros = 0, zeroAt = -1, nonZeroAt = -1;
        for (int m = 0; m < 3; ++m) {
            if (p.w[std::size_t(m)] == 0) { ++zeros; zeroAt = m; }
            else nonZeroAt = m;
        }
        if (zeros == 0) {
            faces[0] = p.face; nf = 1;
        } else if (zeros == 1) {
            faces[0] = p.face;
            faces[1] = T.acrossFace[std::size_t(p.face)][std::size_t(zeroAt)];
            nf = 2;
        } else {
            // Two zero weights: the point IS an icosahedron vertex, the one with
            // the surviving weight. Five faces meet there.
            const int v = T.face[std::size_t(p.face)][std::size_t(nonZeroAt)];
            for (int q = 0; q < 5; ++q)
                faces[std::size_t(q)] = T.vertFaces[std::size_t(v)][std::size_t(q)];
            nf = 5;
        }

        const std::array<int, 12> vw = vertex_weights(p);
        int nOut = 0;
        for (int q = 0; q < nf; ++q) {
            const int g = faces[std::size_t(q)];
            const PCell c = cell_in_chart(vw, T.chartOfFace[std::size_t(g)], g);
            bool dup = false;
            for (int s = 0; s < nOut; ++s) if (out[std::size_t(s)] == c) dup = true;
            if (!dup) out[std::size_t(nOut++)] = c;
        }
        // Canonical first: the lexicographic minimum. A total rule, no table,
        // and no pole special case — the count falls out at 10n^2+2 by itself.
        for (int a = 1; a < nOut; ++a)
            if (out[std::size_t(a)] < out[0]) std::swap(out[0], out[std::size_t(a)]);
        return nOut;
    }

    [[nodiscard]] PCell canonical(FacePt p) const {
        std::array<PCell, 5> reps{};
        representations(p, reps);
        return reps[0];
    }

    // A canonical cell back to a face point. Uses the chart's first triangle
    // when i+j <= n, the second otherwise; on the diagonal either gives the same
    // point, which is the property that makes the rhombus one uniform lattice.
    [[nodiscard]] FacePt face_point(PCell c) const {
        const PlanetTopology& T = topology();
        const PlanetTopology::Chart& C = T.chart[std::size_t(c.chart)];
        FacePt p;
        if (c.i + c.j <= n_) {
            p.face = C.face0;
            set_weight(p, C.A, n_ - c.i - c.j);
            set_weight(p, C.B, c.i);
            set_weight(p, C.D, c.j);
        } else {
            p.face = C.face1;
            const int i2 = n_ - c.i, j2 = n_ - c.j;      // apex-C coordinates
            set_weight(p, C.C, n_ - i2 - j2);
            set_weight(p, C.D, i2);
            set_weight(p, C.B, j2);
        }
        return p;
    }

    // ── geometry ────────────────────────────────────────────────────────────

    // Barycentric on the flat face then normalised — the gnomonic projection,
    // and exactly what hexplanet.hpp's build() does to place its vertices.
    [[nodiscard]] PV3 direction(FacePt p) const {
        const PlanetTopology& T = topology();
        const auto& f = T.face[std::size_t(p.face)];
        PV3 s = pv_mul(T.vert[std::size_t(f[0])], double(p.w[0]));
        s = pv_add(s, pv_mul(T.vert[std::size_t(f[1])], double(p.w[1])));
        s = pv_add(s, pv_mul(T.vert[std::size_t(f[2])], double(p.w[2])));
        return pv_norm(s);
    }
    [[nodiscard]] PV3 direction(PCell c) const { return direction(face_point(c)); }

    // Direction back to a cell, in closed form. Twenty barycentric solves, one
    // per face, and the face is the one where all three come out non-negative.
    // O(1) in the cell count: this is the operation hexplanet.hpp does with an
    // O(F) scan over every face, and the whole reason that scan had to go.
    [[nodiscard]] PCell cell_of(PV3 dir) const {
        const PlanetTopology& T = topology();
        dir = pv_norm(dir);
        int best = 0; double bestWorst = -1e300;
        std::array<double, 3> bestB{{0.0, 0.0, 0.0}};
        for (int f = 0; f < 20; ++f) {
            const auto& M = T.faceInv[std::size_t(f)];
            const double a = M[0]*dir.x + M[1]*dir.y + M[2]*dir.z;
            const double b = M[3]*dir.x + M[4]*dir.y + M[5]*dir.z;
            const double c = M[6]*dir.x + M[7]*dir.y + M[8]*dir.z;
            const double sum = a + b + c;
            if (sum <= 0.0) continue;                    // the antipodal solution
            const double na = a / sum, nb = b / sum, ncc = c / sum;
            const double worst = std::min(na, std::min(nb, ncc));
            if (worst > bestWorst) { bestWorst = worst; best = f; bestB = {na, nb, ncc}; }
        }
        // Round to the lattice, then repair the sum: rounding three numbers that
        // must total n can be off by one, and the correction goes to the largest
        // weight, which is the one that cannot be pushed negative by it.
        FacePt p; p.face = best;
        for (int m = 0; m < 3; ++m)
            p.w[std::size_t(m)] = int(std::llround(bestB[std::size_t(m)] * double(n_)));
        for (int m = 0; m < 3; ++m) p.w[std::size_t(m)] = std::max(0, p.w[std::size_t(m)]);
        int sum = p.w[0] + p.w[1] + p.w[2];
        while (sum != n_) {
            int at = 0;
            for (int m = 1; m < 3; ++m)
                if (p.w[std::size_t(m)] > p.w[std::size_t(at)]) at = m;
            if (sum > n_) { --p.w[std::size_t(at)]; --sum; }
            else          { ++p.w[std::size_t(at)]; ++sum; }
        }
        return canonical(p);
    }

    // ── neighbours ──────────────────────────────────────────────────────────
    //
    // Six lateral neighbours, five at the twelve pentagons. EVERY consumer must
    // iterate what this returns and must never assume six. That is the single
    // invasive change this scheme forces on any neighbour-list algorithm — flood
    // fill, light propagation, fluid spread — and it is invasive precisely
    // because a hardcoded 6 is invisible until it walks onto a pentagon.
    int neighbours(PCell c, std::array<PCell, 6>& out) const {
        int nOut = 0;
        const FacePt base = face_point(c);
        const bool interior = base.w[0] > 0 && base.w[1] > 0 && base.w[2] > 0;
        if (interior) {
            // Fast path. Strictly inside a face, so no step can cross an edge
            // and no second representation is in play: six steps, six
            // canonicalisations, no dedupe needed.
            for (int d = 0; d < 6; ++d) {
                FacePt p = base;
                step(p, d);
                out[std::size_t(nOut++)] = canonical(p);
            }
            return nOut;
        }
        // On an icosahedron edge or at an icosahedron vertex. Walk every
        // representation, take every step that stays on the solid, and dedupe.
        std::array<PCell, 5> reps{};
        const int nr = representations(base, reps);
        for (int r = 0; r < nr; ++r) {
            const FacePt fp = face_point(reps[std::size_t(r)]);
            for (int d = 0; d < 6; ++d) {
                FacePt p = fp;
                step(p, d);
                if (!cross(p)) continue;                 // no image: a pentagon
                const PCell g = canonical(p);
                if (g == c) continue;
                bool dup = false;
                for (int s = 0; s < nOut; ++s) if (out[std::size_t(s)] == g) dup = true;
                if (!dup && nOut < 6) out[std::size_t(nOut++)] = g;
            }
        }
        return nOut;
    }

    // The twelve, forever. By Euler: with only pentagons and hexagons and three
    // faces at every vertex, 3V = 2E = 5P + 6H and V - E + F = 2 give P = 12
    // with H cancelling identically. Subdividing buys hexagons and never touches
    // the pentagon count.
    [[nodiscard]] bool is_pentagon(PCell c) const {
        const FacePt p = face_point(c);
        int zeros = 0;
        for (int m = 0; m < 3; ++m) if (p.w[std::size_t(m)] == 0) ++zeros;
        return zeros == 2;              // an icosahedron vertex, and only those
    }

    // ── the hierarchy: aperture 4 ───────────────────────────────────────────
    //
    // THE BIT SHIFT, which is what the research pass promised and what most of
    // the literature quotes: parent is (i>>1, j>>1) in the cell's own chart, and
    // children are the four (2i+a, 2j+b). O(1), no rotation, no per-level
    // orientation state — all true, and it is the honest reason to prefer
    // aperture 4 over H3's aperture 7.
    //
    // It is also NOT A PARTITION, and this file says so with a number rather
    // than shipping it as one. The shift is defined in a CHART FRAME, and a
    // point on a chart seam has several frames that disagree: for an odd
    // coordinate t on a seam whose two charts run it in opposite directions,
    // (n-t)>>1 and n/2-(t>>1) differ by one. Measured on this mesh at base 4,
    // the fine cells that the coarse cells' shifted children fail to cover are
    // 98, 202, 410 and 826 at depths 0->1 through 3->4 — exactly 104*2^d - 6,
    // shrinking as 1/N because it is a boundary effect, but never zero.
    //
    // These two are kept because the shift is the thing everyone reaches for and
    // its exact failure is worth being able to reproduce.
    [[nodiscard]] PCell parent_shift(PCell c) const {
        return PCell{ c.chart, c.i >> 1, c.j >> 1 };
    }
    int children_shift(PCell c, std::array<PCell, 4>& out) const {
        int nOut = 0;
        for (int a = 0; a < 2; ++a)
            for (int b = 0; b < 2; ++b)
                out[std::size_t(nOut++)] = PCell{ c.chart, 2 * c.i + a, 2 * c.j + b };
        return nOut;
    }

    // THE EXACT HIERARCHY, which is what everything else here uses.
    //
    // Work in weights instead of chart coordinates and the frame problem goes
    // away, because weights are keyed to icosahedron vertices and every face
    // agrees on them. A fine point has weights summing to 2N. If all three are
    // even it IS a coarse point and is its own parent — which is also why the
    // twelve pentagons are pentagons at every level. Otherwise exactly two are
    // odd (the sum is even), the point is the midpoint of a coarse edge, and it
    // has exactly TWO equally good parents: (w_a+1, w_b-1) and (w_a-1, w_b+1),
    // halved.
    //
    // That tie is intrinsic. Aperture 4 subdivides TRIANGLES perfectly, but the
    // cells here are lattice POINTS, and the fine points that are not coarse
    // points sit on coarse edges, equidistant from two coarse points. Any
    // tie-break is a choice, and the choice is not cosmetic — it decides the
    // SHAPE of a zone.
    //
    // The tie-break is: step toward the odd weight belonging to the lower-
    // numbered icosahedron vertex on even levels and the higher-numbered one on
    // odd levels. Frame-free (vertex ids and weights are both face-independent),
    // total, no table.
    //
    // The alternation is the part that was measured rather than reasoned. Any
    // fixed rule biases the parent one way, and over r levels that bias
    // COMPOUNDS into a drift, so zones starve at one end of a face and bloat at
    // the other. Zone sizes at base 4, depth 5, ideal 1011.4:
    //
    //   lexicographically smaller canonical name   256 .. 2016   max/min 7.9
    //   always the lower vertex id (fixed shear)     1 .. 2481   max/min 2481
    //   alternating by level parity                276 .. 1376   max/min 5.0
    //
    // Alternating cancels the drift pairwise. It does not make zones equal — a
    // gnomonic grid has no such thing — but it keeps them bounded and puts the
    // median exactly on the ideal at every depth.
    //
    // Counting confirms it has to work out this way: 10N^2+2 coarse points and
    // 40N^2+2 fine points leaves 30N^2 midpoints for 10N^2+2 parents to share,
    // so the average is 3 midpoints each and exactly six coarse cells must come
    // up one short. Four children per parent is the average, not a guarantee.
    [[nodiscard]] PCell parent(PCell c) const {
        const PlanetLattice up(depth_ > 0 ? depth_ - 1 : 0, base_);
        FacePt p = face_point(c);
        int odd[2] = {-1, -1}; int no = 0;
        for (int m = 0; m < 3; ++m)
            if ((p.w[std::size_t(m)] & 1) && no < 2) odd[no++] = m;
        if (no == 0) {
            FacePt q = p;
            for (int m = 0; m < 3; ++m) q.w[std::size_t(m)] >>= 1;
            return up.canonical(q);
        }
        const PlanetTopology& T = topology();
        const int va = T.face[std::size_t(p.face)][std::size_t(odd[0])];
        const int vb = T.face[std::size_t(p.face)][std::size_t(odd[1])];
        const bool preferLower = (depth_ & 1) == 0;
        const bool firstWins = preferLower ? (va < vb) : (va > vb);
        const int up1 = firstWins ? odd[0] : odd[1];        // gains the step
        const int dn1 = firstWins ? odd[1] : odd[0];        // loses it
        p.w[std::size_t(up1)] += 1;
        p.w[std::size_t(dn1)] -= 1;
        for (int m = 0; m < 3; ++m) p.w[std::size_t(m)] >>= 1;
        return up.canonical(p);
    }

    // The exact inverse of parent(), computed in O(1): a coarse cell's children
    // are itself, plus whichever of its at most six incident coarse-edge
    // midpoints chose it. Never searched, never more than seven candidates.
    //
    // The candidate midpoints come from neighbours() rather than from six raw
    // steps in one face frame, and that is not tidiness. A pentagon's five
    // neighbours are spread across its five incident faces, so stepping in a
    // single frame reaches only four of them — which left exactly twelve fine
    // cells, one per pentagon, with a parent that did not claim them back.
    int children(PCell c, std::array<PCell, 7>& out) const {
        const PlanetLattice down(depth_ + 1, base_);
        FacePt self = face_point(c);
        for (int m = 0; m < 3; ++m) self.w[std::size_t(m)] <<= 1;   // same point, fine lattice
        const PCell selfFine = down.canonical(self);
        int nOut = 0;
        out[std::size_t(nOut++)] = selfFine;
        std::array<PCell, 6> nb{};
        const int nn = down.neighbours(selfFine, nb);
        for (int q = 0; q < nn; ++q) {
            const PCell fine = nb[std::size_t(q)];
            if (down.parent(fine) != c) continue;
            bool dup = false;
            for (int s = 0; s < nOut; ++s) if (out[std::size_t(s)] == fine) dup = true;
            if (!dup && nOut < 7) out[std::size_t(nOut++)] = fine;
        }
        return nOut;
    }

    // The zone (level-0 cell) a cell belongs to. A zone is a LABEL — biome,
    // region, ownership, hexplanet.hpp's 62-region decomposition — and never a
    // storage or streaming unit. That is what lets a zone boundary be invisible:
    // there is only ever ONE cell lattice, so crossing a zone edge changes a
    // label and not a resolution, and the only size variation anywhere is the
    // projection's own smooth gradient.
    //
    // O(depth), by walking the exact parent chain. The one-shift version below
    // is O(1) and disagrees on chart seams; the test prints how often.
    [[nodiscard]] PCell zone_of(PCell c) const {
        PCell cur = c;
        for (int r = depth_; r > 0; --r) {
            const PlanetLattice at(r, base_);
            cur = at.parent(cur);
        }
        return cur;
    }
    [[nodiscard]] PCell zone_of_shift(PCell c) const {
        return PCell{ c.chart, c.i >> depth_, c.j >> depth_ };
    }

    // ── nominal scale, from the published angles ────────────────────────────
    [[nodiscard]] double nominal_spacing(double R) const {
        return kIcosaEdgeRad * R / double(n_);
    }
    [[nodiscard]] double nominal_cell_edge(double R) const {
        return nominal_spacing(R) * kHexEdgeOverSpacing;
    }

    // ── the dual, computed and never stored ─────────────────────────────────
    //
    // The hexagon (or pentagon) around a cell: the normalised centroids of the
    // geodesic triangles that touch it, which is the same construction
    // hexplanet.hpp uses for its Goldberg corners. Only the renderer and the
    // area measurement ever ask.
    int corners(PCell c, std::array<PV3, 6>& out) const {
        std::array<PCell, 6> nb{};
        const int nn = neighbours(c, nb);
        const PV3 p = direction(c);
        // Sort the neighbours around the cell, then a corner is the centroid of
        // the cell and two consecutive neighbours.
        PV3 up = std::fabs(p.z) < 0.9 ? PV3{0, 0, 1} : PV3{1, 0, 0};
        const PV3 e1 = pv_norm(pv_cross(up, p));
        const PV3 e2 = pv_cross(p, e1);
        std::array<std::pair<double, int>, 6> ring{};
        for (int q = 0; q < nn; ++q) {
            const PV3 d = pv_sub(direction(nb[std::size_t(q)]), p);
            ring[std::size_t(q)] = { std::atan2(pv_dot(d, e2), pv_dot(d, e1)), q };
        }
        // Insertion sort by hand rather than std::sort. libstdc++ switches to a
        // 16-element insertion sort below its threshold, and on a 6-element
        // std::array GCC then reports a false -Warray-bounds against a subscript
        // it never reaches. This project builds at zero warnings, and suppressing
        // a bounds warning is a bad habit to acquire for six elements.
        for (int a = 1; a < nn; ++a) {
            const auto key = ring[std::size_t(a)];
            int b = a - 1;
            while (b >= 0 && ring[std::size_t(b)].first > key.first) {
                ring[std::size_t(b + 1)] = ring[std::size_t(b)];
                --b;
            }
            ring[std::size_t(b + 1)] = key;
        }
        for (int q = 0; q < nn; ++q) {
            const PV3 a = direction(nb[std::size_t(ring[std::size_t(q)].second)]);
            const PV3 b = direction(nb[std::size_t(ring[std::size_t((q + 1) % nn)].second)]);
            out[std::size_t(q)] = pv_norm(pv_add(p, pv_add(a, b)));
        }
        return nn;
    }

    // Spherical area of a cell, in steradians, by the triangle fan from the
    // centre. Multiply by R*R for square metres.
    [[nodiscard]] double area(PCell c) const {
        std::array<PV3, 6> k{};
        const int nk = corners(c, k);
        const PV3 p = direction(c);
        double a = 0.0;
        for (int q = 0; q < nk; ++q) {
            const PV3 u = k[std::size_t(q)];
            const PV3 v = k[std::size_t((q + 1) % nk)];
            a += 0.5 * std::fabs(pv_dot(p, pv_cross(pv_sub(u, p), pv_sub(v, p))));
        }
        return a;
    }

private:
    void set_weight(FacePt& p, int vertexId, int value) const {
        const PlanetTopology& T = topology();
        for (int m = 0; m < 3; ++m)
            if (T.face[std::size_t(p.face)][std::size_t(m)] == vertexId)
                p.w[std::size_t(m)] = value;
    }
    // The point's weights keyed by icosahedron vertex id rather than by slot.
    // -1 marks a vertex the point's face does not touch.
    [[nodiscard]] std::array<int, 12> vertex_weights(FacePt p) const {
        const PlanetTopology& T = topology();
        std::array<int, 12> vw{};
        for (auto& x : vw) x = -1;
        for (int m = 0; m < 3; ++m)
            vw[std::size_t(T.face[std::size_t(p.face)][std::size_t(m)])] = p.w[std::size_t(m)];
        return vw;
    }
    // Read a point out in one chart's (i,j), as seen from face `face` — which
    // must be one of that chart's two triangles and must contain the point.
    //
    // The weights come keyed by icosahedron vertex id, and a vertex the SOURCE
    // face did not touch reads as -1. That is not missing information: a point
    // is only shared with another face when it lies on their common edge, and
    // then its weight for every vertex off that edge is exactly zero. So -1
    // means zero here, and reading it as "absent" was the bug that made a
    // boundary point canonicalise to two different names and left the cell count
    // 84 over at n=4, with cells of degree 0 and 4 that do not exist.
    //
    // Both of a chart's triangles contain B and D — they are the shared edge —
    // so those two weights are always available whichever face is asked, and the
    // face decides only which half of the block the point is read into.
    [[nodiscard]] PCell cell_in_chart(const std::array<int, 12>& vw, int chartId, int face) const {
        const PlanetTopology::Chart& C = topology().chart[std::size_t(chartId)];
        const int wB = std::max(0, vw[std::size_t(C.B)]);
        const int wD = std::max(0, vw[std::size_t(C.D)]);
        if (face == C.face0) return PCell{ chartId, wB, wD };     // the i+j <= n half
        // The far half: i2 = weight of D, j2 = weight of B, and i = n - i2.
        return PCell{ chartId, n_ - wD, n_ - wB };
    }
    // One axial step, in weights. Each of the six is a +1/-1 pair, so at most
    // one weight can reach -1 and a crossing is always a single event.
    void step(FacePt& p, int d) const {
        // The six axial directions, written in the face's own (w1,w2) axes with
        // w0 = n - i - j carrying the balance.
        static const int dw[6][3] = {
            {-1, 1, 0}, { 1,-1, 0}, {-1, 0, 1}, { 1, 0,-1}, { 0, 1,-1}, { 0,-1, 1} };
        for (int m = 0; m < 3; ++m) p.w[std::size_t(m)] += dw[d][m];
    }
    // Fold a one-step overshoot onto the neighbouring face. Derived, not
    // guessed: unfold the two faces into one plane and the point at weights
    // (a_u, a_v, -1) of face (u,v,m) sits at (a_u - 1, a_v - 1, +1) in the face
    // (u,v,x) across that edge. The sum is preserved — (a_u-1)+(a_v-1)+1 = n —
    // which is the check that this is the right fold and not a plausible one.
    //
    // Returns false when the step has no image at all. That happens only at the
    // twelve pentagons, where one of the six axial directions does not exist.
    bool cross(FacePt& p) const {
        int neg = -1;
        for (int m = 0; m < 3; ++m) if (p.w[std::size_t(m)] < 0) neg = m;
        if (neg < 0) return true;
        if (p.w[std::size_t(neg)] < -1) return false;
        const PlanetTopology& T = topology();
        const int u = T.face[std::size_t(p.face)][std::size_t((neg + 1) % 3)];
        const int v = T.face[std::size_t(p.face)][std::size_t((neg + 2) % 3)];
        const int au = p.w[std::size_t((neg + 1) % 3)] - 1;
        const int av = p.w[std::size_t((neg + 2) % 3)] - 1;
        if (au < 0 || av < 0) return false;              // stepped past a pentagon
        const int g = T.acrossFace[std::size_t(p.face)][std::size_t(neg)];
        FacePt q; q.face = g;
        for (int m = 0; m < 3; ++m) {
            const int id = T.face[std::size_t(g)][std::size_t(m)];
            q.w[std::size_t(m)] = (id == u) ? au : (id == v) ? av : 1;
        }
        p = q;
        return true;
    }

    int base_ = 8;
    int depth_ = 0;
    int n_ = 8;
};

// Canonicalise a possibly non-canonical (chart,i,j) that is known to be in
// range. Used after parent_raw / children_raw, which work in one chart's frame.
[[nodiscard]] inline PCell canonical_cell(const PlanetLattice& L, PCell c) {
    if (c.i < 0 || c.j < 0 || c.i > L.n() || c.j > L.n()) return c;
    return L.canonical(L.face_point(c));
}

// ── terrain: a pure function, which is the whole streaming contract ─────────
//
// block(cell, layer) depends on the seed and the cell and NOTHING else. No
// cache is consulted, no neighbour is read, no generation order exists. That is
// what makes "a cell must generate identically whether it is reached directly or
// by walking to it and back" true by construction rather than by testing — the
// test still checks it, because a claim of purity is exactly the kind that rots
// the first time somebody adds a memo.
//
// This is why ore cannot be BlockWorld's. BlockWorld::vein() is a seeded random
// walk over a finite array: to know whether one block is iron you must replay
// every vein from the start of the world. Here ore is a threshold on a
// positional noise field, which is evaluable at one cell in isolation. The
// blocks, the depth bands, the hardness, the tool tiers and the drop rules are
// all blockworld.hpp's, unchanged.
class PlanetTerrain {
public:
    PlanetTerrain(std::uint64_t seed, double radiusMetres, const PlanetLattice& lat,
                  int layers = 128)
        : seed_(seed), R_(radiusMetres), lat_(lat),
          layers_(std::max(16, layers)),
          h_(lat.nominal_cell_edge(radiusMetres)) {}

    [[nodiscard]] int layers() const { return layers_; }
    // Y=63, the game's own sea level, the reason BlockWorld is 128 tall and the
    // reason the published ore bands can be quoted in absolute layers here.
    [[nodiscard]] int sea_layer() const { return std::min(63, layers_ / 2 - 1); }
    // Cube-ish prisms: the radial step equals the hexagon edge, so a voxel is
    // about as tall as it is wide.
    [[nodiscard]] double layer_height() const { return h_; }
    [[nodiscard]] double radius_of_layer(int k) const {
        return R_ + double(k - sea_layer()) * h_;
    }
    [[nodiscard]] double surface_radius() const { return R_; }
    [[nodiscard]] std::uint64_t seed() const { return seed_; }

    // Top solid layer of a column. Two octaves of large shape plus two of
    // texture, sampled in 3D on the sphere itself — so there is no projection,
    // no seam to hide, and no pole to special-case.
    [[nodiscard]] int ground_layer(PCell c) const { return ground_dir(lat_.direction(c)); }

    // kReliefGain is not a taste knob, it is a correction, and it was measured
    // rather than guessed. A normalised fBm sum of octaves is an average of
    // independent uniform values, so it concentrates hard around 0.5 — the tails
    // that make hills and valleys are exactly the part averaging destroys.
    // Without the gain the whole planet came out one layer either side of sea
    // level and therefore entirely BEACH: a 900-cell transect measured 3.12%
    // sand, 6.56% water and not one cell of grass or dirt anywhere. The gain
    // stretches the distribution about its midpoint so the height field reaches
    // the range the amplitude already claimed it had.
    static constexpr double kReliefGain = 2.6;

    [[nodiscard]] int ground_dir(PV3 d) const {
        const double amp = double(layers_) * 0.22;
        const double f = fbm(pv_mul(d, 2.6), 4, 0x1234u);
        const double g = std::clamp(0.5 + (f - 0.5) * kReliefGain, 0.0, 1.0);
        const int gh = sea_layer() + int(amp * (g * 2.0 - 1.0));
        return std::clamp(gh, 2, layers_ - 6);
    }

    // The block at one voxel. Layered exactly the way BlockWorld::generate()
    // layers it — terrain, water table, ore by depth band, caves carved last so
    // they cut ore open — with every step rewritten as a function of position.
    [[nodiscard]] std::uint8_t block(PCell c, int k) const {
        return block_dir(lat_.direction(c), k);
    }

    [[nodiscard]] std::uint8_t block_dir(PV3 d, int k) const {
        if (k < 0 || k >= layers_) return Bedrock;      // the shell is the world
        if (k == 0) return Bedrock;
        const int gh = ground_dir(d);
        const int sea = sea_layer();
        const bool beach = gh <= sea + 1;

        std::uint8_t b = Air;
        if (k <= gh) {
            if (k == gh)         b = beach ? Sand : Grass;
            else if (k > gh - 4) b = beach ? Sand : Dirt;
            else                 b = Stone;
        } else if (k <= sea) {
            b = Water;
        }

        // Ore, at Java Edition's pre-1.18 depth bands. The bands and the
        // relative rarity are the published ones; the SHAPE is a noise blob
        // rather than BlockWorld's random walk, for the reason above.
        if (b == Stone) {
            const PV3 q = ore_point(d, k);
            if (k <= 15 && vein(q, 0x51A4u) > 0.9825) b = DiamondOre;
            else if (k <= 15 && vein(q, 0x51A5u) > 0.9550) b = RedstoneOre;
            else if (k <= 30 && vein(q, 0x51A6u) > 0.9800) b = LapisOre;
            else if (k <= 31 && vein(q, 0x51A3u) > 0.9700) b = GoldOre;
            else if (k >= 4 && k <= 31 && vein(q, 0x51A7u) > 0.9880) b = EmeraldOre;
            else if (k <= 63 && vein(q, 0x51A2u) > 0.8600) b = IronOre;
            else if (vein(q, 0x51A1u) > 0.8500) b = Coal;
            if (b == Stone && k >= 1 && k <= 10 && vein(q, 0x51A8u) > 0.9700) b = Lava;
        }

        // Caves, carved last so a vein can end up open in a cave wall. Same
        // threshold shape BlockWorld uses, faded out near the surface so the
        // terrain does not dissolve.
        if (b == Stone || b == Dirt || b == Coal || b == IronOre) {
            const int top = layers_ - 3;
            if (k >= 2 && k < top) {
                const double depth = 1.0 - double(k) / double(top);
                const double cut = 0.58 + 0.22 * (1.0 - depth);
                if (cave(d, k) > cut) b = Air;
            }
        }
        return b;
    }

private:
    // Integer hash, seeded. No table to initialise and no floating-point state,
    // so the same seed gives the same planet on any compiler.
    [[nodiscard]] std::uint32_t hash(std::uint32_t a) const {
        a ^= std::uint32_t(seed_) + 0x9E3779B9u + (a << 6) + (a >> 2);
        a ^= a >> 16; a *= 0x7FEB352Du;
        a ^= a >> 15; a *= 0x846CA68Bu;
        a ^= a >> 16;
        return a;
    }
    [[nodiscard]] double lattice(int x, int y, int z, std::uint32_t salt) const {
        return double(hash(std::uint32_t(x) * 374761393u
                         + std::uint32_t(y) * 2246822519u
                         + std::uint32_t(z) * 668265263u + salt) & 0xFFFFFFu)
             / double(0xFFFFFF);
    }
    static double smooth(double t) { return t * t * (3.0 - 2.0 * t); }

    // Trilinear value noise at a point in 3-space. 3D everywhere, including for
    // the height field, which is what removes the seam: a flat map's height
    // field has to be made to agree with itself at the edges, and this one
    // cannot disagree because there are no edges.
    [[nodiscard]] double noise3(PV3 p, std::uint32_t salt) const {
        const int xi = int(std::floor(p.x)), yi = int(std::floor(p.y)), zi = int(std::floor(p.z));
        const double tx = smooth(p.x - double(xi));
        const double ty = smooth(p.y - double(yi));
        const double tz = smooth(p.z - double(zi));
        auto plane = [&](int dy) {
            const double a = lattice(xi,   yi+dy, zi,   salt), b = lattice(xi+1, yi+dy, zi,   salt);
            const double c = lattice(xi,   yi+dy, zi+1, salt), e = lattice(xi+1, yi+dy, zi+1, salt);
            return (a + (b - a) * tx) * (1.0 - tz) + (c + (e - c) * tx) * tz;
        };
        const double l0 = plane(0), l1 = plane(1);
        return l0 + (l1 - l0) * ty;
    }
    [[nodiscard]] double fbm(PV3 p, int octaves, std::uint32_t salt) const {
        double sum = 0.0, amp = 0.5, freq = 1.0, norm = 0.0;
        for (int o = 0; o < octaves; ++o) {
            sum  += amp * noise3(pv_mul(p, freq), salt + std::uint32_t(o) * 7919u);
            norm += amp;
            amp  *= 0.5; freq *= 2.0;
        }
        return norm > 0.0 ? sum / norm : 0.0;
    }
    // The noise lattice for ore and caves is scaled so one noise cell spans
    // about eight voxels in every direction — a corridor an agent can stand in,
    // and a vein worth following. Because it is built from the real 3D metric
    // position, the feature size is the same at every depth of subdivision.
    [[nodiscard]] PV3 ore_point(PV3 d, int k) const {
        const double s = 1.0 / (8.0 * h_);
        return pv_mul(d, radius_of_layer(k) * s);
    }
    [[nodiscard]] double vein(PV3 q, std::uint32_t salt) const {
        return noise3(q, salt);
    }
    [[nodiscard]] double cave(PV3 d, int k) const {
        const double s = 1.0 / (8.0 * h_);
        PV3 q = pv_mul(d, radius_of_layer(k) * s);
        q.y *= 1.7;                     // flatter than tall, as BlockWorld does
        return noise3(q, 0xCAFEu);
    }

    std::uint64_t seed_;
    double R_;
    PlanetLattice lat_;
    int layers_;
    double h_;
};

// ── streaming: chunks, materialised on demand and released ──────────────────
//
// A chunk is 32 x 32 columns x 32 layers = 32,768 cells = 32 KB at one byte a
// cell, matching BlockWorld's uint8 Block enum. It is keyed off the CANONICAL
// cell name, so a cell on a chart seam lives in exactly one chunk and mining it
// cannot leave a second copy behind.
//
// Chunks are deliberately NOT zones. A zone at base 8 and depth 8 holds 256x256
// columns; a chunk holds 32x32. The two never align, and that is the point: a
// streaming unit that coincided with a label boundary would make the label
// visible.
inline constexpr int kChunkXY = 32;
inline constexpr int kChunkZ  = 32;
inline constexpr std::size_t kChunkCells = std::size_t(kChunkXY) * kChunkXY * kChunkZ;

struct PlanetChunk {
    std::vector<std::uint8_t> cell;      // kChunkCells, indexed [k][j][i]
    std::uint64_t touched = 0;           // a step counter, never a clock
};

class PlanetCache {
public:
    PlanetCache(const PlanetLattice& lat, const PlanetTerrain& gen)
        : lat_(lat), gen_(gen) {}

    [[nodiscard]] static std::uint64_t chunk_key(PCell c, int k) {
        return (std::uint64_t(c.chart & 0xF) << 60)
             | (std::uint64_t(std::uint32_t(c.i / kChunkXY) & 0x7FFFFFu) << 37)
             | (std::uint64_t(std::uint32_t(c.j / kChunkXY) & 0x7FFFFFu) << 14)
             | (std::uint64_t(std::uint32_t(k / kChunkZ) & 0x3FFFu));
    }

    // Build one chunk, cell by cell, from the pure terrain function.
    const PlanetChunk& materialise(std::uint64_t key) {
        auto it = live_.find(key);
        if (it != live_.end()) { it->second.touched = ++clockless_; return it->second; }
        PlanetChunk ch;
        ch.cell.resize(kChunkCells);
        const int chart = int((key >> 60) & 0xF);
        const int ci = int((key >> 37) & 0x7FFFFFu) * kChunkXY;
        const int cj = int((key >> 14) & 0x7FFFFFu) * kChunkXY;
        const int ck = int(key & 0x3FFFu) * kChunkZ;
        for (int dj = 0; dj < kChunkXY; ++dj)
            for (int di = 0; di < kChunkXY; ++di) {
                const int i = ci + di, j = cj + dj;
                if (i > lat_.n() || j > lat_.n()) {
                    for (int dk = 0; dk < kChunkZ; ++dk)
                        ch.cell[slot(di, dj, dk)] = Bedrock;
                    continue;
                }
                // Generate from the CANONICAL name, so the two chart-local
                // names of a seam cell cannot generate different terrain.
                const PCell can = canonical_cell(lat_, PCell{chart, i, j});
                const PV3 d = lat_.direction(can);
                for (int dk = 0; dk < kChunkZ; ++dk)
                    ch.cell[slot(di, dj, dk)] = gen_.block_dir(d, ck + dk);
            }
        ch.touched = ++clockless_;
        ++built_;
        auto ins = live_.emplace(key, std::move(ch));
        bytes_ = live_.size() * kChunkCells;
        high_  = std::max(high_, bytes_);
        return ins.first->second;
    }

    void release(std::uint64_t key) {
        live_.erase(key);
        bytes_ = live_.size() * kChunkCells;
    }

    // Read through the cache, materialising if needed. Always canonicalises
    // first: two names for one cell would be two answers for one question.
    std::uint8_t at(PCell c, int k) {
        if (k < 0 || k >= gen_.layers()) return Bedrock;
        const PCell can = canonical_cell(lat_, c);
        const std::uint64_t key = chunk_key(can, k);
        const PlanetChunk& ch = materialise(key);
        return ch.cell[slot(can.i % kChunkXY, can.j % kChunkXY, k % kChunkZ)];
    }
    // An edit. Lives only as long as its chunk does — this is a workbench for
    // measuring the addressing and the streaming, not a save-game format, and a
    // persistence layer that was not asked for would be a stub pretending to be
    // a feature.
    void set(PCell c, int k, std::uint8_t b) {
        if (k < 0 || k >= gen_.layers()) return;
        const PCell can = canonical_cell(lat_, c);
        const std::uint64_t key = chunk_key(can, k);
        materialise(key);
        live_[key].cell[slot(can.i % kChunkXY, can.j % kChunkXY, k % kChunkZ)] = b;
    }

    [[nodiscard]] int layers() const { return gen_.layers(); }
    [[nodiscard]] const PlanetTerrain& terrain() const { return gen_; }
    [[nodiscard]] std::size_t live_chunks() const { return live_.size(); }
    [[nodiscard]] std::size_t bytes() const { return bytes_; }
    [[nodiscard]] std::size_t high_water() const { return high_; }
    [[nodiscard]] std::uint64_t chunks_built() const { return built_; }
    void reset_high_water() { high_ = bytes_; }

    // Materialise everything within `radiusCells` of `here` — a view distance,
    // the thing a renderer or a physics step actually needs resident.
    //
    // It walks the neighbour graph rather than a rectangle in chart indices, and
    // that is deliberate: a disc near a chart seam spills into the next chart,
    // and a disc containing a pentagon has five-fold structure. Both are already
    // correct in neighbours(), so reusing it makes ensure() correct for free
    // instead of correct-looking.
    void ensure(PCell here, int radiusCells) {
        const PCell can = canonical_cell(lat_, here);
        std::vector<PCell> frontier{can}, next;
        std::unordered_map<std::uint64_t, int> seen;
        seen.emplace(pack_voxel(can, 0), 0);
        for (int r = 0; r <= radiusCells && !frontier.empty(); ++r) {
            for (const PCell& c : frontier) {
                for (int k = 0; k < gen_.layers(); k += kChunkZ)
                    materialise(chunk_key(c, k));
                if (r == radiusCells) continue;
                std::array<PCell, 6> nb{};
                const int m = lat_.neighbours(c, nb);
                for (int q = 0; q < m; ++q)
                    if (seen.emplace(pack_voxel(nb[std::size_t(q)], 0), r + 1).second)
                        next.push_back(nb[std::size_t(q)]);
            }
            frontier.swap(next);
            next.clear();
        }
    }

    // Keep the chunks whose centres are within `radius` cells of `here`, drop
    // the rest. The materialised neighbourhood follows the character and
    // nothing else is resident.
    void trim(PCell here, int radiusCells) {
        const PCell can = canonical_cell(lat_, here);
        std::vector<std::uint64_t> doomed;
        for (const auto& [key, ch] : live_) {
            (void)ch;
            const int chart = int((key >> 60) & 0xF);
            const int ci = int((key >> 37) & 0x7FFFFFu) * kChunkXY + kChunkXY / 2;
            const int cj = int((key >> 14) & 0x7FFFFFu) * kChunkXY + kChunkXY / 2;
            if (chart != can.chart) { doomed.push_back(key); continue; }
            if (std::abs(ci - can.i) > radiusCells || std::abs(cj - can.j) > radiusCells)
                doomed.push_back(key);
        }
        for (auto k : doomed) live_.erase(k);
        bytes_ = live_.size() * kChunkCells;
    }

private:
    [[nodiscard]] static std::size_t slot(int di, int dj, int dk) {
        return (std::size_t(dk) * kChunkXY + std::size_t(dj)) * kChunkXY + std::size_t(di);
    }
    PlanetLattice lat_;
    PlanetTerrain gen_;
    std::unordered_map<std::uint64_t, PlanetChunk> live_;
    std::size_t bytes_ = 0, high_ = 0;
    std::uint64_t built_ = 0;
    std::uint64_t clockless_ = 0;        // a step counter, so nothing reads a clock
};

// ── a character on the surface ──────────────────────────────────────────────
//
// Walks a great circle by choosing, at each step, the lateral neighbour that
// advances along it — which is the honest way to walk "in a straight line" on a
// sphere, and the only way to find out what a pentagon does to one.
class PlanetWalker {
public:
    PlanetWalker(const PlanetLattice& lat, PlanetCache& cache, PCell start, int toolTier = 0)
        : lat_(lat), cache_(cache), at_(canonical_cell(lat, start)), tier_(toolTier) {
        stand();
    }

    [[nodiscard]] PCell cell() const { return at_; }
    [[nodiscard]] int layer() const { return k_; }
    [[nodiscard]] int tier() const { return tier_; }
    void set_tier(int t) { tier_ = t; }
    [[nodiscard]] std::uint64_t steps() const { return steps_; }
    [[nodiscard]] std::uint64_t mined() const { return mined_; }
    [[nodiscard]] std::uint64_t break_ticks_spent() const { return ticks_; }

    // Aim along the great circle through the current cell with this axis. The
    // axis is the pole of the circle, so the forward direction is axis x here.
    void aim(PV3 axis) { axis_ = pv_norm(axis); }
    void aim_toward(PV3 target) { aim(pv_cross(lat_.direction(at_), target)); }

    [[nodiscard]] PV3 forward() const { return pv_norm(pv_cross(axis_, lat_.direction(at_))); }

    // One lateral step. Picks the neighbour that goes furthest forward while
    // staying nearest the great circle; the cross-track term is what stops a
    // greedy walk from drifting into a spiral.
    bool step() {
        std::array<PCell, 6> nb{};
        const int n = lat_.neighbours(at_, nb);
        if (n == 0) return false;
        const PV3 here = lat_.direction(at_);
        const PV3 fwd = pv_norm(pv_cross(axis_, here));
        int best = -1; double bestScore = -1e300;
        for (int q = 0; q < n; ++q) {
            const PV3 d = pv_norm(pv_sub(lat_.direction(nb[std::size_t(q)]), here));
            const double along = pv_dot(d, fwd);
            const double off   = std::fabs(pv_dot(lat_.direction(nb[std::size_t(q)]), axis_));
            const double score = along - 40.0 * off;
            if (score > bestScore) { bestScore = score; best = q; }
        }
        if (best < 0) return false;
        if (n == 5) ++pentSteps_;
        at_ = nb[std::size_t(best)];
        ++steps_;
        stand();
        return true;
    }

    // One step greedily toward a target direction: take the neighbour that gets
    // nearest it. On a sphere this follows the great circle, and unlike aim() +
    // step() it lands ON the target rather than beside it.
    //
    // Both are needed, and the difference is a real property of the lattice
    // rather than a quirk of two functions. A great-circle walk aimed straight
    // through a pentagon's centre passes within 0.81 cells of it and never
    // stands on it — the lattice steps AROUND a point defect. To measure what
    // crossing one costs you have to insist on crossing it.
    bool step_toward(PV3 target) {
        std::array<PCell, 6> nb{};
        const int n = lat_.neighbours(at_, nb);
        if (n == 0) return false;
        const PV3 t = pv_norm(target);
        double bestDot = pv_dot(lat_.direction(at_), t);
        int best = -1;
        for (int q = 0; q < n; ++q) {
            const double d = pv_dot(lat_.direction(nb[std::size_t(q)]), t);
            if (d > bestDot) { bestDot = d; best = q; }
        }
        if (best < 0) return false;                  // already the nearest cell
        if (n == 5) ++pentSteps_;
        at_ = nb[std::size_t(best)];
        ++steps_;
        stand();
        return true;
    }

    // Put the feet on the first non-solid layer above the ground in this column.
    void stand() {
        int k = 0;
        for (int q = cacheLayers() - 1; q >= 0; --q)
            if (block_solid(cache_.at(at_, q))) { k = q + 1; break; }
        k_ = std::min(k, cacheLayers() - 1);
    }

    // Mine the block at (cell, layer) using blockworld.hpp's own arithmetic.
    // Returns the ticks it took, or -1 if the block can never be broken.
    // `dropped` says whether the tool tier was high enough for a drop.
    int mine(PCell c, int k, bool& dropped) {
        const std::uint8_t b = cache_.at(c, k);
        const int t = break_ticks(b, tier_);
        if (t < 0) { dropped = false; return -1; }
        dropped = drops_for(b, tier_);
        cache_.set(c, k, Air);
        ++mined_;
        ticks_ += std::uint64_t(t);
        return t;
    }
    // Dig straight down from the feet.
    int mine_down(bool& dropped) { return mine(at_, k_ - 1, dropped); }

    [[nodiscard]] std::uint64_t pentagon_steps() const { return pentSteps_; }

private:
    [[nodiscard]] int cacheLayers() const { return cache_.layers(); }
    PlanetLattice lat_;
    PlanetCache& cache_;
    PCell at_;
    PV3 axis_{0, 1, 0};
    int k_ = 0;
    int tier_ = 0;
    std::uint64_t steps_ = 0, mined_ = 0, ticks_ = 0, pentSteps_ = 0;
};

} // namespace bench
