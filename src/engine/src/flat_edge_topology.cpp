// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include "flat_edge_topology.hpp"
#include "flat_modifier_tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>

namespace cadml::engine::detail {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Hash combiner for EdgeKey. Pack the two uint32 indices into a
// uint64 — they're already canonicalized (v0 < v1) so the pair is
// unique per edge.
struct EdgeKeyHash {
    std::size_t operator()(const EdgeKey& k) const noexcept {
        return (static_cast<std::uint64_t>(k.v0) << 32) |
                static_cast<std::uint64_t>(k.v1);
    }
};

// Make the canonical form of an edge with v0 < v1.
EdgeKey canonical(std::uint32_t a, std::uint32_t b) {
    return (a < b) ? EdgeKey{a, b} : EdgeKey{b, a};
}

}  // namespace

bool triangle_traverses_edge_forward(const FlatMesh& mesh,
                                       std::uint32_t tri,
                                       std::uint32_t v0,
                                       std::uint32_t v1)
{
    const auto i0 = mesh.indices[tri * 3 + 0];
    const auto i1 = mesh.indices[tri * 3 + 1];
    const auto i2 = mesh.indices[tri * 3 + 2];
    return (i0 == v0 && i1 == v1)
        || (i1 == v0 && i2 == v1)
        || (i2 == v0 && i0 == v1);
}

Vec3 triangle_normal(const FlatMesh& mesh, std::uint32_t tri) {
    const auto i0 = mesh.indices[tri * 3 + 0];
    const auto i1 = mesh.indices[tri * 3 + 1];
    const auto i2 = mesh.indices[tri * 3 + 2];
    const auto& a = mesh.vertices[i0];
    const auto& b = mesh.vertices[i1];
    const auto& c = mesh.vertices[i2];
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 n  = ab.cross(ac);
    const double len_sq = n.length_sq();
    if (len_sq < 1e-30) return {0, 0, 0};
    return n * (1.0 / std::sqrt(len_sq));
}

EdgeTopology build_edge_topology(const FlatMesh& mesh) {
    EdgeTopology topo;
    const auto tri_count = mesh.triangle_count();

    // Pre-compute per-triangle outward normals (re-used for
    // classification below).
    std::vector<Vec3> tri_normals(tri_count);
    for (std::uint32_t t = 0; t < tri_count; ++t) {
        tri_normals[t] = triangle_normal(mesh, t);
    }

    // Pass 1: walk every triangle's three edges, accumulate per-edge
    // triangle adjacency. For each triangle, also remember which
    // *position* (0/1/2) each edge sits in so we can reconstruct
    // `triangle_edges` in the second pass.
    struct TriSlot {
        std::uint32_t tri;
        std::uint8_t  slot;          // 0/1/2: which of the triangle's edges
    };
    std::unordered_map<EdgeKey, std::vector<TriSlot>, EdgeKeyHash> map;
    map.reserve(tri_count * 2);

    auto add = [&](std::uint32_t a, std::uint32_t b,
                    std::uint32_t tri, std::uint8_t slot) {
        if (a == b) return;          // degenerate edge
        map[canonical(a, b)].push_back({tri, slot});
    };

    for (std::uint32_t t = 0; t < tri_count; ++t) {
        const auto i0 = mesh.indices[t * 3 + 0];
        const auto i1 = mesh.indices[t * 3 + 1];
        const auto i2 = mesh.indices[t * 3 + 2];
        add(i0, i1, t, 0);
        add(i1, i2, t, 1);
        add(i2, i0, t, 2);
    }

    // Pass 2: materialise EdgeInfos and the triangle_edges lookup.
    // `map` is an unordered_map; iterating it directly would assign
    // edge indices in hash-iteration order — non-portable across stdlib
    // implementations. Collect keys into a deterministic order first
    // so `EdgeTopology::edges` indexing is stable.
    topo.edges.reserve(map.size());
    topo.triangle_edges.assign(tri_count, {-1, -1, -1});

    std::vector<EdgeKey> ordered_keys;
    ordered_keys.reserve(map.size());
    for (const auto& kv : map) ordered_keys.push_back(kv.first);
    std::sort(ordered_keys.begin(), ordered_keys.end(),
              [](const EdgeKey& a, const EdgeKey& b) {
                  if (a.v0 != b.v0) return a.v0 < b.v0;
                  return a.v1 < b.v1;
              });

    for (const auto& key : ordered_keys) {
        auto& slots = map[key];
        EdgeInfo info;
        info.key = key;
        info.triangles.reserve(slots.size());
        for (const auto& s : slots) info.triangles.push_back(s.tri);
        const auto edge_idx = static_cast<std::uint32_t>(topo.edges.size());
        for (const auto& s : slots) {
            topo.triangle_edges[s.tri][s.slot] =
                static_cast<std::int32_t>(edge_idx);
        }

        // Classify based on adjacency count.
        if (slots.size() == 1) {
            info.classification = EdgeClass::Boundary;
        } else if (slots.size() >= 3) {
            info.classification = EdgeClass::NonManifold;
        } else {
            // Manifold edge — compute dihedral.
            //
            // Identify the triangle whose CCW edge order matches the
            // canonical (v0, v1) (call it t1, normal n1) versus the
            // one that traverses (v1, v0) (t2, n2). We need this
            // orientation to give the cross product a defined sign.
            //
            // For each adjacent triangle, look at indices (i0,i1,i2)
            // and find which pair matches (v0,v1) in canonical order.
            std::uint32_t t1 = slots[0].tri;
            std::uint32_t t2 = slots[1].tri;
            // Swap so t1 is the triangle whose CCW order has v0→v1.
            if (!triangle_traverses_edge_forward(mesh, t1, key.v0, key.v1)) {
                std::swap(t1, t2);
            }

            const auto& n1 = tri_normals[t1];
            const auto& n2 = tri_normals[t2];
            const Vec3 edge_vec =
                mesh.vertices[key.v1] - mesh.vertices[key.v0];

            const double dot = std::clamp(n1.dot(n2), -1.0, 1.0);
            const Vec3 cross = n1.cross(n2);
            const double cross_along_edge = cross.dot(edge_vec);

            // Sign of cross_along_edge tells us convex / concave / flat.
            //
            // Derivation (with both normals outward and the canonical
            // edge running v0 → v1 in t1, v1 → v0 in t2):
            //   - For a CONVEX edge of a CCW-from-outside cube,
            //     n1 × n2 points along +edge_vec (same direction).
            //     => cross_along_edge > 0.
            //   - For a CONCAVE edge it points against.
            //   - For a flat (coplanar) edge n1 ≈ n2, cross ≈ 0.
            const double cross_len_sq = cross.length_sq();
            if (cross_len_sq < kFlatCrossSqEps) {
                info.classification = EdgeClass::Flat;
                info.dihedral_rad = kPi;
            } else if (cross_along_edge > 0) {
                info.classification = EdgeClass::Convex;
                info.dihedral_rad = kPi - std::acos(dot);
            } else {
                info.classification = EdgeClass::Concave;
                info.dihedral_rad = kPi + std::acos(dot);
            }
        }

        topo.edges.push_back(std::move(info));
    }

    return topo;
}

// ─── Tier 1 predicate ─────────────────────────────────────────────────

namespace {

// kSharpDihedral imported from flat_modifier_tolerances.hpp; rationale
// + boundary case (12-segment cylinder rejection) live there.

}  // namespace

Tier1Result tier1_check(const FlatMesh& mesh,
                          const EdgeTopology& topo,
                          const std::vector<std::uint32_t>& selected_edges)
{
    auto fail = [](std::string msg) {
        return Tier1Result{ /*ok=*/false, std::move(msg) };
    };
    auto edge_label = [](std::uint32_t idx, const EdgeInfo& e) {
        std::ostringstream os;
        os << "edge " << idx << " (verts " << e.key.v0 << "-" << e.key.v1 << ")";
        return os.str();
    };

    // Whole-mesh check: Tier 1 modifiers don't handle concave geometry
    // ANYWHERE in the input, even on edges that aren't selected. The
    // per-edge adjacency check below only sees triangles immediately
    // touching a selected edge — a concave edge in the same coplanar
    // FACE region but on the far side wouldn't be caught locally.
    //
    // Per the plan's "Composition over generality" philosophy
    // (decompose concave shapes across files), reject the input
    // outright when any concave edge is present.
    for (std::uint32_t i = 0; i < topo.edges.size(); ++i) {
        if (topo.edges[i].classification == EdgeClass::Concave) {
            return fail("input mesh has a concave edge (verts "
                + std::to_string(topo.edges[i].key.v0) + "-"
                + std::to_string(topo.edges[i].key.v1)
                + ") — Tier 1 modifiers require fully convex inputs;"
                " split the shape across files and assemble");
        }
        if (topo.edges[i].classification == EdgeClass::NonManifold) {
            return fail("input mesh has a non-manifold edge — "
                "Tier 1 modifiers require manifold inputs");
        }
    }

    for (const auto edge_idx : selected_edges) {
        if (edge_idx >= topo.edges.size()) {
            return fail("selected edge index out of range");
        }
        const auto& sel = topo.edges[edge_idx];
        if (sel.classification != EdgeClass::Convex) {
            return fail(edge_label(edge_idx, sel) +
                          ": selected edge must be convex");
        }

        // For each adjacent triangle, walk its 3 edges and confirm
        // each non-selected edge is flat, boundary, or sharp-convex.
        for (const auto tri : sel.triangles) {
            const auto& tri_edges = topo.triangle_edges[tri];
            for (int slot = 0; slot < 3; ++slot) {
                const auto eidx = tri_edges[slot];
                if (eidx < 0) continue;                // shouldn't happen
                const auto eu = static_cast<std::uint32_t>(eidx);
                if (eu == edge_idx) continue;          // the edge we're chamfering
                const auto& other = topo.edges[eu];
                switch (other.classification) {
                    case EdgeClass::Flat:
                    case EdgeClass::Boundary:
                        continue;                      // OK
                    case EdgeClass::Concave:
                        return fail(edge_label(edge_idx, sel) +
                                      ": adjacent triangle has a concave edge");
                    case EdgeClass::NonManifold:
                        return fail(edge_label(edge_idx, sel) +
                                      ": adjacent triangle has a non-manifold edge");
                    case EdgeClass::Convex:
                        if (other.dihedral_rad >= kSharpDihedral) {
                            return fail(edge_label(edge_idx, sel) +
                                ": adjacent triangle has a near-flat convex"
                                " edge (suspected curve approximation)");
                        }
                        // sharp corner — OK
                        break;
                }
            }
        }
    }

    (void)mesh;     // currently unused; reserved for future face-area / area-check
    return { /*ok=*/true, {} };
}

}  // namespace cadml::engine::detail
