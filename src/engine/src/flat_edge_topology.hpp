// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/types.hpp>     // libcadml — Vec3
#include <cadml/engine/flat_evaluator.hpp>   // FlatMesh

#include <array>
#include <cstdint>
#include <vector>

namespace cadml::engine::detail {

// Canonical edge: vertex indices in ascending order.
struct EdgeKey {
    std::uint32_t v0 = 0;
    std::uint32_t v1 = 0;
    bool operator==(const EdgeKey& o) const {
        return v0 == o.v0 && v1 == o.v1;
    }
};

enum class EdgeClass {
    Convex,        // outward bend; dihedral ∈ (0, π)
    Concave,       // inward bend; dihedral ∈ (π, 2π)
    Flat,          // coplanar; dihedral ≈ π
    Boundary,      // only one adjacent triangle (open mesh)
    NonManifold    // 3+ adjacent triangles (invalid manifold)
};

struct EdgeInfo {
    EdgeKey                       key;
    std::vector<std::uint32_t>    triangles;        // adjacent triangle indices
    EdgeClass                     classification = EdgeClass::Boundary;
    double                        dihedral_rad   = 3.14159265358979323846;
};

// Per-mesh edge topology. Edges are indexed; `triangle_edges[t]`
// gives the three edge indices for triangle `t` (in the same
// vertex-pair order as the triangle's CCW indices: edge 0 = v0→v1,
// edge 1 = v1→v2, edge 2 = v2→v0). -1 means the triangle had a
// degenerate edge that couldn't be matched (shouldn't happen on
// well-formed input).
struct EdgeTopology {
    std::vector<EdgeInfo>                          edges;
    std::vector<std::array<std::int32_t, 3>>       triangle_edges;
};

// Compute the outward face normal for triangle `tri`. Uses CCW
// winding; if the triangle is degenerate (zero area) returns the
// zero vector.
Vec3 triangle_normal(const FlatMesh& mesh, std::uint32_t tri);

// Returns true iff triangle `tri`'s CCW vertex order traverses the
// edge as v0 → v1 (i.e., the triangle has v0 immediately followed by
// v1 in its winding). Used by topology classification and any
// modifier that needs to know which adjacent triangle is "t1" of an
// edge in canonical (v0 < v1) form.
bool triangle_traverses_edge_forward(const FlatMesh& mesh,
                                       std::uint32_t tri,
                                       std::uint32_t v0,
                                       std::uint32_t v1);

// Build the topology of `mesh`. O(T) where T = triangle count.
EdgeTopology build_edge_topology(const FlatMesh& mesh);

// ─── Tier 1 predicate (slice C2) ──────────────────────────────────────
//
// Yes/no answer to "is this edge selection valid input for a Tier 1
// modifier (chamfer / fillet / shell)?". Tier 1 means: planar-face
// inputs only, no curve approximation, no concave or non-manifold
// surfaces.
//
// Per-edge requirements:
//   * Selected edge classification == Convex.
//
// Per-adjacent-triangle requirements (each of the selected edge's two
// adjacent triangles): every OTHER edge of that triangle must fall into
// one of:
//   * Flat (coplanar triangulation seam — same face)
//   * Boundary (open mesh edge)
//   * Convex with a "sharp" dihedral (real face boundary)
//
// "Sharp" threshold: dihedral ≤ π − π/6  (i.e., bend ≥ 30°). Convex
// edges with smaller bends are rejected as suspected curve
// approximations (a 32-segment cylinder has ~11° bends → rejected;
// a cube has 90° bends → accepted). 30° was chosen as the boundary
// because a 12-segment cylinder also has 30° bends and is the
// coarsest "obviously curved" geometry we want to reject.
//
// Concave or non-manifold edges adjacent to a selected edge fail
// the predicate outright.
struct Tier1Result {
    bool        ok = false;
    std::string reason;          // empty when ok; populated on failure
};

// `selected_edges` are indices into `topo.edges`.
Tier1Result tier1_check(const FlatMesh& mesh,
                          const EdgeTopology& topo,
                          const std::vector<std::uint32_t>& selected_edges);

}  // namespace cadml::engine::detail
