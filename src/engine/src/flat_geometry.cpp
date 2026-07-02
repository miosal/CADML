// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include "flat_geometry.hpp"
#include "flat_edge_topology.hpp"
#include "flat_modifier_tolerances.hpp"

#include <manifold/manifold.h>
#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace cadml::engine::detail {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Append a triangle with vertices a/b/c (each pre-pushed to vertices),
// recording the source node for attribution.
void emit_triangle(FlatMesh& m,
                    std::uint32_t a, std::uint32_t b, std::uint32_t c,
                    std::uint32_t source_node)
{
    m.indices.push_back(a);
    m.indices.push_back(b);
    m.indices.push_back(c);
    m.triangle_node.push_back(source_node);
}

}  // namespace

// ─── is_convex ─────────────────────────────────────────────────────────

bool is_convex(const Polygon2D& p) {
    const auto N = p.points.size();
    if (N < 3) return true;        // degenerate, no caller cares
    int sign = 0;                  // 0 = unset; +1 / -1 once seen
    constexpr double kEps = 1e-9;
    for (std::size_t i = 0; i < N; ++i) {
        const Vec2 a = p.points[i];
        const Vec2 b = p.points[(i + 1) % N];
        const Vec2 c = p.points[(i + 2) % N];
        const Vec2 ab = b - a;
        const Vec2 bc = c - b;
        const double z = ab.cross(bc);
        if (std::abs(z) < kEps) continue;        // collinear — skip
        const int s = (z > 0) ? +1 : -1;
        if (sign == 0)        sign = s;
        else if (sign != s)   return false;       // sign flip = concave
    }
    return true;
}

// ─── ensure_ccw ──────────────────────────────────────────────────────
//
// Compute the signed area; if the polygon is CW (negative area),
// reverse it in place so it's CCW. Both extrude_linear's side-face
// emission AND triangulate_polygon's ear test require CCW input.
//
// Why this matters in practice: a polygon authored in SVG mental
// space (y-down) is CW in CADML's y-up math semantics. Without
// canonicalising, ear-clipping bails (every vertex looks reflex)
// and side faces emit inward-facing triangles. Cheap O(N) check
// + a vector reverse — runs once per profile.

double polygon_signed_area(const Polygon2D& p) {
    double a = 0;
    const auto N = p.points.size();
    for (std::size_t i = 0; i < N; ++i) {
        const auto& v0 = p.points[i];
        const auto& v1 = p.points[(i + 1) % N];
        a += v0.x * v1.y - v1.x * v0.y;
    }
    return a * 0.5;
}

void ensure_ccw(Polygon2D& p) {
    if (p.points.size() < 3) return;
    if (polygon_signed_area(p) < 0) {
        std::reverse(p.points.begin(), p.points.end());
    }
}

// ─── triangulate_polygon (ear clipping) ───────────────────────────────
//
// Standard O(N²) ear-clipping for a simple CCW polygon. Replaces the
// centroid-fan that used to live in extrude_linear and revolve, which
// only worked for convex profiles — concave shapes (L-bracket,
// U-channel, plus, gear teeth) silently rendered with crossed cap
// triangles before. Ear-clipping handles arbitrary simple polygons,
// including the convex case (every vertex is an ear so it produces a
// fan-shaped result).

namespace {

// Signed double-area of triangle (a, b, c). Positive → CCW.
double tri_area2(Vec2 a, Vec2 b, Vec2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Whether `p` lies inside (or on the boundary of) triangle (a, b, c).
// Used as a guard inside the ear test: if any *other* polygon vertex
// is inside the candidate ear, that ear is invalid. Boundary points
// also disqualify so the algorithm doesn't carve off triangles whose
// edges pass through other polygon vertices.
bool point_in_triangle(Vec2 p, Vec2 a, Vec2 b, Vec2 c) {
    const double s1 = tri_area2(a, b, p);
    const double s2 = tri_area2(b, c, p);
    const double s3 = tri_area2(c, a, p);
    const bool all_nonneg = (s1 >= 0 && s2 >= 0 && s3 >= 0);
    const bool all_nonpos = (s1 <= 0 && s2 <= 0 && s3 <= 0);
    return all_nonneg || all_nonpos;
}

}  // namespace

std::vector<std::array<std::uint32_t, 3>>
triangulate_polygon(const Polygon2D& poly) {
    std::vector<std::array<std::uint32_t, 3>> out;
    const auto N = static_cast<std::uint32_t>(poly.points.size());
    if (N < 3) return out;
    if (N == 3) { out.push_back({ 0u, 1u, 2u }); return out; }

    // Doubly-linked list of remaining vertex indices into poly.points.
    // Using arrays of indices (rather than rebuilding the polygon) keeps
    // the output indices stable to the input numbering, which matters:
    // callers (extrude_linear / revolve / cap-emit in revolve) reuse
    // these indices to address into a vertex ring, so any renumbering
    // here would silently corrupt cap windings.
    std::vector<std::uint32_t> prev_v(N), next_v(N);
    for (std::uint32_t i = 0; i < N; ++i) {
        prev_v[i] = (i + N - 1) % N;
        next_v[i] = (i + 1) % N;
    }

    // Convex at v iff cross(prev→v, v→next) > 0 (CCW polygon).
    // Reflex (concave) vertices are NEVER ears.
    auto is_convex_at = [&](std::uint32_t v) {
        const auto& a = poly.points[prev_v[v]];
        const auto& b = poly.points[v];
        const auto& c = poly.points[next_v[v]];
        return tri_area2(a, b, c) > 0.0;
    };

    auto is_ear = [&](std::uint32_t v) {
        if (!is_convex_at(v)) return false;
        const auto& a = poly.points[prev_v[v]];
        const auto& b = poly.points[v];
        const auto& c = poly.points[next_v[v]];
        // Walk every OTHER remaining vertex; if any reflex vertex sits
        // inside (a, b, c), the ear is invalid (cutting it would
        // produce a triangle that overlaps the rest of the polygon).
        // Optimisation: only reflex vertices need to be tested — convex
        // ones can never be inside a same-polygon triangle. We skip the
        // check for simplicity (N is typically small).
        for (std::uint32_t k = next_v[next_v[v]]; k != prev_v[v]; k = next_v[k]) {
            if (point_in_triangle(poly.points[k], a, b, c)) return false;
        }
        return true;
    };

    std::uint32_t remaining = N;
    std::uint32_t v = 0;
    while (remaining > 3) {
        bool found = false;
        // One full sweep — if no ear is found in `remaining` candidates
        // the polygon is non-simple (self-intersecting / wound CW /
        // duplicate vertices) and we bail out with a partial result.
        for (std::uint32_t scan = 0; scan < remaining; ++scan) {
            if (is_ear(v)) {
                out.push_back({ prev_v[v], v, next_v[v] });
                next_v[prev_v[v]] = next_v[v];
                prev_v[next_v[v]] = prev_v[v];
                v = next_v[v];
                --remaining;
                found = true;
                break;
            }
            v = next_v[v];
        }
        if (!found) return out;
    }
    out.push_back({ prev_v[v], v, next_v[v] });
    return out;
}

// ─── tessellate_rect ───────────────────────────────────────────────────

Polygon2D tessellate_rect(double x, double y, double width, double height,
                            double rx, double ry, int corner_segments)
{
    Polygon2D p;
    if (width <= 0 || height <= 0) return p;

    // Clamp rx/ry to half the relevant edge so adjacent corners
    // don't overlap and produce a self-intersecting polygon. SVG-ish
    // semantics: ry==0 mirrors rx (a single-radius rounded rect).
    if (ry <= 0) ry = rx;
    rx = std::max(0.0, std::min(rx, width  * 0.5));
    ry = std::max(0.0, std::min(ry, height * 0.5));

    // Sharp corners: emit the bare 4-vertex rectangle so output
    // stays byte-identical across runs.
    if (rx == 0 && ry == 0) {
        p.points.push_back({ x,         y         });
        p.points.push_back({ x + width, y         });
        p.points.push_back({ x + width, y + height});
        p.points.push_back({ x,         y + height});
        return p;
    }

    // Rounded: emit `corner_segments` arc points per corner. Each
    // corner sweeps 90°; the four corners together close the path.
    // Order is CCW: bottom-right → top-right → top-left → bottom-left.
    if (corner_segments < 1) corner_segments = 8;
    const int steps = corner_segments;
    p.points.reserve(steps * 4);

    auto arc = [&](double cx, double cy, double a0) {
        // Quarter-arc starting at angle a0, sweeping +π/2 CCW.
        for (int i = 0; i <= steps; ++i) {
            const double t = a0 + (kPi * 0.5) * (double(i) / double(steps));
            p.points.push_back({ cx + rx * std::cos(t),
                                  cy + ry * std::sin(t) });
        }
    };

    arc(x + width  - rx, y          + ry, -kPi * 0.5);  // bottom-right
    arc(x + width  - rx, y + height - ry,  0.0);        // top-right
    arc(x          + rx, y + height - ry,  kPi * 0.5);  // top-left
    arc(x          + rx, y          + ry,  kPi);        // bottom-left
    return p;
}

// ─── tessellate_circle ────────────────────────────────────────────────

Polygon2D tessellate_circle(double cx, double cy, double radius,
                              int segments)
{
    Polygon2D p;
    if (radius <= 0) return p;

    // Adaptive default: pick `segments` so the maximum chord-to-arc
    // deviation (sagitta) stays below kCircleSagitta. For a regular
    // N-gon inscribed in a circle of radius r, sagitta s satisfies
    // s = r * (1 − cos(π/N)), so
    //   N >= π / acos(1 − s/r).
    // Clamp to [kMinSegments, kMaxSegments] so tiny radii don't slip
    // below a usable triangle count and very large radii don't
    // explode the mesh. The tolerance is expressed in the document's
    // OWN coordinate units: CADML's `units` declaration is a pure label
    // and triggers no internal scaling (see docs/spec/coordinate-system.md),
    // so a `units in` document tessellates to 0.005 *inch*, not 0.005 mm.
    // The worked figures below assume the common `units mm` case.
    // Sagitta tolerance picked so faceting is invisible at typical
    // viewer zoom. 0.005 gives N=64 at r=4 (a typical M8 shank in mm),
    // N=89 at r=8, and saturates the clamp around r=80. The bound lives
    // in geometry coordinates, not pixels, because CADML is a CAD format
    // — tessellation is a property of the geometry, not of any display.
    // Rendering quality and triangle budget are different concerns from
    // "is this circle round enough to act like a circle in downstream
    // booleans."
    constexpr double kCircleSagitta = 0.005;  // document units (units is a pure label)
    constexpr int    kMinSegments   = 8;
    constexpr int    kMaxSegments   = 256;
    int seg = segments;
    if (seg <= 0) {
        if (radius <= kCircleSagitta) {
            seg = kMinSegments;
        } else {
            const double cos_arg = 1.0 - kCircleSagitta / radius;
            const double n = kPi / std::acos(cos_arg);
            seg = static_cast<int>(std::ceil(n));
        }
        if (seg < kMinSegments) seg = kMinSegments;
        if (seg > kMaxSegments) seg = kMaxSegments;
    }
    if (seg < 3) return p;

    p.points.reserve(seg);
    for (int i = 0; i < seg; ++i) {
        const double t = (2.0 * kPi * i) / seg;
        p.points.push_back({ cx + radius * std::cos(t),
                              cy + radius * std::sin(t) });
    }
    return p;
}

// ─── tessellate_path ──────────────────────────────────────────────────
//
// Mini SVG-path parser supporting the geometric subset commonly used
// in real-world SVG assets:
//
//   M / m  moveto (absolute / relative). Implicit-lineto chain
//          for trailing coordinate pairs.
//   L / l  lineto.
//   H / h  horizontal lineto.
//   V / v  vertical lineto.
//   C / c  cubic Bezier (3 control points).
//   S / s  smooth cubic — reflects previous cubic's last control point.
//   Q / q  quadratic Bezier (1 control point).
//   T / t  smooth quadratic — reflects previous quadratic's control point.
//   A / a  elliptical arc (rx, ry, x-axis-rotation, large-arc, sweep, x, y).
//   Z / z  close path (no-op for Polygon2D — caller closes implicitly).
//
// Curves are flattened into line segments via adaptive de Casteljau
// subdivision; arcs are converted to a sequence of ≤ 90° cubic
// Beziers (the standard cubic-approximation-of-arcs trick) and run
// through the same flattener. The subdivision tolerance is the SAME
// chord-deviation bound as the circle sagitta (kCircleSagitta = 0.005
// document units), so an arc drawn via <path> flattens as finely as the
// same arc drawn via <circle> — curve quality is independent of which
// primitive produced it. Could be exposed as a `<path>` attribute later
// if real assets demand finer control.
//
// Out-of-grammar tokens (e.g. an unrecognised command letter)
// terminate parsing cleanly; the caller sees whatever was emitted
// up to that point. Empty / malformed inputs return an empty
// polygon. Subpath chaining (multiple M's) is honoured for the
// pen-position semantics but the result is still flattened into
// one Polygon2D — the current consumer (extrude / revolve) can't
// represent multi-loop polygons anyway.

namespace {

// Local aliases for the public constants — kept so existing
// references at this scope (e.g. `kPathTolerance` in flatten_bezier)
// resolve without qualifying. Both MUST match the header's values.
using ::cadml::kPathTolerance;
constexpr int    kMaxSubdivDepth = ::cadml::kBezierMaxSubdivDepth;

// Walk one number out of `s` starting at `pos`. Skips leading
// whitespace and commas. Returns nullopt at end-of-string or if
// the next non-separator char isn't a number-start. Advances `pos`
// past whatever it consumed (only on success — failure leaves
// `pos` at the offending non-numeric character).
std::optional<double> next_number(std::string_view s, std::size_t& pos) {
    auto is_sep = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',';
    };
    while (pos < s.size() && is_sep(s[pos])) ++pos;
    if (pos >= s.size()) return std::nullopt;

    const char c0 = s[pos];
    if (!(std::isdigit(static_cast<unsigned char>(c0)) ||
          c0 == '-' || c0 == '+' || c0 == '.')) {
        return std::nullopt;
    }

    const std::size_t start = pos;
    if (s[pos] == '-' || s[pos] == '+') ++pos;
    while (pos < s.size() && (std::isdigit(static_cast<unsigned char>(s[pos]))
                                || s[pos] == '.')) ++pos;
    if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
        ++pos;
        if (pos < s.size() && (s[pos] == '-' || s[pos] == '+')) ++pos;
        while (pos < s.size() &&
                std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
    }
    return cadml::parse_double_strict(s.substr(start, pos - start));
}

// Adaptive flattening of a cubic Bezier from (x0,y0) to (x3,y3) with
// control points (x1,y1) and (x2,y2). Recursive de Casteljau split:
// if both control points are within `tol` of the chord (p0→p3), emit
// the endpoint as a polyline vertex; otherwise split at t=0.5 and
// recurse on both halves. `depth` caps recursion so a degenerate
// curve can't explode triangle counts. Caller appends p3 and updates
// pen position implicitly through `append`.
void flatten_cubic(Polygon2D& poly,
                    double x0, double y0,
                    double x1, double y1,
                    double x2, double y2,
                    double x3, double y3,
                    double tol, int depth)
{
    // Perpendicular distance from a point to the chord (p0, p3).
    // For degenerate chords (p0 == p3) fall back to distance from p0
    // so a tight loop still flattens.
    const double chx = x3 - x0, chy = y3 - y0;
    const double chord_len2 = chx * chx + chy * chy;
    auto perp_dist = [&](double px, double py) {
        if (chord_len2 < 1e-24) {
            const double dx = px - x0, dy = py - y0;
            return std::sqrt(dx * dx + dy * dy);
        }
        const double cross = (px - x0) * chy - (py - y0) * chx;
        return std::abs(cross) / std::sqrt(chord_len2);
    };

    if (depth >= kMaxSubdivDepth ||
        (perp_dist(x1, y1) < tol && perp_dist(x2, y2) < tol))
    {
        poly.points.push_back({ x3, y3 });
        return;
    }

    // De Casteljau split at t = 0.5.
    const double m01x = (x0 + x1) * 0.5,  m01y = (y0 + y1) * 0.5;
    const double m12x = (x1 + x2) * 0.5,  m12y = (y1 + y2) * 0.5;
    const double m23x = (x2 + x3) * 0.5,  m23y = (y2 + y3) * 0.5;
    const double m012x  = (m01x + m12x) * 0.5,  m012y  = (m01y + m12y) * 0.5;
    const double m123x  = (m12x + m23x) * 0.5,  m123y  = (m12y + m23y) * 0.5;
    const double m0123x = (m012x + m123x) * 0.5, m0123y = (m012y + m123y) * 0.5;

    flatten_cubic(poly, x0,     y0,     m01x,  m01y,
                          m012x, m012y, m0123x, m0123y, tol, depth + 1);
    flatten_cubic(poly, m0123x, m0123y, m123x, m123y,
                          m23x,  m23y,  x3,     y3,     tol, depth + 1);
}

// Flatten a quadratic by promoting it to a cubic. The standard
// degree-elevation formula: cubic control points c1, c2 are
// 2/3 of the way from each endpoint toward the quadratic's lone
// control point. The cubic and quadratic trace identical curves;
// reusing flatten_cubic keeps the subdivision logic in one place.
void flatten_quadratic(Polygon2D& poly,
                        double x0, double y0,
                        double x1, double y1,    // quad control point
                        double x2, double y2,
                        double tol, int depth)
{
    const double c1x = x0 + (2.0 / 3.0) * (x1 - x0);
    const double c1y = y0 + (2.0 / 3.0) * (y1 - y0);
    const double c2x = x2 + (2.0 / 3.0) * (x1 - x2);
    const double c2y = y2 + (2.0 / 3.0) * (y1 - y2);
    flatten_cubic(poly, x0, y0, c1x, c1y, c2x, c2y, x2, y2, tol, depth);
}

// Flatten an elliptical arc per SVG 1.1 appendix B.2.4. Endpoint
// parameterization → centre parameterization → segmented cubic
// approximation (≤ 90° per cubic, k = 4/3 * tan(angle/4) offset).
// Each cubic runs through flatten_cubic for actual line emission.
//
// Out-of-spec inputs are degraded gracefully:
//   * rx == 0 or ry == 0 → emit a straight line (per SVG spec).
//   * end == start → no-op (per SVG spec).
//   * |radii| too small to span the chord → scale up uniformly
//     (also per SVG spec).
void flatten_arc(Polygon2D& poly,
                  double x0, double y0,
                  double rx, double ry, double phi_deg,
                  bool large_arc, bool sweep,
                  double x1, double y1,
                  double tol)
{
    if (std::abs(x1 - x0) < 1e-12 && std::abs(y1 - y0) < 1e-12) return;
    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx < 1e-12 || ry < 1e-12) {
        poly.points.push_back({ x1, y1 });
        return;
    }

    const double phi = phi_deg * (kPi / 180.0);
    const double cos_phi = std::cos(phi);
    const double sin_phi = std::sin(phi);

    // Step 1: compute (x1', y1') — start point in rotated/translated frame.
    const double dx = (x0 - x1) * 0.5;
    const double dy = (y0 - y1) * 0.5;
    const double x1p =  cos_phi * dx + sin_phi * dy;
    const double y1p = -sin_phi * dx + cos_phi * dy;

    // Step 2: ensure the radii are big enough to span the chord. If
    // the implicit lambda (ratio test) exceeds 1, scale rx/ry up so
    // they exactly fit (SVG appendix B.2.5).
    double lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0) {
        const double s = std::sqrt(lambda);
        rx *= s;
        ry *= s;
    }

    // Step 3: compute (cx', cy') — centre in rotated/translated frame.
    const double rx2 = rx * rx, ry2 = ry * ry;
    const double x1p2 = x1p * x1p, y1p2 = y1p * y1p;
    const double sign = (large_arc == sweep) ? -1.0 : 1.0;
    const double num = std::max(0.0,
        rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2);
    const double den = rx2 * y1p2 + ry2 * x1p2;
    const double coeff = sign * std::sqrt(den > 1e-24 ? num / den : 0.0);
    const double cxp =  coeff * (rx * y1p) / ry;
    const double cyp = -coeff * (ry * x1p) / rx;

    // Step 4: map centre back to original coordinate system.
    const double cx = cos_phi * cxp - sin_phi * cyp + (x0 + x1) * 0.5;
    const double cy = sin_phi * cxp + cos_phi * cyp + (y0 + y1) * 0.5;

    // Step 5: compute start angle θ1 and angle delta Δθ.
    auto angle = [](double ux, double uy, double vx, double vy) {
        const double dot = ux * vx + uy * vy;
        const double len = std::sqrt((ux*ux + uy*uy) * (vx*vx + vy*vy));
        const double cos_a = std::clamp(dot / (len > 1e-24 ? len : 1.0),
                                          -1.0, 1.0);
        const double cross = ux * vy - uy * vx;
        return std::acos(cos_a) * (cross < 0 ? -1.0 : 1.0);
    };
    const double v1x = (x1p - cxp) / rx;
    const double v1y = (y1p - cyp) / ry;
    const double v2x = (-x1p - cxp) / rx;
    const double v2y = (-y1p - cyp) / ry;
    double theta1 = angle(1.0, 0.0, v1x, v1y);
    double dtheta = angle(v1x, v1y, v2x, v2y);
    if (!sweep && dtheta > 0)        dtheta -= 2 * kPi;
    else if ( sweep && dtheta < 0)   dtheta += 2 * kPi;

    // Step 6: split the arc into ≤ 90° segments and emit each as a
    // cubic Bezier approximation. The k-coefficient comes from the
    // standard "two-control-point cubic approximating a circular
    // arc" formula: 4/3 * tan(angle/4).
    const int n_seg = std::max(1,
        static_cast<int>(std::ceil(std::abs(dtheta) / (kPi * 0.5))));
    const double seg_dtheta = dtheta / n_seg;
    const double k = (4.0 / 3.0) * std::tan(seg_dtheta * 0.25);

    auto eval_arc = [&](double theta) {
        const double cos_t = std::cos(theta), sin_t = std::sin(theta);
        return std::pair<double, double>{
            cx + cos_phi * rx * cos_t - sin_phi * ry * sin_t,
            cy + sin_phi * rx * cos_t + cos_phi * ry * sin_t
        };
    };
    auto eval_arc_tangent = [&](double theta) {
        // Derivative of the parametric arc equation.
        const double cos_t = std::cos(theta), sin_t = std::sin(theta);
        return std::pair<double, double>{
            -cos_phi * rx * sin_t - sin_phi * ry * cos_t,
            -sin_phi * rx * sin_t + cos_phi * ry * cos_t
        };
    };

    for (int i = 0; i < n_seg; ++i) {
        const double t_a = theta1 + seg_dtheta * i;
        const double t_b = theta1 + seg_dtheta * (i + 1);
        const auto [pa_x, pa_y] = eval_arc(t_a);
        const auto [pb_x, pb_y] = eval_arc(t_b);
        const auto [ta_x, ta_y] = eval_arc_tangent(t_a);
        const auto [tb_x, tb_y] = eval_arc_tangent(t_b);
        flatten_cubic(poly,
            pa_x,             pa_y,
            pa_x + k * ta_x,  pa_y + k * ta_y,
            pb_x - k * tb_x,  pb_y - k * tb_y,
            pb_x,             pb_y,
            tol, 0);
    }
}

}  // namespace

Polygon2D tessellate_path(std::string_view d) {
    Polygon2D poly;
    char cmd = 0;             // current command letter (case preserved)
    double pen_x = 0, pen_y = 0;
    double subpath_start_x = 0, subpath_start_y = 0;

    // Smooth-curve state: S/s reflects last_cubic_ctrl across the
    // current pen position; T/t does the same for last_quad_ctrl.
    // When the previous command WASN'T a matching curve, the
    // reflection point coincides with the pen position (per SVG).
    double last_cubic_ctrl_x = 0, last_cubic_ctrl_y = 0;
    double last_quad_ctrl_x  = 0, last_quad_ctrl_y  = 0;
    bool last_was_cubic = false;
    bool last_was_quad  = false;

    std::size_t pos = 0;

    auto append_point = [&](double x, double y) {
        poly.points.push_back({ x, y });
        pen_x = x;
        pen_y = y;
    };

    auto read_pair = [&]() -> std::optional<std::pair<double, double>> {
        const auto x = next_number(d, pos);
        if (!x) return std::nullopt;
        const auto y = next_number(d, pos);
        if (!y) return std::nullopt;
        return std::pair<double, double>{ *x, *y };
    };
    auto read_one = [&]() -> std::optional<double> {
        return next_number(d, pos);
    };
    // Read a 0/1 SVG flag (one digit, no exponent). Per SVG 1.1
    // appendix the arc command's flags are exactly one digit each.
    auto read_flag = [&]() -> std::optional<bool> {
        // Skip separators.
        while (pos < d.size() &&
                (d[pos] == ' ' || d[pos] == '\t' || d[pos] == '\n' ||
                 d[pos] == '\r' || d[pos] == ',')) ++pos;
        if (pos >= d.size()) return std::nullopt;
        const char c = d[pos];
        if (c != '0' && c != '1') return std::nullopt;
        ++pos;
        return c == '1';
    };

    while (pos < d.size()) {
        // Skip separators before the next token.
        while (pos < d.size() &&
                (d[pos] == ' ' || d[pos] == '\t' || d[pos] == '\n' ||
                 d[pos] == '\r' || d[pos] == ',')) ++pos;
        if (pos >= d.size()) break;

        const char c = d[pos];
        if (std::isalpha(static_cast<unsigned char>(c))) {
            cmd = c;
            ++pos;
            if (cmd == 'Z' || cmd == 'z') {
                // Close-path is implicit for Polygon2D consumers; just
                // reset pen to the subpath start so subsequent relative
                // commands compose correctly. Don't emit a duplicate
                // vertex (extrude / revolve close polygons themselves).
                pen_x = subpath_start_x;
                pen_y = subpath_start_y;
                last_was_cubic = false;
                last_was_quad  = false;
                cmd = 0;
                continue;
            }
            // Unknown command: bail cleanly. Caller sees whatever
            // was emitted up to here. Comment retained from the M/L
            // -only era; the supported set has just grown.
            const bool known =
                cmd == 'M' || cmd == 'm' || cmd == 'L' || cmd == 'l' ||
                cmd == 'H' || cmd == 'h' || cmd == 'V' || cmd == 'v' ||
                cmd == 'C' || cmd == 'c' || cmd == 'S' || cmd == 's' ||
                cmd == 'Q' || cmd == 'q' || cmd == 'T' || cmd == 't' ||
                cmd == 'A' || cmd == 'a';
            if (!known) return poly;
        }

        if (cmd == 0) return poly;  // numbers without a leading command

        const bool relative = std::islower(static_cast<unsigned char>(cmd)) != 0;
        const double rx0 = relative ? pen_x : 0.0;
        const double ry0 = relative ? pen_y : 0.0;

        switch (cmd) {
            case 'M': case 'm': {
                const auto p = read_pair();
                if (!p) return poly;
                append_point(rx0 + p->first, ry0 + p->second);
                subpath_start_x = pen_x;
                subpath_start_y = pen_y;
                // After the first pair of an M, subsequent pairs are
                // implicit linetos (per SVG spec).
                cmd = (cmd == 'M') ? 'L' : 'l';
                last_was_cubic = false;
                last_was_quad  = false;
                break;
            }
            case 'L': case 'l': {
                const auto p = read_pair();
                if (!p) return poly;
                append_point(rx0 + p->first, ry0 + p->second);
                last_was_cubic = false;
                last_was_quad  = false;
                break;
            }
            case 'H': case 'h': {
                const auto x = read_one();
                if (!x) return poly;
                append_point(rx0 + *x, pen_y);
                last_was_cubic = false;
                last_was_quad  = false;
                break;
            }
            case 'V': case 'v': {
                const auto y = read_one();
                if (!y) return poly;
                append_point(pen_x, ry0 + *y);
                last_was_cubic = false;
                last_was_quad  = false;
                break;
            }
            case 'C': case 'c': {
                const auto p1 = read_pair();
                const auto p2 = read_pair();
                const auto p3 = read_pair();
                if (!p1 || !p2 || !p3) return poly;
                const double x1 = rx0 + p1->first, y1 = ry0 + p1->second;
                const double x2 = rx0 + p2->first, y2 = ry0 + p2->second;
                const double x3 = rx0 + p3->first, y3 = ry0 + p3->second;
                flatten_cubic(poly, pen_x, pen_y, x1, y1, x2, y2, x3, y3,
                                kPathTolerance, 0);
                pen_x = x3; pen_y = y3;
                last_cubic_ctrl_x = x2; last_cubic_ctrl_y = y2;
                last_was_cubic = true;
                last_was_quad  = false;
                break;
            }
            case 'S': case 's': {
                const auto p2 = read_pair();
                const auto p3 = read_pair();
                if (!p2 || !p3) return poly;
                // Smooth: implicit p1 = pen + (pen - last_cubic_ctrl).
                // If previous command wasn't C/c/S/s, p1 = pen.
                const double x1 = last_was_cubic
                    ? 2 * pen_x - last_cubic_ctrl_x : pen_x;
                const double y1 = last_was_cubic
                    ? 2 * pen_y - last_cubic_ctrl_y : pen_y;
                const double x2 = rx0 + p2->first, y2 = ry0 + p2->second;
                const double x3 = rx0 + p3->first, y3 = ry0 + p3->second;
                flatten_cubic(poly, pen_x, pen_y, x1, y1, x2, y2, x3, y3,
                                kPathTolerance, 0);
                pen_x = x3; pen_y = y3;
                last_cubic_ctrl_x = x2; last_cubic_ctrl_y = y2;
                last_was_cubic = true;
                last_was_quad  = false;
                break;
            }
            case 'Q': case 'q': {
                const auto p1 = read_pair();
                const auto p2 = read_pair();
                if (!p1 || !p2) return poly;
                const double x1 = rx0 + p1->first, y1 = ry0 + p1->second;
                const double x2 = rx0 + p2->first, y2 = ry0 + p2->second;
                flatten_quadratic(poly, pen_x, pen_y, x1, y1, x2, y2,
                                    kPathTolerance, 0);
                pen_x = x2; pen_y = y2;
                last_quad_ctrl_x = x1; last_quad_ctrl_y = y1;
                last_was_quad  = true;
                last_was_cubic = false;
                break;
            }
            case 'T': case 't': {
                const auto p2 = read_pair();
                if (!p2) return poly;
                const double x1 = last_was_quad
                    ? 2 * pen_x - last_quad_ctrl_x : pen_x;
                const double y1 = last_was_quad
                    ? 2 * pen_y - last_quad_ctrl_y : pen_y;
                const double x2 = rx0 + p2->first, y2 = ry0 + p2->second;
                flatten_quadratic(poly, pen_x, pen_y, x1, y1, x2, y2,
                                    kPathTolerance, 0);
                pen_x = x2; pen_y = y2;
                last_quad_ctrl_x = x1; last_quad_ctrl_y = y1;
                last_was_quad  = true;
                last_was_cubic = false;
                break;
            }
            case 'A': case 'a': {
                const auto rx     = read_one();
                const auto ry     = read_one();
                const auto phi    = read_one();
                const auto large  = read_flag();
                const auto sweep  = read_flag();
                const auto endpt  = read_pair();
                if (!rx || !ry || !phi || !large || !sweep || !endpt)
                    return poly;
                const double x1 = rx0 + endpt->first;
                const double y1 = ry0 + endpt->second;
                flatten_arc(poly, pen_x, pen_y, *rx, *ry, *phi,
                              *large, *sweep, x1, y1, kPathTolerance);
                pen_x = x1; pen_y = y1;
                last_was_cubic = false;
                last_was_quad  = false;
                break;
            }
            default:
                return poly;     // unreachable; known set guards above
        }
    }

    // Dedupe consecutive duplicate vertices. Authored shapes can
    // produce them from two directions:
    //   * Closed paths that come back to the starting M via curves
    //     emit a final vertex coinciding with the first.
    //   * Lua-generated profiles often have a "seam" point where a
    //     flank ends at the same world position the next segment
    //     (tip arc, root arc, etc.) begins — the gear's right-flank-
    //     to-tip-arc transition is the canonical case.
    // Ear-clipping and the side-face emitter both produce zero-area
    // triangles on consecutive duplicates and the ear-test then
    // (correctly) refuses every candidate, leaving a partial cap.
    //
    // Use a tight, ABSOLUTE tolerance — decoupled from the curve-
    // flattening tolerance so changes to the latter don't perturb
    // which seams collapse here. Legitimately distinct polygon
    // vertices can be much closer together than the flattening
    // tolerance — e.g., airfoil sections at small chord have their
    // first-and-last points naturally within ~0.05 units of each
    // other. Only collapse when the points are essentially identical.
    if (poly.points.size() >= 2) {
        const double tight_tol = 5e-4;
        std::vector<Vec2> kept;
        kept.reserve(poly.points.size());
        for (const auto& p : poly.points) {
            if (!kept.empty()) {
                const auto& q = kept.back();
                if (std::abs(p.x - q.x) < tight_tol &&
                    std::abs(p.y - q.y) < tight_tol) continue;
            }
            kept.push_back(p);
        }
        // Also collapse the wraparound seam if last == first.
        if (kept.size() >= 2) {
            const auto& a = kept.front();
            const auto& b = kept.back();
            if (std::abs(a.x - b.x) < tight_tol &&
                std::abs(a.y - b.y) < tight_tol) {
                kept.pop_back();
            }
        }
        poly.points = std::move(kept);
    }
    return poly;
}

// ─── extrude_linear ───────────────────────────────────────────────────
//
// Given a CCW polygon in the XY plane, sweep it +Z by `height` and
// close the top + bottom with ear-clipping triangulation. The
// triangulator handles convex AND concave profiles uniformly, so an
// L-bracket / U-channel / plus / chevron extrudes correctly without
// the centroid-fan crossing-triangles bug.
//
// Vertex layout (N = profile.points.size()):
//   [ 0    .. N-1   ] — bottom ring (z=0)
//   [ N    .. 2N-1  ] — top    ring (z=height)
//
// Per-vertex normals are computed lazily — set to zero here; a
// follow-up pass can normalize. Slice B2.0 keeps this simple.

FlatMesh extrude_linear(const Polygon2D& profile_in,
                          double height,
                          std::uint32_t source_node)
{
    FlatMesh m;
    if (profile_in.empty() || height <= 0) return m;

    // Canonicalise winding so the ear-clipper and the side-face
    // emitter (which both assume CCW) work on user-authored CW
    // polygons too — particularly content from `<svg>` blocks
    // whose y-down semantics inverts winding.
    Polygon2D profile = profile_in;
    ensure_ccw(profile);

    const auto N = static_cast<std::uint32_t>(profile.points.size());

    // Vertex emission (bottom ring, then top ring).
    m.vertices.reserve(2 * N);
    m.normals.reserve(2 * N);
    for (const auto& p : profile.points) {
        m.vertices.push_back({ p.x, p.y, 0.0 });
        m.normals.push_back({ 0, 0, 0 });   // filled by follow-up pass
    }
    for (const auto& p : profile.points) {
        m.vertices.push_back({ p.x, p.y, height });
        m.normals.push_back({ 0, 0, 0 });
    }

    // Side faces — N quads, each split into 2 triangles. CCW from
    // outside the solid.
    for (std::uint32_t i = 0; i < N; ++i) {
        const auto j = (i + 1) % N;
        const auto b0 = i;
        const auto b1 = j;
        const auto t0 = i + N;
        const auto t1 = j + N;
        // (b0, b1, t1) and (b0, t1, t0) — outward CCW.
        emit_triangle(m, b0, b1, t1, source_node);
        emit_triangle(m, b0, t1, t0, source_node);
    }

    // Caps — ear-clip the profile once and emit the same triangulation
    // for both top and bottom. The triangulator returns CCW triangles
    // (matching the input orientation), so:
    //   • Top cap (z=height, faces +Z): emit triangles unchanged with
    //     indices shifted by +N to address the top ring.
    //   • Bottom cap (z=0, faces -Z): swap two indices to flip
    //     winding so the cap faces outward (-Z).
    const auto cap_tris = triangulate_polygon(profile);
    for (const auto& t : cap_tris) {
        // Bottom: (a, c, b) — flipped CCW so it's CW from +Z, CCW from -Z.
        emit_triangle(m, t[0], t[2], t[1], source_node);
        // Top: (a+N, b+N, c+N) — outward (+Z) CCW.
        emit_triangle(m, t[0] + N, t[1] + N, t[2] + N, source_node);
    }

    return m;
}

// ─── revolve ─────────────────────────────────────────────────────────
//
// Sample `profile` at `ring_count` angles between 0 and angle_deg,
// producing a 2D grid of vertices (ring × profile_index). Connect
// neighbouring grid cells with quad strips → triangles.
//
// For closed (angle ≈ 360) revolution the rings wrap (last == first
// is implicit; we don't duplicate it). For open revolution we cap
// each end with a fan triangulation of the rotated profile.
//
// All transforms use libcadml's Mat4 directly.

FlatMesh revolve(const Polygon2D& profile_in,
                  Vec3 axis,
                  double angle_deg,
                  int segments,
                  std::uint32_t source_node)
{
    FlatMesh m;
    if (profile_in.empty() || segments < 3) return m;

    // Canonicalise winding — same reason as extrude_linear.
    Polygon2D profile = profile_in;
    ensure_ccw(profile);

    // Treat angle ≥ 360° as a closed full revolution (mod 360).
    const bool closed = std::abs(angle_deg) >= 360.0 - 1e-9;
    const double effective_angle = closed ? 360.0 : angle_deg;

    // Scale segment count proportionally to angle (min 3).
    int ring_steps = closed
        ? segments
        : std::max(3, static_cast<int>(std::ceil(
              static_cast<double>(segments) * std::abs(effective_angle) / 360.0)));

    const auto P = static_cast<std::uint32_t>(profile.points.size());
    const std::uint32_t ring_count =
        closed ? static_cast<std::uint32_t>(ring_steps)
               : static_cast<std::uint32_t>(ring_steps + 1);

    // Place the 2D profile in a 3D plane before rotating. The
    // historic placement was always (p.x, p.y, 0): for axis ∈
    // {±X, ±Y} this puts profile.y on the rotation axis (because
    // either x or y is the rotation axis and that coordinate stays
    // fixed under rotation), so the existing axis="x" and axis="y"
    // tests are pinned to that convention. For axis ∈ {±Z}, however,
    // (p.x, p.y, 0) leaves both profile coords in the XY plane and
    // the rotation around Z just spins them in-place — the result
    // collapses to a flat disc instead of a swept body. Special-case
    // Z-dominant axes so profile.y maps to Z (along the axis).
    const bool z_axis_dominant =
        std::abs(axis.z) > std::abs(axis.x) &&
        std::abs(axis.z) > std::abs(axis.y);

    auto rotate_profile = [&](double angle) {
        const auto R = Mat4::rotation(angle, axis.x, axis.y, axis.z);
        std::vector<Vec3> ring;
        ring.reserve(P);
        for (const auto& p : profile.points) {
            const Vec3 base = z_axis_dominant
                ? Vec3{ p.x, 0.0, p.y }
                : Vec3{ p.x, p.y, 0.0 };
            ring.push_back(R.transform_point(base));
        }
        return ring;
    };

    // Emit ring vertices. Sample fraction is r/ring_steps for both
    // closed and open: closed uses ring_count == ring_steps so frac
    // ∈ [0, 1) and the wrap-around closes the surface; open uses
    // ring_count == ring_steps + 1 so frac ∈ [0, 1].
    m.vertices.reserve(static_cast<std::size_t>(ring_count) * P
                       + (closed ? 0 : 2));
    m.normals.reserve(m.vertices.capacity());
    for (std::uint32_t r = 0; r < ring_count; ++r) {
        const double frac =
            static_cast<double>(r) / static_cast<double>(ring_steps);
        const double angle = effective_angle * frac;
        const auto ring = rotate_profile(angle);
        for (const auto& v : ring) {
            m.vertices.push_back(v);
            m.normals .push_back({ 0, 0, 0 });
        }
    }

    // Side faces: connect ring r to ring r+1 (or r→0 if closed and
    // r is the last). Each ring step produces P quads × 2 triangles.
    //
    // Winding: a CCW profile in 2D with the rotation around +axis
    // produces outward-facing side faces with the ordering
    // (a0, b1, a1) + (a0, b0, b1). The "naive" (a0, a1, b1) + (a0, b1, b0)
    // gives inward-facing normals (verified algebraically and via the
    // path_all_faces_outward checker on a closed revolve), which makes
    // the surface get back-face-culled in the renderer and breaks
    // unions with neighbouring solids.
    const std::uint32_t last = closed ? ring_count : ring_count - 1;
    for (std::uint32_t r = 0; r < last; ++r) {
        const auto r_next = (r + 1) % ring_count;
        const auto base_a = r       * P;
        const auto base_b = r_next  * P;
        for (std::uint32_t i = 0; i < P; ++i) {
            const auto j = (i + 1) % P;
            const auto a0 = base_a + i;
            const auto a1 = base_a + j;
            const auto b0 = base_b + i;
            const auto b1 = base_b + j;
            emit_triangle(m, a0, b1, a1, source_node);
            emit_triangle(m, a0, b0, b1, source_node);
        }
    }

    // Open revolutions need start + end caps (the rotated profile,
    // fan-tessellated from its centroid). Closed surfaces already
    // wrap and need no caps.
    if (!closed) {
        // Triangulate the 2D profile once via ear-clipping and reuse
        // the index list for both end caps. The cap vertices already
        // exist as the first / last ring's points (rotated to angle 0
        // and `effective_angle` respectively) — we just connect them
        // with the right winding.
        const auto cap_tris = triangulate_polygon(profile);

        auto add_cap = [&](std::uint32_t ring_base, bool flip) {
            for (const auto& t : cap_tris) {
                if (flip) emit_triangle(m, ring_base + t[0],
                                            ring_base + t[2],
                                            ring_base + t[1], source_node);
                else      emit_triangle(m, ring_base + t[0],
                                            ring_base + t[1],
                                            ring_base + t[2], source_node);
            }
        };
        // Start cap: faces back along the rotation; flip winding.
        add_cap(0, /*flip=*/true);
        // End cap: faces forward; CCW.
        add_cap((ring_count - 1) * P, /*flip=*/false);
    }

    return m;
}

// ─── loft_polyhedral ─────────────────────────────────────────────────
//
// Connect a sequence of pre-positioned 3D rings (one per <sketch>
// section) into a closed solid with quad-strip side surfaces and
// ear-clipped caps. See header for the LoftSection contract.

FlatMesh loft_polyhedral(const std::vector<LoftSection>& sections_in,
                          std::uint32_t source_node)
{
    FlatMesh m;
    if (sections_in.size() < 2) return m;
    const auto P = static_cast<std::uint32_t>(sections_in[0].ring_3d.size());
    if (P < 3) return m;
    for (const auto& s : sections_in) {
        if (s.ring_3d.size() != P) return m;  // caller validates; defensive
    }

    // The loft's side-face winding (a0, a1, b1) gives outward normals
    // when the loft progresses in the +normal direction of the first
    // section's polygon (i.e. centroid_last - centroid_first has a
    // positive component along the section's face normal). When the
    // loft progresses backwards (decreasing Z, etc.), the same
    // winding gives INWARD normals and Manifold rejects the result
    // as non-manifold. Detect this and reverse the section order so
    // the loft always goes "forward" along the first ring's normal.
    auto ring_centroid = [](const std::vector<Vec3>& ring) {
        Vec3 c{ 0, 0, 0 };
        for (const auto& v : ring) c = c + v;
        return c * (1.0 / static_cast<double>(ring.size()));
    };
    auto polygon_normal_3d = [](const std::vector<Vec3>& ring) {
        // Newell's method: signed-area-weighted average of edge
        // crosses. Robust to non-planar polygons (handles small
        // numerical jitter without flipping sign).
        Vec3 n{ 0, 0, 0 };
        const auto N = ring.size();
        for (std::size_t i = 0; i < N; ++i) {
            const auto& a = ring[i];
            const auto& b = ring[(i + 1) % N];
            n.x += (a.y - b.y) * (a.z + b.z);
            n.y += (a.z - b.z) * (a.x + b.x);
            n.z += (a.x - b.x) * (a.y + b.y);
        }
        const double L = std::sqrt(n.x*n.x + n.y*n.y + n.z*n.z);
        if (L < 1e-12) return Vec3{ 0, 0, 1 };
        return Vec3{ n.x / L, n.y / L, n.z / L };
    };
    const Vec3 n0 = polygon_normal_3d(sections_in[0].ring_3d);
    const Vec3 c_first = ring_centroid(sections_in[0].ring_3d);
    const Vec3 c_last  = ring_centroid(sections_in.back().ring_3d);
    const Vec3 dir{ c_last.x - c_first.x,
                     c_last.y - c_first.y,
                     c_last.z - c_first.z };
    const double dir_dot_n0 = dir.x * n0.x + dir.y * n0.y + dir.z * n0.z;

    std::vector<LoftSection> sections;
    if (dir_dot_n0 < 0) {
        sections.assign(sections_in.rbegin(), sections_in.rend());
    } else {
        sections = sections_in;
    }

    // Emit ring vertices.
    m.vertices.reserve(static_cast<std::size_t>(sections.size()) * P);
    m.normals.reserve(m.vertices.capacity());
    for (const auto& s : sections) {
        for (const auto& v : s.ring_3d) {
            m.vertices.push_back(v);
            m.normals.push_back({ 0, 0, 0 });
        }
    }

    // Side faces: quad strip between consecutive rings, closest-
    // by-index pairing. Winding (a0, a1, b1) + (a0, b1, b0) matches
    // extrude_linear: a CCW 2D profile lifted through a sketch's
    // right/up frame produces a 3D polygon whose face normal is in
    // the +out-of-plane direction (e.g., +Z for plane=xy). The same
    // pattern that gives extrude its outward side-face normals
    // applies here. (Sweep_along_helix uses INVERTED winding because
    // its profile.y → +Z mapping flips handedness; the loft frame
    // maps profile.y → +up which preserves it.)
    for (std::size_t i = 0; i + 1 < sections.size(); ++i) {
        const std::uint32_t base_a = static_cast<std::uint32_t>(i)     * P;
        const std::uint32_t base_b = static_cast<std::uint32_t>(i + 1) * P;
        for (std::uint32_t v = 0; v < P; ++v) {
            const std::uint32_t w = (v + 1) % P;
            const std::uint32_t a0 = base_a + v;
            const std::uint32_t a1 = base_a + w;
            const std::uint32_t b0 = base_b + v;
            const std::uint32_t b1 = base_b + w;
            emit_triangle(m, a0, a1, b1, source_node);
            emit_triangle(m, a0, b1, b0, source_node);
        }
    }

    // Caps: triangulate each end's 2D profile, map indices to the
    // corresponding ring's vertex range, and pick winding so the
    // cap face normal aligns with -tangent (start) / +tangent (end).
    auto first_tri_normal = [&](std::uint32_t base,
                                 const std::array<std::uint32_t, 3>& tri) {
        const auto& a = m.vertices[base + tri[0]];
        const auto& b = m.vertices[base + tri[1]];
        const auto& c = m.vertices[base + tri[2]];
        return Vec3{
            (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y),
            (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z),
            (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)
        };
    };
    // ring_centroid + polygon_normal_3d are both already declared
    // above (used to determine loft direction). Reuse them here.
    // Tangent estimated from the centroid-to-centroid direction
    // between rings i and i+1 (or i-1 and i for the end cap).
    const Vec3 c0 = ring_centroid(sections[0].ring_3d);
    const Vec3 c1 = ring_centroid(sections[1].ring_3d);
    const Vec3 cN_prev = ring_centroid(sections[sections.size() - 2].ring_3d);
    const Vec3 cN      = ring_centroid(sections.back().ring_3d);
    const Vec3 tangent_start = (c1 - c0);          // points away from start
    const Vec3 tangent_end   = (cN - cN_prev);     // points away from end

    auto emit_cap = [&](std::uint32_t base,
                          const std::vector<std::array<std::uint32_t, 3>>& tris,
                          bool flip) {
        for (const auto& tri : tris) {
            if (flip) emit_triangle(m, base + tri[0], base + tri[2],
                                        base + tri[1], source_node);
            else      emit_triangle(m, base + tri[0], base + tri[1],
                                        base + tri[2], source_node);
        }
    };

    // Start cap.
    {
        const auto tris = triangulate_polygon(sections.front().profile_2d);
        if (!tris.empty()) {
            const auto cap_n_start = first_tri_normal(0, tris[0]);
            // Want cap_n_start to align with -tangent_start (face backward).
            const double dot = -(tangent_start.x * cap_n_start.x +
                                  tangent_start.y * cap_n_start.y +
                                  tangent_start.z * cap_n_start.z);
            emit_cap(0, tris, /*flip=*/dot < 0);
        }
    }
    // End cap.
    {
        const std::uint32_t base = static_cast<std::uint32_t>(sections.size() - 1) * P;
        const auto tris = triangulate_polygon(sections.back().profile_2d);
        if (!tris.empty()) {
            const auto ne = first_tri_normal(base, tris[0]);
            // Want ne to align with +tangent_end.
            const double dot = tangent_end.x * ne.x +
                                tangent_end.y * ne.y +
                                tangent_end.z * ne.z;
            emit_cap(base, tris, /*flip=*/dot < 0);
        }
    }
    return m;
}

// ─── sweep_along_helix ────────────────────────────────────────────────
//
// Helical sweep using the "axis-perpendicular" frame: profile.x maps
// to outward-radial, profile.y maps to +Z. Standard convention for
// thread modeling. See header for the full helix parameterization.

FlatMesh sweep_along_helix(const Polygon2D& profile_in,
                            double radius, double pitch,
                            double turns, double taper,
                            int direction_sign,
                            int segments_per_turn,
                            std::uint32_t source_node)
{
    FlatMesh m;
    if (profile_in.empty() || turns <= 0) return m;
    if (segments_per_turn < 4) segments_per_turn = 4;

    Polygon2D prof = profile_in;
    ensure_ccw(prof);
    const auto P = static_cast<std::uint32_t>(prof.points.size());
    if (P < 3) return m;

    const int n_samples = static_cast<int>(
        std::ceil(static_cast<double>(segments_per_turn) * turns)) + 1;
    if (n_samples < 2) return m;

    // Emit ring vertices. At each t, profile point (px, py) lands at
    //   ((r+px) cos θ, (r+px) sin θ, z + py)
    // where θ = 2π * t * direction_sign and (r, z) come from the helix.
    m.vertices.reserve(static_cast<std::size_t>(n_samples) * P);
    m.normals.reserve(m.vertices.capacity());
    for (int i = 0; i < n_samples; ++i) {
        const double t = turns *
            static_cast<double>(i) / static_cast<double>(n_samples - 1);
        const double theta = 2.0 * kPi * t * direction_sign;
        const double r_at_t = radius + taper * t;
        const double cos_th = std::cos(theta);
        const double sin_th = std::sin(theta);
        const double z_at_t = pitch * t;
        for (const auto& p : prof.points) {
            m.vertices.push_back({
                (r_at_t + p.x) * cos_th,
                (r_at_t + p.x) * sin_th,
                z_at_t + p.y
            });
            m.normals.push_back({ 0, 0, 0 });
        }
    }

    // Side faces: P quads × 2 triangles per ring step, (n_samples-1)
    // ring steps. Winding inverted relative to extrude_linear's
    // pattern: profile is CCW in 2D (interior on left of each edge),
    // and our mapping puts profile.x→radial-out, profile.y→+Z. The
    // resulting 3D polygon has face-normal -Y at theta=0 (opposite
    // of profile-2D's +Z normal because the X,Y,Z handedness flips
    // when y maps to Z). Side-face winding (a0, b1, a1) keeps the
    // 2D-outward direction outward in 3D.
    for (int i = 0; i + 1 < n_samples; ++i) {
        const std::uint32_t base_a = static_cast<std::uint32_t>(i)     * P;
        const std::uint32_t base_b = static_cast<std::uint32_t>(i + 1) * P;
        for (std::uint32_t v = 0; v < P; ++v) {
            const std::uint32_t w = (v + 1) % P;
            const std::uint32_t a0 = base_a + v;
            const std::uint32_t a1 = base_a + w;
            const std::uint32_t b0 = base_b + v;
            const std::uint32_t b1 = base_b + w;
            emit_triangle(m, a0, b1, a1, source_node);
            emit_triangle(m, a0, b0, b1, source_node);
        }
    }

    // Caps — the profile is identical at start and end, just placed at
    // different frames. Triangulate once. Cap winding chosen by a
    // sign-test: the start cap should face -tangent, the end cap
    // should face +tangent. Because direction_sign and turns can flip
    // tangent orientation in non-trivial ways, we determine the
    // winding programmatically by comparing the candidate face's
    // normal against the cap's tangent direction.
    const auto cap_tris = triangulate_polygon(prof);
    if (cap_tris.empty()) return m;

    auto tangent_at = [&](double t) -> Vec3 {
        const double theta  = 2.0 * kPi * t * direction_sign;
        const double r_at_t = radius + taper * t;
        const Vec3 raw{
            taper * std::cos(theta)
                - r_at_t * std::sin(theta) * 2.0 * kPi * direction_sign,
            taper * std::sin(theta)
                + r_at_t * std::cos(theta) * 2.0 * kPi * direction_sign,
            pitch
        };
        const double L = std::sqrt(
            raw.x * raw.x + raw.y * raw.y + raw.z * raw.z);
        if (L < 1e-12) return { 0, 0, 1 };
        return { raw.x / L, raw.y / L, raw.z / L };
    };

    auto first_tri_normal = [&](std::uint32_t base) {
        const auto& tri = cap_tris[0];
        const auto& a = m.vertices[base + tri[0]];
        const auto& b = m.vertices[base + tri[1]];
        const auto& c = m.vertices[base + tri[2]];
        return Vec3{
            (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y),
            (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z),
            (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)
        };
    };

    auto emit_cap = [&](std::uint32_t base, bool flip) {
        for (const auto& tri : cap_tris) {
            if (flip) {
                emit_triangle(m, base + tri[0], base + tri[2],
                                  base + tri[1], source_node);
            } else {
                emit_triangle(m, base + tri[0], base + tri[1],
                                  base + tri[2], source_node);
            }
        }
    };

    // Start cap (at sample 0) — face should point -tangent_0.
    {
        const auto t0 = tangent_at(0.0);
        const auto n0 = first_tri_normal(0);
        // CCW-as-emitted gives n0; flip if dot(n0, -t0) < 0, i.e., if
        // n0 already aligns with -t0 we keep it, otherwise flip.
        const double dot = -(t0.x*n0.x + t0.y*n0.y + t0.z*n0.z);
        emit_cap(0, /*flip=*/dot < 0);
    }
    // End cap (at last sample) — face should point +tangent_end.
    {
        const auto te = tangent_at(turns);
        const std::uint32_t base = static_cast<std::uint32_t>(n_samples - 1) * P;
        const auto ne = first_tri_normal(base);
        const double dot = te.x*ne.x + te.y*ne.y + te.z*ne.z;
        emit_cap(base, /*flip=*/dot < 0);
    }

    return m;
}

// ─── boolean_combine ──────────────────────────────────────────────────
//
// Convert FlatMesh ↔ Manifold's MeshGL64 and apply Manifold's
// constructive solid geometry op. faceID round-trips triangle_node
// so source attribution survives boolean operations (per Manifold's
// spec, surviving triangles keep the faceID they entered with).

namespace {

manifold::MeshGL64 to_meshgl(const FlatMesh& mesh) {
    manifold::MeshGL64 gl;
    gl.numProp = 3;

    gl.vertProperties.reserve(mesh.vertices.size() * 3);
    for (const auto& v : mesh.vertices) {
        gl.vertProperties.push_back(v.x);
        gl.vertProperties.push_back(v.y);
        gl.vertProperties.push_back(v.z);
    }

    gl.triVerts.reserve(mesh.indices.size());
    for (const auto idx : mesh.indices) {
        gl.triVerts.push_back(static_cast<std::uint64_t>(idx));
    }

    // Always populate faceID — Manifold's contract is that faceID is
    // either empty (whole mesh) or has exactly numTri entries. Mixed
    // populated/unpopulated inputs across a Boolean call put Manifold
    // in an undocumented state; uniform population avoids that. When
    // the producer didn't fill triangle_node (or filled it with the
    // wrong count, which is a producer bug), pad with 0 — losing
    // attribution for that mesh but keeping Manifold happy.
    const auto ntri = mesh.triangle_count();
    gl.faceID.reserve(ntri);
    if (mesh.triangle_node.size() == ntri) {
        for (const auto src : mesh.triangle_node) {
            gl.faceID.push_back(static_cast<std::uint64_t>(src));
        }
    } else {
        gl.faceID.assign(ntri, 0);
    }

    // Per-vertex properties may have been duplicated to keep sharp
    // edges separate (extrude/revolve currently emit one vertex per
    // ring sample with zero normals — same position can repeat).
    // Manifold requires a manifold input topology, so collapse them.
    gl.Merge();
    return gl;
}

FlatMesh from_meshgl(const manifold::MeshGL64& gl, std::uint32_t fallback_node) {
    FlatMesh m;
    if (gl.numProp < 3) {
        // Defensive — Manifold's contract says numProp >= 3, but if
        // a future version returned otherwise we'd silently misread
        // every vertex from offset 0. Better to surface it.
        throw std::runtime_error(
            "manifold returned mesh with numProp < 3");
    }

    const std::size_t nv = gl.NumVert();
    m.vertices.reserve(nv);
    m.normals.reserve(nv);
    for (std::size_t i = 0; i < nv; ++i) {
        const std::size_t off = i * gl.numProp;
        m.vertices.push_back({
            gl.vertProperties[off],
            gl.vertProperties[off + 1],
            gl.vertProperties[off + 2]
        });
        m.normals.push_back({ 0, 0, 0 });   // recomputed by a follow-up
                                              // pass; B2.x leaves zero
    }

    const std::size_t ntri = gl.NumTri();
    m.indices.reserve(ntri * 3);
    for (std::size_t i = 0; i < ntri * 3; ++i) {
        m.indices.push_back(static_cast<std::uint32_t>(gl.triVerts[i]));
    }

    m.triangle_node.reserve(ntri);
    if (gl.faceID.size() == ntri) {
        for (std::size_t i = 0; i < ntri; ++i) {
            m.triangle_node.push_back(
                static_cast<std::uint32_t>(gl.faceID[i]));
        }
    } else {
        // No faceID came back (e.g. all input meshes lacked it) —
        // attribute every triangle to the boolean op's own node.
        m.triangle_node.assign(ntri, fallback_node);
    }
    return m;
}

const char* status_to_string(manifold::Manifold::Error err) {
    switch (err) {
        case manifold::Manifold::Error::NoError:               return "no error";
        case manifold::Manifold::Error::NonFiniteVertex:       return "non-finite vertex";
        case manifold::Manifold::Error::NotManifold:           return "not manifold";
        case manifold::Manifold::Error::VertexOutOfBounds:     return "vertex index out of bounds";
        case manifold::Manifold::Error::PropertiesWrongLength: return "properties wrong length";
        case manifold::Manifold::Error::MissingPositionProperties:
            return "missing position properties";
        case manifold::Manifold::Error::MergeVectorsDifferentLengths:
            return "merge vectors different lengths";
        case manifold::Manifold::Error::MergeIndexOutOfBounds: return "merge index out of bounds";
        case manifold::Manifold::Error::TransformWrongLength:  return "transform wrong length";
        case manifold::Manifold::Error::RunIndexWrongLength:   return "run index wrong length";
        case manifold::Manifold::Error::FaceIDWrongLength:     return "face ID wrong length";
        default:                                                return "unknown error";
    }
}

manifold::OpType to_manifold_op(BoolOp op) {
    switch (op) {
        case BoolOp::Union:      return manifold::OpType::Add;
        case BoolOp::Difference: return manifold::OpType::Subtract;
        case BoolOp::Intersect:  return manifold::OpType::Intersect;
    }
    return manifold::OpType::Add;  // unreachable
}

}  // namespace

// Defined at detail scope (outside the anonymous namespace above) so
// flat_evaluator can call it, while still reusing this translation unit's
// to_meshgl / from_meshgl / status_to_string. to_meshgl already runs
// Manifold's vertex Merge(), so constructing a Manifold from the raw soup
// both welds coincident vertices and validates the topology in one step.
MeshImportResult weld_mesh(FlatMesh raw, std::uint32_t source_node) {
    MeshImportResult r;
    if (raw.indices.empty()) {
        r.error = "imported mesh is empty (no triangles)";
        return r;
    }

    // Attribute every imported triangle to the <stl> node — the same
    // contract native primitives uphold. to_meshgl copies triangle_node
    // into faceID, so from_meshgl reads the attribution back after the
    // weld, and the best-effort path below carries it directly. Restoring
    // the normals invariant here keeps downstream consumers (merge_mesh,
    // per-element analysis) positionally aligned on both paths.
    raw.triangle_node.assign(raw.triangle_count(), source_node);
    raw.normals.assign(raw.vertices.size(), Vec3{0, 0, 0});

    manifold::Manifold m(to_meshgl(raw));
    if (m.Status() != manifold::Manifold::Error::NoError) {
        r.mesh  = std::move(raw);   // best-effort — let the caller still
                                    // show the soup
        r.error = std::string("imported mesh is not a valid manifold "
                              "(not watertight — CSG may be unreliable): ") +
                  status_to_string(m.Status());
        return r;
    }
    r.mesh = from_meshgl(m.GetMeshGL64(), source_node);
    r.ok   = true;
    return r;
}

// ─── modifier helpers ────────────────────────────────────────────────

namespace {

// Pick any unit vector orthogonal to `axis_unit`. Used to build a
// 2D basis in the plane perpendicular to a cylinder axis.
Vec3 any_orthogonal_unit(const Vec3& axis_unit) {
    // Pick the world-axis least aligned with `axis_unit`, then
    // Gram-Schmidt against axis_unit.
    Vec3 candidate = (std::abs(axis_unit.x) < 0.9) ? Vec3{1, 0, 0}
                                                    : Vec3{0, 1, 0};
    const double d = candidate.dot(axis_unit);
    Vec3 orth { candidate.x - d * axis_unit.x,
                candidate.y - d * axis_unit.y,
                candidate.z - d * axis_unit.z };
    return orth.normalized();
}

// Closed cylinder mesh between two points, `segments` sides around
// the axis. CCW outward winding throughout. Used by fillet().
FlatMesh make_cylinder(const Vec3& axis_start,
                        const Vec3& axis_end,
                        double radius,
                        int segments,
                        std::uint32_t source_node)
{
    FlatMesh m;
    if (radius <= 0 || segments < 3) return m;
    const Vec3 axis_vec = axis_end - axis_start;
    const double axis_len = std::sqrt(axis_vec.length_sq());
    if (axis_len < 1e-12) return m;
    const Vec3 axis_unit = axis_vec * (1.0 / axis_len);
    const Vec3 e1 = any_orthogonal_unit(axis_unit);
    const Vec3 e2 = axis_unit.cross(e1);   // unit by construction

    const auto N = static_cast<std::uint32_t>(segments);
    m.vertices.reserve(2 * N + 2);
    m.normals .reserve(2 * N + 2);
    // Bottom ring [0..N-1].
    for (std::uint32_t i = 0; i < N; ++i) {
        const double t = (2.0 * kPi * i) / segments;
        const double c = std::cos(t), s = std::sin(t);
        const Vec3 r = e1 * (radius * c) + e2 * (radius * s);
        m.vertices.push_back({ axis_start.x + r.x,
                                 axis_start.y + r.y,
                                 axis_start.z + r.z });
        m.normals.push_back({ 0, 0, 0 });
    }
    // Top ring [N..2N-1].
    for (std::uint32_t i = 0; i < N; ++i) {
        const double t = (2.0 * kPi * i) / segments;
        const double c = std::cos(t), s = std::sin(t);
        const Vec3 r = e1 * (radius * c) + e2 * (radius * s);
        m.vertices.push_back({ axis_end.x + r.x,
                                 axis_end.y + r.y,
                                 axis_end.z + r.z });
        m.normals.push_back({ 0, 0, 0 });
    }
    // Center caps.
    const auto bot_c = static_cast<std::uint32_t>(m.vertices.size());
    m.vertices.push_back(axis_start);
    m.normals .push_back({ -axis_unit.x, -axis_unit.y, -axis_unit.z });
    const auto top_c = static_cast<std::uint32_t>(m.vertices.size());
    m.vertices.push_back(axis_end);
    m.normals .push_back({ axis_unit.x, axis_unit.y, axis_unit.z });

    auto emit = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        m.indices.push_back(a);
        m.indices.push_back(b);
        m.indices.push_back(c);
        m.triangle_node.push_back(source_node);
    };
    // Sides — N quads, each split into 2 triangles. CCW outward.
    for (std::uint32_t i = 0; i < N; ++i) {
        const auto j = (i + 1) % N;
        const auto b0 = i;
        const auto b1 = j;
        const auto t0 = i + N;
        const auto t1 = j + N;
        emit(b0, b1, t1);
        emit(b0, t1, t0);
    }
    // Bottom cap — fan from bot_c. Bottom faces -axis_unit, so wind
    // such that the resulting normal is -axis_unit (CW from +axis).
    for (std::uint32_t i = 0; i < N; ++i) {
        const auto j = (i + 1) % N;
        emit(bot_c, j, i);
    }
    // Top cap — fan from top_c, CCW from +axis_unit.
    for (std::uint32_t i = 0; i < N; ++i) {
        const auto j = (i + 1) % N;
        emit(top_c, i + N, j + N);
    }
    return m;
}

// Closed parallelogram-prism mesh: cross-section is a parallelogram
// with corners (edge_v0), (edge_v0 + side*t1), (edge_v0 + side*(t1+t2)),
// (edge_v0 + side*t2). Sweep along edge_v0 → edge_v1.
//
// Used by fillet to build the "outer container" prism that's then
// reduced by the cylinder cutter.
FlatMesh make_parallelogram_prism(const Vec3& edge_v0, const Vec3& edge_v1,
                                    const Vec3& t1, const Vec3& t2,
                                    double side,
                                    std::uint32_t source_node)
{
    FlatMesh m;
    if (side <= 0) return m;

    // 8 vertices: 4 in the v0 cross-section, 4 in the v1.
    const Vec3 a0 = edge_v0;
    const Vec3 a1 = edge_v0 + (t1 * side);
    const Vec3 a2 = edge_v0 + (t1 * side) + (t2 * side);
    const Vec3 a3 = edge_v0 + (t2 * side);
    const Vec3 b0 = edge_v1;
    const Vec3 b1 = edge_v1 + (t1 * side);
    const Vec3 b2 = edge_v1 + (t1 * side) + (t2 * side);
    const Vec3 b3 = edge_v1 + (t2 * side);

    m.vertices = { a0, a1, a2, a3, b0, b1, b2, b3 };
    m.normals.assign(8, { 0, 0, 0 });

    // Determine orientation: cross-section CCW order from +edge_dir
    // should give cross product parallel to +edge_dir. If not, swap
    // a1↔a3 (and b1↔b3) effectively by reversing winding.
    const Vec3 e_dir = edge_v1 - edge_v0;
    const Vec3 cross_check = (a1 - a0).cross(a3 - a0);
    const bool flip = (cross_check.dot(e_dir) < 0);

    auto emit = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        if (flip) { m.indices.push_back(a); m.indices.push_back(c); m.indices.push_back(b); }
        else      { m.indices.push_back(a); m.indices.push_back(b); m.indices.push_back(c); }
        m.triangle_node.push_back(source_node);
    };

    // End cap at v0 (faces -e_dir): viewed from -e_dir, vertices in
    // CCW order are a0 → a3 → a2 → a1.
    emit(0, 3, 2);
    emit(0, 2, 1);
    // End cap at v1 (faces +e_dir): a4 → a5 → a6 → a7 in CCW from +e.
    emit(4, 5, 6);
    emit(4, 6, 7);
    // Side a0-a1 ⇄ b0-b1 (face along t1 direction at "bottom").
    emit(0, 1, 5);   emit(0, 5, 4);
    // Side a1-a2 ⇄ b1-b2 (face along t2 direction at "right").
    emit(1, 2, 6);   emit(1, 6, 5);
    // Side a2-a3 ⇄ b2-b3 (face along -t1 at "top").
    emit(2, 3, 7);   emit(2, 7, 6);
    // Side a3-a0 ⇄ b3-b0 (face along -t2 at "left").
    emit(3, 0, 4);   emit(3, 4, 7);
    return m;
}

}  // namespace

// ─── selector support (spec §13) ─────────────────────────────────────

namespace {

// Build the §13 edge-property bundle for one topology edge so a parsed
// Selector can be evaluated against it. Direction is left un-normalised
// (Selector::matches normalises); dihedral is converted to degrees.
EdgeProps make_edge_props(const FlatMesh& mesh, const EdgeInfo& info) {
    const Vec3 v0 = mesh.vertices[info.key.v0];
    const Vec3 v1 = mesh.vertices[info.key.v1];
    EdgeProps p;
    p.direction    = v1 - v0;
    p.length       = (v1 - v0).length();
    p.midpoint     = { (v0.x + v1.x) * 0.5,
                       (v0.y + v1.y) * 0.5,
                       (v0.z + v1.z) * 0.5 };
    p.dihedral_deg = info.dihedral_rad * 180.0 / kPi;
    return p;
}

// Select the convex edges of `topo` that pass `sel`. `convex_total`
// receives the number of convex edges considered (so the caller can
// distinguish "no convex edges at all" from "the selector excluded
// them all" for a precise zero-match warning).
std::vector<std::uint32_t> select_convex_edges(const FlatMesh& mesh,
                                                const EdgeTopology& topo,
                                                const Selector& sel,
                                                std::uint32_t& convex_total) {
    std::vector<std::uint32_t> selected;
    convex_total = 0;
    selected.reserve(topo.edges.size());
    for (std::uint32_t i = 0; i < topo.edges.size(); ++i) {
        if (topo.edges[i].classification != EdgeClass::Convex) continue;
        ++convex_total;
        if (sel.is_all() || sel.matches(make_edge_props(mesh, topo.edges[i])))
            selected.push_back(i);
    }
    return selected;
}

}  // namespace

// ─── chamfer ─────────────────────────────────────────────────────────
//
// For each convex edge of `input`, construct a triangular prism
// representing the material to remove (cross-section: right triangle
// at the edge with legs of length `width` along each face's in-plane
// perpendicular to the edge). Union all prisms, subtract from input.
//
// Manifold's CSG handles miter patches at corners automatically: at
// a 3-edge cube corner the three prisms overlap, their union is a
// corner wedge, and the subtraction leaves a triangular miter patch.

namespace {

// Compute the "in-face perpendicular to edge" tangent for a triangle
// adjacent to the edge. The result is a UNIT vector pointing AWAY
// from the edge into the face (toward the triangle's third vertex).
//
// `edge_in_ccw_order`: true iff the triangle's CCW vertex order has
// the edge running v0→v1 (canonical). For the "reverse" face the
// tangent points the other way.
Vec3 in_face_tangent(Vec3 normal, Vec3 edge_dir_canonical,
                       bool edge_in_ccw_order)
{
    // Tangent = n × edge_unit (when edge runs in CCW order of this face)
    //         or n × (-edge_unit)  (when reversed)
    const Vec3 e_unit = edge_dir_canonical.normalized();
    const Vec3 e = edge_in_ccw_order ? e_unit
                                       : Vec3{-e_unit.x, -e_unit.y, -e_unit.z};
    return normal.cross(e);    // unit by construction (|n|=|e|=1, perpendicular)
}

// Build a triangular prism for one chamfer edge. Cross-section
// (perpendicular to edge): right triangle with corner ON the edge,
// legs of length `width` along t1 (into face 1) and t2 (into face 2).
// Sweep that triangle from v0 to v1 along the edge. 6 vertices,
// 8 triangles (2 caps + 3 quad sides each split into 2 tris).
FlatMesh build_chamfer_prism(const FlatMesh& mesh,
                              const EdgeInfo& info,
                              const Vec3& t1, const Vec3& t2,
                              double width,
                              std::uint32_t source_node)
{
    const auto& v0 = mesh.vertices[info.key.v0];
    const auto& v1 = mesh.vertices[info.key.v1];

    // Six prism vertices, ordered so sides are easy to triangulate:
    //   [0] v0 — corner of cross-section at v0
    //   [1] v0 + width * t1
    //   [2] v0 + width * t2
    //   [3] v1
    //   [4] v1 + width * t1
    //   [5] v1 + width * t2
    FlatMesh prism;
    prism.vertices.reserve(6);
    prism.normals.reserve(6);
    prism.vertices.push_back(v0);
    prism.vertices.push_back({ v0.x + width * t1.x,
                                 v0.y + width * t1.y,
                                 v0.z + width * t1.z });
    prism.vertices.push_back({ v0.x + width * t2.x,
                                 v0.y + width * t2.y,
                                 v0.z + width * t2.z });
    prism.vertices.push_back(v1);
    prism.vertices.push_back({ v1.x + width * t1.x,
                                 v1.y + width * t1.y,
                                 v1.z + width * t1.z });
    prism.vertices.push_back({ v1.x + width * t2.x,
                                 v1.y + width * t2.y,
                                 v1.z + width * t2.z });
    for (int k = 0; k < 6; ++k) prism.normals.push_back({0, 0, 0});

    auto emit = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        prism.indices.push_back(a);
        prism.indices.push_back(b);
        prism.indices.push_back(c);
        prism.triangle_node.push_back(source_node);
    };

    // The prism's outward orientation is set by the cross-section's
    // CCW order viewed from +edge_dir. With (0, 1, 2) as the cross-
    // section vertices, edge direction e = v1 - v0, we want CCW from
    // +e direction.
    //
    // Cross product (1-0) × (2-0) should be parallel to +e if CCW.
    // If anti-parallel, swap (1) and (2).
    const Vec3 e_dir = v1 - v0;
    const Vec3 c01 = prism.vertices[1] - prism.vertices[0];
    const Vec3 c02 = prism.vertices[2] - prism.vertices[0];
    const double sign = c01.cross(c02).dot(e_dir);
    const bool flip = (sign < 0);
    auto idx1 = flip ? 2u : 1u;
    auto idx2 = flip ? 1u : 2u;
    auto idx4 = flip ? 5u : 4u;
    auto idx5 = flip ? 4u : 5u;

    // End cap at v0 (faces -e direction) — CW from +e == CCW from -e.
    emit(0, idx2, idx1);
    // End cap at v1 (faces +e direction) — CCW from +e.
    emit(3, idx4, idx5);
    // Side 1: 0-idx1 (along v0_face1) edge ⇄ 3-idx4 (along v1_face1).
    emit(0, idx1, idx4);   emit(0, idx4, 3);
    // Side 2: idx1-idx2 (the bevel face) ⇄ idx4-idx5.
    emit(idx1, idx2, idx5);  emit(idx1, idx5, idx4);
    // Side 3: idx2-0 (along v0_face2) ⇄ idx5-3 (along v1_face2).
    emit(idx2, 0, 3);   emit(idx2, 3, idx5);

    return prism;
}

}  // namespace

ModifierResult chamfer(const FlatMesh& input,
                        double width,
                        const Selector& sel,
                        std::uint32_t source_node)
{
    ModifierResult r;
    if (input.vertices.empty() || input.indices.empty()) {
        r.ok = true; r.mesh = input; return r;
    }
    if (width < 0) {
        r.error = "chamfer: negative distance is not supported";
        return r;
    }
    // Sub-machine-epsilon widths (e.g. `width = a - b` where a and b
    // are nominally equal but differ by a ULP) would otherwise produce
    // degenerate cutter prisms that Manifold either rejects or
    // consumes incorrectly. Treat them as a no-op.
    if (width < 1e-9) {
        r.ok = true; r.mesh = input; return r;
    }

    auto topo = build_edge_topology(input);

    // Convex edges passing the §13 selector (default `all`).
    std::uint32_t convex_total = 0;
    auto selected = select_convex_edges(input, topo, sel, convex_total);
    if (selected.empty()) {
        // Nothing to chamfer — return input unchanged. Distinguish a
        // mesh with no convex edges (silent) from a selector that
        // excluded every convex edge (loud zero-match warning, §13.5).
        r.ok = true; r.mesh = input;
        if (!sel.is_all() && convex_total > 0)
            r.warning = "chamfer: select matched no edges; "
                        "mesh left unchanged";
        return r;
    }

    auto check = tier1_check(input, topo, selected);
    if (!check.ok) {
        r.error = "chamfer Tier 1 predicate: " + check.reason;
        return r;
    }

    // Build a cutter prism per selected edge.
    std::vector<FlatMesh> cutters;
    cutters.reserve(selected.size());
    for (auto eidx : selected) {
        const auto& info = topo.edges[eidx];
        // Identify which adjacent triangle has CCW edge as v0→v1.
        const auto t_a = info.triangles[0];
        const auto t_b = info.triangles[1];
        const bool a_forward = triangle_traverses_edge_forward(
            input, t_a, info.key.v0, info.key.v1);
        const auto t1 = a_forward ? t_a : t_b;
        const auto t2 = a_forward ? t_b : t_a;
        const Vec3 n1 = triangle_normal(input, t1);
        const Vec3 n2 = triangle_normal(input, t2);
        const Vec3 edge_dir =
            input.vertices[info.key.v1] - input.vertices[info.key.v0];
        const Vec3 tan1 = in_face_tangent(n1, edge_dir, /*forward=*/true);
        const Vec3 tan2 = in_face_tangent(n2, edge_dir, /*forward=*/false);
        cutters.push_back(build_chamfer_prism(input, info, tan1, tan2,
                                                width, source_node));
    }

    // Union all cutters, then subtract from input.
    auto cutter_union =
        boolean_combine(cutters, BoolOp::Union, source_node);
    if (!cutter_union.ok) {
        r.error = "chamfer cutter union: " + cutter_union.error;
        return r;
    }
    auto subbed = boolean_combine({ input, cutter_union.mesh },
                                    BoolOp::Difference, source_node);
    if (!subbed.ok) {
        r.error = "chamfer subtract: " + subbed.error;
        return r;
    }

    // Sanity: the subtract should leave most of the input behind. An
    // empty result with a non-empty input means the cutter consumed
    // everything — typically because `width` is too large for the
    // geometry. Manifold treats empty as a legitimate boolean outcome
    // (so boolean_combine returns ok), but for chamfer it's never what
    // the user wanted; surface it as an error.
    if (subbed.mesh.vertices.empty()) {
        r.error = "chamfer: result is empty (distance="
                + std::to_string(width)
                + " too large for the input geometry)";
        return r;
    }

    r.mesh = std::move(subbed.mesh);
    r.ok = true;
    return r;
}

// ─── fillet ──────────────────────────────────────────────────────────

namespace {

// Build a fillet "cutter" for a single convex edge: the parallelogram
// prism MINUS the inscribed cylinder. The cylinder is tangent to both
// adjacent faces, axis parallel to the edge.
FlatMesh build_fillet_cutter(const FlatMesh& input,
                              const EdgeInfo& info,
                              const Vec3& t1, const Vec3& t2,
                              double radius,
                              int segments,
                              std::uint32_t source_node)
{
    const Vec3 v0 = input.vertices[info.key.v0];
    const Vec3 v1 = input.vertices[info.key.v1];

    // Interior angle θ between t1 and t2 (both are unit, perpendicular
    // to edge, pointing into their faces). cos(θ) = t1 · t2.
    const double cos_t = std::clamp(t1.dot(t2), -1.0, 1.0);
    const double theta = std::acos(cos_t);
    const double sin_t = std::sin(theta);

    // Cylinder offset from the edge along the bisector:
    //   center = v + (radius / sin θ) · (t1 + t2)
    // (See header note: this places the cylinder tangent to both faces
    // at distance radius/tan(θ/2) from the edge along each face.)
    if (sin_t < 1e-9) return {};   // degenerate (t1 ≈ ±t2); skip
    const double cyl_offset_scale = radius / sin_t;
    const Vec3 cyl_offset = (t1 + t2) * cyl_offset_scale;

    // Parallelogram side length: tangent point is at distance
    // radius / tan(θ/2) from the edge along each face. Use the
    // half-angle identity tan(θ/2) = sin θ / (1 + cos θ).
    const double tan_half = sin_t / (1.0 + cos_t);
    if (tan_half < 1e-9) return {};   // θ ≈ 0 (degenerate sliver); skip
    const double side = radius / tan_half;

    // Pad the prism in the -t1 / -t2 directions so the original mesh
    // vertices at v0 and v1 are STRICTLY in the cutter's interior
    // (see flat_modifier_tolerances.hpp::kFilletPrismPad for why).
    const Vec3 v0_pad = v0 - (t1 * kFilletPrismPad) - (t2 * kFilletPrismPad);
    const Vec3 v1_pad = v1 - (t1 * kFilletPrismPad) - (t2 * kFilletPrismPad);
    auto prism = make_parallelogram_prism(v0_pad, v1_pad, t1, t2,
                                            side + kFilletPrismPad, source_node);
    auto cyl   = make_cylinder(v0 + cyl_offset, v1 + cyl_offset,
                                 radius, segments, source_node);
    if (prism.vertices.empty() || cyl.vertices.empty()) return {};

    // Cutter = prism - cylinder. Manifold computes the difference;
    // empty result is unexpected here (would mean cyl ⊇ prism) but
    // handled defensively.
    auto diff = boolean_combine({ prism, cyl }, BoolOp::Difference, source_node);
    if (!diff.ok) return {};
    return diff.mesh;
}

}  // namespace

ModifierResult fillet(const FlatMesh& input,
                       double radius,
                       const Selector& sel,
                       std::uint32_t source_node)
{
    ModifierResult r;
    if (input.vertices.empty() || input.indices.empty()) {
        r.ok = true; r.mesh = input; return r;
    }
    if (radius < 0) {
        r.error = "fillet: negative radius is not supported";
        return r;
    }
    // Sub-machine-epsilon radii are a no-op (same rationale as
    // chamfer above — parametric widths from `a - b` can differ from
    // zero only by a ULP and produce degenerate cylinder cutters).
    if (radius < 1e-9) {
        r.ok = true; r.mesh = input; return r;
    }

    auto topo = build_edge_topology(input);

    // Convex edges passing the §13 selector (default `all`).
    std::uint32_t convex_total = 0;
    auto selected = select_convex_edges(input, topo, sel, convex_total);
    if (selected.empty()) {
        r.ok = true; r.mesh = input;
        if (!sel.is_all() && convex_total > 0)
            r.warning = "fillet: select matched no edges; "
                        "mesh left unchanged";
        return r;
    }

    auto check = tier1_check(input, topo, selected);
    if (!check.ok) {
        r.error = "fillet Tier 1 predicate: " + check.reason;
        return r;
    }

    std::vector<FlatMesh> cutters;
    cutters.reserve(selected.size());
    for (auto eidx : selected) {
        const auto& info = topo.edges[eidx];
        const auto t_a = info.triangles[0];
        const auto t_b = info.triangles[1];
        const bool a_forward = triangle_traverses_edge_forward(
            input, t_a, info.key.v0, info.key.v1);
        const auto t1 = a_forward ? t_a : t_b;
        const auto t2 = a_forward ? t_b : t_a;
        const Vec3 n1 = triangle_normal(input, t1);
        const Vec3 n2 = triangle_normal(input, t2);
        const Vec3 edge_dir =
            input.vertices[info.key.v1] - input.vertices[info.key.v0];

        // Same `in_face_tangent` formula as chamfer (declared in
        // chamfer's anon namespace just above; reuse).
        const Vec3 e_unit = edge_dir.normalized();
        const Vec3 tan1 = n1.cross(e_unit);                    // forward face
        const Vec3 tan2 = n2.cross({-e_unit.x, -e_unit.y, -e_unit.z});
        auto cutter = build_fillet_cutter(input, info, tan1, tan2,
                                            radius, kFilletCylinderSegments,
                                            source_node);
        if (!cutter.vertices.empty()) cutters.push_back(std::move(cutter));
    }
    if (cutters.empty()) {
        r.error = "fillet: every edge produced a degenerate cutter";
        return r;
    }

    auto cutter_union =
        boolean_combine(cutters, BoolOp::Union, source_node);
    if (!cutter_union.ok) {
        r.error = "fillet cutter union: " + cutter_union.error;
        return r;
    }
    auto subbed = boolean_combine({ input, cutter_union.mesh },
                                    BoolOp::Difference, source_node);
    if (!subbed.ok) {
        r.error = "fillet subtract: " + subbed.error;
        return r;
    }
    if (subbed.mesh.vertices.empty()) {
        r.error = "fillet: result is empty (radius="
                + std::to_string(radius)
                + " too large for the input geometry)";
        return r;
    }

    r.mesh = std::move(subbed.mesh);
    r.ok = true;
    return r;
}

// ─── shell ────────────────────────────────────────────────────────────
//
// Hollow out an extruded prism by re-extruding the 2D-offset profile
// and subtracting it from the outer extrusion. Open faces extend the
// inner extrusion past the cap so Manifold's subtract removes that
// cap too.

namespace {

// Convert our Polygon2D → Clipper2's PathD type. Direct point-by-point
// copy; coordinates carry through as doubles.
Clipper2Lib::PathD polygon_to_clipper(const Polygon2D& p) {
    Clipper2Lib::PathD path;
    path.reserve(p.points.size());
    for (const auto& v : p.points) path.emplace_back(v.x, v.y);
    return path;
}

// Convert the first PathD in a PathsD back to Polygon2D. Returns
// nullopt if the result is empty (offset collapsed the polygon).
std::optional<Polygon2D> clipper_to_polygon(const Clipper2Lib::PathsD& paths) {
    if (paths.empty() || paths.front().size() < 3) return std::nullopt;
    Polygon2D p;
    p.points.reserve(paths.front().size());
    for (const auto& pt : paths.front()) p.points.push_back({ pt.x, pt.y });
    return p;
}

}  // namespace

ModifierResult shell_extrude(const Polygon2D& profile,
                              double height,
                              double thickness,
                              bool open_start,
                              bool open_end,
                              std::uint32_t source_node)
{
    ModifierResult r;
    if (profile.empty()) {
        r.error = "shell: empty profile";
        return r;
    }
    if (thickness <= 0) {
        r.error = "shell: thickness must be positive";
        return r;
    }
    if (height <= 0) {
        r.error = "shell: height must be positive";
        return r;
    }
    if (!is_convex(profile)) {
        r.error = "shell: Tier 1 requires a convex profile";
        return r;
    }

    // Clipper2 inward offset (positive thickness → negative offset).
    Clipper2Lib::PathsD subject = { polygon_to_clipper(profile) };
    auto offset_paths = Clipper2Lib::InflatePaths(
        subject, -thickness,
        Clipper2Lib::JoinType::Miter,
        Clipper2Lib::EndType::Polygon);
    auto inner_profile = clipper_to_polygon(offset_paths);
    if (!inner_profile) {
        r.error = "shell: thickness "
                + std::to_string(thickness)
                + " too large for the profile (offset collapsed)";
        return r;
    }

    // Outer mesh: full original prism.
    auto outer = extrude_linear(profile, height, source_node);
    if (outer.vertices.empty()) {
        r.error = "shell: outer extrude failed";
        return r;
    }

    // Inner mesh: offset profile, with z-range extended to subtract
    // the corresponding caps when those faces are open.
    //
    //   closed: inner z ∈ [thickness, height − thickness]
    //   start (bottom open): inner z ∈ [-pad, height − thickness]
    //   end (top open):     inner z ∈ [thickness, height + pad]
    //   both open:          inner z ∈ [-pad, height + pad]
    const double inner_z_min = open_start ? -kShellCapPad : thickness;
    const double inner_z_max = open_end   ? (height + kShellCapPad)
                                            : (height - thickness);
    if (inner_z_max - inner_z_min <= 0) {
        r.error = "shell: thickness "
                + std::to_string(thickness)
                + " too large for the height";
        return r;
    }
    auto inner = extrude_linear(*inner_profile,
                                  inner_z_max - inner_z_min, source_node);
    if (inner.vertices.empty()) {
        r.error = "shell: inner extrude failed";
        return r;
    }
    // Translate inner so its base sits at inner_z_min.
    for (auto& v : inner.vertices) v.z += inner_z_min;

    // Outer minus inner.
    auto subbed = boolean_combine({ outer, inner }, BoolOp::Difference,
                                    source_node);
    if (!subbed.ok) {
        r.error = "shell subtract: " + subbed.error;
        return r;
    }
    if (subbed.mesh.vertices.empty()) {
        r.error = "shell: result is empty";
        return r;
    }
    r.mesh = std::move(subbed.mesh);
    r.ok = true;
    return r;
}

BoolResult boolean_combine(const std::vector<FlatMesh>& inputs,
                            BoolOp op,
                            std::uint32_t source_node)
{
    BoolResult r;
    if (inputs.empty()) { r.ok = true; return r; }
    if (inputs.size() == 1) { r.mesh = inputs[0]; r.ok = true; return r; }

    try {
        manifold::Manifold acc(to_meshgl(inputs[0]));
        if (acc.Status() != manifold::Manifold::Error::NoError) {
            r.error = std::string("boolean input 0: ") +
                       status_to_string(acc.Status());
            return r;
        }

        const auto m_op = to_manifold_op(op);
        for (std::size_t i = 1; i < inputs.size(); ++i) {
            manifold::Manifold rhs(to_meshgl(inputs[i]));
            if (rhs.Status() != manifold::Manifold::Error::NoError) {
                r.error = std::string("boolean input ") + std::to_string(i) +
                          ": " + status_to_string(rhs.Status());
                return r;
            }
            acc = acc.Boolean(rhs, m_op);
            if (acc.Status() != manifold::Manifold::Error::NoError) {
                r.error = std::string("boolean step ") + std::to_string(i) +
                          ": " + status_to_string(acc.Status());
                return r;
            }
        }

        if (acc.IsEmpty()) {
            // Empty result is legitimate (e.g. no overlap on Intersect).
            r.ok = true;
            return r;
        }
        r.mesh = from_meshgl(acc.GetMeshGL64(), source_node);
        r.ok = true;
    } catch (const std::exception& e) {
        r.error = std::string("boolean exception: ") + e.what();
    }
    return r;
}

// ─── hull_combine ────────────────────────────────────────────────────

BoolResult hull_combine(const std::vector<FlatMesh>& inputs,
                         std::uint32_t source_node)
{
    BoolResult r;
    if (inputs.empty()) { r.ok = true; return r; }

    try {
        // Build a Manifold per input, validate, then call the
        // multi-input static Hull. For a single input we use the
        // member Hull() which is what Manifold provides for the
        // 1-arg form.
        std::vector<manifold::Manifold> ms;
        ms.reserve(inputs.size());
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            manifold::Manifold m(to_meshgl(inputs[i]));
            if (m.Status() != manifold::Manifold::Error::NoError) {
                r.error = std::string("hull input ") + std::to_string(i) +
                          ": " + status_to_string(m.Status());
                return r;
            }
            ms.push_back(std::move(m));
        }

        manifold::Manifold acc =
            (ms.size() == 1)
                ? ms[0].Hull()
                : manifold::Manifold::Hull(ms);

        if (acc.Status() != manifold::Manifold::Error::NoError) {
            r.error = std::string("hull failed: ") +
                       status_to_string(acc.Status());
            return r;
        }
        if (acc.IsEmpty()) {
            // Legitimate outcome — degenerate inputs (collinear
            // points, zero-volume meshes) can produce an empty
            // hull. Caller decides whether to warn.
            r.ok = true;
            return r;
        }
        r.mesh = from_meshgl(acc.GetMeshGL64(), source_node);
        r.ok = true;
    } catch (const std::exception& e) {
        r.error = std::string("hull exception: ") + e.what();
    }
    return r;
}

// ─── intersect_volume ────────────────────────────────────────────────

IntersectVolumeResult intersect_volume(const FlatMesh& a, const FlatMesh& b) {
    IntersectVolumeResult r;
    if (a.vertices.empty() || b.vertices.empty()) {
        r.ok = true;   // either side empty → intersection is empty
        return r;
    }
    try {
        manifold::Manifold ma(to_meshgl(a));
        if (ma.Status() != manifold::Manifold::Error::NoError) {
            r.error = std::string("intersect_volume input A: ") +
                       status_to_string(ma.Status());
            return r;
        }
        manifold::Manifold mb(to_meshgl(b));
        if (mb.Status() != manifold::Manifold::Error::NoError) {
            r.error = std::string("intersect_volume input B: ") +
                       status_to_string(mb.Status());
            return r;
        }
        manifold::Manifold mr = ma.Boolean(mb, manifold::OpType::Intersect);
        if (mr.Status() != manifold::Manifold::Error::NoError) {
            r.error = std::string("intersect_volume result: ") +
                       status_to_string(mr.Status());
            return r;
        }
        if (mr.IsEmpty()) {
            r.ok = true;     // disjoint inputs — legitimate outcome
            return r;
        }
        // Manifold's Volume() is signed; absolute value matches what
        // an interference check intuitively wants ("how much do these
        // overlap").
        r.volume = std::abs(mr.Volume());
        r.ok = true;
    } catch (const std::exception& e) {
        r.error = std::string("intersect_volume exception: ") + e.what();
    }
    return r;
}

}  // namespace cadml::engine::detail
