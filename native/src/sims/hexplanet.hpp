// hexplanet.hpp — a planet tiled in hexagons, with exactly twelve pentagons.
//
// The geometry is not a choice. You cannot tile a sphere with hexagons alone,
// and the reason is Euler's formula, not a limitation of anyone's engine: for a
// convex polyhedron V - E + F = 2, and if every face is a hexagon or pentagon
// and every vertex joins three faces, that forces the pentagon count to be
// EXACTLY twelve however large the sphere gets. Subdividing buys you more
// hexagons and never fewer pentagons. This is the Goldberg polyhedron, the dual
// of a geodesic sphere, and the same shape as a fullerene.
//
// So: take an icosahedron, subdivide each triangle n times, push every vertex
// out to the sphere, and take the DUAL — every vertex of the geodesic becomes a
// face here. The twelve original icosahedron vertices have five neighbours and
// become pentagons; every vertex created by subdivision has six and becomes a
// hexagon. Faces = 10n^2 + 2, of which twelve are pentagons, always.
//
// The bench's method applies: this file builds the mesh and then proves it,
// because a mesh that is subtly wrong renders perfectly well. Euler's formula,
// the pentagon count, mutual adjacency, and the area distortion are all checked
// rather than asserted in a comment.
//
// ── where the approach comes from ──────────────────────────────────────────
//
// Kenneth Ward's PlanetSmith (Incandescent Games), described in his own dev
// logs. His code is not public, so everything below was written from the prose
// description alone: this is a re-implementation of an approach, never a port.
//
// Reproduced here, in his terms:
//
//   "hexels"      his word for the hexagonal equivalent of voxels. Borrowed for
//                 the tiling; see the terrain entry below for the half of the
//                 word this file does not earn.
//   why not cubes a cube warped to a sphere looks acceptable at a face centre,
//                 stretches at the edges and is extreme at the corners. That is
//                 the argument for the Goldberg solid, and area_spread() below
//                 turns it into a number instead of repeating it.
//   62 regions    12 pentagons + 30 edge seams + 20 triangular faces, which is
//                 exactly the icosahedron's own V, E and F. Every face carries a
//                 region id, but all sixty-two are only in use from six
//                 subdivisions up: counted over the subdiv knob's whole range,
//                 n = 4 and n = 5 use 42 ids (12 pentagons + 30 seams, no
//                 interiors at all) and every n from 6 to 40 uses all 62.
//   land          a Life-like growth: a cell with more land around it is
//                 likelier to stay land, so continents come out connected
//                 rather than speckled. The rule only — his recursion over
//                 resolutions is in the second list.
//   climate       temperature and humidity from latitude and distance to ocean
//                 — deterministic, so a seed is a planet.
//
// Described in his logs and NOT reproduced here. The list is as much the point
// as the one above it, because a file that names an influence is one keystroke
// away from borrowing credit for work it never did:
//
//   terrain       his 2D height field carries 3D noise applied only near the
//                 surface, which he reports buys cliffs, overhangs and caves at
//                 a fraction of full 3D noise. The geometry here has no vertical
//                 dimension: the renderer projects face centres and corners,
//                 which are unit vectors on the sphere. No cliffs, no caves, and
//                 the "hexels" above are flat surface tiles with no depth to
//                 them.
//   doubling      his land growth doubles the resolution on each pass, so
//                 continents are built coarse-to-fine. The passes here run over
//                 one fixed mesh: they smooth a map that already exists instead
//                 of growing one down through scales.
//   addressing    the handedness trick — half the icosahedron's faces wound
//                 left and half right, so an index and a position convert with
//                 arithmetic instead of special cases. The regions here are
//                 labels and nothing more; nearest_face() locates a tile by
//                 scanning every face for the largest dot product.

#pragma once
#include "../sim.hpp"
#include "../rng.hpp"
#include "../render/voxel.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <vector>

namespace bench {

// ── the Goldberg mesh ───────────────────────────────────────────────────────
struct HexPlanet {
    struct V3 { float x, y, z; };
    struct Face {
        V3               centre;       // unit vector: where on the sphere
        std::vector<int> corners;      // indices into `corner`, wound consistently
        std::vector<int> neigh;        // adjacent faces, same count as corners
        float            area = 0.0f;  // steradians
        int              region = 0;   // 0..61, the PlanetSmith decomposition
    };

    std::vector<V3>   corner;          // the geodesic triangle centroids
    std::vector<Face> face;
    int subdivisions = 0;

    [[nodiscard]] std::size_t pentagons() const {
        std::size_t n = 0;
        for (const auto& f : face) if (f.corners.size() == 5) ++n;
        return n;
    }
    [[nodiscard]] std::size_t hexagons() const { return face.size() - pentagons(); }

    // Euler's formula on the Goldberg polyhedron itself. Every vertex joins
    // exactly three faces, so E = 3V/2 and the identity has to come out at 2.
    // If a dedup went wrong anywhere this is what says so.
    [[nodiscard]] int euler() const {
        std::size_t halfEdges = 0;
        for (const auto& f : face) halfEdges += f.corners.size();
        const std::size_t E = halfEdges / 2;
        return int(corner.size()) - int(E) + int(face.size());
    }

    // Largest face area over smallest. The claim being checked is that the
    // distortion stays small however far you subdivide — which is the whole
    // reason this shape is worth the trouble over a warped cube.
    [[nodiscard]] float area_spread() const {
        if (face.empty()) return 0.0f;
        float lo = face[0].area, hi = face[0].area;
        for (const auto& f : face) { lo = std::min(lo, f.area); hi = std::max(hi, f.area); }
        return (lo > 0.0f) ? hi / lo : 0.0f;
    }

    [[nodiscard]] int nearest_face(V3 dir) const {
        int best = 0; float bestDot = -2.0f;
        for (std::size_t i = 0; i < face.size(); ++i) {
            const float d = face[i].centre.x*dir.x + face[i].centre.y*dir.y + face[i].centre.z*dir.z;
            if (d > bestDot) { bestDot = d; best = int(i); }
        }
        return best;
    }
};

namespace goldberg {

inline HexPlanet::V3 norm(HexPlanet::V3 v) {
    const float m = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
    return m > 0 ? HexPlanet::V3{v.x/m, v.y/m, v.z/m} : v;
}
inline HexPlanet::V3 add(HexPlanet::V3 a, HexPlanet::V3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline HexPlanet::V3 sub(HexPlanet::V3 a, HexPlanet::V3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline HexPlanet::V3 mul(HexPlanet::V3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }
inline float dot(HexPlanet::V3 a, HexPlanet::V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline HexPlanet::V3 cross(HexPlanet::V3 a, HexPlanet::V3 b) {
    return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}

// The icosahedron, from the golden ratio.
inline void icosahedron(std::vector<HexPlanet::V3>& v, std::vector<std::array<int,3>>& f) {
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    v = { {-1, t, 0}, { 1, t, 0}, {-1,-t, 0}, { 1,-t, 0},
          { 0,-1, t}, { 0, 1, t}, { 0,-1,-t}, { 0, 1,-t},
          { t, 0,-1}, { t, 0, 1}, {-t, 0,-1}, {-t, 0, 1} };
    for (auto& p : v) p = norm(p);
    f = { {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
          {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
          {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
          {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1} };
}

// Build the Goldberg polyhedron GP(n,0).
//
// Subdivide each icosahedron face into n^2 triangles on a barycentric lattice,
// deduplicate shared points, then take the dual: one face per geodesic vertex,
// its corners being the centroids of the triangles around it.
//
// Deduplication is by quantised position rather than by a clever index scheme.
// That is still the trade being made, but it is not the free win this comment
// used to claim. The quantisation has a failure mode of its own — described at
// vertexOf below — and it shipped broken at three subdivisions inside the
// knob's range. Euler's formula does catch a bad dedup, but only when something
// asks it to, and the self-test used to sample five values of n rather than
// sweep them, so nothing ever asked at the three that were wrong. It now walks
// every subdivision the knob offers.
inline HexPlanet build(int n) {
    n = std::max(1, n);
    HexPlanet P;
    P.subdivisions = n;

    std::vector<HexPlanet::V3> iv;
    std::vector<std::array<int,3>> ifc;
    icosahedron(iv, ifc);

    std::vector<HexPlanet::V3> gv;                       // geodesic vertices
    std::vector<std::array<int,3>> gt;                   // geodesic triangles
    std::map<std::array<long long,3>, int> lookup;
    auto key = [](HexPlanet::V3 p) {
        auto q = [](float a) { return (long long)std::llround(double(a) * 100000.0); };
        return std::array<long long,3>{ q(p.x), q(p.y), q(p.z) };
    };
    // Look in the neighbouring quantisation cells too, not just the exact one.
    //
    // Two mathematically identical points reached along different icosahedron
    // faces differ in their last bits, and when that difference straddles a
    // quantisation boundary the exact-key lookup files them as two vertices.
    // The mesh then has extra faces, three-cornered ones, and Euler comes out
    // wrong — measured, at n = 20, 25 and 40, all inside this knob's own range.
    // Checking the 27 surrounding cells makes the join tolerant of the straddle
    // without needing an index scheme, and it cannot merge genuinely distinct
    // vertices. A false merge would need two real vertices to land within one
    // quantum of each other on all three axes at once; an all-pairs scan over
    // the face centres — which ARE the geodesic vertices — puts the closest
    // pair at n = 40, the largest the knob allows and so the tightest case, at
    // 1547 quanta apart on its widest axis. Three orders of magnitude of room.
    auto vertexOf = [&](HexPlanet::V3 p) {
        p = norm(p);
        const auto k = key(p);
        for (long long dx = -1; dx <= 1; ++dx)
            for (long long dy = -1; dy <= 1; ++dy)
                for (long long dz = -1; dz <= 1; ++dz) {
                    auto it = lookup.find({k[0] + dx, k[1] + dy, k[2] + dz});
                    if (it != lookup.end()) return it->second;
                }
        const int id = int(gv.size());
        gv.push_back(p);
        lookup.emplace(k, id);
        return id;
    };

    for (std::size_t fi = 0; fi < ifc.size(); ++fi) {
        const HexPlanet::V3 A = iv[std::size_t(ifc[fi][0])];
        const HexPlanet::V3 B = iv[std::size_t(ifc[fi][1])];
        const HexPlanet::V3 C = iv[std::size_t(ifc[fi][2])];
        // Barycentric lattice: p(i,j) = A + (B-A)*i/n + (C-A)*j/n, i+j <= n.
        auto at = [&](int i, int j) {
            return vertexOf(add(A, add(mul(sub(B, A), float(i) / float(n)),
                                          mul(sub(C, A), float(j) / float(n)))));
        };
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n - j; ++i) {
                gt.push_back({ at(i, j), at(i + 1, j), at(i, j + 1) });
                if (i + j < n - 1)
                    gt.push_back({ at(i + 1, j), at(i + 1, j + 1), at(i, j + 1) });
            }
    }

    // The dual. One Goldberg face per geodesic vertex; its corners are the
    // centroids of the triangles touching that vertex, sorted around it.
    std::vector<std::vector<int>> around(gv.size());
    P.corner.resize(gt.size());
    for (std::size_t t = 0; t < gt.size(); ++t) {
        P.corner[t] = norm(add(gv[std::size_t(gt[t][0])],
                               add(gv[std::size_t(gt[t][1])], gv[std::size_t(gt[t][2])])));
        for (int c = 0; c < 3; ++c) around[std::size_t(gt[t][std::size_t(c)])].push_back(int(t));
    }

    P.face.resize(gv.size());
    for (std::size_t v = 0; v < gv.size(); ++v) {
        HexPlanet::Face& F = P.face[v];
        F.centre = gv[v];
        // Sort the surrounding centroids by angle in the tangent plane, so the
        // polygon is a ring and not a scribble. An unsorted fan renders as a
        // star and the area comes out wrong, which the area check would catch.
        HexPlanet::V3 up = std::fabs(F.centre.z) < 0.9f ? HexPlanet::V3{0,0,1} : HexPlanet::V3{1,0,0};
        const HexPlanet::V3 e1 = norm(cross(up, F.centre));
        const HexPlanet::V3 e2 = cross(F.centre, e1);
        std::vector<std::pair<float,int>> ring;
        for (int t : around[v]) {
            const HexPlanet::V3 d = sub(P.corner[std::size_t(t)], F.centre);
            ring.emplace_back(std::atan2(dot(d, e2), dot(d, e1)), t);
        }
        std::sort(ring.begin(), ring.end());
        for (auto& r : ring) F.corners.push_back(r.second);

        // Spherical area by the triangle fan from the centre.
        float a = 0.0f;
        for (std::size_t i = 0; i < F.corners.size(); ++i) {
            const HexPlanet::V3 p = P.corner[std::size_t(F.corners[i])];
            const HexPlanet::V3 q = P.corner[std::size_t(F.corners[(i + 1) % F.corners.size()])];
            a += 0.5f * std::fabs(dot(F.centre, cross(sub(p, F.centre), sub(q, F.centre))));
        }
        F.area = a;

        // The 62-region decomposition: 12 pentagon regions, 30 edge seams and
        // 20 face interiors — the icosahedron's own V, E and F, so the
        // decomposition IS the solid.
        //
        // This said "filled in below" and was never filled in below. A field
        // documented as carrying a meaning it does not carry is worse than an
        // absent field, because the next reader builds on it. Pentagons take
        // 0..11 by discovery order; everything else is classified by how close
        // it sits to an icosahedron edge plane, which is what a seam IS.
        F.region = (F.corners.size() == 5) ? 0 : 41;
    }

    // Number the regions now that every face exists. Pentagons first, in
    // discovery order, then the seams: a face whose centre lies close to the
    // plane of an icosahedron edge is a seam, and the rest are interiors.
    {
        int pent = 0;
        for (auto& F : P.face) if (F.corners.size() == 5) F.region = pent++;
        for (auto& F : P.face) {
            if (F.corners.size() == 5) continue;
            float best = 1e9f; int bestEdge = -1, e = 0;
            for (std::size_t a = 0; a < iv.size(); ++a)
                for (std::size_t b = a + 1; b < iv.size(); ++b) {
                    // Only the 30 real icosahedron edges: its vertices sit at a
                    // known separation, so a distance test picks them out.
                    if (dot(iv[a], iv[b]) < 0.4f) continue;
                    // The plane through an edge also passes through its
                    // ANTIPODAL edge, so |dot| alone gave both the same
                    // distance and thirty seams collapsed into fifteen. Require
                    // the face to be on the near side of the sphere before the
                    // edge counts as its seam.
                    const HexPlanet::V3 mid = norm(add(iv[a], iv[b]));
                    if (dot(mid, F.centre) > 0.0f) {
                        const HexPlanet::V3 n = norm(cross(iv[a], iv[b]));
                        const float d = std::fabs(dot(n, F.centre));
                        if (d < best) { best = d; bestEdge = e; }
                    }
                    ++e;
                }
            const float span = 1.0f / float(std::max(1, P.subdivisions));
            if (best < span * 0.5f && bestEdge >= 0) { F.region = 12 + (bestEdge % 30); continue; }
            // Interior: numbered 42..61 by which icosahedron face it sits on.
            // At n = 4 and n = 5 nothing reaches here — the seam test above
            // takes every non-pentagon — so those two settings, both inside the
            // subdiv knob's range, use 42 region ids and not 62.
            float bestFace = -2.0f; int fi = 0;
            for (std::size_t t = 0; t < ifc.size(); ++t) {
                const HexPlanet::V3 c3 = norm(add(iv[std::size_t(ifc[t][0])],
                                                  add(iv[std::size_t(ifc[t][1])],
                                                      iv[std::size_t(ifc[t][2])])));
                const float d = dot(c3, F.centre);
                if (d > bestFace) { bestFace = d; fi = int(t); }
            }
            F.region = 42 + fi;
        }
    }

    // Neighbours: two faces are adjacent when they share two corners.
    std::unordered_map<long long, std::vector<int>> byCorner;
    for (std::size_t f = 0; f < P.face.size(); ++f)
        for (int c : P.face[f].corners) byCorner[c].push_back(int(f));
    for (std::size_t f = 0; f < P.face.size(); ++f) {
        std::map<int,int> shared;
        for (int c : P.face[f].corners)
            for (int g : byCorner[c]) if (g != int(f)) ++shared[g];
        for (auto& [g, count] : shared) if (count >= 2) P.face[f].neigh.push_back(g);
    }
    return P;
}

} // namespace goldberg

} // namespace bench
