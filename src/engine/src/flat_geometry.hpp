// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/engine/flat_evaluator.hpp>   // for FlatMesh
#include <cadml/selector.hpp>                // Selector (spec §13)
#include <cadml/types.hpp>                   // libcadml — Vec2, Vec3

#include <array>
#include <cstdint>
#include <vector>

namespace cadml::engine::detail {

// Closed 2D polygon, point sequence in counter-clockwise order
// (so that linear extrude produces outward-facing side normals).
struct Polygon2D {
    std::vector<Vec2> points;

    bool empty() const { return points.empty(); }
};

// True iff `p` is a simple convex polygon. Used by `<shell>`'s
// Tier-1 predicate (offset-based shelling can't handle concavity)
// — NOT used as a guard for extrude/revolve any more, since their
// cap triangulator now handles arbitrary simple polygons via
// ear-clipping (see `triangulate_polygon` below). Returns true for
// empty / degenerate polygons so the caller's empty-input guard
// still fires first.
//
// Algorithm: walk consecutive edges, check that all cross products
// have the same sign (with tolerance). Collinear runs are allowed.
bool is_convex(const Polygon2D& p);

// Signed area of a simple polygon. Positive → CCW, negative → CW,
// zero → degenerate / self-intersecting / collinear. Used by
// ensure_ccw and by callers that want to detect winding without
// modifying the polygon (e.g., to surface a "your path is wound
// CW" diagnostic to the user).
double polygon_signed_area(const Polygon2D& p);

// In-place canonicalisation: if `p` is wound clockwise (negative
// signed area), reverse it so it's CCW. No-op for already-CCW
// inputs and for degenerate (<3-vertex) polygons. Called at the
// entry of extrude_linear / revolve so user-authored CW polygons
// extrude correctly: CCW is the convention the side-face emitter
// and the ear-clipper both depend on. Particularly important for
// content inside a `<svg>` block, where the SVG y-down convention
// inverts winding relative to CADML's y-up math semantics.
void ensure_ccw(Polygon2D& p);

// Triangulate a CCW simple polygon by ear-clipping. Returns a list
// of (i, j, k) index triples into `poly.points`. Each triangle is
// CCW (matching the input orientation), so callers using the
// indices for the +Z-facing cap of an extrude get correct outward
// winding for free; the -Z cap should swap two indices to flip.
//
// For convex inputs the algorithm degenerates to a fan-like
// triangulation (every vertex is an ear). For concave inputs it
// finds the right triangles. For self-intersecting polygons no
// ear can be found at some point and the function returns
// whatever it produced so far — the caller can detect the
// truncation by comparing `out.size()` against the expected
// `N - 2`. Empty / 1-vertex / 2-vertex inputs return empty.
//
// Complexity O(N²) — fine for typical CADML profiles (10s of
// vertices). For very-high-vertex profiles (e.g., hundreds of
// path segments) a sweep-line triangulator would be better.
std::vector<std::array<std::uint32_t, 3>>
triangulate_polygon(const Polygon2D& poly);

// ─── Profile tessellation (NodeType::Rect / Circle) ────────────────────

// `rx` / `ry` enable rounded corners (SVG-ish semantics). Pass 0 for
// either to get sharp corners on that axis; ry<=0 mirrors rx so a
// single-radius rounded rect just needs `rx`. `corner_segments` is
// the number of arc steps per corner (default 8 = 32 vertices around
// the rect; tweak up for visual smoothness, down for triangle budget).
// Both rx and ry get clamped to half their respective edge so adjacent
// corners can't overlap and produce a self-intersecting polygon.
Polygon2D tessellate_rect(double x, double y,
                            double width, double height,
                            double rx = 0, double ry = 0,
                            int corner_segments = 8);

// `segments` is the number of sides on the polygonal approximation
// of the circle. The default `0` means "adaptive" — the engine picks
// a count that keeps the chord-to-arc sagitta below a fixed tolerance
// (~0.02 mm) and clamps to [8, 256] so tiny holes don't go below
// triangle-mesh and giant flanges don't blow up the triangle count.
// Pass an explicit positive integer to override (e.g. for a part
// where the polygonal facets are part of the design).
Polygon2D tessellate_circle(double cx, double cy, double radius,
                              int segments = 0);

// SVG-style `<path d="...">` tessellator. Supported commands:
//
//   M / m   moveto (absolute / relative). Starts a new subpath; for
//           CADML use cases this just sets the first vertex.
//   L / l   lineto. Multiple coordinate pairs after a single M or L
//           are treated as implicit linetos (SVG semantics).
//   H / h   horizontal lineto.
//   V / v   vertical lineto.
//   C / c   cubic Bezier (3 control points).
//   S / s   smooth cubic — reflects previous cubic's last control
//           point as the implicit first control point.
//   Q / q   quadratic Bezier (1 control point).
//   T / t   smooth quadratic — reflects previous quadratic's
//           control point.
//   A / a   elliptical arc (rx, ry, x-axis-rotation, large-arc-flag,
//           sweep-flag, x, y) per SVG 1.1 endpoint parameterization.
//   Z / z   close path. No-op for Polygon2D since the polygon is
//           implicitly closed by the consumer (extrude, revolve).
//           The pen is reset to the current subpath's start so
//           relative commands after Z compose correctly.
//
// Curves are flattened into line segments via adaptive de Casteljau
// subdivision; arcs are converted to ≤ 90° cubic Bezier segments and
// run through the same flattener. Subdivision tolerance is
// `cadml::kPathTolerance` (= `kCircleSagitta` = 0.005 doc units) so
// `<path>` curves and `<circle>` tessellate to matching smoothness.
// Any other command terminates parsing cleanly — caller sees whatever
// was emitted up to that point.
//
// Out-of-bounds inputs (no commands, no points, malformed numbers)
// produce an empty polygon. The caller is responsible for
// downstream null-mesh handling (extrude on an empty profile already
// emits a warning).
Polygon2D tessellate_path(std::string_view d);

// ─── 3D primitives ────────────────────────────────────────────────────

// Linear extrude `profile` along +Z by `height`, producing a closed
// triangle mesh. Top + bottom caps are fan-tessellated from the
// centroid; sides are quad-strips split into triangles.
//
// `source_node` is the index of the originating node in the document
// — written into FlatMesh::triangle_node for source attribution.
FlatMesh extrude_linear(const Polygon2D& profile,
                          double height,
                          std::uint32_t source_node);

// Polyhedral loft over a sequence of pre-positioned 3D rings with
// matching vertex counts. Each LoftSection carries:
//   - profile_2d: the original CCW polygon in the section's own 2D
//     plane (used for ear-clipping the start/end caps).
//   - ring_3d: that polygon mapped to 3D positions via the
//     section's own frame (origin + right + up).
// All ring_3d sequences must have the same vertex count P. Caller
// is responsible for ensure_ccw on each profile_2d before mapping.
//
// Side faces are quad strips between consecutive rings using
// closest-by-index vertex pairing (suitable for loft sections that
// don't twist drastically between neighbours; large twists may
// produce oblique side triangles but still topologically valid
// surface).
//
// Caps: triangulate the FIRST section's profile_2d for the start
// cap and the LAST section's profile_2d for the end cap (each
// section can have a different polygon shape — compressor blades
// have airfoils that morph station-by-station). Cap winding chosen
// by a tangent dot-product test so caps face outward regardless
// of sketch orientation.
struct LoftSection {
    Polygon2D         profile_2d;
    std::vector<Vec3> ring_3d;
};

FlatMesh loft_polyhedral(const std::vector<LoftSection>& sections,
                          std::uint32_t source_node);

// Sweep `profile` along a helical guide curve. The helix lies along
// the +Z axis, parameterized by t ∈ [0, turns]:
//
//   x(t) = (radius + taper*t) * cos(2π * t * direction_sign)
//   y(t) = (radius + taper*t) * sin(2π * t * direction_sign)
//   z(t) = pitch * t
//
// where `direction_sign` is +1 for "ccw" and -1 for "cw".
//
// At each sample frame, the 2D `profile` is placed in 3D using the
// "axis-perpendicular" convention: profile.x maps to the outward
// radial direction (away from the helix axis), profile.y maps to
// +Z. This is the standard convention for thread-modeling — the
// cross-section stays parallel to the helix axis at every step,
// so a triangular thread profile produces a clean spiral groove.
//
// `segments_per_turn` controls polyline approximation density of
// the helical centerline. Total sample count = ceil(segments_per_turn
// * turns) + 1. Sides emitted as N-1 quad rings. Start + end caps
// share the same ear-clipping triangulation, with windings chosen
// so the start cap faces -tangent and the end cap faces +tangent.
//
// Empty / degenerate inputs (turns ≤ 0, profile empty) return an
// empty mesh; callers should check & warn.
FlatMesh sweep_along_helix(const Polygon2D& profile,
                            double radius, double pitch,
                            double turns, double taper,
                            int direction_sign,
                            int segments_per_turn,
                            std::uint32_t source_node);

// Revolve `profile` (in the XY plane) around `axis` by `angle_deg`
// degrees. `segments` is the count for a full 360° revolution; for
// partial angles the count scales proportionally (min 3).
//
// For full revolution (angle ≈ 360°) the surface closes on itself.
// For partial angles, start and end caps are added (copies of the
// profile rotated to those angles, fan-tessellated from the centroid).
FlatMesh revolve(const Polygon2D& profile,
                  Vec3 axis,
                  double angle_deg,
                  int segments,
                  std::uint32_t source_node);

// ─── Modifiers ────────────────────────────────────────────────────────
//
// Source attribution across modifiers: output triangles can carry
// attribution to either the modifier's `source_node` (newly-created
// surfaces — bevels, cylinders, cavities, caps) OR the input
// primitive's source node (surviving original surfaces). Manifold's
// faceID round-trip preserves whichever input owned each triangle, so
// an output mesh with mixed attribution is normal — click-to-source
// from a rendered triangle navigates to whichever originator was
// closest. This is intentional; consumers should not assume uniform
// attribution per modifier.

struct ModifierResult {
    FlatMesh    mesh;
    bool        ok = false;
    std::string error;
    // Non-fatal diagnostic surfaced to the caller's warning channel
    // (e.g. a selector that matched zero edges — spec §13.5). Empty
    // when there is nothing to report.
    std::string warning;
};

// Native Tier 1 shell: hollow out a `<extrude>`-produced solid by
// computing the 2D inward offset of its profile via Clipper2,
// re-extruding both outer and inner profiles, and subtracting the
// inner from the outer.
//
// `profile`     — the original 2D profile (must be convex per Tier 1).
// `height`      — extrusion height of the source `<extrude>`.
// `thickness`   — wall thickness; must be positive and small enough
//                 that the inward offset is non-empty.
// `open_start`  — true iff the bottom face (z=0) is open (no cap).
// `open_end`    — true iff the top face (z=height) is open.
//
// Implementation:
//   - Run Clipper2 inflate by -thickness on the polygon.
//   - Build outer mesh = extrude_linear(profile, height).
//   - Build inner mesh = extrude_linear(offset_profile, height') where
//     height' / z-range is extended past the open faces so the
//     subtract removes those caps too.
//   - Result = outer - inner via Manifold.
//
// Errors out via `ok=false`/`error` on:
//   - non-convex profile (Tier 1 requirement);
//   - thickness ≤ 0;
//   - thickness too large for the profile (Clipper2 returns empty);
//   - Manifold boolean failures.
ModifierResult shell_extrude(const Polygon2D& profile,
                              double height,
                              double thickness,
                              bool open_start,
                              bool open_end,
                              std::uint32_t source_node);

// Native Tier 1 fillet: round every convex edge of `input` with a
// quarter-cylinder of radius `radius`, tangent to both adjacent
// faces. Like chamfer this is CSG-based — for each convex edge,
// build a "fillet cutter" (parallelogram prism along the edge,
// MINUS a cylinder of `radius` tangent to both faces). Union all
// cutters, subtract from input.
//
// **Corner geometry caveat**: at a vertex where 3 fillet edges
// meet, the three quarter-cylinders intersect to form a Steinmetz-
// solid-style patch, not a true spherical octant. For most CAD
// inputs the visual difference is negligible; the plan calls for
// spherical octants strictly, which would need additional
// per-corner sphere cutters. Tracked for follow-up.
//
// Errors out via `ok=false`/`error` on:
//   - `radius` < 0  (negative not supported).
//   - Tier 1 predicate failure.
//   - Manifold boolean failures.
//   - Empty result (`radius` too large for the geometry).
// `sel` restricts which convex edges are rounded (spec §13). The default
// `all` selector rounds every convex edge (the historical behaviour). A
// selector that matches no edge leaves the mesh unmodified and reports
// via ModifierResult::warning (spec §13.5). A face-scoped selector is
// rejected by the bundler before reaching here.
ModifierResult fillet(const FlatMesh& input,
                       double radius,
                       const Selector& sel,
                       std::uint32_t source_node);

// Native Tier 1 chamfer: planar bevels along every convex edge of
// `input`, with miter patches at corners. `width` is the chamfer
// distance measured in each adjacent face from the edge.
//
// Implementation: for each convex edge, build a triangular prism
// representing the material to remove (cross-section is a right
// triangle with legs of length `width` along each face's in-plane
// perpendicular to the edge). Union all prisms via Manifold, then
// subtract from `input`. Manifold's CSG handles the corner miter
// patches automatically — at a 3-edge cube corner the three prisms
// overlap, the union is a corner wedge, and the subtraction leaves
// a triangular miter patch.
//
// The CSG approach here builds the same prism geometry regardless of
// the adjacent faces' angle, so it composes correctly for any sharp-
// corner input the Tier 1 predicate accepts (cube, hex prism,
// right-triangle prism, ...). The Tier 1 predicate still rejects
// curve-approximation segments (~30° threshold), so curvy inputs are
// rejected up front.
//
// Errors out via `ok=false`/`error` on:
//   - `width` < 0  (negative distance not supported).
//   - Tier 1 predicate failure (curves, concave, non-manifold).
//   - Manifold boolean failures.
//   - Empty result (`width` too large for the geometry — the cutter
//     union consumed the whole input).
// `sel` restricts which convex edges are bevelled (spec §13); see the
// note on fillet() above — same selector semantics.
ModifierResult chamfer(const FlatMesh& input,
                        double width,
                        const Selector& sel,
                        std::uint32_t source_node);

// ─── Boolean ops (via Manifold) ───────────────────────────────────────

enum class BoolOp { Union, Difference, Intersect };

struct BoolResult {
    FlatMesh    mesh;
    bool        ok = false;
    std::string error;
};

// Combine an ordered sequence of meshes with the given op. Reduces
// left-to-right: union/intersect are associative; difference is
// `m[0] - m[1] - m[2] - ...` (the standard CSG convention).
//
// Empty input list returns ok with an empty mesh. A single input
// returns it unchanged. On Manifold failure (non-manifold input,
// validation errors, etc.) ok=false and `error` is filled.
BoolResult boolean_combine(const std::vector<FlatMesh>& inputs,
                            BoolOp op,
                            std::uint32_t source_node);

// Convex hull of a set of input meshes. With one input returns the
// hull of that mesh (useful for hulling around protrusions). With
// multiple inputs returns the hull spanning all of them — this is
// the underlying primitive for `<hull>` and the way to express a
// 2-section convex loft (place each profile in a thin extrude at
// the desired z, hull the pair).
//
// Empty input list returns ok with an empty mesh. Non-manifold or
// degenerate inputs surface as ok=false with `error` filled.
BoolResult hull_combine(const std::vector<FlatMesh>& inputs,
                         std::uint32_t source_node);

// ─── Interference measurement ─────────────────────────────────────────

struct IntersectVolumeResult {
    double      volume = 0.0;     // signed volume of `a ∩ b`; 0 = no overlap
    bool        ok = false;
    std::string error;
};

// Compute the volume of `a ∩ b` via Manifold's Boolean+Volume. Used
// by cadml_check to flag pairwise part interference.
//
// Returns `ok = true` with `volume = 0` for disjoint inputs (the
// intersection is empty — a legitimate outcome). Returns `ok = false`
// with `error` on Manifold validation failures (non-manifold input,
// etc.).
IntersectVolumeResult intersect_volume(const FlatMesh& a, const FlatMesh& b);

}  // namespace cadml::engine::detail
