// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/engine/flat_evaluator.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cadml::engine {

// ── Mass properties (volume / surface / COM / inertia) ────────────────
//
// Computed from triangle data via the divergence theorem and the
// Mirtich (1996) closed-form integration. No watertight check — the
// numbers are only meaningful when the mesh actually closes. The
// `is_watertight` heuristic flags grossly degenerate input but won't
// catch e.g. a missing single triangle on a vast otherwise-closed
// hull. Treat the heuristic as advisory, not certifying.
struct MassProperties {
    double volume_mm3        = 0;   // signed; sign reflects winding
    double surface_area_mm2  = 0;
    std::array<double, 3> center_of_mass{ 0, 0, 0 };

    // Inertia tensor (symmetric, 3×3) about the *origin* of the
    // mesh's local frame, row-major. Units:
    //   - When `density_kg_per_m3 == 0` (the geometry-only path), the
    //     tensor is the pure ∫(...) mm⁵ integral — i.e. mass-density
    //     has not been folded in. These are mm⁵ geometric moments.
    //   - When `density_kg_per_m3 > 0`, the integrals are multiplied
    //     by ρ in kg/mm³ so the result is in kg · mm².
    // `inertia_com` is the same tensor parallel-axis-shifted to the
    // centre of mass, in the same units.
    std::array<double, 9> inertia_origin{};   // row-major
    std::array<double, 9> inertia_com{};      // row-major (parallel-axis shifted)

    // Density × volume. When `density_kg_per_m3` is 0 (the default in
    // `flat_mass_properties`), `mass_kg = 0` and the caller is
    // expected to apply density externally.
    double density_kg_per_m3 = 0;
    double mass_kg           = 0;

    // Coarse heuristic: triangle_count > 0 and no degenerate
    // (zero-area) triangles encountered. NAMED `is_watertight` for
    // backward compatibility, but the value only confirms "no
    // degenerate triangles" — NOT topological closure. For
    // boundary-edge / closure analysis, use FlatTopologyAdjacency
    // from `flat_topology_adjacency()`, whose `watertight` field IS
    // the boundary-edge-count==0 check.
    bool is_watertight       = false;
    std::uint64_t degenerate_triangles = 0;
};

// `unit_to_mm` is the linear scale factor from the document's units
// to millimetres (see `cadml::units_to_mm_scale`). Defaults to 1.0
// — the historical assumption that the FlatMesh is already in mm.
// Pass the correct scale for `units in` / `units m` etc. to get
// real-unit volume / mass / inertia. The output fields keep their
// `_mm3` / `_mm2` / kg·mm² suffixes; they describe the physical
// unit, not the document's authoring unit.
MassProperties flat_mass_properties(const FlatMesh& mesh,
                                     double density_kg_per_m3 = 0,
                                     double unit_to_mm = 1.0);

// ── Bounding shapes ──────────────────────────────────────────────────
struct BoundingShapes {
    // Axis-aligned bounding box in the local frame.
    std::array<double, 3> aabb_min{ 0, 0, 0 };
    std::array<double, 3> aabb_max{ 0, 0, 0 };

    // **Principal-axes box**: PCA-aligned box whose axes are the
    // covariance eigenvectors. This is NOT the minimum-volume
    // oriented bounding box (OBB) — for an asymmetric mass
    // distribution the PCA axes point along the principal moments,
    // not the tightest enclosure, and the resulting box can exceed
    // the AABB. We deliberately don't call this "OBB" to avoid the
    // false promise; a rotating-calipers / Klee min-OBB would be a
    // separate refactor.
    //
    // `principal_box_axes` rows are the principal axes (unit
    // vectors); `principal_box_extents` are the half-widths in each
    // axis; `principal_box_center` is the centre point.
    std::array<double, 9> principal_box_axes{};
    std::array<double, 3> principal_box_extents{ 0, 0, 0 };
    std::array<double, 3> principal_box_center{ 0, 0, 0 };
    double                principal_box_volume_mm3 = 0;

    // Ritter's bounding sphere (small, fast, 5–10 % larger than the
    // optimal Welzl sphere). Sufficient for shipping volume / fixture
    // selection; use a tighter algorithm for collision.
    std::array<double, 3> sphere_center{ 0, 0, 0 };
    double                sphere_radius = 0;

    // Bounding cylinder per principal axis (X / Y / Z). For each axis:
    //   - cyl_axis_point[k]: the (x, y, z) point on the AABB midline
    //     orthogonal to axis k — i.e., the cylinder's axis runs
    //     parallel to axis k and passes through this point.
    //   - cyl_radius[k]: max distance from the axis line to any vertex.
    //   - cyl_height[k]: mirrors the AABB extent on axis k.
    //   - cyl_volume[k]: π · cyl_radius² · cyl_height.
    // The axis is anchored on the AABB MIDLINE (not the part's
    // centroid) so the {radius, height, axis-point} triplet fully
    // describes the cylinder in world coordinates.
    std::array<double, 3> cyl_radius{ 0, 0, 0 };   // [x_axis, y_axis, z_axis]
    std::array<double, 3> cyl_height{ 0, 0, 0 };
    std::array<double, 3> cyl_volume{ 0, 0, 0 };
    std::array<std::array<double, 3>, 3> cyl_axis_point{};   // [axis][xyz]
};

BoundingShapes flat_bounds(const FlatMesh& mesh);

// ── Per-element topology breakdown ───────────────────────────────────
//
// One entry per Document Node that contributed at least one triangle
// to `mesh`. Triangles' `triangle_node` are the keys.
struct FlatTopology {
    std::uint64_t triangles    = 0;     // whole-mesh aggregates
    std::uint64_t vertices     = 0;
    double        volume_mm3   = 0;
    double        surface_area_mm2 = 0;
    std::array<double, 3> bbox_min{ 0, 0, 0 };
    std::array<double, 3> bbox_max{ 0, 0, 0 };

    struct Element {
        std::uint32_t node_id       = 0;
        std::string   tag;            // "extrude", "circle", ...
        std::string   name;           // <... name="X"> when set
        std::uint64_t triangles     = 0;
        // Signed-tetrahedron volume contribution: positive for an
        // element whose triangles were added (a normal solid),
        // NEGATIVE for an element that contributed as a cutter inside
        // <difference>. A consumer summing per-element volumes
        // recovers the whole part's volume; CLI consumers should
        // surface the sign so users don't mistake the cutter row for
        // a negative-volume bug.
        double        volume_mm3   = 0;
        double        surface_area_mm2 = 0;
        std::array<double, 3> bbox_min{ 0, 0, 0 };
        std::array<double, 3> bbox_max{ 0, 0, 0 };
    };
    std::vector<Element> elements;
};

FlatTopology flat_topology(const FlatMesh& mesh, const Document& doc);

// ── Topology adjacency / manifold check ──────────────────────────────
//
// Half-edge walk over the mesh: counts shells, classifies edges
// (manifold / boundary / non-manifold), and from the largest shell
// estimates the surface genus via the Euler characteristic χ = V-E+F
// (g = (2 - χ) / 2 for closed orientable manifolds).
//
// Watertight ⇔ every edge is shared by exactly two triangles AND no
// boundary edges. Manifold ⇔ no non-manifold edges (an edge shared
// by ≥3 triangles is non-manifold; boundary edges are not part of
// the manifold check, only of watertightness).
struct TopologyAdjacency {
    bool          watertight       = false;
    bool          manifold         = false;
    std::uint32_t shell_count      = 0;
    std::uint64_t boundary_edges   = 0;
    std::uint64_t non_manifold_edges = 0;
    double        min_edge_len_mm  = 0;
    double        max_edge_len_mm  = 0;
    int           genus_estimate   = 0;   // -1 when not computable
};

TopologyAdjacency flat_topology_adjacency(const FlatMesh& mesh);


// ── File-to-file diff ────────────────────────────────────────────────
struct DiffEntry {
    std::string part;
    bool   in_a = false;
    bool   in_b = false;
    double volume_a   = 0;
    double volume_b   = 0;
    double surface_a  = 0;
    double surface_b  = 0;
    std::uint64_t triangles_a = 0;
    std::uint64_t triangles_b = 0;
    // Sum of |center_a - center_b| component diffs — small means the
    // part stayed put even if its volume changed.
    double center_shift_mm = 0;
};

struct DiffReport {
    std::vector<DiffEntry> entries;          // alphabetical by part name
    double total_volume_a = 0;
    double total_volume_b = 0;
};

DiffReport flat_diff(const FlatEvalResult& a, const FlatEvalResult& b);

// ── Measurement probes (bbox + element-pair distances) ───────────────
//
// Element-keyed probes against a single part's FlatMesh. For
// distance kinds we walk every vertex tagged with element_a against
// every vertex tagged with element_b — O(|A|*|B|), brute force; fine
// at cookbook scale, swap to a BVH if it ever becomes a bottleneck.
//
// Source attribution (element_a / element_b) comes from
// `FlatMesh::triangle_node`. Run cadmltopo first to discover ids.
enum class MeasureKind {
    Bbox,            // axis-aligned bbox of one element's triangles
    DistanceMin,     // min vertex-to-vertex distance between two elements
    DistanceMean,    // arithmetic mean of all pairwise vertex distances
    DistanceMax,     // max vertex-to-vertex distance (diameter of pair)
};

struct MeasureProbe {
    MeasureKind kind;
    std::uint32_t element_a = 0;
    std::uint32_t element_b = 0;        // ignored for Bbox
};

struct MeasureItem {
    std::string kind;
    std::uint32_t element_a = 0;
    std::uint32_t element_b = 0;
    bool ok = false;
    std::string error;

    // Bbox outputs.
    std::array<double, 3> bbox_min{ 0, 0, 0 };
    std::array<double, 3> bbox_max{ 0, 0, 0 };
    std::array<double, 3> size{ 0, 0, 0 };

    // Distance outputs.
    double distance = 0;            // min/mean/max depending on kind
    std::uint64_t pair_count = 0;   // |A| * |B| (DistanceMean / DistanceMax)
};

struct [[nodiscard]] MeasureResult {
    std::vector<MeasureItem> items;
};

MeasureResult flat_measure(const FlatMesh& mesh,
                            const std::vector<MeasureProbe>& probes);

// ── Cylindrical-feature inventory (taps / drills BOM) ────────────────
//
// Walks the Document looking for `<circle>` primitives that get
// extruded under a `<difference>` child position — that pattern is
// the canonical "drill a hole" idiom in CADML. Each match becomes a
// HoleEntry with the circle's diameter and the extrude's height
// (the depth). For revolved bores (a `<circle>` inside an
// `<intersect>` with an axis-aligned slab, or a `<difference>` with
// a `<revolve>`) the heuristic is best-effort — see HoleEntry::role.
//
// Limits — this is a structural walk, not a Manifold extraction:
//   * Diameters / depths come from the resolved param values; if a
//     diameter expression couldn't be evaluated to a number, the
//     entry is still emitted with `diameter_mm = 0` and a non-empty
//     `error_hint`.
//   * Holes lowered through `<for>` / `<pattern>` count as one
//     entry per unrolled instance (compiler-expanded by the time
//     the document reaches this pass).
struct HoleEntry {
    std::uint32_t node_id = 0;        // the <circle> Node id
    std::string   part_name;          // owning part (or "<def:...>")
    std::string   role;               // "drilled" / "bore" / "clearance"
    double        diameter_mm = 0;
    double        depth_mm    = 0;    // 0 if no enclosing extrude
    std::string   axis;               // e.g. "+z" / "x" / "y" — extrude direction
    std::string   error_hint;         // populated when a number couldn't resolve
};

struct HoleReport {
    std::vector<HoleEntry> entries;   // doc-order, then part-grouped
};

HoleReport flat_holes(const Document& doc);

// ── Wall-thickness analysis ──────────────────────────────────────────
//
// For each vertex, cast a ray inward (along the negated per-vertex
// normal) and find the distance to the nearest non-coincident
// triangle hit. That distance is the local wall thickness — the
// minimum across the surface flags risk areas for sheet metal,
// castings, or 3D printing (where walls below ~0.8 mm tend to fail).
//
// Brute-force scan: O(verts × tris) per part. Adequate for parts up
// to a few thousand triangles; a BVH-accelerated variant is the
// natural follow-up if it ever becomes a hot path.
//
// Vertices whose ray exits the body without re-hitting it (e.g. on a
// concave outer corner) get reported with `samples_with_no_hit > 0`
// and are excluded from the percentile statistics.
//
// **Sharp-corner caveat**: at a sharp convex corner (cube corner
// vertex) the averaged vertex normal points diagonally and the
// inward ray traverses the body diagonal — for a 10 mm cube the
// corner-vertex samples report ~17.3 mm instead of the true 10 mm
// minimum wall thickness. The implementation skips vertices whose
// pre-normalised normal magnitude is below a threshold (catches
// vertices where the contributing triangle normals canceled), but
// the cube-corner case still slips through. Treat min_mm / p1_mm /
// p10_mm as advisory for parts with many sharp external corners;
// the percentile statistics are biased upward by corner inflation.
// A per-triangle-centroid sampling mode is on the 0.2 roadmap.
struct WallThicknessOptions {
    // Coincident-triangle hits closer than this value are treated as
    // the originating triangle and ignored — keeps a vertex from
    // "hitting itself". The field is named `_mm` for the common
    // case, but the value is compared against the engine's raw
    // DOC-UNIT coordinates — callers that drive analysis on a file
    // declared `units in` should pre-scale by units_to_mm_scale
    // (the cadmlwalls CLI does this).
    double min_thickness_mm = 1e-4;

    // Cap how many vertex samples we scan. 0 = scan every vertex.
    // Useful for VERY large meshes where the brute-force scan would
    // be excessive — sample 1024 random vertices instead and infer.
    std::uint32_t max_samples = 0;

    // When > 0, fill WallThicknessReport::hotspots with the N
    // thinnest sample locations (xyz + thickness + owning node).
    // Lets callers flag specific positions to thicken.
    std::uint32_t hotspot_count = 0;
};

struct WallThicknessSample {
    std::array<double, 3> xyz{0,0,0};
    double                thickness_mm = 0;
    std::uint32_t         node_id      = 0;   // owning element (NO_NODE if unknown)
};

struct WallThicknessReport {
    std::uint64_t samples            = 0;   // vertices actually scanned
    std::uint64_t samples_with_hit   = 0;
    std::uint64_t samples_with_no_hit = 0;  // ray escaped the body
    double min_mm   = 0;
    double p1_mm    = 0;     // 1st percentile (thinnest 1 % of walls)
    double p10_mm   = 0;
    double median_mm = 0;
    double mean_mm  = 0;
    double max_mm   = 0;

    // Thinnest N samples, sorted ascending. Populated when
    // WallThicknessOptions::hotspot_count > 0. Empty otherwise.
    std::vector<WallThicknessSample> hotspots;
};

WallThicknessReport flat_wall_thickness(const FlatMesh& mesh,
                                         const WallThicknessOptions& opts = {});

}  // namespace cadml::engine
