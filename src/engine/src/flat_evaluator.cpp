// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/engine/flat_evaluator.hpp>

#include "flat_geometry.hpp"
#include "flat_mesh_cache_internal.hpp"

#include <cadml/expression.hpp>
#include <cadml/selector.hpp>

#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>


namespace cadml::engine {

namespace {

// True iff `t` is an authoring construct that the bundler should have
// lowered. Bare instances (no at/port) ARE allowed in flat docs as
// inline composition (spec §6.7) — only mating instances are caught
// separately below.
const char* authoring_construct_name(NodeType t) {
    switch (t) {
        case NodeType::Assembly: return "<assembly>";
        case NodeType::Connect:  return "<connect>";
        case NodeType::For:      return "<for>";
        case NodeType::Pattern:  return "<pattern>";
        // <cut> is NOT listed here: per spec §12.5 it survives compile
        // and is resolved at engine time (the cutting wedge depends on
        // the target's bbox, which the bundler can't know without
        // running the geometry).
        default: return nullptr;
    }
}

void validate_boundary(const Document& doc,
                        std::vector<FlatEvalError>& errors)
{
    // Frontmatter fields must be empty in flat docs: the bundler clears
    // doc.imports after import resolution, and hoist_entry_params moves
    // doc.params into <param> children of the export. A non-empty value
    // here means we're seeing an unprocessed (or hand-crafted) input.
    for (const auto& imp : doc.imports) {
        errors.push_back({
            "flat-doc boundary: leftover import directive `" + imp.path +
            "`; imports must be resolved by the bundler",
            imp.source });
    }
    for (const auto& p : doc.params) {
        errors.push_back({
            "flat-doc boundary: leftover frontmatter param `" + p.name +
            "`; params must be hoisted into the body by the bundler",
            p.source });
    }

    for (const auto& n : doc.nodes) {
        if (n.dead) continue;

        if (const char* name = authoring_construct_name(n.type)) {
            errors.push_back({
                std::string("flat-doc boundary: ") + name +
                " must be lowered by the bundler before reaching the engine",
                n.source });
            continue;
        }

        if (n.type == NodeType::Instance) {
            const auto& ia = std::get<InstanceAttrs>(n.attrs);
            if (!ia.at.empty() || !ia.port.empty()) {
                errors.push_back({
                    "flat-doc boundary: instance `" + ia.ref_name +
                    "` carries at/port; mating instances must be lowered"
                    " by the assembly compiler",
                    n.source });
            }
        }

        if (n.type == NodeType::Unknown) {
            const auto& ua = std::get<UnknownAttrs>(n.attrs);
            errors.push_back({
                "flat-doc boundary: unknown element `" + ua.raw_tag_name + "`",
                n.source });
        }
    }
}

// Get the index of `node` within doc.nodes (the node was obtained via
// the children() iterator which yields const Node&).
std::uint32_t node_index(const Document& doc, const Node& n) {
    return static_cast<std::uint32_t>(&n - doc.nodes.data());
}

// Single-place evaluator for a numeric attribute expression. On
// failure surfaces the underlying expression-error message verbatim
// (so e.g. a div-by-zero throw reaches the user as "division by
// zero in expression `a/b`" instead of "could not evaluate ...")
// and returns 0 so geometry routines can still run with sensible
// degenerate output. A second pass at the end of evaluation
// (`promote_fatal_eval_warnings`) scans for fatal-class messages
// and reclassifies them into FlatEvalResult.errors so the spec's
// "hard error for division by zero" promise reaches the result.
double eval_num(ExpressionEvaluator& e,
                 std::string_view expr,
                 SourceRange src,
                 std::vector<FlatEvalError>& warnings,
                 const char* label)
{
    std::vector<ExpressionError> errs;
    auto v = e.evaluate_number(expr, src, errs);
    if (!v) {
        std::string msg;
        if (!errs.empty()) {
            msg = std::string("could not evaluate ") + label +
                  " expression `" + std::string(expr) + "`: " +
                  errs.front().message;
        } else {
            msg = std::string("could not evaluate ") + label +
                  " expression `" + std::string(expr) + "`";
        }
        warnings.push_back({ std::move(msg), src });
        return 0.0;
    }
    return *v;
}

// Append `b`'s vertices/normals/triangles into `a`, rebasing indices.
void merge_mesh(FlatMesh& a, const FlatMesh& b) {
    const auto base = static_cast<std::uint32_t>(a.vertices.size());
    a.vertices.insert(a.vertices.end(), b.vertices.begin(), b.vertices.end());
    a.normals .insert(a.normals .end(), b.normals .begin(), b.normals .end());
    for (auto idx : b.indices)         a.indices.push_back(idx + base);
    for (auto src : b.triangle_node)   a.triangle_node.push_back(src);
}

// Apply Mat4 to every vertex + normal in `m`. Vertices use full
// transform; normals use direction (rotation only) so they don't pick
// up translation. Operates in place.
//
// Caveat: `transform_direction` includes any scale/shear in M, so
// non-uniform group transforms ARE NOT preserved as unit normals
// after this call. Today it's moot — every primitive emits zero
// normals and a follow-up pass recomputes them from triangle geometry
// — but once real per-vertex normals land, scale-only transforms
// will need explicit re-normalization here (transpose-inverse for
// non-uniform scale).
// 3x3 determinant of M's upper-left rotational/scaling block. Mat4
// is column-major (m[col*4 + row]). We don't need the affine
// translation column. Result is negative iff the transform reverses
// orientation (mirrors, scales with an odd number of negative
// factors, etc.) — used below to keep triangle winding outward.
double mat4_det3(const Mat4& M) {
    const auto& m = M.m;
    return m[0] * (m[5] * m[10] - m[6] * m[9])
         - m[4] * (m[1] * m[10] - m[2] * m[9])
         + m[8] * (m[1] * m[6]  - m[2] * m[5]);
}

void apply_transform_to_mesh(FlatMesh& m, const Mat4& M) {
    for (auto& v : m.vertices) v = M.transform_point(v);
    for (auto& n : m.normals)  n = M.transform_direction(n);
    // If the transform reverses orientation (e.g., scale(1, -1, 1)
    // for SVG y-flip, or any odd reflection / odd-negative scale),
    // every triangle's CCW winding becomes CW. Flip them back so
    // outward-facing triangles stay outward — otherwise back-face
    // culling would erase the entire surface.
    if (mat4_det3(M) < 0.0 && !m.indices.empty()) {
        for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            std::swap(m.indices[i + 1], m.indices[i + 2]);
        }
    }
}

// Parse a CADML transform attribute string into a Mat4. Supported
// SVG-like calls (left-to-right composition; leftmost in string is
// outermost / applied last to a point):
//   translate(tx, ty, tz)
//   rotate(angle_deg, ax, ay, az)
//   scale(sx, sy, sz)
//   mirror(nx, ny, nz)
//
// Diagnostics — appended to `warnings` with `src` as the location:
//   * unparsable number tokens
//   * wrong arg count for a known function
//   * unknown function name
//   * truncated input (missing '(' after a name, or missing ')')
//
// Per-call recovery: on any failure for a given function call we skip
// that call and keep parsing the rest of the chain; the user gets all
// the diagnostics in one pass.
// Pre-process a structured-string attribute by substituting `{expr}`
// tokens with the numeric value of `expr` evaluated in `e`'s scope.
// Used for both `group/@transform` and `path/@d`: their grammars
// only handle literal numbers, so this turns
// `translate(0, 0, {block-height - peg-length})` into
// `translate(0, 0, -5)` before the structured parser sees it.
// Attributes like `extrude.height` flow through evaluate_number
// directly; structured-string attrs are special because their
// values are not single expressions.
//
// Diagnostic strings still say "transform string" — fine for
// transforms, slightly off for path `d`, but the warning text is
// generic enough to remain understandable in either context.
std::string substitute_transform_exprs(
    std::string_view s,
    ExpressionEvaluator& e,
    SourceRange src,
    std::vector<FlatEvalError>& warnings)
{
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    while (pos < s.size()) {
        if (s[pos] == '{') {
            const std::size_t end = s.find('}', pos + 1);
            if (end == std::string_view::npos) {
                warnings.push_back({
                    "transform string: unclosed `{` in `" +
                    std::string(s) + "`", src });
                out.append(s.substr(pos));
                break;
            }
            const auto expr = s.substr(pos, end - pos + 1); // include braces
            std::vector<ExpressionError> errs;
            auto v = e.evaluate_number(expr, src, errs);
            if (!v) {
                // Surface the underlying expression error verbatim
                // so promote_fatal_eval_warnings can detect the
                // div/mod-by-zero marker and reclassify to error.
                std::string detail;
                if (!errs.empty()) {
                    detail = ": " + errs.front().message;
                }
                warnings.push_back({
                    "transform string: cannot evaluate `" +
                    std::string(expr) + "`" + detail, src });
                out.push_back('0');     // fall through; parser will use 0
            } else {
                out.append(cadml::format_double_canonical(*v, 17));
            }
            pos = end + 1;
        } else {
            out.push_back(s[pos++]);
        }
    }
    return out;
}

Mat4 parse_transform_string(std::string_view s,
                              SourceRange src,
                              std::vector<FlatEvalError>& warnings)
{
    auto is_alpha = [](unsigned char c) { return std::isalpha(c) != 0; };
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };

    Mat4 m = Mat4::identity();
    std::size_t pos = 0;
    while (pos < s.size()) {
        while (pos < s.size() && is_space(s[pos])) ++pos;
        if (pos >= s.size()) break;

        const std::size_t name_start = pos;
        while (pos < s.size() && is_alpha(s[pos])) ++pos;
        const std::string name(s.substr(name_start, pos - name_start));
        if (pos >= s.size() || s[pos] != '(') {
            warnings.push_back({
                "transform string: expected `(` after `" + name + "`", src });
            break;
        }
        ++pos;

        std::vector<double> args;
        std::string buf;
        bool numeric_failure = false;
        auto flush_buf = [&]() {
            if (buf.empty()) return;
            if (auto v = parse_double_strict(buf)) {
                args.push_back(*v);
            } else {
                warnings.push_back({
                    "transform string: cannot parse number `" + buf +
                    "` in `" + name + "(...)`", src });
                numeric_failure = true;
            }
            buf.clear();
        };
        while (pos < s.size() && s[pos] != ')') {
            const char c = s[pos];
            if (c == ',' || is_space(static_cast<unsigned char>(c))) flush_buf();
            else                                                     buf.push_back(c);
            ++pos;
        }
        flush_buf();
        if (pos >= s.size()) {
            warnings.push_back({
                "transform string: missing `)` for `" + name + "(...)`", src });
            break;
        }
        ++pos;  // consume ')'

        if (numeric_failure) continue;  // already warned per-token

        const auto wrong_arity = [&](std::size_t expected) {
            warnings.push_back({
                "transform string: `" + name + "` expects " +
                std::to_string(expected) + " args, got " +
                std::to_string(args.size()), src });
        };

        if (name == "translate") {
            if (args.size() == 3) m = m * Mat4::translation(args[0], args[1], args[2]);
            else                  wrong_arity(3);
        } else if (name == "rotate") {
            if (args.size() == 4) m = m * Mat4::rotation(args[0], args[1], args[2], args[3]);
            else                  wrong_arity(4);
        } else if (name == "scale") {
            if (args.size() == 3) m = m * Mat4::scaling(args[0], args[1], args[2]);
            else                  wrong_arity(3);
        } else if (name == "mirror") {
            if (args.size() == 3) m = m * Mat4::mirror(args[0], args[1], args[2]);
            else                  wrong_arity(3);
        } else {
            warnings.push_back({
                "transform string: unknown function `" + name + "`"
                " (supported: translate, rotate, scale, mirror)", src });
        }
    }
    return m;
}

// Forward declarations for mutual recursion between the dispatch helpers.
// `depth` tracks instance nesting and is checked at eval_instance to
// stop unbounded recursion on self-referential defs.
std::optional<detail::Polygon2D>
    eval_2d(const Document& doc, const Node& n,
             ExpressionEvaluator& e, std::vector<FlatEvalError>& warnings);

FlatMesh eval_3d(const Document& doc, const Node& n,
                  ExpressionEvaluator& e,
                  std::vector<FlatEvalError>& warnings,
                  FlatMeshCache* cache,
                  int depth);

// Maximum nested-instance depth before the evaluator bails to stop
// stack overflow on self-referential defs (`<def name="A"><A/></def>`)
// or longer cycles. The bundler now rejects such cycles at compile time
// (compile/bundler.cpp::check_def_cycles), so a well-formed .fcadml
// from cadmlc never trips this — but the engine keeps the guard as
// defense in depth for hand-authored or third-party .fcadml that
// skipped the bundler's cycle check.
constexpr int kMaxInstanceDepth = 64;

FlatMesh eval_group(const Document& doc, const Node& group_node,
                     ExpressionEvaluator& e,
                     std::vector<FlatEvalError>& warnings,
                     FlatMeshCache* cache,
                     int depth);

FlatMesh eval_svg(const Document& doc, const Node& svg_node,
                   ExpressionEvaluator& e,
                   std::vector<FlatEvalError>& warnings,
                   FlatMeshCache* cache,
                   int depth);

// True iff we're currently evaluating geometry inside an <svg> block
// (any nesting depth). Used to suppress the CW-polygon warning for
// SVG-pasted content where CW is the expected handedness. Defined
// next to eval_svg further down in this file.
bool inside_svg_frame() noexcept;

// Evaluate a <sketch> wrapper: tessellate the contained 2D primitive
// AND lift its vertices into 3D using the sketch's plane / origin /
// rotation / normal frame. Returns nullopt if the sketch lacks a
// 2D primitive child or a frame attribute fails to parse.
std::optional<detail::LoftSection>
    eval_sketch(const Document& doc, const Node& sketch_node,
                 ExpressionEvaluator& e,
                 std::vector<FlatEvalError>& warnings);

FlatMesh eval_instance(const Document& doc, const Node& inst_node,
                        ExpressionEvaluator& caller_scope,
                        std::vector<FlatEvalError>& warnings,
                        FlatMeshCache* cache,
                        int depth);

// Single-child dispatch: returns the mesh produced by `child`, whether
// it's a primitive (extrude/revolve/booleans), a wrapping <group>, or
// something we can't render (returns empty mesh in that case). Shared
// between eval_geometry_children (which merges results) and the boolean
// dispatch (which keeps them as separate operands).
//
// Only emits a "skipping" diagnostic for SUSPICIOUS types — types we
// know are wrong here. 2D primitives are silently skipped because they
// commonly appear as profile children of a 3D primitive (consumed
// directly by eval_3d, not by this helper).
FlatMesh eval_node(const Document& doc, const Node& child,
                    ExpressionEvaluator& e,
                    std::vector<FlatEvalError>& warnings,
                    FlatMeshCache* cache,
                    int depth)
{
    switch (child.type) {
        case NodeType::Param:
        case NodeType::Port:
            return {};                  // structural, no geometry
        case NodeType::Group:
            return eval_group(doc, child, e, warnings, cache, depth);
        case NodeType::Svg:
            return eval_svg(doc, child, e, warnings, cache, depth);
        case NodeType::Instance:
            // Bare-instance dispatch: look up the def, evaluate the def's
            // body in a fresh scope. The instance's override expressions
            // evaluate in the CALLER's scope (so `{outer * 2}` can read
            // caller params); the def's own geometry then evaluates in a
            // fresh scope where def-defaults + override RESULTS are bound.
            return eval_instance(doc, child, e, warnings, cache, depth);
        case NodeType::Extrude:
        case NodeType::Revolve:
        case NodeType::Sweep:
        case NodeType::Loft:
        case NodeType::Helix:
        case NodeType::Union:
        case NodeType::Difference:
        case NodeType::Intersect:
        case NodeType::Hull:
        case NodeType::Fillet:
        case NodeType::Chamfer:
        case NodeType::Shell:
        case NodeType::Cut:
            return eval_3d(doc, child, e, warnings, cache, depth);
        default:
            return {};                  // 2D primitives etc.
    }
}

// 2D dispatch: NodeType::Circle / Rect produce profiles. Sketch /
// Path could be added later. Returns nullopt for unsupported types.
std::optional<detail::Polygon2D>
eval_2d(const Document& /*doc*/, const Node& n,
         ExpressionEvaluator& e, std::vector<FlatEvalError>& warnings)
{
    switch (n.type) {
        case NodeType::Rect: {
            const auto& a = std::get<RectAttrs>(n.attrs);
            const auto x  = eval_num(e, a.x_expr,      n.source, warnings, "rect.x");
            const auto y  = eval_num(e, a.y_expr,      n.source, warnings, "rect.y");
            const auto w  = eval_num(e, a.width_expr,  n.source, warnings, "rect.width");
            const auto h  = eval_num(e, a.height_expr, n.source, warnings, "rect.height");
            // rx defaults to 0 (sharp corners); ry empty -> mirror rx.
            const auto rx = a.rx_expr.empty() ? 0.0
                : eval_num(e, a.rx_expr, n.source, warnings, "rect.rx");
            const auto ry = a.ry_expr.empty() ? 0.0
                : eval_num(e, a.ry_expr, n.source, warnings, "rect.ry");
            return detail::tessellate_rect(x, y, w, h, rx, ry);
        }
        case NodeType::Circle: {
            const auto& a = std::get<CircleAttrs>(n.attrs);
            const auto cx = eval_num(e, a.cx_expr, n.source, warnings, "circle.cx");
            const auto cy = eval_num(e, a.cy_expr, n.source, warnings, "circle.cy");
            const auto r  = eval_num(e, a.r_expr,  n.source, warnings, "circle.r");
            int segments = 0;  // 0 = adaptive
            if (!a.segments_expr.empty()) {
                segments = static_cast<int>(eval_num(
                    e, a.segments_expr, n.source, warnings, "circle.segments"));
            }
            return detail::tessellate_circle(cx, cy, r, segments);
        }
        case NodeType::Path: {
            // Path's `d` is a structured string (SVG-style commands +
            // numbers) that the engine's expression evaluator can't
            // see through. Substitute any `{expr}` tokens here — same
            // pattern as transform strings — before handing the
            // result to tessellate_path. The bundler's SUBST_FIELD
            // pass only handles <for>-loop var substitution; this
            // catches frontmatter params and runtime expressions.
            const auto& a = std::get<PathAttrs>(n.attrs);
            const auto subbed =
                substitute_transform_exprs(a.d, e, n.source, warnings);
            auto poly = detail::tessellate_path(subbed);
            if (poly.points.size() < 3) {
                warnings.push_back({
                    "path: tessellated to fewer than 3 points (need at"
                    " least M + 2 L for a polygon); supported commands"
                    " are M / L / Z (and lowercase relatives)",
                    n.source });
                return std::nullopt;
            }
            return poly;
        }
        default:
            return std::nullopt;
    }
}

// Evaluate a <sketch> wrapper. Returns the 2D-tessellated profile
// AND that profile lifted into 3D using the sketch's frame:
//
//   normal   - from `plane` ("xy"->+Z, "xz"->+Y, "yz"->+X), or
//              from `normal` attribute if explicitly given.
//   up       - "xy"->+Y, "xz"->+Z, "yz"->+Z, or for explicit normal
//              the projection of +Z onto the plane perpendicular to
//              normal (falling back to +Y if normal is parallel
//              to +Z).
//   right    - up x normal (per spec §5.2).
//   rotation - 2D rotation around the normal, applied to the
//              profile vertices in the (right, up) plane before
//              lifting to 3D.
//   origin   - 3D anchor; profile point (px, py) lands at
//              origin + px*right + py*up after rotation.
std::optional<detail::LoftSection>
    eval_sketch(const Document& doc, const Node& sketch_node,
                 ExpressionEvaluator& e,
                 std::vector<FlatEvalError>& warnings)
{
    const auto& a = std::get<SketchAttrs>(sketch_node.attrs);

    // Find the 2D-primitive child.
    std::optional<detail::Polygon2D> profile;
    for (auto& child : doc.children(node_index(doc, sketch_node))) {
        if (child.dead) continue;
        if (auto p = eval_2d(doc, child, e, warnings)) {
            profile = std::move(p);
            break;
        }
    }
    if (!profile) {
        warnings.push_back({
            "sketch: missing 2D primitive child", sketch_node.source });
        return std::nullopt;
    }

    // Resolve normal.
    Vec3 normal;
    if (!a.normal_expr.empty()) {
        std::vector<ExpressionError> errs;
        auto n = e.evaluate_vector(a.normal_expr, sketch_node.source, errs);
        if (!n) {
            std::string detail;
            if (!errs.empty()) detail = ": " + errs.front().message;
            warnings.push_back({
                "sketch: invalid `normal=\"" + a.normal_expr + "\"`" +
                detail, sketch_node.source });
            return std::nullopt;
        }
        normal = n->normalized();
    } else if (a.plane == "xy") {
        normal = { 0, 0, 1 };
    } else if (a.plane == "xz") {
        normal = { 0, 1, 0 };
    } else if (a.plane == "yz") {
        normal = { 1, 0, 0 };
    } else {
        warnings.push_back({
            "sketch: unknown `plane=\"" + a.plane + "\"`; expected "
            "xy / xz / yz", sketch_node.source });
        return std::nullopt;
    }

    // Resolve up. When a custom normal overrides the plane's default
    // normal we still keep the plane's default up axis (projected onto
    // perp-of-normal so it stays orthogonal). This matters for swept-
    // body authoring like the compressor blade, which sets normal to
    // the meridional tangent and relies on up = +Y so that
    // right = up × normal lands in the meridional plane (radial /
    // axial) rather than purely tangential. If the chosen up is
    // parallel to normal we fall back to a different canonical axis.
    auto project_perp = [&](const Vec3& candidate) -> std::optional<Vec3> {
        const double dot = candidate.x * normal.x +
                            candidate.y * normal.y +
                            candidate.z * normal.z;
        Vec3 p{ candidate.x - dot * normal.x,
                 candidate.y - dot * normal.y,
                 candidate.z - dot * normal.z };
        const double L = std::sqrt(p.x*p.x + p.y*p.y + p.z*p.z);
        if (L < 1e-9) return std::nullopt;
        return Vec3{ p.x / L, p.y / L, p.z / L };
    };
    Vec3 up;
    {
        Vec3 primary  = (a.plane == "xy") ? Vec3{ 0, 1, 0 } : Vec3{ 0, 0, 1 };
        Vec3 fallback = (a.plane == "xy") ? Vec3{ 0, 0, 1 } : Vec3{ 0, 1, 0 };
        if (auto u = project_perp(primary))      up = *u;
        else if (auto u = project_perp(fallback)) up = *u;
        else                                       up = { 1, 0, 0 };
    }

    // right = up × normal (spec §5.2).
    const Vec3 right{
        up.y * normal.z - up.z * normal.y,
        up.z * normal.x - up.x * normal.z,
        up.x * normal.y - up.y * normal.x
    };

    // Origin.
    Vec3 origin{ 0, 0, 0 };
    if (!a.origin_expr.empty()) {
        std::vector<ExpressionError> errs;
        if (auto o = e.evaluate_vector(a.origin_expr, sketch_node.source, errs)) {
            origin = *o;
        } else {
            std::string detail;
            if (!errs.empty()) detail = ": " + errs.front().message;
            warnings.push_back({
                "sketch: invalid `origin=\"" + a.origin_expr + "\"`" +
                detail, sketch_node.source });
            return std::nullopt;
        }
    }

    // Rotation around the normal (2D rotation in the right/up plane
    // applied to each profile vertex pre-lift). Surface the rich
    // expression error if it threw — promote_fatal_eval_warnings
    // keys off the message prefix.
    double rotation_deg = 0;
    if (!a.rotation_expr.empty()) {
        std::vector<ExpressionError> errs;
        if (auto r = e.evaluate_number(a.rotation_expr, sketch_node.source, errs)) {
            rotation_deg = *r;
        } else if (!errs.empty()) {
            warnings.push_back({
                "sketch: invalid `rotation=\"" + a.rotation_expr +
                "\"`: " + errs.front().message,
                sketch_node.source });
        }
    }
    constexpr double kPi = 3.14159265358979323846;
    const double cos_r = std::cos(rotation_deg * kPi / 180.0);
    const double sin_r = std::sin(rotation_deg * kPi / 180.0);

    // Canonicalize and lift.
    detail::ensure_ccw(*profile);
    detail::LoftSection sec;
    sec.profile_2d = *profile;
    sec.ring_3d.reserve(profile->points.size());
    for (const auto& p : profile->points) {
        const double rx = p.x * cos_r - p.y * sin_r;
        const double ry = p.x * sin_r + p.y * cos_r;
        sec.ring_3d.push_back({
            origin.x + rx * right.x + ry * up.x,
            origin.y + rx * right.y + ry * up.y,
            origin.z + rx * right.z + ry * up.z
        });
    }
    return sec;
}

// 3D dispatch. Handles Extrude, Revolve, Sweep, Loft, and the
// booleans (Union/Difference/Intersect/Hull). Helix and the
// modifiers (Fillet/Chamfer/Shell) fall through to default and
// warn — they land in subsequent slices.
FlatMesh eval_3d(const Document& doc, const Node& n,
                  ExpressionEvaluator& e,
                  std::vector<FlatEvalError>& warnings,
                  FlatMeshCache* cache,
                  int depth)
{
    auto first_2d_child = [&]() -> std::optional<detail::Polygon2D> {
        for (auto& child : doc.children(node_index(doc, n))) {
            if (child.dead) continue;
            if (auto p = eval_2d(doc, child, e, warnings)) return p;
        }
        return std::nullopt;
    };

    // Surface a diagnostic when a profile polygon is wound CW outside
    // an <svg> block. Inside <svg> CW is expected (the y-flip wrapper
    // is exactly the place where SVG y-down polygons land); outside it
    // it's almost always a mistake — typically a path copy-pasted from
    // an SVG file without the wrapper. ensure_ccw silently fixes the
    // winding either way, so this is purely educational, not blocking.
    auto warn_if_cw = [&](const detail::Polygon2D& p, const char* op) {
        if (inside_svg_frame()) return;
        if (p.points.size() < 3) return;
        if (detail::polygon_signed_area(p) >= 0) return;
        warnings.push_back({
            std::string(op) + ": profile polygon is wound clockwise. CADML's "
            "<path>/<rect>/<circle> coordinates are math y-up; an SVG-style "
            "(y-down) CCW polygon comes out CW in this frame. The engine will "
            "silently reverse it so the extrude succeeds, but if you pasted "
            "this from an SVG file, wrapping the content in <svg>...</svg> "
            "will both flip Y and silence this warning.",
            n.source });
    };

    switch (n.type) {
        case NodeType::Extrude: {
            const auto& a = std::get<ExtrudeAttrs>(n.attrs);
            // The v0.1 engine implements straight prismatic extrusion only;
            // tapered (scale), drafted, and non-+z extrudes are reserved
            // (see docs/spec/language.md). The bundler rejects them earlier;
            // this second check catches hand-edited .fcadml that bypasses
            // the bundler. Match semantically — `scale="1.0"` is the
            // default just like `scale="1"`.
            auto literal_equals = [](const std::string& expr, double want) {
                auto v = parse_double_strict(expr);
                return v.has_value() && *v == want;
            };
            auto direction_is_plus_z = [](std::string_view expr) {
                while (!expr.empty() && (expr.front() == ' ' || expr.front() == '\t'))
                    expr.remove_prefix(1);
                while (!expr.empty() && (expr.back() == ' ' || expr.back() == '\t'))
                    expr.remove_suffix(1);
                return expr == "+z" || expr == "z" ||
                       expr == "+Z" || expr == "Z";
            };
            if (!literal_equals(a.scale_expr, 1.0) ||
                !literal_equals(a.draft_expr, 0.0) ||
                !direction_is_plus_z(a.direction_expr)) {
                warnings.push_back({
                    "<extrude scale=|draft=|direction=> is not supported "
                    "in 0.1 — remove the attribute or use <loft> for a "
                    "tapered profile.",
                    n.source });
                return {};
            }
            const auto height = eval_num(e, a.height_expr, n.source,
                                          warnings, "extrude.height");
            auto profile = first_2d_child();
            if (!profile) {
                warnings.push_back({ "extrude has no 2D profile child", n.source });
                return {};
            }
            warn_if_cw(*profile, "extrude");
            auto mesh = detail::extrude_linear(
                *profile, height, node_index(doc, n));
            // symmetric="true" centres the extrude on z=0 (output spans
            // -h/2..+h/2). Implemented as a post-shift rather than a
            // dedicated extrude_linear path so existing callers stay
            // byte-identical for the default symmetric=false case.
            if (a.symmetric) {
                apply_transform_to_mesh(mesh,
                    Mat4::translation(0, 0, -height * 0.5));
            }
            return mesh;
        }
        case NodeType::Revolve: {
            const auto& a = std::get<RevolveAttrs>(n.attrs);
            if (a.axis.empty()) {
                warnings.push_back({
                    "revolve missing required `axis` attribute", n.source });
                return {};
            }
            const auto axis = parse_axis_alias(a.axis);
            if (!axis) {
                warnings.push_back({
                    "revolve `axis=\"" + a.axis + "\"` is not a known axis"
                    " alias (expected x/y/z, +x/+y/+z, or -x/-y/-z)",
                    n.source });
                return {};
            }
            const auto angle = eval_num(e, a.angle_expr, n.source,
                                          warnings, "revolve.angle");
            // Rotational facet count. Empty `segments` attr falls
            // back to the historical default (32). Authors bump
            // this for hero renders / precision parts where the
            // facet edges would otherwise be visible.
            int segments = 32;
            if (!a.segments_expr.empty()) {
                const auto raw = eval_num(e, a.segments_expr, n.source,
                                            warnings, "revolve.segments");
                if (raw >= 3.0 && raw < 4096.0) {
                    segments = static_cast<int>(raw);
                } else {
                    warnings.push_back({
                        "revolve.segments out of range [3, 4096); "
                        "using default 32", n.source });
                }
            }
            auto profile = first_2d_child();
            if (!profile) {
                warnings.push_back({ "revolve has no 2D profile child", n.source });
                return {};
            }
            warn_if_cw(*profile, "revolve");
            return detail::revolve(*profile, *axis, angle, segments,
                                     node_index(doc, n));
        }
        case NodeType::Loft: {
            // Polyhedral loft: 2+ <sketch> children, each providing
            // a positioned 3D ring. Connect with quad strips, ear-
            // clip the start and end caps. All sections must have
            // the same vertex count (we error otherwise).
            //
            // The bundler wraps each <for>-iteration in a <group> for
            // transform composition, so a `<for><sketch/></for>`
            // becomes `<group><sketch/></group>` after unrolling.
            // Descend through transparent <group> wrappers (no
            // transform attribute) to find the actual <sketch>.
            std::vector<detail::LoftSection> sections;
            std::function<void(const Node&)> visit;
            visit = [&](const Node& nd) {
                if (nd.dead) return;
                if (nd.type == NodeType::Param ||
                    nd.type == NodeType::Port) return;
                if (nd.type == NodeType::Sketch) {
                    if (auto s = eval_sketch(doc, nd, e, warnings)) {
                        sections.push_back(std::move(*s));
                    }
                    return;
                }
                if (nd.type == NodeType::Group) {
                    // Transparent passthrough — `<for>` unrolling wraps
                    // sketches in `<group iteration="...">` with no
                    // transform.
                    const auto& ga = std::get<GroupAttrs>(nd.attrs);
                    if (ga.transform.empty()) {
                        for (auto& gc : doc.children(node_index(doc, nd))) {
                            visit(gc);
                        }
                        return;
                    }
                }
                warnings.push_back({
                    "loft: child must be <sketch> (or transparent "
                    "<group>/<for> wrapping a sketch); ignoring",
                    nd.source });
            };
            for (auto& child : doc.children(node_index(doc, n))) {
                visit(child);
            }
            if (sections.size() < 2) {
                warnings.push_back({
                    "loft: need at least 2 <sketch> sections (got " +
                    std::to_string(sections.size()) + ")",
                    n.source });
                return {};
            }
            const auto P = sections[0].ring_3d.size();
            for (std::size_t i = 1; i < sections.size(); ++i) {
                if (sections[i].ring_3d.size() != P) {
                    warnings.push_back({
                        "loft: section " + std::to_string(i) +
                        " has " + std::to_string(sections[i].ring_3d.size()) +
                        " vertices but section 0 has " +
                        std::to_string(P) + "; all sections must have "
                        "the same vertex count for the polyhedral loft",
                        n.source });
                    return {};
                }
            }
            return detail::loft_polyhedral(sections, node_index(doc, n));
        }
        case NodeType::Sweep: {
            // <sweep> per spec §5.3: exactly two children, a 2D
            // profile and a <helix> guide curve. Find them, evaluate,
            // hand to detail::sweep_along_helix.
            std::optional<detail::Polygon2D> profile;
            const Node* helix_node = nullptr;
            for (auto& child : doc.children(node_index(doc, n))) {
                if (child.dead) continue;
                if (child.type == NodeType::Param ||
                    child.type == NodeType::Port) continue;
                if (child.type == NodeType::Helix) {
                    helix_node = &child;
                    continue;
                }
                if (!profile) {
                    if (auto p = eval_2d(doc, child, e, warnings)) {
                        profile = std::move(p);
                        continue;
                    }
                }
            }
            if (!profile) {
                warnings.push_back({
                    "sweep: missing 2D profile child", n.source });
                return {};
            }
            warn_if_cw(*profile, "sweep");
            if (!helix_node) {
                warnings.push_back({
                    "sweep: missing <helix> guide-curve child", n.source });
                return {};
            }

            const auto& ha = std::get<HelixAttrs>(helix_node->attrs);
            const auto rad = eval_num(e, ha.radius_expr,
                helix_node->source, warnings, "helix.radius");
            const auto pit = eval_num(e, ha.pitch_expr,
                helix_node->source, warnings, "helix.pitch");
            const auto tns = eval_num(e, ha.turns_expr,
                helix_node->source, warnings, "helix.turns");
            const auto tap = ha.taper_expr.empty() ? 0.0
                : eval_num(e, ha.taper_expr,
                    helix_node->source, warnings, "helix.taper");

            if (rad <= 0) {
                warnings.push_back({
                    "helix: radius must be positive", helix_node->source });
                return {};
            }
            if (tns <= 0) {
                warnings.push_back({
                    "helix: turns must be positive", helix_node->source });
                return {};
            }
            if (std::abs(pit) < 1e-12) {
                warnings.push_back({
                    "helix: pitch=0 collapses to a planar circle "
                    "(use <revolve> for that case)",
                    helix_node->source });
                return {};
            }

            int dir_sign = 1;
            if (ha.direction == "cw") {
                dir_sign = -1;
            } else if (!ha.direction.empty() && ha.direction != "ccw") {
                warnings.push_back({
                    "helix: direction must be 'ccw' or 'cw'; got `" +
                    ha.direction + "`", helix_node->source });
            }

            return detail::sweep_along_helix(
                *profile, rad, pit, tns, tap, dir_sign,
                /*segments_per_turn=*/32, node_index(doc, n));
        }
        case NodeType::Chamfer: {
            const auto& a = std::get<ChamferAttrs>(n.attrs);
            const auto distance = eval_num(e, a.distance_expr, n.source,
                                              warnings, "chamfer.distance");
            // Walk children — chamfer takes a single 3D input mesh
            // (typically `<extrude>` or a boolean) and applies the bevel.
            FlatMesh subject;
            for (auto& child : doc.children(node_index(doc, n))) {
                if (child.dead) continue;
                FlatMesh sub = eval_node(doc, child, e, warnings, cache, depth);
                if (!sub.vertices.empty()) {
                    if (subject.vertices.empty()) subject = std::move(sub);
                    else                          merge_mesh(subject, sub);
                }
            }
            if (subject.vertices.empty()) {
                warnings.push_back({ "chamfer has no 3D child", n.source });
                return {};
            }
            auto sp = cadml::parse_selector(a.select);
            if (!sp.ok) {
                // Syntax is validated by the bundler; a bad selector here
                // means a hand-authored .fcadml skipped that check. Surface
                // it and leave the mesh unmodified (no silent fallback).
                warnings.push_back({ "chamfer: invalid select `" + a.select +
                    "`: " + sp.error + "; mesh left unchanged", n.source });
                return subject;
            }
            auto chr = detail::chamfer(subject, distance, sp.selector,
                                        node_index(doc, n));
            if (!chr.ok) {
                warnings.push_back({ chr.error, n.source });
                return subject;       // best-effort: return unchamfered subject
            }
            if (!chr.warning.empty())
                warnings.push_back({ chr.warning, n.source });
            return chr.mesh;
        }
        case NodeType::Fillet: {
            const auto& a = std::get<FilletAttrs>(n.attrs);
            const auto radius = eval_num(e, a.radius_expr, n.source,
                                            warnings, "fillet.radius");
            FlatMesh subject;
            for (auto& child : doc.children(node_index(doc, n))) {
                if (child.dead) continue;
                FlatMesh sub = eval_node(doc, child, e, warnings, cache, depth);
                if (!sub.vertices.empty()) {
                    if (subject.vertices.empty()) subject = std::move(sub);
                    else                          merge_mesh(subject, sub);
                }
            }
            if (subject.vertices.empty()) {
                warnings.push_back({ "fillet has no 3D child", n.source });
                return {};
            }
            auto sp = cadml::parse_selector(a.select);
            if (!sp.ok) {
                warnings.push_back({ "fillet: invalid select `" + a.select +
                    "`: " + sp.error + "; mesh left unchanged", n.source });
                return subject;
            }
            auto fr = detail::fillet(subject, radius, sp.selector,
                                      node_index(doc, n));
            if (!fr.ok) {
                warnings.push_back({ fr.error, n.source });
                return subject;       // best-effort: unfilleted subject
            }
            if (!fr.warning.empty())
                warnings.push_back({ fr.warning, n.source });
            return fr.mesh;
        }
        case NodeType::Shell: {
            // <shell> needs to inspect its child's TYPE: shell-via-2D-
            // offset works on <extrude> of a convex profile (and
            // <revolve> in a future slice). Other inputs error per
            // spec §13.
            const auto& a = std::get<ShellAttrs>(n.attrs);
            const auto thickness = eval_num(e, a.thickness_expr,
                n.source, warnings, "shell.thickness");

            // Find the single 3D-producing child.
            const Node* child_3d = nullptr;
            for (auto& child : doc.children(node_index(doc, n))) {
                if (child.dead) continue;
                if (child.type == NodeType::Param ||
                    child.type == NodeType::Port) continue;
                child_3d = &child;
                break;
            }
            if (!child_3d) {
                warnings.push_back({ "shell has no 3D child", n.source });
                return {};
            }

            // `open` is resolved per-profile inside the extrude branch
            // below — it may be a §13 face selector that has to be
            // evaluated against the shell's actual cap faces.

            if (child_3d->type == NodeType::Extrude) {
                const auto& ea = std::get<ExtrudeAttrs>(child_3d->attrs);
                const auto height = eval_num(e, ea.height_expr,
                    child_3d->source, warnings, "extrude.height");
                std::optional<detail::Polygon2D> profile;
                for (auto& gc : doc.children(node_index(doc, *child_3d))) {
                    if (gc.dead) continue;
                    if (auto p = eval_2d(doc, gc, e, warnings)) {
                        profile = std::move(p);
                        break;
                    }
                }
                if (!profile) {
                    warnings.push_back({
                        "shell: child <extrude> has no 2D profile",
                        n.source });
                    return {};
                }

                // Resolve `open`. Two forms are accepted:
                //   * legacy keywords `start` / `end` (and `start end`),
                //     mapping to the -Z / +Z cap faces, and
                //   * a §13 face selector (e.g. face:normal=+z,
                //     face:position.z=0), evaluated against the two cap
                //     faces (start = -Z at z=0, end = +Z at z=height).
                // Empty = closed; open="all" is invalid (spec §12.4).
                bool open_start = false, open_end = false;
                {
                    const std::string raw = a.open;
                    std::size_t b = 0, en = raw.size();
                    auto ws = [](char c){ return c==' '||c=='\t'||c=='\n'||c=='\r'; };
                    while (b < en && ws(raw[b])) ++b;
                    while (en > b && ws(raw[en - 1])) --en;
                    const std::string s = raw.substr(b, en - b);
                    if (!s.empty()) {
                        std::vector<std::string> toks;
                        std::size_t i = 0;
                        auto is_sep = [](char c){ return c==','||c==' '||c=='\t'; };
                        while (i < s.size()) {
                            while (i < s.size() && is_sep(s[i])) ++i;
                            const std::size_t ts = i;
                            while (i < s.size() && !is_sep(s[i])) ++i;
                            if (i > ts) toks.push_back(s.substr(ts, i - ts));
                        }
                        bool all_kw = !toks.empty();
                        for (const auto& t : toks)
                            if (t != "start" && t != "end") { all_kw = false; break; }
                        if (all_kw) {
                            for (const auto& t : toks) {
                                if (t == "start") open_start = true;
                                else              open_end   = true;
                            }
                        } else {
                            auto sp = cadml::parse_selector(s);
                            if (!sp.ok) {
                                warnings.push_back({ "shell open=`" + s + "`: " +
                                    sp.error + "; treating shell as closed",
                                    n.source });
                            } else if (sp.selector.is_all()) {
                                warnings.push_back({ "shell open=\"all\" is "
                                    "invalid (would remove every face); "
                                    "treating shell as closed", n.source });
                            } else if (!sp.selector.is_face()) {
                                warnings.push_back({ "shell open expects a face "
                                    "selector (face:…) or `start`/`end`; "
                                    "treating shell as closed", n.source });
                            } else {
                                double cx = 0, cy = 0;
                                for (const auto& pt : profile->points) {
                                    cx += pt.x; cy += pt.y;
                                }
                                if (!profile->points.empty()) {
                                    cx /= static_cast<double>(profile->points.size());
                                    cy /= static_cast<double>(profile->points.size());
                                }
                                const double area = std::fabs(
                                    detail::polygon_signed_area(*profile));
                                const cadml::FaceProps start_cap{
                                    {0, 0, -1}, {cx, cy, 0.0}, area };
                                const cadml::FaceProps end_cap{
                                    {0, 0, 1}, {cx, cy, height}, area };
                                open_start = sp.selector.matches(start_cap);
                                open_end   = sp.selector.matches(end_cap);
                                if (!open_start && !open_end) {
                                    warnings.push_back({ "shell open=`" + s +
                                        "` matched no cap face; shell will be "
                                        "closed", n.source });
                                }
                            }
                        }
                    }
                }

                auto sr = detail::shell_extrude(*profile, height,
                    thickness, open_start, open_end,
                    node_index(doc, n));
                if (!sr.ok) {
                    warnings.push_back({ sr.error, n.source });
                    // Fall back to the unshelled subject so the user
                    // still sees something.
                    return detail::extrude_linear(*profile, height,
                        node_index(doc, *child_3d));
                }
                return sr.mesh;
            }

            warnings.push_back({
                "shell: child must be <extrude> (got an unsupported"
                " node type — <revolve> is a planned follow-up)",
                n.source });
            return {};
        }
        case NodeType::Union:
        case NodeType::Difference:
        case NodeType::Intersect: {
            // Each child evaluates to a 3D mesh via eval_node, which
            // handles primitives AND <group> wrappers (so a bundler
            // assembly's `<group transform="..."><extrude/></group>`
            // composes correctly inside booleans).
            std::vector<FlatMesh> inputs;
            for (auto& child : doc.children(node_index(doc, n))) {
                if (child.dead) continue;
                FlatMesh sub = eval_node(doc, child, e, warnings, cache, depth);
                if (!sub.vertices.empty()) inputs.push_back(std::move(sub));
            }
            const auto op = (n.type == NodeType::Union)      ? detail::BoolOp::Union
                          : (n.type == NodeType::Difference) ? detail::BoolOp::Difference
                                                              : detail::BoolOp::Intersect;
            auto br = detail::boolean_combine(inputs, op, node_index(doc, n));
            if (!br.ok) {
                warnings.push_back({ br.error, n.source });
                return {};
            }
            return br.mesh;
        }
        case NodeType::Hull: {
            // Convex hull of all 3D-producing children. Same child-
            // collection pattern as the booleans above. Single child
            // is allowed (returns the hull of that single solid);
            // empty child list is a warning.
            std::vector<FlatMesh> inputs;
            for (auto& child : doc.children(node_index(doc, n))) {
                if (child.dead) continue;
                FlatMesh sub = eval_node(doc, child, e, warnings, cache, depth);
                if (!sub.vertices.empty()) inputs.push_back(std::move(sub));
            }
            if (inputs.empty()) {
                warnings.push_back({
                    "hull: no 3D children", n.source });
                return {};
            }
            auto hr = detail::hull_combine(inputs, node_index(doc, n));
            if (!hr.ok) {
                warnings.push_back({ hr.error, n.source });
                return {};
            }
            return hr.mesh;
        }
        case NodeType::Cut: {
            // <cut> per spec §12.5: build a tilted cutting slab pivoted
            // on the "stays" edge of the target's bbox, intersect with
            // a bounding box so steep angles don't clip the opposite
            // face, then subtract from the target. Mode 1 (typed:
            // miter/bevel/compound): synthesised slab. Mode 2 (freeform):
            // additional children act as user-supplied cutter geometry,
            // subtracted in turn.
            const auto& a = std::get<CutAttrs>(n.attrs);

            // Collect children. First = target, rest = freeform cutters.
            std::vector<FlatMesh> children;
            for (auto& child : doc.children(node_index(doc, n))) {
                if (child.dead) continue;
                FlatMesh sub = eval_node(doc, child, e, warnings, cache, depth);
                if (!sub.vertices.empty()) children.push_back(std::move(sub));
            }
            if (children.empty()) {
                warnings.push_back({
                    "cut: no target mesh (first child must produce geometry)",
                    n.source });
                return {};
            }
            FlatMesh target = std::move(children[0]);

            // Validate face attribute.
            const std::string& face = a.face;
            if (face != "start" && face != "end") {
                warnings.push_back({
                    "cut: face must be `start` or `end` (got `" + face + "`)",
                    n.source });
                return target;
            }

            // ── Mode 1: synthesised cut wedge ─────────────────────────
            const std::string& cut_type = a.type;
            if (!cut_type.empty()) {
                double miter_deg = 0;
                double bevel_deg = 0;
                if      (cut_type == "miter")   miter_deg = eval_num(e, a.angle_expr,  n.source, warnings, "cut.angle");
                else if (cut_type == "bevel")   bevel_deg = eval_num(e, a.angle_expr,  n.source, warnings, "cut.angle");
                else if (cut_type == "compound") {
                    miter_deg = eval_num(e, a.miter_expr, n.source, warnings, "cut.miter");
                    bevel_deg = eval_num(e, a.bevel_expr, n.source, warnings, "cut.bevel");
                } else {
                    warnings.push_back({
                        "cut: type must be one of miter/bevel/compound (got `" +
                        cut_type + "`)", n.source });
                    return target;
                }

                // Sub-degree angles: nothing to cut. Skip the slab and
                // return the target unmodified.
                if (std::abs(miter_deg) > 0.01 || std::abs(bevel_deg) > 0.01) {
                    // Compute target's bbox by walking vertices.
                    double mnx = target.vertices[0].x, mxx = mnx;
                    double mny = target.vertices[0].y, mxy = mny;
                    double mnz = target.vertices[0].z, mxz = mnz;
                    for (const auto& v : target.vertices) {
                        mnx = std::min(mnx, v.x); mxx = std::max(mxx, v.x);
                        mny = std::min(mny, v.y); mxy = std::max(mxy, v.y);
                        mnz = std::min(mnz, v.z); mxz = std::max(mxz, v.z);
                    }
                    const double face_z = (face == "end") ? mxz : mnz;
                    const double cx     = (mnx + mxx) * 0.5;
                    const double cy     = (mny + mxy) * 0.5;
                    const double dx     = mxx - mnx;
                    const double dy     = mxy - mny;
                    double extent = std::max(dx, dy);
                    if (extent < 1e-6) extent = 1.0;
                    const double pad = 1.0;
                    const double stock_len = mxz - mnz;
                    const double slab_size = std::max(extent * 3, stock_len + extent);

                    // Slab: a flat 2D rectangle extruded along +z. Big
                    // enough to cover anything past the cutting plane
                    // before we bound it.
                    auto slab_profile = detail::tessellate_rect(
                        -slab_size, -slab_size, slab_size * 2, slab_size * 2);
                    FlatMesh slab = detail::extrude_linear(
                        slab_profile, slab_size, node_index(doc, n));

                    // For the start face, the slab must extend in -z
                    // before rotation (it lives below z=0 in its local
                    // frame). End face gets the natural +z extrusion.
                    if (face == "start") {
                        apply_transform_to_mesh(slab,
                            Mat4::translation(0, 0, -slab_size));
                    }

                    // Pivot at the stays-edge per the spec sign rule.
                    // Positive miter -> -X edge stays, +X edge cut.
                    // Negative miter -> the reverse. Bevel = same on Y.
                    const double sign = (face == "end") ? 1.0 : -1.0;
                    Mat4 rot = Mat4::identity();
                    double pivot_x = cx;
                    double pivot_y = cy;
                    if (std::abs(miter_deg) > 0.01) {
                        rot = Mat4::rotation(sign * miter_deg, 0, 1, 0) * rot;
                        pivot_x = (miter_deg > 0) ? mnx : mxx;
                    }
                    if (std::abs(bevel_deg) > 0.01) {
                        rot = Mat4::rotation(-sign * bevel_deg, 1, 0, 0) * rot;
                        pivot_y = (bevel_deg > 0) ? mny : mxy;
                    }

                    apply_transform_to_mesh(slab,
                        Mat4::translation(pivot_x, pivot_y, face_z) * rot);

                    // Bounding box for the slab so steep angles don't
                    // accidentally clip the opposite face. cut_depth
                    // approximates how far into the stock the rotated
                    // slab dips along z.
                    const double max_tan = std::max(
                        std::abs(miter_deg) > 0.01
                            ? std::abs(std::tan(miter_deg * 3.14159265358979323846 / 180.0))
                            : 0.0,
                        std::abs(bevel_deg) > 0.01
                            ? std::abs(std::tan(bevel_deg * 3.14159265358979323846 / 180.0))
                            : 0.0);
                    const double cut_depth = (extent / 2) * max_tan;
                    const double bound_h = std::min(cut_depth + extent, stock_len) + 2 * pad;

                    auto bound_profile = detail::tessellate_rect(
                        mnx - pad, mny - pad, dx + 2 * pad, dy + 2 * pad);
                    FlatMesh bound = detail::extrude_linear(
                        bound_profile, bound_h, node_index(doc, n));
                    const double bound_z = (face == "end")
                        ? face_z - bound_h + pad
                        : face_z - pad;
                    apply_transform_to_mesh(bound,
                        Mat4::translation(0, 0, bound_z));

                    // cutting_volume = slab ∩ bound; target = target − cutting_volume.
                    std::vector<FlatMesh> isect_inputs;
                    isect_inputs.push_back(std::move(slab));
                    isect_inputs.push_back(std::move(bound));
                    auto ir = detail::boolean_combine(
                        isect_inputs, detail::BoolOp::Intersect, node_index(doc, n));
                    if (!ir.ok) {
                        warnings.push_back({
                            std::string("cut: bound intersect failed: ") + ir.error,
                            n.source });
                    } else {
                        std::vector<FlatMesh> diff_inputs;
                        diff_inputs.push_back(std::move(target));
                        diff_inputs.push_back(std::move(ir.mesh));
                        auto dr = detail::boolean_combine(
                            diff_inputs, detail::BoolOp::Difference, node_index(doc, n));
                        if (!dr.ok) {
                            warnings.push_back({
                                std::string("cut: difference failed: ") + dr.error,
                                n.source });
                            // Recover the target from the diff inputs (it
                            // was moved). On boolean failure we just skip
                            // the slab and continue to mode-2 freeform.
                            target = std::move(diff_inputs[0]);
                        } else {
                            target = std::move(dr.mesh);
                        }
                    }
                }
            }

            // ── Mode 2: freeform cutters (additional children) ───────
            for (std::size_t i = 1; i < children.size(); ++i) {
                std::vector<FlatMesh> diff_inputs;
                diff_inputs.push_back(std::move(target));
                diff_inputs.push_back(std::move(children[i]));
                auto dr = detail::boolean_combine(
                    diff_inputs, detail::BoolOp::Difference, node_index(doc, n));
                if (!dr.ok) {
                    warnings.push_back({
                        std::string("cut: freeform subtract failed: ") + dr.error,
                        n.source });
                    target = std::move(diff_inputs[0]);
                    break;
                }
                target = std::move(dr.mesh);
            }
            return target;
        }
        default:
            warnings.push_back({
                "unsupported 3D node type (B2.x supports extrude, revolve,"
                " union, difference, intersect)",
                n.source });
            return {};
    }
}

// Walk a node's children and union every geometry-producing result.
// Shared by eval_part, eval_group, and eval_instance; routes per-child
// through eval_node so the dispatch table lives in one place.
FlatMesh eval_geometry_children(const Document& doc, const Node& parent,
                                  ExpressionEvaluator& e,
                                  std::vector<FlatEvalError>& warnings,
                                  FlatMeshCache* cache,
                                  int depth)
{
    FlatMesh out;
    for (auto& child : doc.children(node_index(doc, parent))) {
        if (child.dead) continue;
        FlatMesh sub = eval_node(doc, child, e, warnings, cache, depth);
        if (!sub.vertices.empty()) merge_mesh(out, sub);
    }
    return out;
}

// <group transform="..."> — evaluate children, then apply the
// transform to the result mesh (vertices + normals).
FlatMesh eval_group(const Document& doc, const Node& group_node,
                     ExpressionEvaluator& e,
                     std::vector<FlatEvalError>& warnings,
                     FlatMeshCache* cache,
                     int depth)
{
    const auto& ga = std::get<GroupAttrs>(group_node.attrs);
    FlatMesh inner = eval_geometry_children(doc, group_node, e, warnings,
                                              cache, depth);
    if (!ga.transform.empty()) {
        const auto subbed = substitute_transform_exprs(
            ga.transform, e, group_node.source, warnings);
        const Mat4 M = parse_transform_string(
            subbed, group_node.source, warnings);
        apply_transform_to_mesh(inner, M);
    }
    return inner;
}

// <svg> — coordinate-frame wrapper for pasted SVG content. SVG's
// y-down convention puts +y on screen as -y in CADML's y-up math
// space; we apply scale(1, -1, 1) to descendant geometry so a
// pasted snippet renders right-side-up (matching how the SVG
// looked in its source). Per-polygon winding canonicalisation
// (`ensure_ccw` in extrude_linear / revolve) plus the negative-
// determinant winding flip in `apply_transform_to_mesh` together
// keep face orientation outward through the y-flip.
//
// Nested <svg> composes naturally — two flips compose to identity
// (det = +1, no winding flip), so an <svg> inside <svg> renders
// in the original CADML orientation. Useful pattern for
// re-orienting an authored CADML snippet inside an SVG paste.
//
// `<svg>` carries no authored attributes today; future revisions
// may add `viewBox` for SVG-units-to-mm mapping.
// Thread-local depth counter for "are we inside an <svg> block?" —
// used to suppress the CW-polygon warning for content inside <svg>
// (where SVG-pasted polygons are EXPECTED to be CW in math y-up).
// Engine evaluation is single-threaded today (per the FlatMeshCache
// header), so a thread_local counter is well-defined.
thread_local int g_svg_depth = 0;

struct SvgDepthGuard {
    SvgDepthGuard()  noexcept { ++g_svg_depth; }
    ~SvgDepthGuard() noexcept { --g_svg_depth; }
    SvgDepthGuard(const SvgDepthGuard&) = delete;
    SvgDepthGuard& operator=(const SvgDepthGuard&) = delete;
};

bool inside_svg_frame() noexcept { return g_svg_depth > 0; }

FlatMesh eval_svg(const Document& doc, const Node& svg_node,
                   ExpressionEvaluator& e,
                   std::vector<FlatEvalError>& warnings,
                   FlatMeshCache* cache,
                   int depth)
{
    SvgDepthGuard guard;
    FlatMesh inner = eval_geometry_children(doc, svg_node, e, warnings,
                                              cache, depth);
    apply_transform_to_mesh(inner, Mat4::scaling(1, -1, 1));
    return inner;
}

FlatMesh eval_part(const Document& doc, const Node& part_node,
                    std::vector<FlatEvalError>& warnings,
                    FlatMeshCache* cache)
{
    ExpressionEvaluator e;
    // Bind <param> children of the part into the eval scope. If the
    // default expression throws (div-by-zero, undefined identifier,
    // …) surface the rich message as a warning so the user knows
    // why a downstream expression references an unbound param.
    // promote_fatal_eval_warnings keys off the div/mod-by-zero
    // prefix and reclassifies to a hard error.
    for (auto& child : doc.children(node_index(doc, part_node))) {
        if (child.dead) continue;
        if (child.type != NodeType::Param) continue;
        const auto& pa = std::get<ParamAttrs>(child.attrs);
        std::vector<ExpressionError> errs;
        if (auto v = e.evaluate_number(pa.value_expr, child.source, errs)) {
            e.set_param(pa.name, *v);
        } else if (!errs.empty()) {
            warnings.push_back({
                "param `" + pa.name +
                "` default expression `" + pa.value_expr +
                "` failed: " + errs.front().message,
                child.source });
        }
    }
    return eval_geometry_children(doc, part_node, e, warnings, cache,
                                    /*depth=*/0);
}

// Inside an imported def's body, a sub-instance's `ref_name` is local
// to that def's namespace. The host's def index keys the imported
// type as "<containing-def>.<ref_name>" (assigned by the bundler's
// merge_imported_doc). Resolve by trying the qualified name first
// then falling back to the bare name.
//
// Mirror of the same fix in libcadml_compile's validate_param_overrides
// — engine resolution should match bundler resolution.
std::int64_t resolve_def_for_instance(const Document& doc,
                                        const Node& inst_node)
{
    const auto& ia = std::get<InstanceAttrs>(inst_node.attrs);
    // Walk up to the closest Def/Part ancestor and qualify with its name.
    auto cur = inst_node.parent;
    while (cur != NO_NODE) {
        const auto& a = doc.nodes[cur];
        std::string prefix;
        if (a.type == NodeType::Def)       prefix = std::get<DefAttrs>(a.attrs).name;
        else if (a.type == NodeType::Part) prefix = std::get<PartAttrs>(a.attrs).name;
        if (!prefix.empty()) {
            const auto it = doc.defs.find(prefix + "." + ia.ref_name);
            if (it != doc.defs.end()) return static_cast<std::int64_t>(it->second);
            break;   // closest ancestor wins; don't keep walking
        }
        cur = a.parent;
    }
    const auto it = doc.defs.find(ia.ref_name);
    if (it == doc.defs.end()) return -1;
    return static_cast<std::int64_t>(it->second);
}

// <my-part-name [k="v" ...]/> — bare instance dispatch.
//
// Two-stage scope handling:
//   1. Override expressions evaluate in the CALLER's scope so that
//      `<plate size="{outer * 2}"/>` can reference caller params. The
//      caller's ExpressionEvaluator (`caller_scope`) is used here.
//   2. The def's body evaluates in a FRESH scope built from:
//        a. the def's own <param> children (defaults),
//        b. then override RESULTS (numeric, post-eval) shadow them.
//      The caller's scope is NOT visible inside the def — defs have
//      their own scope per the CADML spec (defs are reusable, scope
//      is local).
//
// Recursion guard: `depth` is incremented per nested instance and
// short-circuits at kMaxInstanceDepth so self-referential defs
// don't stack-overflow (`<def name="A"><A/></def>`).
//
// Source attribution note: triangles produced by an
// instance are attributed to the def's internal nodes (extrude,
// revolve, etc.), not the instance node. Click-to-source from a
// rendered triangle jumps to the def's geometry, not the
// instantiation site. This is the right default — the geometry is
// authored at the def. Future work could carry both via a separate
// channel if highlighting wants it.
//
// Override-of-unknown-param note: an override targeting
// a param the def doesn't declare gets stuffed into the eval scope and
// is unreferenced (silently no-op). Matches the bundler's
// validate_param_overrides — both layers are deliberately permissive
// here.
//
// Instance-children note: a flat-doc instance with own
// children (`<plate><something/></plate>`) ignores the children
// entirely; only the def's body is walked. The bundler doesn't emit
// such constructs, but a hand-crafted .fcadml could. Children are
// dropped without warning.
FlatMesh eval_instance(const Document& doc, const Node& inst_node,
                        ExpressionEvaluator& caller_scope,
                        std::vector<FlatEvalError>& warnings,
                        FlatMeshCache* cache,
                        int depth)
{
    const auto& ia = std::get<InstanceAttrs>(inst_node.attrs);

    if (depth >= kMaxInstanceDepth) {
        warnings.push_back({
            "instance `" + ia.ref_name + "`: recursion depth exceeded "
            "(possible self-referential def chain); aborting",
            inst_node.source });
        return {};
    }

    const auto def_resolved = resolve_def_for_instance(doc, inst_node);
    if (def_resolved < 0) {
        warnings.push_back({
            "instance `" + ia.ref_name + "` references unknown def",
            inst_node.source });
        return {};
    }
    const auto def_idx = static_cast<std::uint32_t>(def_resolved);
    const auto& def_node = doc.nodes[def_idx];
    if (def_node.dead) return {};   // defensive — shouldn't happen

    // Stage 1: evaluate override expressions in the CALLER's scope.
    // std::map (sorted) so the cache key is canonical regardless of
    // the param_overrides hash-map iteration order.
    std::map<std::string, double> override_values;
    for (const auto& [name, expr] : ia.param_overrides) {
        std::vector<ExpressionError> errs;
        auto v = caller_scope.evaluate_number(expr, inst_node.source, errs);
        if (!v) {
            warnings.push_back({
                "instance `" + ia.ref_name + "`: cannot evaluate override `" +
                name + "=" + expr + "` in caller scope", inst_node.source });
            continue;
        }
        override_values.emplace(name, *v);
    }

    // Cache key uses the def's CANONICAL name (the host-level qualified
    // entry from doc.defs, not the local ref_name) so two different
    // call sites that resolve to the same def share the same cache
    // entry. Pull it from the resolved def's attrs.
    const std::string canonical_def_name =
        (def_node.type == NodeType::Def)  ? std::get<DefAttrs>(def_node.attrs).name
      : (def_node.type == NodeType::Part) ? std::get<PartAttrs>(def_node.attrs).name
                                           : ia.ref_name;

    if (cache) {
        if (auto* hit = FlatMeshCacheAccess::lookup(
                *cache, canonical_def_name, override_values)) {
            return *hit;     // copy out — caller may transform/merge
        }
    }

    // Stage 2: build the def's scope — defaults first, then overrides.
    // Same diagnostic propagation as eval_part above so a
    // div-by-zero in a def's param default surfaces (and gets
    // promoted to an error by promote_fatal_eval_warnings).
    ExpressionEvaluator def_scope;
    for (auto& child : doc.children(def_idx)) {
        if (child.dead) continue;
        if (child.type != NodeType::Param) continue;
        const auto& pa = std::get<ParamAttrs>(child.attrs);
        std::vector<ExpressionError> errs;
        if (auto v = def_scope.evaluate_number(pa.value_expr, child.source, errs)) {
            def_scope.set_param(pa.name, *v);
        } else if (!errs.empty()) {
            warnings.push_back({
                "param `" + pa.name +
                "` default expression `" + pa.value_expr +
                "` failed: " + errs.front().message,
                child.source });
        }
    }
    for (const auto& [name, val] : override_values) {
        def_scope.set_param(name, val);
    }

    auto mesh = eval_geometry_children(doc, def_node, def_scope, warnings,
                                         cache, depth + 1);
    if (cache) {
        FlatMeshCacheAccess::store(*cache, canonical_def_name,
                                     override_values, mesh);
    }
    return mesh;
}

// Walk every descendant of `root` looking for the first 2D
// primitive that carries a non-empty SVG `fill="..."` attribute,
// or — when the walk crosses an Instance of a Def whose original
// `<part color="...">` was preserved by the bundler — the def's
// stored colour. Returns the colour verbatim; colour-name
// canonicalisation (e.g. "red" → "#ff0000") happens later inside
// parse_color_rgba.
//
// Pre-order DFS by node order: matches authoring intent of "first
// shape's colour wins" and is independent of how the bundler
// permutes siblings.
std::string first_descendant_fill(const Document& doc, const Node& root) {
    std::string found;
    auto check = [&](const Node& n) -> bool {
        return std::visit([&](const auto& a) -> bool {
            using A = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<A, CircleAttrs> ||
                          std::is_same_v<A, RectAttrs>   ||
                          std::is_same_v<A, PathAttrs>) {
                if (!a.fill.empty()) { found = a.fill; return true; }
            }
            return false;
        }, n.attrs);
    };
    std::function<bool(const Node&)> walk = [&](const Node& n) {
        if (check(n)) return true;
        // When the walk crosses an Instance, follow it into the
        // referenced def. The def's own colour (set by the bundler
        // when the imported `<part color="…">` was converted) wins
        // first; otherwise we recurse into the def body so a `fill=`
        // on a child primitive can still propagate. Tracking
        // `visited_defs` prevents an infinite loop on cyclic refs
        // (`check_def_cycles` catches them earlier, but guard here
        // too — colour lookup must not crash on malformed input).
        std::unordered_set<std::uint32_t> visited_defs;
        std::function<bool(const Node&)> walk_inner;
        walk_inner = [&](const Node& nd) {
            if (check(nd)) return true;
            if (nd.type == NodeType::Instance) {
                const auto& ia = std::get<InstanceAttrs>(nd.attrs);
                const auto it = doc.defs.find(ia.ref_name);
                if (it != doc.defs.end() &&
                    visited_defs.insert(it->second).second) {
                    const auto& def_node = doc.nodes[it->second];
                    if (def_node.type == NodeType::Def) {
                        const auto& da = std::get<DefAttrs>(def_node.attrs);
                        if (!da.color.empty()) {
                            found = da.color;
                            return true;
                        }
                        for (const auto& dc : doc.children(it->second)) {
                            if (dc.dead) continue;
                            if (walk_inner(dc)) return true;
                        }
                    }
                }
            }
            for (const auto& c : doc.children(node_index(doc, nd))) {
                if (c.dead) continue;
                if (walk_inner(c)) return true;
            }
            return false;
        };
        return walk_inner(n);
    };
    walk(root);
    return found;
}

void collect_parts(const Document& doc, FlatEvalResult& out,
                    FlatMeshCache* cache)
{
    for (const auto& n : doc.nodes) {
        if (n.dead) continue;
        if (n.type != NodeType::Part) continue;
        // Only top-level <part> elements count as renderable outputs.
        // A <part> nested inside a <def> is malformed authoring (defs
        // hold reusable geometry, not parts) — we ignore it here; the
        // parser/bundler is responsible for catching it upstream.
        if (n.parent != NO_NODE) continue;
        const auto& pa = std::get<PartAttrs>(n.attrs);
        // Color resolution order:
        //   1. Explicit `<part color="...">`.
        //   2. First non-empty `fill="..."` on a 2D primitive
        //      descendant — convention for SVG-pasted content where
        //      the author left fills on the source shapes instead of
        //      hand-authoring a part-level color.
        //   3. Empty (the renderer falls back to its default).
        std::string color = pa.color;
        if (color.empty()) {
            color = first_descendant_fill(doc, n);
        }
        out.parts.push_back({
            pa.name,
            color,
            eval_part(doc, n, out.warnings, cache) });
    }
}

}  // namespace

// Promote any warning whose payload signals a documented HARD error
// (today: arithmetic division/modulo by zero) into the result's
// errors channel. The recursive evaluator threads only a warnings
// channel; keeping that signature unchanged but reclassifying after
// the fact gives us the spec-promised behaviour without rewriting
// every recursive function. The message prefixes match the ones
// thrown by src/cadml/src/expression.cpp.
void promote_fatal_eval_warnings(FlatEvalResult& result) {
    static const char* const kFatalSubstrings[] = {
        "division by zero in expression",
        "modulo by zero in expression",
    };
    std::vector<FlatEvalError> survivors;
    survivors.reserve(result.warnings.size());
    for (auto& w : result.warnings) {
        bool fatal = false;
        for (const char* needle : kFatalSubstrings) {
            if (w.message.find(needle) != std::string::npos) {
                fatal = true;
                break;
            }
        }
        if (fatal) result.errors.push_back(std::move(w));
        else       survivors.push_back(std::move(w));
    }
    result.warnings = std::move(survivors);
}

FlatEvalResult evaluate_flat(const Document& doc, const EvalOptions& opts) {
    FlatEvalResult result;

    validate_boundary(doc, result.errors);
    if (!result.ok()) return result;

    collect_parts(doc, result, opts.cache);

    // Per-node world transforms — populated by re-walking the doc
    // with the same parse + substitute logic the eval recursion
    // uses. Independent pass keeps the eval recursion uncluttered.
    result.node_world_transforms =
        accumulated_transforms(doc, result.warnings);

    // Reclassify fatal-class warnings (div-by-zero etc.) into errors.
    promote_fatal_eval_warnings(result);

    return result;
}

// Walk a part subtree binding its <param> children into a fresh
// evaluator (mirrors eval_part), then recurse over <group transform>
// children to accumulate transforms.
namespace {
void walk_part_for_transforms(const Document& doc,
                                std::uint32_t part_root,
                                Mat4 root_world,
                                std::unordered_map<std::uint32_t, Mat4>& out,
                                std::vector<FlatEvalError>& warnings)
{
    if (part_root == NO_NODE || part_root >= doc.nodes.size()) return;
    const auto& part = doc.nodes[part_root];
    out[part_root] = root_world;

    ExpressionEvaluator e;
    // Both <part> and <def> can host their own <param> scope.
    // Defs are visited so that flat_holes' def-internal <circle>
    // nodes get a world transform too.
    if (part.type == NodeType::Part || part.type == NodeType::Def) {
        for (auto& cn : doc.children(part_root)) {
            if (cn.dead || cn.type != NodeType::Param) continue;
            const auto& pa = std::get<ParamAttrs>(cn.attrs);
            std::vector<ExpressionError> errs;
            if (auto v = e.evaluate_number(pa.value_expr, cn.source, errs)) {
                e.set_param(pa.name, *v);
            } else if (!errs.empty()) {
                // Same diagnostic propagation as eval_part /
                // eval_instance so the world-transform walk's
                // unbound params get a user-visible explanation.
                warnings.push_back({
                    "param `" + pa.name +
                    "` default expression `" + pa.value_expr +
                    "` failed: " + errs.front().message,
                    cn.source });
            }
        }
    }

    // Iterative DFS within this part's subtree. Use doc.children()
    // consistently — same idiom every other engine pass uses.
    struct Frame { std::uint32_t node_idx; Mat4 parent_world; };
    std::vector<Frame> stack;
    for (auto& cn : doc.children(part_root)) {
        stack.push_back({node_index(doc, cn), root_world});
    }
    while (!stack.empty()) {
        const Frame f = stack.back(); stack.pop_back();
        if (f.node_idx == NO_NODE || f.node_idx >= doc.nodes.size()) continue;
        const auto& n = doc.nodes[f.node_idx];
        if (n.dead) continue;

        Mat4 local = Mat4::identity();
        if (n.type == NodeType::Group) {
            const auto& ga = std::get<GroupAttrs>(n.attrs);
            if (!ga.transform.empty()) {
                const auto subbed = substitute_transform_exprs(
                    ga.transform, e, n.source, warnings);
                local = parse_transform_string(
                    subbed, n.source, warnings);
            }
        }
        const Mat4 world = f.parent_world * local;
        out[f.node_idx] = world;

        for (auto& cn : doc.children(f.node_idx)) {
            stack.push_back({node_index(doc, cn), world});
        }
    }
}
}  // namespace

std::unordered_map<std::uint32_t, Mat4>
accumulated_transforms(const Document& doc,
                        std::vector<FlatEvalError>& warnings) {
    std::unordered_map<std::uint32_t, Mat4> out;
    // Each Part / Def is its own param scope. Walk every reachable
    // Part or Def root and accumulate transforms from there. Other
    // root types get identity (they have no geometry).
    //
    // Defs are walked because flat_holes / flat_topology surface
    // node ids from inside def bodies — callers that hand those
    // ids to anchors / measure / hole-axis features must find a
    // world transform.
    for (std::uint32_t i = 0; i < doc.nodes.size(); ++i) {
        const auto& n = doc.nodes[i];
        if (n.dead) continue;
        if (n.parent != NO_NODE) continue;   // only top-level
        if (n.type == NodeType::Part || n.type == NodeType::Def) {
            walk_part_for_transforms(doc, i, Mat4::identity(),
                                       out, warnings);
        } else {
            out[i] = Mat4::identity();
        }
    }
    return out;
}

}  // namespace cadml::engine
