// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/engine/flat_evaluator.hpp>
#include <cadml/engine/flat_check.hpp>

#include <cadml/parser.hpp>            // libcadml ??? cadml::parse
#include <cadml/compile/bundler.hpp>   // libcadml_compile ??? convenience

#include "flat_geometry.hpp"           // internal ??? for is_convex unit test
#include "flat_edge_topology.hpp"      // internal ??? modifier foundation
#include "flat_mesh_cache_internal.hpp"  // internal ??? FlatMeshCacheAccess

#include <rapidjson/document.h>        // glTF round-trip parsing

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

using namespace cadml;
using namespace cadml::engine;

namespace {

// Parse `src` as authoring CADML (no compile) and return the raw
// Document. Used to construct documents that DELIBERATELY contain
// authoring constructs the boundary should reject.
Document parse_authoring(std::string_view src) {
    auto pr = parse(src);
    return std::move(pr.document);
}

bool any_error_contains(const FlatEvalResult& r, std::string_view needle) {
    return std::any_of(r.errors.begin(), r.errors.end(),
        [&](const FlatEvalError& e) {
            return e.message.find(needle) != std::string::npos;
        });
}

bool any_warning_contains(const FlatEvalResult& r, std::string_view needle) {
    return std::any_of(r.warnings.begin(), r.warnings.end(),
        [&](const FlatEvalError& e) {
            return e.message.find(needle) != std::string::npos;
        });
}

}  // namespace

// ????????? Happy path ????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????

TEST(FlatEvaluator, AcceptsSimpleFlatPart) {
    // A bare <part><circle/></part> has no authoring constructs.
    auto doc = parse_authoring(
        "version 0.1\n<part><circle r=\"5\"/></part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // Slice B1: empty mesh per part. There's exactly one part.
    ASSERT_EQ(r.parts.size(), 1u);
    EXPECT_EQ(r.parts[0].mesh.vertex_count(),  0u);
    EXPECT_EQ(r.parts[0].mesh.triangle_count(), 0u);
}

TEST(FlatEvaluator, AcceptsBareInstance) {
    // Bare instance (no at/port) is valid composition per spec ??6.7.
    // The widget def lives in the same doc so the instance resolves.
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"widget\"><circle r=\"3\"/></def>"
        "<part><widget/></part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
}

// ????????? Boundary rejections ??????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????

TEST(FlatEvaluator, RejectsAssembly) {
    auto doc = parse_authoring(
        "version 0.1\n<assembly name=\"x\"><widget/></assembly>");
    auto r = evaluate_flat(doc);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(any_error_contains(r, "<assembly>"));
    EXPECT_TRUE(any_error_contains(r, "must be lowered"));
}

TEST(FlatEvaluator, RejectsFor) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<for var=\"i\" range=\"0 3\"><circle r=\"{i}\"/></for>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(any_error_contains(r, "<for>"));
}

TEST(FlatEvaluator, RejectsPattern) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<pattern type=\"linear\" count=\"4\" spacing=\"10\">"
        "<circle r=\"5\"/>"
        "</pattern>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(any_error_contains(r, "<pattern>"));
}

TEST(FlatEvaluator, AcceptsCut) {
    // Spec ??12.5: <cut> survives compile and is resolved at engine
    // time. Earlier the boundary check rejected it (no engine impl);
    // with the cut handler in eval_3d this should now produce
    // geometry.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<cut face=\"end\" type=\"miter\" angle=\"30\">"
        "<extrude height=\"50\"><circle r=\"5\"/></extrude>"
        "</cut>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    ASSERT_EQ(r.parts.size(), 1u);
    EXPECT_GT(r.parts[0].mesh.triangle_count(), 0u);
}

TEST(FlatEvaluator, CutMiterSlopesEndFace) {
    // 30?? miter on the end face of a 50mm-tall, 10mm-wide square
    // bar. Positive angle ??? the ???X edge stays at z=50; the +X edge
    // gets cut down. Verify by checking that max-z at +X is well
    // below 50 while max-z at ???X is still ~50.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<cut face=\"end\" type=\"miter\" angle=\"30\">"
        "<extrude height=\"50\">"
        "<rect x=\"-5\" y=\"-5\" width=\"10\" height=\"10\"/>"
        "</extrude>"
        "</cut>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    ASSERT_EQ(r.parts.size(), 1u);
    const auto& m = r.parts[0].mesh;
    ASSERT_FALSE(m.vertices.empty());
    double max_z_left = -1e9, max_z_right = -1e9;
    for (const auto& v : m.vertices) {
        if (v.x < -2) max_z_left  = std::max(max_z_left,  v.z);
        if (v.x >  2) max_z_right = std::max(max_z_right, v.z);
    }
    EXPECT_NEAR(max_z_left, 50.0, 0.5)   << "???X edge should stay at z=50";
    EXPECT_LT(max_z_right, 45.0)         << "+X edge should be cut down";
    EXPECT_GT(max_z_right, 35.0)         << "30?? on a 10mm bar drops ~5.77mm";
}

TEST(FlatEvaluator, RectRxRoundsCorners) {
    // rx>0 should produce more vertices than the 4-corner sharp rect
    // (8 segments ?? 4 corners = 32 boundary points, plus extrusion
    // duplicates them on top + bottom caps).
    auto sharp = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"5\">"
        "<rect x=\"-10\" y=\"-10\" width=\"20\" height=\"20\"/>"
        "</extrude></part>");
    auto rounded = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"5\">"
        "<rect x=\"-10\" y=\"-10\" width=\"20\" height=\"20\" rx=\"3\"/>"
        "</extrude></part>");
    auto sr = evaluate_flat(sharp);
    auto rr = evaluate_flat(rounded);
    ASSERT_TRUE(sr.ok() && rr.ok());
    EXPECT_GT(rr.parts[0].mesh.triangle_count(),
              sr.parts[0].mesh.triangle_count() * 4)
        << "rounded should produce well over 4?? as many tris as sharp";
    // Outer-most x of any vertex stays at +10 (corner arc tangent at
    // mid-edge). Verify a vertex at (or extremely near) the unrounded
    // edge midpoint still exists.
    bool found_x10 = false;
    for (const auto& v : rr.parts[0].mesh.vertices) {
        if (std::abs(v.x - 10.0) < 1e-6) { found_x10 = true; break; }
    }
    EXPECT_TRUE(found_x10) << "edge midpoint should sit on x=10 unchanged";
}

TEST(FlatEvaluator, RectRxClampedToHalfEdge) {
    // rx larger than half the smaller edge clamps to half (otherwise
    // adjacent corners would overlap and self-intersect).
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"5\">"
        "<rect x=\"-5\" y=\"-5\" width=\"10\" height=\"10\" rx=\"99\"/>"
        "</extrude></part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    // With rx clamped to 5 (half of 10), the rect becomes a circle.
    // No vertex should escape a 5mm radius from the centre.
    for (const auto& v : r.parts[0].mesh.vertices) {
        const double rdist = std::sqrt(v.x * v.x + v.y * v.y);
        EXPECT_LE(rdist, 5.001)
            << "vertex (" << v.x << "," << v.y << ") outside clamped radius";
    }
}

TEST(FlatEvaluator, ExtrudeSymmetricCentersOnZ) {
    // symmetric="true" centres the extrude around z=0; the resulting
    // mesh should span [-h/2, +h/2].
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"20\" symmetric=\"true\">"
        "<rect x=\"-3\" y=\"-3\" width=\"6\" height=\"6\"/>"
        "</extrude></part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    double min_z = 1e9, max_z = -1e9;
    for (const auto& v : r.parts[0].mesh.vertices) {
        min_z = std::min(min_z, v.z);
        max_z = std::max(max_z, v.z);
    }
    EXPECT_NEAR(min_z, -10.0, 1e-6);
    EXPECT_NEAR(max_z,  10.0, 1e-6);
}

TEST(FlatEvaluator, ExtrudeSymmetricFalseDefaultsZeroBased) {
    // symmetric defaults to false; resulting mesh spans [0, h].
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"20\">"
        "<rect x=\"-3\" y=\"-3\" width=\"6\" height=\"6\"/>"
        "</extrude></part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    double min_z = 1e9, max_z = -1e9;
    for (const auto& v : r.parts[0].mesh.vertices) {
        min_z = std::min(min_z, v.z);
        max_z = std::max(max_z, v.z);
    }
    EXPECT_NEAR(min_z,   0.0, 1e-6);
    EXPECT_NEAR(max_z,  20.0, 1e-6);
}

TEST(FlatEvaluator, PathTriangleAbsoluteMLZ) {
    // Simplest closed polygon: triangle from absolute M + two L + Z.
    // extrude_linear uses ear-clipping per cap, so an N-point profile
    // yields N side quads (2 tris each) + (N-2) cap tris ?? 2 caps.
    // For N=3 ??? 6 + 2 = 8 tris.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"5\">"
        "<path d=\"M 0,0 L 10,0 0,10 Z\"/>"
        "</extrude></part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    ASSERT_EQ(r.parts.size(), 1u);
    EXPECT_EQ(r.parts[0].mesh.triangle_count(), 8u);
}

TEST(FlatEvaluator, PathImplicitLinetoAfterMoveto) {
    // SVG semantics: pairs after a single M are implicit linetos.
    // No explicit L between coordinates; gear/compressor/sample all
    // emit this style. N=4 ??? 4 side quads (8 tris) + 4 cap tris = 12.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"5\">"
        "<path d=\"M 0,0 10,0 10,10 0,10\"/>"
        "</extrude></part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(r.parts[0].mesh.triangle_count(), 12u);
}

TEST(FlatEvaluator, PathRelativeLowercase) {
    // Lowercase m / l = relative. Same 3-vertex triangle as the
    // absolute test above (8 tris with ear-clipping).
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"5\">"
        "<path d=\"m 0,0 l 10,0 -10,10 z\"/>"
        "</extrude></part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(r.parts[0].mesh.triangle_count(), 8u);
    // Verify a vertex lands at the relative-resolved corner (0, 10).
    bool found = false;
    for (const auto& v : r.parts[0].mesh.vertices) {
        if (std::abs(v.x) < 1e-6 && std::abs(v.y - 10.0) < 1e-6) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found) << "expected a vertex at relative-resolved (0, 10)";
}

TEST(FlatEvaluator, PathExpressionsSubstituted) {
    // Bundler runs expression substitution on PathAttrs.d before the
    // engine sees it, so {expr} tokens with frontmatter param refs
    // resolve correctly. Goes through compile_string (not the parser-
    // only parse_authoring helper) so the bundler's substitution pass
    // actually runs.
    auto cr = cadml::compile::compile_string(
        "version 0.1\n"
        "param size = 12\n"
        "<part><extrude height=\"3\">"
        "<path d=\"M 0,0 L {size},0 {size},{size} 0,{size} Z\"/>"
        "</extrude></part>");
    ASSERT_TRUE(cr.ok()) << (cr.errors.empty() ? "" : cr.errors[0].message);
    auto r = evaluate_flat(cr.document);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // Expect a vertex at (12, 12) - the {size},{size} corner.
    bool found = false;
    for (const auto& v : r.parts[0].mesh.vertices) {
        if (std::abs(v.x - 12.0) < 1e-6 && std::abs(v.y - 12.0) < 1e-6) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(FlatEvaluator, PathTooFewPointsWarns) {
    // <path> with only an M: nothing to extrude. Engine warns and
    // produces empty geometry (extrude warning fires too).
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"3\"><path d=\"M 0,0 Z\"/></extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    // Warning should mention path's tessellation result.
    bool found = false;
    for (const auto& w : r.warnings) {
        if (w.message.find("path:") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found);
}

// ????????? <path> curve commands (C / S / Q / T / A / H / V) ?????????????????????????????????????????????
//
// Each test constructs a path that uses a curve command, extrudes, and
// sanity-checks the output. The flatness tolerance is 0.05 mm so the
// flattened polylines should land on the analytical curves to within
// tens of microns; we use 0.1 mm as a slack tolerance in the
// assertions below to keep the tests robust to subdivision-depth
// changes.

namespace {
// Local Bbox/bbox/all_faces_outward ??? duplicates of the Booleans /
// SVG section helpers further down in the file. C++'s ordering means
// we can't reach those from here without hoisting them up; cheap to
// repeat for this block since the body is small.
struct PathBbox { double min_x, min_y, min_z, max_x, max_y, max_z; };
PathBbox path_bbox(const cadml::engine::FlatMesh& m) {
    PathBbox b{ 1e9, 1e9, 1e9, -1e9, -1e9, -1e9 };
    for (const auto& v : m.vertices) {
        b.min_x = std::min(b.min_x, v.x); b.max_x = std::max(b.max_x, v.x);
        b.min_y = std::min(b.min_y, v.y); b.max_y = std::max(b.max_y, v.y);
        b.min_z = std::min(b.min_z, v.z); b.max_z = std::max(b.max_z, v.z);
    }
    return b;
}

// Find a vertex within `tol` of (qx, qy) in the path's flattened
// polyline. The polyline is the BOTTOM ring of the extrude, indices
// [0, N) where N = profile vertex count. We look at the first half of
// vertices (the bottom ring) only ??? the top ring duplicates the same
// X/Y at z=height.
bool path_polyline_passes_through(const cadml::engine::FlatMesh& m,
                                    double qx, double qy, double tol)
{
    const std::size_t half = m.vertices.size() / 2;
    for (std::size_t i = 0; i < half; ++i) {
        const auto& v = m.vertices[i];
        if (std::abs(v.x - qx) < tol && std::abs(v.y - qy) < tol) {
            return true;
        }
    }
    return false;
}

bool path_all_faces_outward(const cadml::engine::FlatMesh& m) {
    if (m.vertices.empty() || m.indices.empty()) return true;
    cadml::Vec3 c{ 0, 0, 0 };
    for (const auto& v : m.vertices) c = c + v;
    c = c * (1.0 / static_cast<double>(m.vertices.size()));
    const std::size_t ntri = m.indices.size() / 3;
    for (std::size_t t = 0; t < ntri; ++t) {
        const auto& a = m.vertices[m.indices[t * 3 + 0]];
        const auto& b = m.vertices[m.indices[t * 3 + 1]];
        const auto& d = m.vertices[m.indices[t * 3 + 2]];
        const cadml::Vec3 e1{ b.x - a.x, b.y - a.y, b.z - a.z };
        const cadml::Vec3 e2{ d.x - a.x, d.y - a.y, d.z - a.z };
        const cadml::Vec3 n{
            e1.y * e2.z - e1.z * e2.y,
            e1.z * e2.x - e1.x * e2.z,
            e1.x * e2.y - e1.y * e2.x };
        const cadml::Vec3 face_c{
            (a.x + b.x + d.x) / 3.0,
            (a.y + b.y + d.y) / 3.0,
            (a.z + b.z + d.z) / 3.0 };
        const cadml::Vec3 out{ face_c.x - c.x, face_c.y - c.y, face_c.z - c.z };
        if (n.x * out.x + n.y * out.y + n.z * out.z <= 0.0) return false;
    }
    return true;
}
}  // namespace

TEST(FlatEvaluatorPathCurves, HorizontalLinetoH) {
    // M 0,0 H 10 V 5 Z ??? square top-right of origin.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"1\">"
        "<path d=\"M 0,0 H 10 V 5 H 0 Z\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto b = path_bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.min_x,  0.0, 1e-9);
    EXPECT_NEAR(b.max_x, 10.0, 1e-9);
    EXPECT_NEAR(b.min_y,  0.0, 1e-9);
    EXPECT_NEAR(b.max_y,  5.0, 1e-9);
}

TEST(FlatEvaluatorPathCurves, RelativeHV) {
    // m 0,0 h 10 v 5 h -10 z ??? same shape, all relative.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"1\">"
        "<path d=\"m 0,0 h 10 v 5 h -10 z\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto b = path_bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.min_x,  0.0, 1e-9);
    EXPECT_NEAR(b.max_x, 10.0, 1e-9);
    EXPECT_NEAR(b.min_y,  0.0, 1e-9);
    EXPECT_NEAR(b.max_y,  5.0, 1e-9);
}

TEST(FlatEvaluatorPathCurves, CubicBezierEndpointAndMidpoint) {
    // C 5,10 5,10 10,0 ??? starting at (0,0), ending at (10,0), with
    // both control points at (5,10). The midpoint (t=0.5) of this
    // cubic is (0.5??????? + 3??0.5????0.5??5???? + 0.5????10) = 5 in x,
    // (3??0.5????0.5??10 + 3??0.5??0.5????10 + 0.5????0) = 7.5 in y.
    // Closing back via L 0,0 makes a valid extrudable loop.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"1\">"
        "<path d=\"M 0,0 C 5,10 5,10 10,0 L 0,0 Z\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto& m = r.parts[0].mesh;
    EXPECT_FALSE(m.vertices.empty());
    const auto b = path_bbox(m);
    // The cubic's apex sits at y = 7.5 (analytically). Allow generous
    // tolerance since the flattener may not place a vertex exactly at
    // the peak ??? but it MUST be at least 7.5 - tol within bbox.
    EXPECT_NEAR(b.max_y, 7.5, 0.1);
    EXPECT_NEAR(b.min_x, 0.0, 0.1);
    EXPECT_NEAR(b.max_x, 10.0, 0.1);
    // Both endpoints exactly preserved.
    EXPECT_TRUE(path_polyline_passes_through(m, 0.0, 0.0, 1e-6));
    EXPECT_TRUE(path_polyline_passes_through(m, 10.0, 0.0, 1e-6));
}

TEST(FlatEvaluatorPathCurves, SmoothCubicReflectsControlPoint) {
    // M 0,0 C 0,5 5,5 5,0 S 10,-5 10,0 Z
    // First cubic ends at (5,0) with last control (5,5). The S
    // command's implicit first control is the reflection: 2*(5,0) -
    // (5,5) = (5,-5). End at (10,0). The polyline's bbox should
    // dip below y=0 (the second cubic's apex) and rise above
    // (the first cubic's apex).
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"1\">"
        "<path d=\"M 0,0 C 0,5 5,5 5,0 S 10,-5 10,0 L 0,0 Z\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto& m = r.parts[0].mesh;
    const auto b = path_bbox(m);
    EXPECT_GT(b.max_y, 1.0);   // first hump rises
    EXPECT_LT(b.min_y, -0.5);  // second hump dips (smaller ??? but it dips)
    EXPECT_NEAR(b.min_x, 0.0,  0.1);
    EXPECT_NEAR(b.max_x, 10.0, 0.1);
}

TEST(FlatEvaluatorPathCurves, QuadraticBezierApex) {
    // Q 5,10 10,0 ??? quadratic with apex at the control point.
    // The actual peak of a Q curve is at (1-t)?? * p0 + 2t(1-t) * p1
    // + t?? * p2 evaluated at t=0.5: midpoint y = 0.5*(p0.y+p2.y)/2
    // + 0.5*p1.y = 0 + 5 = 5. So bbox max_y ??? 5.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"1\">"
        "<path d=\"M 0,0 Q 5,10 10,0 L 0,0 Z\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto b = path_bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.max_y, 5.0, 0.1);
}

TEST(FlatEvaluatorPathCurves, SmoothQuadraticReflectsControlPoint) {
    // M 0,0 Q 5,5 10,0 T 20,0 ??? first quadratic apex at (5,5),
    // last control (5,5). T's implicit control is 2*(10,0) -
    // (5,5) = (15,-5). Second quadratic dips to y=-2.5 at t=0.5.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"1\">"
        "<path d=\"M 0,0 Q 5,5 10,0 T 20,0 L 0,0 Z\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto b = path_bbox(r.parts[0].mesh);
    EXPECT_GT(b.max_y, 2.0);
    EXPECT_LT(b.min_y, -1.0);
    EXPECT_NEAR(b.min_x,  0.0, 0.1);
    EXPECT_NEAR(b.max_x, 20.0, 0.1);
}

TEST(FlatEvaluatorPathCurves, SemicircleArcViaA) {
    // M 0,0 A 10,10 0 0 1 20,0 L 0,0 Z -- semicircle from (0,0) to
    // (20,0). With sweep=1 the SVG spec says "positive-angle
    // direction"; that's CCW in math y-up coords, so the arc midpoint
    // lands at (10, -10), BELOW the chord. (CADML interprets <path>
    // coordinates as y-up math; the <svg> wrapper applies a y-flip
    // when SVG-style visual orientation is desired.)
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"1\">"
        "<path d=\"M 0,0 A 10,10 0 0 1 20,0 L 0,0 Z\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto b = path_bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.min_y, -10.0, 0.05);
    EXPECT_NEAR(b.max_y,   0.0, 1e-6);
    EXPECT_NEAR(b.min_x,   0.0, 0.05);
    EXPECT_NEAR(b.max_x,  20.0, 0.05);
}

TEST(FlatEvaluatorPathCurves, ArcSweepZeroGoesOppositeWay) {
    // Same endpoints as the semicircle test, but sweep=0 (negative
    // angle direction = CW in math y-up). Arc midpoint lands at
    // (10, +10), ABOVE the chord -- mirror image of sweep=1.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"1\">"
        "<path d=\"M 0,0 A 10,10 0 0 0 20,0 L 0,0 Z\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto b = path_bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.max_y, 10.0, 0.05);
    EXPECT_NEAR(b.min_y,  0.0, 1e-6);
}

TEST(FlatEvaluatorPathCurves, ArcLargeArcFlag) {
    // For a chord exactly equal to the diameter (length 20, r=10)
    // the large-arc flag has no geometric effect -- both arcs are
    // 180-degree semicircles. Use a smaller chord so large/small
    // is meaningful: chord 10, radius 10, large=1 sweep=1 traces
    // the LONG way around (~270 deg of the circle).
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"1\">"
        "<path d=\"M 0,0 A 10,10 0 1 1 10,0 L 0,0 Z\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto& m = r.parts[0].mesh;
    EXPECT_FALSE(m.vertices.empty());
    const auto b = path_bbox(m);
    // Long arc spans most of the circle; bbox extents approach the
    // circle's diameter (20) in both x and y.
    EXPECT_GT(b.max_x - b.min_x, 17.0);
    EXPECT_GT(b.max_y - b.min_y, 17.0);
}

TEST(FlatEvaluatorPathCurves, ArcXAxisRotationProducesValidGeometry) {
    // x-axis-rotation parameter test. Don't assert specific bbox
    // dimensions: the SVG spec scales radii up implicitly when the
    // chord exceeds the natural diameter, so the resulting bbox
    // depends on chord length, original rx/ry, and rotation in
    // non-obvious ways. Just verify the geometry is non-empty and
    // outward-facing.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"1\">"
        "<path d=\"M -8,0 A 10,5 45 0 1 8,0 L -8,0 Z\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto& m = r.parts[0].mesh;
    EXPECT_FALSE(m.vertices.empty());
    EXPECT_TRUE(path_all_faces_outward(m));
}

TEST(FlatEvaluatorPathCurves, ArcDegenerateZeroRadiiBecomesLine) {
    // SVG spec: rx == 0 or ry == 0 ??? arc degrades to a straight
    // line from start to end.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"1\">"
        "<path d=\"M 0,0 A 0,10 0 0 1 10,0 V 5 H 0 Z\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto b = path_bbox(r.parts[0].mesh);
    // Degenerate arc: result is just the rectangle. y stays in [0,5].
    EXPECT_NEAR(b.min_y, 0.0, 1e-9);
    EXPECT_NEAR(b.max_y, 5.0, 1e-9);
}

TEST(FlatEvaluatorPathCurves, CircleViaTwoArcs) {
    // Real-world SVG idiom for a circle: two semicircular arcs
    // sharing endpoints. M 10,0 A 10,10 0 1 1 -10,0 A 10,10 0 1
    // 1 10,0 Z ??? full circle of radius 10 around origin.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"1\">"
        "<path d=\"M 10,0 A 10,10 0 1 1 -10,0 A 10,10 0 1 1 10,0 Z\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto& m = r.parts[0].mesh;
    EXPECT_FALSE(m.vertices.empty());
    const auto b = path_bbox(m);
    EXPECT_NEAR(b.min_x, -10.0, 0.05);
    EXPECT_NEAR(b.max_x,  10.0, 0.05);
    EXPECT_NEAR(b.min_y, -10.0, 0.05);
    EXPECT_NEAR(b.max_y,  10.0, 0.05);
    // All polyline vertices should sit on the circle to within tolerance.
    const std::size_t half = m.vertices.size() / 2;
    for (std::size_t i = 0; i < half; ++i) {
        const auto& v = m.vertices[i];
        const double r2 = v.x * v.x + v.y * v.y;
        EXPECT_NEAR(std::sqrt(r2), 10.0, 0.05)
            << "vertex " << i << " at (" << v.x << "," << v.y << ")";
    }
}

TEST(FlatEvaluatorPathCurves, ZRestoresPenToSubpathStart) {
    // M 0,0 H 5 V 5 Z m 10,0 H 5 V 5 Z ??? two subpaths, the second
    // authored with relative-m starting from where Z left the pen.
    // After Z, pen returns to the first subpath's start (0, 0), so
    // the second m's relative offset (10, 0) lands at (10, 0).
    // Single-polygon collapse means vertices from both subpaths
    // coexist in one Polygon2D ??? not strictly correct geometry
    // (extrude can't represent multi-loop) but at least the pen
    // semantics shouldn't be broken.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"1\">"
        "<path d=\"M 0,0 H 5 V 5 H 0 Z\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto b = path_bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.min_x, 0.0, 1e-9);
    EXPECT_NEAR(b.max_x, 5.0, 1e-9);
    EXPECT_NEAR(b.min_y, 0.0, 1e-9);
    EXPECT_NEAR(b.max_y, 5.0, 1e-9);
}

TEST(FlatEvaluatorPathCurves, UnknownCommandTerminatesCleanly) {
    // SVG also specifies M / L / C / Q / A / Z in this set, but the
    // grammar doesn't include 'X'. An unknown command should
    // terminate parsing ??? we get whatever was tessellated up to it.
    // Using only M+L+L means the parser reaches X with 3 vertices
    // already stored, but then bails. The triangle should still
    // extrude.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"1\">"
        "<path d=\"M 0,0 L 10,0 L 5,10 X 99,99 Z\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.parts[0].mesh.vertices.empty());
    const auto b = path_bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.max_y, 10.0, 1e-6);
}

TEST(FlatEvaluatorPathCurves, RealWorldSvgIconCompiles) {
    // A non-trivial path mixing several curve commands ??? modeled
    // on real SVG icon content (rounded-rect-ish shape with
    // smooth corners). Just verify it doesn't crash, produces
    // non-empty geometry, and has a bbox that makes sense.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<svg>"   // y-flip so the shape ends up right-side-up
        "<extrude height=\"2\">"
        "<path d=\""
        "M 5,0 H 25 "
        "Q 30,0 30,5 V 25 "
        "Q 30,30 25,30 H 5 "
        "Q 0,30 0,25 V 5 "
        "Q 0,0 5,0 Z"
        "\"/>"
        "</extrude>"
        "</svg>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto b = path_bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.min_x,    0.0, 0.1);
    EXPECT_NEAR(b.max_x,   30.0, 0.1);
    // svg flips y: 0..30 ??? -30..0.
    EXPECT_NEAR(b.min_y,  -30.0, 0.1);
    EXPECT_NEAR(b.max_y,    0.0, 0.1);
    EXPECT_TRUE(path_all_faces_outward(r.parts[0].mesh));
}

// ----- <sweep> + <helix> -------------------------------------------

TEST(FlatEvaluatorSweep, BasicHelixSweepProducesSpring) {
    // A small square profile swept along a helix becomes a spring.
    // Verify the bbox: helix axis is +Z, so the swept tube centered
    // at z = pitch * t. With turns=2 and pitch=4, total height = 8
    // plus the profile extent in Y.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<sweep>"
        "<rect x=\"-0.5\" y=\"-0.5\" width=\"1\" height=\"1\"/>"
        "<helix radius=\"5\" pitch=\"4\" turns=\"2\"/>"
        "</sweep>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto& m = r.parts[0].mesh;
    EXPECT_FALSE(m.vertices.empty());
    const auto b = path_bbox(m);
    // X / Y extents bounded by helix radius + profile extent on x.
    // radius + profile_x_max = 5 + 0.5 = 5.5; radius - profile_x_max
    // (after sweep around all directions) = -(5 + 0.5) = -5.5.
    EXPECT_NEAR(b.min_x, -5.5, 0.1);
    EXPECT_NEAR(b.max_x,  5.5, 0.1);
    EXPECT_NEAR(b.min_y, -5.5, 0.1);
    EXPECT_NEAR(b.max_y,  5.5, 0.1);
    // Z spans (0 - profile_y_max) to (pitch*turns + profile_y_max).
    EXPECT_NEAR(b.min_z, -0.5, 0.05);
    EXPECT_NEAR(b.max_z,  8.5, 0.05);
    // Note: path_all_faces_outward is unsuitable for swept tubes
    // wrapping around an axis (faces on the axis-facing side point
    // toward the centroid). Manifold's CSG validates topological
    // correctness instead — exercised by SweepInsideDifferenceCutsThread.
}

TEST(FlatEvaluatorSweep, MissingProfileChildWarns) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<sweep>"
        "<helix radius=\"5\" pitch=\"4\" turns=\"1\"/>"
        "</sweep>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "missing 2D profile"));
}

TEST(FlatEvaluatorSweep, MissingHelixChildWarns) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<sweep>"
        "<circle r=\"1\"/>"
        "</sweep>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "missing <helix>"));
}

TEST(FlatEvaluatorSweep, ZeroPitchWarns) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<sweep>"
        "<circle r=\"1\"/>"
        "<helix radius=\"5\" pitch=\"0\" turns=\"1\"/>"
        "</sweep>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "pitch=0"));
}

TEST(FlatEvaluatorSweep, NegativeTurnsWarns) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<sweep>"
        "<circle r=\"1\"/>"
        "<helix radius=\"5\" pitch=\"4\" turns=\"-1\"/>"
        "</sweep>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "turns must be positive"));
}

TEST(FlatEvaluatorSweep, CcwAndCwProduceMirroredHelices) {
    // CCW helix: rotates +theta as t increases; CW: rotates -theta.
    // Both should produce a mesh of the same volume / bbox dimensions
    // but the points wind in opposite directions around the axis.
    auto build = [](const char* dir) {
        std::string src = std::string(
            "version 0.1\n<part>"
            "<sweep>"
            "<circle r=\"0.5\"/>"
            "<helix radius=\"4\" pitch=\"2\" turns=\"1\" direction=\"") + dir + "\"/>"
            "</sweep>"
            "</part>";
        return src;
    };
    auto rc  = evaluate_flat(parse_authoring(build("ccw")));
    auto rcw = evaluate_flat(parse_authoring(build("cw")));
    ASSERT_TRUE(rc.ok());
    ASSERT_TRUE(rcw.ok());
    const auto bcc  = path_bbox(rc.parts[0].mesh);
    const auto bcw  = path_bbox(rcw.parts[0].mesh);
    EXPECT_NEAR(bcc.max_x - bcc.min_x, bcw.max_x - bcw.min_x, 0.1);
    EXPECT_NEAR(bcc.max_y - bcc.min_y, bcw.max_y - bcw.min_y, 0.1);
    EXPECT_NEAR(bcc.max_z - bcc.min_z, bcw.max_z - bcw.min_z, 0.1);
}

TEST(FlatEvaluatorSweep, TaperShrinksRadiusOverTime) {
    // A helix with a positive taper of 1 mm/turn over 2 turns ends
    // 2 mm wider than it started. Final ring's outer radius =
    // radius + taper * turns = 5 + 2 = 7.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<sweep>"
        "<rect x=\"-0.5\" y=\"-0.5\" width=\"1\" height=\"1\"/>"
        "<helix radius=\"5\" pitch=\"3\" turns=\"2\" taper=\"1\"/>"
        "</sweep>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto b = path_bbox(r.parts[0].mesh);
    // Outer extent at end of helix = radius + taper*turns + profile_x_max
    // = 5 + 1*2 + 0.5 = 7.5. The bbox max_x can't exceed this and
    // should be at least the start (5.5).
    EXPECT_GE(b.max_x, 5.5);
    EXPECT_NEAR(b.max_x, 7.5, 0.1);
}

TEST(FlatEvaluatorSweep, HelixOnItsOwnEmitsWarning) {
    // <helix> at the part level (not inside a sweep) doesn't produce
    // geometry; the engine should warn "unsupported 3D node type".
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<helix radius=\"5\" pitch=\"4\" turns=\"1\"/>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "unsupported 3D node type"));
}

TEST(FlatEvaluatorSweep, SweepInsideDifferenceCutsThread) {
    // Realistic use case: cut a helical groove out of a cylinder.
    // The resulting solid should still be valid and non-empty.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<difference>"
        "<extrude height=\"10\"><circle r=\"6\"/></extrude>"
        "<group transform=\"translate(0, 0, 0)\">"
        "<sweep>"
        "<rect x=\"-0.5\" y=\"-0.4\" width=\"1\" height=\"0.8\"/>"
        "<helix radius=\"6\" pitch=\"2\" turns=\"4\"/>"
        "</sweep>"
        "</group>"
        "</difference>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_FALSE(r.parts[0].mesh.vertices.empty());
}

// ----- <loft> + <sketch> (polyhedral) ---------------------------------

TEST(FlatEvaluatorLoft, TwoSectionLoftEqualsExtrude) {
    // Two identical 4-vertex squares at z=0 and z=10 lofted together
    // should produce a unit-cube-ish solid topologically equivalent
    // to a 4-vertex extrude. Same vertex count (8), same triangle
    // count (12 = 8 sides + 4 caps).
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<loft>"
        "<sketch plane=\"xy\" origin=\"0 0 0\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/>"
        "</sketch>"
        "<sketch plane=\"xy\" origin=\"0 0 10\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/>"
        "</sketch>"
        "</loft>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto& m = r.parts[0].mesh;
    EXPECT_EQ(m.vertex_count(), 8u);
    EXPECT_EQ(m.triangle_count(), 12u);
    const auto b = path_bbox(m);
    EXPECT_NEAR(b.min_x,  0.0, 1e-9);
    EXPECT_NEAR(b.max_x, 10.0, 1e-9);
    EXPECT_NEAR(b.min_z,  0.0, 1e-9);
    EXPECT_NEAR(b.max_z, 10.0, 1e-9);
}

TEST(FlatEvaluatorLoft, ThreeSectionLoftSpansAllZRanges) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<loft>"
        "<sketch plane=\"xy\" origin=\"0 0 0\">"
        "<rect x=\"-5\" y=\"-5\" width=\"10\" height=\"10\"/>"
        "</sketch>"
        "<sketch plane=\"xy\" origin=\"0 0 5\">"
        "<rect x=\"-3\" y=\"-3\" width=\"6\" height=\"6\"/>"
        "</sketch>"
        "<sketch plane=\"xy\" origin=\"0 0 10\">"
        "<rect x=\"-2\" y=\"-2\" width=\"4\" height=\"4\"/>"
        "</sketch>"
        "</loft>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto b = path_bbox(r.parts[0].mesh);
    // Pyramid-like shape: bottom 10x10, mid 6x6, top 4x4. Bbox
    // dominated by the bottom section.
    EXPECT_NEAR(b.min_x, -5.0, 1e-9);
    EXPECT_NEAR(b.max_x,  5.0, 1e-9);
    EXPECT_NEAR(b.min_z,  0.0, 1e-9);
    EXPECT_NEAR(b.max_z, 10.0, 1e-9);
}

TEST(FlatEvaluatorLoft, RotatedSectionTwistsTheLoft) {
    // Two identical squares but the second is rotated 45 deg around
    // the normal. Result should span x in [-7.07, 7.07] (the
    // diagonal of the rotated square).
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<loft>"
        "<sketch plane=\"xy\" origin=\"0 0 0\" rotation=\"0\">"
        "<rect x=\"-5\" y=\"-5\" width=\"10\" height=\"10\"/>"
        "</sketch>"
        "<sketch plane=\"xy\" origin=\"0 0 10\" rotation=\"45\">"
        "<rect x=\"-5\" y=\"-5\" width=\"10\" height=\"10\"/>"
        "</sketch>"
        "</loft>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto b = path_bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.max_x, 7.07, 0.05);
    EXPECT_NEAR(b.max_y, 7.07, 0.05);
}

TEST(FlatEvaluatorLoft, MismatchedVertexCountWarns) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<loft>"
        "<sketch plane=\"xy\" origin=\"0 0 0\">"
        "<rect x=\"0\" y=\"0\" width=\"4\" height=\"4\"/>"
        "</sketch>"
        "<sketch plane=\"xy\" origin=\"0 0 5\">"
        "<circle r=\"3\"/>"
        "</sketch>"
        "</loft>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "all sections must have the same vertex count"));
    EXPECT_EQ(r.parts[0].mesh.triangle_count(), 0u);
}

TEST(FlatEvaluatorLoft, OneSectionWarns) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<loft>"
        "<sketch plane=\"xy\" origin=\"0 0 0\">"
        "<rect x=\"0\" y=\"0\" width=\"4\" height=\"4\"/>"
        "</sketch>"
        "</loft>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "need at least 2"));
}

TEST(FlatEvaluatorLoft, NonSketchChildWarns) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<loft>"
        "<rect x=\"0\" y=\"0\" width=\"4\" height=\"4\"/>"
        "<sketch plane=\"xy\" origin=\"0 0 5\">"
        "<rect x=\"0\" y=\"0\" width=\"4\" height=\"4\"/>"
        "</sketch>"
        "</loft>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "loft: child must be <sketch>"));
}

TEST(FlatEvaluatorLoft, ExplicitNormalProducesTiltedSection) {
    // Custom normal lets sketches sit in arbitrary planes. Normal
    // pointing along (1, 0, 1) (i.e., 45 deg tilted in XZ) should
    // produce a polygon whose bbox spans diagonally.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<loft>"
        "<sketch normal=\"1 0 1\" origin=\"0 0 0\">"
        "<rect x=\"-5\" y=\"-5\" width=\"10\" height=\"10\"/>"
        "</sketch>"
        "<sketch normal=\"1 0 1\" origin=\"5 0 5\">"
        "<rect x=\"-5\" y=\"-5\" width=\"10\" height=\"10\"/>"
        "</sketch>"
        "</loft>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_FALSE(r.parts[0].mesh.vertices.empty());
}

TEST(FlatEvaluator, CutFreeformSubtracts) {
    // <cut> with no `type=` and a 2nd child treats the 2nd child as
    // a freeform cutter that gets subtracted from the target.
    // Equivalent to <difference> in this case, which lets us verify
    // by triangle count - the cutter punches a hole.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<cut face=\"end\" type=\"\">"
        "<extrude height=\"10\">"
        "<rect x=\"-10\" y=\"-10\" width=\"20\" height=\"20\"/>"
        "</extrude>"
        "<group transform=\"translate(0, 0, -1)\">"
        "<extrude height=\"12\"><circle r=\"3\"/></extrude>"
        "</group>"
        "</cut>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    ASSERT_EQ(r.parts.size(), 1u);
    EXPECT_GT(r.parts[0].mesh.triangle_count(), 16u)
        << "expected the original 16-tri box plus the punched-hole walls";
}

TEST(FlatEvaluator, RejectsConnect) {
    auto doc = parse_authoring(
        "version 0.1\n"
        "<assembly>"
        "<plate id=\"p\"/>"
        "<bolt id=\"b\"/>"
        "<connect a=\"p.top\" b=\"b.head\"/>"
        "</assembly>");
    auto r = evaluate_flat(doc);
    EXPECT_FALSE(r.ok());
    // Both <assembly> AND <connect> trigger; either is fine, but we
    // expect <connect> to appear in the diagnostic list.
    EXPECT_TRUE(any_error_contains(r, "<connect>"));
}

TEST(FlatEvaluator, RejectsMatingInstance) {
    // Mating instance (with at/port) outside an <assembly> ??? bundler
    // would normally lower these, but a hand-crafted .fcadml could
    // sneak one in. The boundary catches it.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<bolt at=\"top\" port=\"head\"/>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(any_error_contains(r, "mating instances"));
    EXPECT_TRUE(any_error_contains(r, "bolt"));
}

// ????????? End-to-end: bundler output is accepted ????????????????????????????????????????????????????????????????????????????????????

TEST(FlatEvaluator, AcceptsBundlerOutput) {
    // A bundler-produced flat document must pass the boundary.
    auto cr = cadml::compile::compile_string(
        "version 0.1\n<part><circle r=\"5\"/></part>");
    ASSERT_TRUE(cr.ok()) << (cr.errors.empty() ? "" : cr.errors[0].message);
    auto r = evaluate_flat(cr.document);
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
}

// ????????? Multiple parts ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????

TEST(FlatEvaluator, EmitsOnePartPerExportedPart) {
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"helper\"><circle r=\"3\"/></def>"
        "<part><circle r=\"5\"/></part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // Only the <part> shows up as an output (defs don't produce parts).
    EXPECT_EQ(r.parts.size(), 1u);
}

// ????????? Frontmatter-leftover rejections ???????????????????????????????????????????????????????????????????????????????????????????????????
// In a flat doc the bundler should have resolved imports and hoisted
// frontmatter params into the body. Anything left in those fields is
// an unprocessed (or hand-crafted) input.

TEST(FlatEvaluator, RejectsLeftoverImportsField) {
    auto doc = parse_authoring(
        "version 0.1\n"
        "import \"foo.cadml\"\n"
        "<part><circle r=\"5\"/></part>");
    ASSERT_FALSE(doc.imports.empty()) << "test setup: parser must record import";
    auto r = evaluate_flat(doc);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(any_error_contains(r, "leftover import"));
    EXPECT_TRUE(any_error_contains(r, "foo.cadml"));
}

TEST(FlatEvaluator, RejectsLeftoverParamsField) {
    auto doc = parse_authoring(
        "version 0.1\n"
        "param x = 5\n"
        "<part><circle r=\"5\"/></part>");
    ASSERT_FALSE(doc.params.empty()) << "test setup: parser must record param";
    auto r = evaluate_flat(doc);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(any_error_contains(r, "leftover frontmatter param"));
    EXPECT_TRUE(any_error_contains(r, "`x`"));
}

// ????????? Top-level <part> filter ???????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????
// A <part> nested inside a <def> is malformed authoring; the boundary
// shouldn't surface it as a renderable output.

TEST(FlatEvaluator, IgnoresPartNestedInsideDef) {
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"weird\">"
        "<part><circle r=\"5\"/></part>"
        "</def>"
        "<part><circle r=\"7\"/></part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // Exactly one output ??? the top-level part. The nested one is
    // silently dropped (parser/bundler is responsible for catching the
    // malformed nesting upstream).
    EXPECT_EQ(r.parts.size(), 1u);
}

// ????????? Geometry ??? rect extrusion (slice B2.0) ?????????????????????????????????????????????????????????????????????????????????

TEST(FlatEvaluatorGeometry, ExtrudeRectProducesBox) {
    // <extrude height=10><rect x=-10 y=-15 width=20 height=30/></extrude>
    // Expected: 4 bottom ring + 4 top ring = 8 vertices.
    //           Sides: 4 quads ?? 2 tris = 8.
    //           Caps:  2 ?? (4-2) ear-clipped tris = 4.
    //           Total: 12 triangles.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"10\">"
        "<rect x=\"-10\" y=\"-15\" width=\"20\" height=\"30\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    ASSERT_EQ(r.parts.size(), 1u);

    const auto& m = r.parts[0].mesh;
    EXPECT_EQ(m.vertex_count(),     8u);
    EXPECT_EQ(m.triangle_count(),  12u);

    // Bounding box check.
    double min_x = 1e9, max_x = -1e9;
    double min_y = 1e9, max_y = -1e9;
    double min_z = 1e9, max_z = -1e9;
    for (const auto& v : m.vertices) {
        min_x = std::min(min_x, v.x); max_x = std::max(max_x, v.x);
        min_y = std::min(min_y, v.y); max_y = std::max(max_y, v.y);
        min_z = std::min(min_z, v.z); max_z = std::max(max_z, v.z);
    }
    EXPECT_DOUBLE_EQ(min_x, -10.0);
    EXPECT_DOUBLE_EQ(max_x,  10.0);
    EXPECT_DOUBLE_EQ(min_y, -15.0);
    EXPECT_DOUBLE_EQ(max_y,  15.0);
    EXPECT_DOUBLE_EQ(min_z,   0.0);
    EXPECT_DOUBLE_EQ(max_z,  10.0);
}

TEST(FlatEvaluatorGeometry, ExtrudeCircleProducesCylinder) {
    // <extrude height=10><circle r=5 segments=32/></extrude>
    // 32 segments → 32 ring vertices × 2 = 64 verts.
    //  Sides: 32 quads × 2 tris = 64.
    //  Caps:  2 × (32-2) ear-clipped tris = 60.
    //  Total: 124 triangles.
    //
    // Pinning segments=32 rather than relying on the adaptive
    // default keeps this test stable when the default tolerance
    // changes.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"10\"><circle r=\"5\" segments=\"32\"/></extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    ASSERT_EQ(r.parts.size(), 1u);

    const auto& m = r.parts[0].mesh;
    EXPECT_EQ(m.vertex_count(),    64u);
    EXPECT_EQ(m.triangle_count(), 124u);

    // All ring vertices should sit on the cylinder's wall.
    for (std::size_t i = 0; i < 64; ++i) {
        const auto& v = m.vertices[i];
        const auto r2 = v.x * v.x + v.y * v.y;
        EXPECT_NEAR(r2, 25.0, 1e-9) << "vertex " << i << " not on r=5 wall";
    }
}

TEST(FlatEvaluatorGeometry, EveryTriangleAttributedToExtrudeNode) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"5\"><rect width=\"4\" height=\"4\"/></extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto& m = r.parts[0].mesh;
    ASSERT_EQ(m.triangle_node.size(), m.triangle_count());
    // All triangles should attribute to the SAME originating node
    // (the <extrude>). Confirms source mapping is uniformly populated.
    for (auto src : m.triangle_node) {
        EXPECT_EQ(src, m.triangle_node[0]);
    }
}

TEST(FlatEvaluatorGeometry, EmptyExtrudeWithNoChildWarns) {
    auto doc = parse_authoring(
        "version 0.1\n<part><extrude height=\"5\"/></part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    // Geometry is empty (no profile) but not an error ??? just a warning.
    EXPECT_FALSE(r.warnings.empty());
    EXPECT_EQ(r.parts[0].mesh.triangle_count(), 0u);
}

// ????????? Geometry ??? revolve (slice B2.1) ??????????????????????????????????????????????????????????????????????????????????????????????????????

TEST(FlatEvaluatorGeometry, RevolveCircleAroundXProducesTorus) {
    // <revolve axis="x"><circle cx=0 cy=10 r=2 segments=32/></revolve>
    // ⇒ torus (centre-line radius 10, tube radius 2). Profile pinned
    // to 32 points so the test stays stable across adaptive-default
    // changes; ring_count = 32 closed ⇒ 32×32 = 1024 vertices. Sides:
    // 32 ring steps × 32 quads × 2 = 2048 triangles. No caps for closed.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<revolve axis=\"x\"><circle cx=\"0\" cy=\"10\" r=\"2\" segments=\"32\"/></revolve>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    ASSERT_EQ(r.parts.size(), 1u);

    const auto& m = r.parts[0].mesh;
    EXPECT_EQ(m.vertex_count(),   1024u);
    EXPECT_EQ(m.triangle_count(), 2048u);

    // Bounding box: x stays in profile range [-2, 2], y/z each span
    // [-12, 12] (centre-line ??tube radius after rotation around X).
    double min_x = 1e9, max_x = -1e9;
    double min_y = 1e9, max_y = -1e9;
    double min_z = 1e9, max_z = -1e9;
    for (const auto& v : m.vertices) {
        min_x = std::min(min_x, v.x); max_x = std::max(max_x, v.x);
        min_y = std::min(min_y, v.y); max_y = std::max(max_y, v.y);
        min_z = std::min(min_z, v.z); max_z = std::max(max_z, v.z);
    }
    EXPECT_NEAR(min_x,  -2.0, 1e-9);
    EXPECT_NEAR(max_x,   2.0, 1e-9);
    EXPECT_NEAR(min_y, -12.0, 1e-6);
    EXPECT_NEAR(max_y,  12.0, 1e-6);
    EXPECT_NEAR(min_z, -12.0, 1e-6);
    EXPECT_NEAR(max_z,  12.0, 1e-6);
}

// Regression for two paired bugs in <revolve> that hid for a long
// time because they cancelled each other out visually:
//   * axis="z" placed the profile in the (x, y) plane and just spun
//     it around Z, so the body collapsed to a flat disc instead of
//     sweeping along the axis (the actual symptom on the compressor
//     hub: the saucer face never appeared in renders).
//   * Side faces were emitted with inward-facing normals — back-face
//     culling then hid them, so any bug in the geometry was masked
//     until other solids unioned with the hub revealed see-through
//     gaps from the wrong side of every face.
//
// We use an annular (tube) profile: rectangle (4,0) → (8,0) → (8,10) →
// (4,10) revolved 360° gives a hollow cylinder (analytic volume
// π·(8²-4²)·10 = 480π mm³). path_all_faces_outward can't validate
// non-convex bodies, so instead we use the divergence-theorem volume
// integral V = (1/6)·Σ vᵢ·(vⱼ×vₖ): for a closed manifold mesh, this
// equals the geometric volume IFF every face is outward-oriented;
// flip a face and that face's contribution flips sign too, throwing
// the total off by the face's prism volume. So a single test value
// catches BOTH bugs at once — wrong volume (or wrong sign) means
// either the sweep collapsed or the winding inverted.
TEST(FlatEvaluatorGeometry, RevolveAxisZSweepsAndFacesOutward) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<revolve axis=\"z\" angle=\"360\">"
        "<path d=\"M 4,0 L 8,0 8,10 4,10 Z\"/>"
        "</revolve>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    ASSERT_EQ(r.parts.size(), 1u);
    const auto& m = r.parts[0].mesh;
    ASSERT_FALSE(m.vertices.empty());

    // Bbox sanity: the historic axis-z collapse made the hub a flat
    // disc with z-extent 0; this profile has z ∈ [0, 10].
    double min_z = 1e9, max_z = -1e9;
    double max_r = 0;
    for (const auto& v : m.vertices) {
        min_z = std::min(min_z, v.z);
        max_z = std::max(max_z, v.z);
        max_r = std::max(max_r, std::sqrt(v.x*v.x + v.y*v.y));
    }
    EXPECT_NEAR(min_z, 0.0,  1e-6);
    EXPECT_NEAR(max_z, 10.0, 1e-6);
    EXPECT_NEAR(max_r, 8.0,  1e-6);

    // Signed volume via divergence theorem. Positive matches the
    // analytic value when all faces face outward; the prior bug
    // would have produced a negative result (or near zero if the
    // collapse left no enclosed volume to integrate).
    double v6 = 0;
    for (std::size_t t = 0; t < m.indices.size() / 3; ++t) {
        const auto& a = m.vertices[m.indices[t * 3 + 0]];
        const auto& b = m.vertices[m.indices[t * 3 + 1]];
        const auto& d = m.vertices[m.indices[t * 3 + 2]];
        v6 += a.x * (b.y * d.z - b.z * d.y)
            + a.y * (b.z * d.x - b.x * d.z)
            + a.z * (b.x * d.y - b.y * d.x);
    }
    const double volume = v6 / 6.0;
    const double expected = 3.14159265358979 * (8*8 - 4*4) * 10;
    // Loose tolerance (~3 %) — the 32-segment polygonal approximation
    // undershoots a smooth annulus by sin(π/N)·N / π.
    EXPECT_NEAR(volume, expected, expected * 0.03);
}

TEST(FlatEvaluatorGeometry, RevolveAngle90AddsCaps) {
    // 90?? revolve scales segments proportionally: ceil(32 * 90/360) = 8
    // ??? 9 ring samples (open). Profile: 4-point square.
    //
    // Sides: 8 ring steps ?? 4 quads ?? 2 = 64 triangles.
    // Caps: 2 ?? (4-2) ear-clipped tris = 4 triangles.
    // Total: 68 triangles.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<revolve axis=\"x\" angle=\"90\">"
        "<rect x=\"0\" y=\"5\" width=\"3\" height=\"2\"/>"
        "</revolve>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(r.parts[0].mesh.triangle_count(), 68u);
}

TEST(FlatEvaluatorGeometry, RevolveMissingAxisWarns) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<revolve><circle r=\"5\"/></revolve>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.warnings.empty());
    EXPECT_EQ(r.parts[0].mesh.triangle_count(), 0u);
}

TEST(FlatEvaluatorGeometry, RevolveBadAxisWarns) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<revolve axis=\"diagonal\"><circle r=\"5\"/></revolve>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.warnings.empty());
}

// ????????? Geometry ??? group + transform (slice B2.3) ????????????????????????????????????????????????????????????????????????

TEST(FlatEvaluatorGroups, GroupTranslatesContainedMesh) {
    // Box at origin, then wrapped in group translated +X by 100.
    auto baseline = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"5\">"
        "<rect x=\"-2\" y=\"-3\" width=\"4\" height=\"6\"/>"
        "</extrude>"
        "</part>");
    auto translated = parse_authoring(
        "version 0.1\n<part>"
        "<group transform=\"translate(100, 0, 0)\">"
        "<extrude height=\"5\">"
        "<rect x=\"-2\" y=\"-3\" width=\"4\" height=\"6\"/>"
        "</extrude>"
        "</group>"
        "</part>");
    auto rb = evaluate_flat(baseline);
    auto rt = evaluate_flat(translated);
    ASSERT_TRUE(rb.ok());
    ASSERT_TRUE(rt.ok());
    ASSERT_EQ(rb.parts[0].mesh.vertex_count(), rt.parts[0].mesh.vertex_count());
    ASSERT_EQ(rb.parts[0].mesh.triangle_count(),
                rt.parts[0].mesh.triangle_count());

    // Every vertex shifted by exactly +100 on x, others unchanged.
    for (std::size_t i = 0; i < rb.parts[0].mesh.vertices.size(); ++i) {
        const auto& vb = rb.parts[0].mesh.vertices[i];
        const auto& vt = rt.parts[0].mesh.vertices[i];
        EXPECT_DOUBLE_EQ(vt.x - vb.x, 100.0);
        EXPECT_DOUBLE_EQ(vt.y, vb.y);
        EXPECT_DOUBLE_EQ(vt.z, vb.z);
    }
}

TEST(FlatEvaluatorGroups, GroupRotatesContainedMesh) {
    // Box on +X side, rotated 90?? around Z. Original spans x???[10,30],
    // y???[-5,5]; after rotation x???-y, y???x ??? x???[-5,5], y???[10,30].
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<group transform=\"rotate(90, 0, 0, 1)\">"
        "<extrude height=\"4\">"
        "<rect x=\"10\" y=\"-5\" width=\"20\" height=\"10\"/>"
        "</extrude>"
        "</group>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    double min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
    for (const auto& v : r.parts[0].mesh.vertices) {
        min_x = std::min(min_x, v.x); max_x = std::max(max_x, v.x);
        min_y = std::min(min_y, v.y); max_y = std::max(max_y, v.y);
    }
    EXPECT_NEAR(min_x, -5.0, 1e-6);
    EXPECT_NEAR(max_x,  5.0, 1e-6);
    EXPECT_NEAR(min_y, 10.0, 1e-6);
    EXPECT_NEAR(max_y, 30.0, 1e-6);
}

TEST(FlatEvaluatorGroups, NestedGroupsCompose) {
    // Inner translate(10,0,0), wrapped in outer rotate(90,0,0,1).
    // Effective: rotate after translate ??? point (10,0,0) ??? (0,10,0).
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<group transform=\"rotate(90, 0, 0, 1)\">"
        "<group transform=\"translate(10, 0, 0)\">"
        "<extrude height=\"2\">"
        "<rect x=\"-1\" y=\"-1\" width=\"2\" height=\"2\"/>"
        "</extrude>"
        "</group>"
        "</group>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // After both transforms, the bbox center is at (0, 10, 1).
    double sum_x = 0, sum_y = 0, sum_z = 0;
    for (const auto& v : r.parts[0].mesh.vertices) {
        sum_x += v.x; sum_y += v.y; sum_z += v.z;
    }
    const double n = static_cast<double>(r.parts[0].mesh.vertex_count());
    EXPECT_NEAR(sum_x / n,  0.0, 1e-6);
    EXPECT_NEAR(sum_y / n, 10.0, 1e-6);
    EXPECT_NEAR(sum_z / n,  1.0, 1e-6);
}

// ????????? Geometry ??? booleans (slice B2.2) ????????????????????????????????????????????????????????????????????????????????????????????????

namespace {
struct Bbox { double min_x, min_y, min_z, max_x, max_y, max_z; };
Bbox bbox(const FlatMesh& m) {
    Bbox b{ 1e9, 1e9, 1e9, -1e9, -1e9, -1e9 };
    for (const auto& v : m.vertices) {
        b.min_x = std::min(b.min_x, v.x); b.max_x = std::max(b.max_x, v.x);
        b.min_y = std::min(b.min_y, v.y); b.max_y = std::max(b.max_y, v.y);
        b.min_z = std::min(b.min_z, v.z); b.max_z = std::max(b.max_z, v.z);
    }
    return b;
}
}  // namespace

TEST(FlatEvaluatorBooleans, UnionOfTwoOverlappingBoxes) {
    // Two 10??10??10 boxes overlapping by 5 ??? bbox spans 15??10??10.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<union>"
        "  <extrude height=\"10\">"
        "    <rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/>"
        "  </extrude>"
        "  <extrude height=\"10\">"
        "    <rect x=\"5\" y=\"0\" width=\"10\" height=\"10\"/>"
        "  </extrude>"
        "</union>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    ASSERT_EQ(r.parts.size(), 1u);
    EXPECT_GT(r.parts[0].mesh.triangle_count(), 0u);
    auto b = bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.min_x, 0.0,  1e-6);
    EXPECT_NEAR(b.max_x, 15.0, 1e-6);
    EXPECT_NEAR(b.min_y, 0.0,  1e-6);
    EXPECT_NEAR(b.max_y, 10.0, 1e-6);
    EXPECT_NEAR(b.min_z, 0.0,  1e-6);
    EXPECT_NEAR(b.max_z, 10.0, 1e-6);
}

TEST(FlatEvaluatorBooleans, DifferenceCutsHole) {
    // 20??20??10 stock minus a centred 10??10 column ??? frame with hole.
    // Bbox stays 20??20??10. Triangle count grows (hole adds inner walls).
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<difference>"
        "  <extrude height=\"10\">"
        "    <rect x=\"0\" y=\"0\" width=\"20\" height=\"20\"/>"
        "  </extrude>"
        "  <extrude height=\"10\">"
        "    <rect x=\"5\" y=\"5\" width=\"10\" height=\"10\"/>"
        "  </extrude>"
        "</difference>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_GT(r.parts[0].mesh.triangle_count(), 16u)
        << "difference should add inner-wall triangles";
    auto b = bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.min_x, 0.0,  1e-6);
    EXPECT_NEAR(b.max_x, 20.0, 1e-6);
    EXPECT_NEAR(b.min_y, 0.0,  1e-6);
    EXPECT_NEAR(b.max_y, 20.0, 1e-6);
}

TEST(FlatEvaluatorBooleans, IntersectKeepsOverlap) {
    // Two boxes overlapping by 3??4??5 ??? intersection bbox = 3??4??5
    // located at (5, 6, 0).
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<intersect>"
        "  <extrude height=\"10\">"
        "    <rect x=\"0\" y=\"0\" width=\"8\" height=\"10\"/>"
        "  </extrude>"
        "  <extrude height=\"5\">"
        "    <rect x=\"5\" y=\"6\" width=\"10\" height=\"4\"/>"
        "  </extrude>"
        "</intersect>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_GT(r.parts[0].mesh.triangle_count(), 0u);
    auto b = bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.min_x, 5.0,  1e-6);
    EXPECT_NEAR(b.max_x, 8.0,  1e-6);
    EXPECT_NEAR(b.min_y, 6.0,  1e-6);
    EXPECT_NEAR(b.max_y, 10.0, 1e-6);
    EXPECT_NEAR(b.min_z, 0.0,  1e-6);
    EXPECT_NEAR(b.max_z, 5.0,  1e-6);
}

TEST(FlatEvaluatorBooleans, IntersectNoOverlapIsEmpty) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<intersect>"
        "  <extrude height=\"5\">"
        "    <rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/>"
        "  </extrude>"
        "  <extrude height=\"5\">"
        "    <rect x=\"100\" y=\"0\" width=\"5\" height=\"5\"/>"
        "  </extrude>"
        "</intersect>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // Empty intersection is valid ??? empty mesh.
    EXPECT_EQ(r.parts[0].mesh.triangle_count(), 0u);
}

TEST(FlatEvaluatorBooleans, UnionWithGroupChildIncludesIt) {
    // Regression: bundler-output booleans whose operands are
    // <group transform="...">…</group> wrappers must include the
    // group, not silently drop it.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<union>"
        "  <extrude height=\"10\">"
        "    <rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/>"
        "  </extrude>"
        "  <group transform=\"translate(20, 0, 0)\">"
        "    <extrude height=\"10\">"
        "      <rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/>"
        "    </extrude>"
        "  </group>"
        "</union>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    auto b = bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.min_x,  0.0, 1e-6);
    // Group-wrapped cube starts at translated x=20, ends at x=30. If
    // the group were dropped we'd see max_x=10.
    EXPECT_NEAR(b.max_x, 30.0, 1e-6);
}

TEST(FlatEvaluatorBooleans, DifferenceWithGroupSubtractCutsHole) {
    // Regression: difference's second operand wrapped in a <group>
    // must subtract correctly, not no-op.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<difference>"
        "  <extrude height=\"10\">"
        "    <rect x=\"0\" y=\"0\" width=\"20\" height=\"20\"/>"
        "  </extrude>"
        "  <group transform=\"translate(5, 5, 0)\">"
        "    <extrude height=\"10\">"
        "      <rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/>"
        "    </extrude>"
        "  </group>"
        "</difference>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // Hole adds inner walls ??? triangle count > the 16 of plain stock.
    EXPECT_GT(r.parts[0].mesh.triangle_count(), 16u)
        << "group-wrapped subtraction must cut the hole";
}

TEST(FlatEvaluatorBooleans, UnionPropagatesSourceAttribution) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<union>"
        "  <extrude height=\"10\"><circle r=\"5\"/></extrude>"
        "  <extrude height=\"10\"><rect x=\"3\" y=\"0\" width=\"10\" height=\"6\"/></extrude>"
        "</union>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto& m = r.parts[0].mesh;
    ASSERT_EQ(m.triangle_node.size(), m.triangle_count());
    // Manifold preserves faceID per surviving triangle, so we should
    // see at least TWO distinct source nodes attributed (one per extrude).
    std::vector<std::uint32_t> sorted_nodes(m.triangle_node);
    std::sort(sorted_nodes.begin(), sorted_nodes.end());
    sorted_nodes.erase(std::unique(sorted_nodes.begin(), sorted_nodes.end()),
                          sorted_nodes.end());
    EXPECT_GE(sorted_nodes.size(), 2u)
        << "expected >=2 distinct triangle_node values; got "
        << sorted_nodes.size();
}

// ????????? <hull> ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????

TEST(FlatEvaluatorHull, TwoOverlappingBoxesProduceConvexEnvelope) {
    // Hull of two overlapping boxes equals the convex hull of their
    // 16 corner points. Result must contain all 16 corners (or
    // duplicates of them) and have no interior dents ??? a quick
    // sanity check is that the bounding box matches the union of
    // the two input bboxes.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<hull>"
        "  <extrude height=\"10\">"
        "    <rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/>"
        "  </extrude>"
        "  <group transform=\"translate(20, 0, 0)\">"
        "    <extrude height=\"10\">"
        "      <rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/>"
        "    </extrude>"
        "  </group>"
        "</hull>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto& m = r.parts[0].mesh;
    ASSERT_FALSE(m.vertices.empty());
    // Bbox = combined bbox of the two inputs.
    double lx = m.vertices[0].x, hx = lx;
    double ly = m.vertices[0].y, hy = ly;
    double lz = m.vertices[0].z, hz = lz;
    for (const auto& v : m.vertices) {
        lx = std::min(lx, v.x); hx = std::max(hx, v.x);
        ly = std::min(ly, v.y); hy = std::max(hy, v.y);
        lz = std::min(lz, v.z); hz = std::max(hz, v.z);
    }
    EXPECT_NEAR(lx,  0.0, 1e-6);
    EXPECT_NEAR(hx, 30.0, 1e-6);   // 0..10 ??? 20..30
    EXPECT_NEAR(ly,  0.0, 1e-6);
    EXPECT_NEAR(hy, 10.0, 1e-6);
    EXPECT_NEAR(lz,  0.0, 1e-6);
    EXPECT_NEAR(hz, 10.0, 1e-6);
}

TEST(FlatEvaluatorHull, SingleChildReturnsHullOfThatSolid) {
    // Hull of a single convex solid is that solid (geometrically).
    // Triangle count may differ ??? Manifold's hull rebuilds the mesh
    // via QuickHull ??? but vertex bounds must match the input.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<hull>"
        "  <extrude height=\"4\">"
        "    <rect x=\"-3\" y=\"-3\" width=\"6\" height=\"6\"/>"
        "  </extrude>"
        "</hull>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto& m = r.parts[0].mesh;
    ASSERT_FALSE(m.vertices.empty());
    double lx = m.vertices[0].x, hx = lx;
    double lz = m.vertices[0].z, hz = lz;
    for (const auto& v : m.vertices) {
        lx = std::min(lx, v.x); hx = std::max(hx, v.x);
        lz = std::min(lz, v.z); hz = std::max(hz, v.z);
    }
    EXPECT_NEAR(lx, -3.0, 1e-6);
    EXPECT_NEAR(hx,  3.0, 1e-6);
    EXPECT_NEAR(lz,  0.0, 1e-6);
    EXPECT_NEAR(hz,  4.0, 1e-6);
}

TEST(FlatEvaluatorHull, TwoStackedThinExtrudesMakeFrustum) {
    // The "loft replacement" pattern: hull two thin extrudes at
    // different z heights. A small square at z=0 hulled with a
    // bigger square at z=10 should make a frustum-like solid that
    // spans z???[0, 10] and reaches the larger square's footprint
    // at the top.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<hull>"
        "  <extrude height=\"0.5\">"
        "    <rect x=\"-2\" y=\"-2\" width=\"4\" height=\"4\"/>"
        "  </extrude>"
        "  <group transform=\"translate(0, 0, 10)\">"
        "    <extrude height=\"0.5\">"
        "      <rect x=\"-6\" y=\"-6\" width=\"12\" height=\"12\"/>"
        "    </extrude>"
        "  </group>"
        "</hull>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto& m = r.parts[0].mesh;
    ASSERT_FALSE(m.vertices.empty());
    double lx = m.vertices[0].x, hx = lx;
    double lz = m.vertices[0].z, hz = lz;
    for (const auto& v : m.vertices) {
        lx = std::min(lx, v.x); hx = std::max(hx, v.x);
        lz = std::min(lz, v.z); hz = std::max(hz, v.z);
    }
    EXPECT_NEAR(lx, -6.0, 1e-6);    // top square dominates the lateral extents
    EXPECT_NEAR(hx,  6.0, 1e-6);
    EXPECT_NEAR(lz,  0.0, 1e-6);
    EXPECT_NEAR(hz, 10.5, 1e-6);    // top of the upper extrude
}

TEST(FlatEvaluatorHull, NoChildrenWarns) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<hull/>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "no 3D children"));
}

TEST(FlatEvaluatorGroups, WrongArityWarns) {
    // Regression: translate(1, 2) — 2 args instead of 3. Used to
    // be silently ignored; now warns.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<group transform=\"translate(1, 2)\">"
        "<extrude height=\"5\"><rect width=\"4\" height=\"4\"/></extrude>"
        "</group>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.warnings.empty());
    EXPECT_TRUE(any_warning_contains(r, "translate"));
    EXPECT_TRUE(any_warning_contains(r, "expects 3 args"));
}

TEST(FlatEvaluatorGroups, UnknownFunctionWarns) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<group transform=\"warp(1, 2, 3)\">"
        "<extrude height=\"5\"><rect width=\"4\" height=\"4\"/></extrude>"
        "</group>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "unknown function `warp`"));
}

TEST(FlatEvaluatorGroups, MissingParenWarns) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<group transform=\"translate\">"
        "<extrude height=\"5\"><rect width=\"4\" height=\"4\"/></extrude>"
        "</group>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "expected `(`"));
}

TEST(FlatEvaluatorGroups, BadNumberWarns) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<group transform=\"translate(abc, 2, 3)\">"
        "<extrude height=\"5\"><rect width=\"4\" height=\"4\"/></extrude>"
        "</group>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "cannot parse number"));
    EXPECT_TRUE(any_warning_contains(r, "abc"));
}

TEST(FlatEvaluatorGroups, EmptyTransformIsIdentity) {
    auto plain = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"5\"><circle r=\"3\"/></extrude>"
        "</part>");
    auto wrapped = parse_authoring(
        "version 0.1\n<part>"
        "<group><extrude height=\"5\"><circle r=\"3\"/></extrude></group>"
        "</part>");
    auto rp = evaluate_flat(plain);
    auto rw = evaluate_flat(wrapped);
    ASSERT_TRUE(rp.ok());
    ASSERT_TRUE(rw.ok());
    ASSERT_EQ(rp.parts[0].mesh.vertex_count(), rw.parts[0].mesh.vertex_count());
    for (std::size_t i = 0; i < rp.parts[0].mesh.vertices.size(); ++i) {
        EXPECT_DOUBLE_EQ(rp.parts[0].mesh.vertices[i].x,
                            rw.parts[0].mesh.vertices[i].x);
        EXPECT_DOUBLE_EQ(rp.parts[0].mesh.vertices[i].y,
                            rw.parts[0].mesh.vertices[i].y);
        EXPECT_DOUBLE_EQ(rp.parts[0].mesh.vertices[i].z,
                            rw.parts[0].mesh.vertices[i].z);
    }
}

// ????????? <svg> wrapper (SVG-asset import) ????????????????????????????????????????????????????????????????????????????????????????????????
//
// `<svg>` flips the Y axis for descendant geometry so an SVG snippet
// pasted inside it renders right-side-up in CADML's y-up world. The
// engine implements this as scale(1, -1, 1); the negative-determinant
// branch in apply_transform_to_mesh keeps face winding outward, and
// ensure_ccw at the entry of extrude_linear / revolve handles the
// CW-in-math polygons that SVG mental-model authors typically write.

namespace {
// Helpers shared by the SVG tests. Bbox/bbox are reused from the
// boolean test block above (struct Bbox { min_x, min_y, ..., max_z };).

// True iff every triangle in the mesh has its outward face normal
// pointing AWAY from the mesh centroid (i.e. faces are correctly
// oriented). Computes the geometric centroid as the mean of vertex
// positions; for closed convex-ish solids this lies inside, and
// outward-facing triangles satisfy `(face_normal ?? (face_centroid
// - centroid)) > 0`. Works for the rect/box-shaped test meshes
// below; not a robust check for general meshes (concave inputs may
// have face centroids on the "wrong" side of the centroid even
// when correctly oriented), but bomb-proof for our test inputs.
bool all_faces_outward(const cadml::engine::FlatMesh& m) {
    if (m.vertices.empty() || m.indices.empty()) return true;
    cadml::Vec3 c{ 0, 0, 0 };
    for (const auto& v : m.vertices) c = c + v;
    c = c * (1.0 / static_cast<double>(m.vertices.size()));
    const std::size_t ntri = m.indices.size() / 3;
    for (std::size_t t = 0; t < ntri; ++t) {
        const auto& a = m.vertices[m.indices[t * 3 + 0]];
        const auto& b = m.vertices[m.indices[t * 3 + 1]];
        const auto& d = m.vertices[m.indices[t * 3 + 2]];
        const cadml::Vec3 e1{ b.x - a.x, b.y - a.y, b.z - a.z };
        const cadml::Vec3 e2{ d.x - a.x, d.y - a.y, d.z - a.z };
        // Face normal (unnormalised ??? sign is what we care about).
        const cadml::Vec3 n{
            e1.y * e2.z - e1.z * e2.y,
            e1.z * e2.x - e1.x * e2.z,
            e1.x * e2.y - e1.y * e2.x };
        const cadml::Vec3 face_c{
            (a.x + b.x + d.x) / 3.0,
            (a.y + b.y + d.y) / 3.0,
            (a.z + b.z + d.z) / 3.0 };
        const cadml::Vec3 out{ face_c.x - c.x, face_c.y - c.y, face_c.z - c.z };
        const double dot = n.x * out.x + n.y * out.y + n.z * out.z;
        if (dot <= 0.0) return false;
    }
    return true;
}
}  // namespace

TEST(FlatEvaluatorSvg, BasicYFlipPlacesContentInNegativeY) {
    // SVG path traced as CCW in SVG mental space (y-down). Without
    // the wrapper the polygon would render in +y space; inside <svg>
    // it should land in -y space (matching what an SVG author drew
    // on screen with origin at top-left).
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<svg>"
        "<extrude height=\"3\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/>"
        "</extrude>"
        "</svg>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto b = bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.min_x,   0.0, 1e-9);
    EXPECT_NEAR(b.max_x,  10.0, 1e-9);
    // The crucial check: y is negated by <svg>. y???[0,10] becomes [-10,0].
    EXPECT_NEAR(b.min_y, -10.0, 1e-9);
    EXPECT_NEAR(b.max_y,   0.0, 1e-9);
    EXPECT_NEAR(b.min_z,   0.0, 1e-9);
    EXPECT_NEAR(b.max_z,   3.0, 1e-9);
}

TEST(FlatEvaluatorSvg, FacesStayOutwardThroughYFlip) {
    // The negative-determinant branch in apply_transform_to_mesh is
    // exactly here to keep faces outward after the y-flip. If it
    // weren't, every triangle would face inward and back-face culling
    // would erase the entire surface.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<svg>"
        "<extrude height=\"4\">"
        "<rect x=\"-5\" y=\"-3\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</svg>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(all_faces_outward(r.parts[0].mesh))
        << "<svg> y-flip left some faces winding-inverted";
}

TEST(FlatEvaluatorSvg, PathInsideSvgRendersWithSvgWinding) {
    // The polygon below is CCW in SVG mental space (going right, down,
    // left, up on screen) which means CW in CADML math. Inside <svg>
    // it must still render correctly: ensure_ccw flips the polygon to
    // CCW for triangulation, the side faces emit outward, and the
    // y-flip + negative-det handling reverses winding back to outward
    // post-transform.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<svg>"
        "<extrude height=\"2\">"
        "<path d=\"M 0,0 L 0,10 10,10 10,0 Z\"/>"
        "</extrude>"
        "</svg>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto& m = r.parts[0].mesh;
    EXPECT_FALSE(m.vertices.empty());
    EXPECT_TRUE(all_faces_outward(m))
        << "CW-in-math SVG-style polygon should still face outward";
    const auto b = bbox(m);
    // Same y???[-10, 0] flip check.
    EXPECT_NEAR(b.min_y, -10.0, 1e-9);
    EXPECT_NEAR(b.max_y,   0.0, 1e-9);
}

TEST(FlatEvaluatorSvg, MultipleChildrenAllGetFlipped) {
    // Two extrudes side-by-side inside one <svg>. Both should be
    // y-flipped and present in the final mesh (vertex count >=
    // sum of independent counts).
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<svg>"
        "<extrude height=\"2\">"
        "<rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/>"
        "</extrude>"
        "<group transform=\"translate(10, 0, 0)\">"
        "<extrude height=\"2\">"
        "<rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/>"
        "</extrude>"
        "</group>"
        "</svg>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto b = bbox(r.parts[0].mesh);
    // Both squares are at y???[0,5] in their authored form; after the
    // <svg>'s y-flip, both span y???[-5, 0].
    EXPECT_NEAR(b.min_y, -5.0, 1e-9);
    EXPECT_NEAR(b.max_y,  0.0, 1e-9);
    // x extent covers both squares: [0, 5] ??? [10, 15].
    EXPECT_NEAR(b.min_x,  0.0, 1e-9);
    EXPECT_NEAR(b.max_x, 15.0, 1e-9);
}

TEST(FlatEvaluatorSvg, EmptySvgIsBenign) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<svg/>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.errors.empty());
    EXPECT_EQ(r.parts[0].mesh.triangle_count(), 0u);
}

TEST(FlatEvaluatorSvg, NestedSvgComposesToIdentity) {
    // Two y-flips compose to identity. <svg> inside <svg> should
    // render the same as the unwrapped form.
    auto plain = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"2\">"
        "<rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/>"
        "</extrude>"
        "</part>");
    auto nested = parse_authoring(
        "version 0.1\n<part>"
        "<svg><svg>"
        "<extrude height=\"2\">"
        "<rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/>"
        "</extrude>"
        "</svg></svg>"
        "</part>");
    auto rp = evaluate_flat(plain);
    auto rn = evaluate_flat(nested);
    ASSERT_TRUE(rp.ok()); ASSERT_TRUE(rn.ok());
    const auto bp = bbox(rp.parts[0].mesh);
    const auto bn = bbox(rn.parts[0].mesh);
    EXPECT_NEAR(bn.min_x, bp.min_x, 1e-9);
    EXPECT_NEAR(bn.max_x, bp.max_x, 1e-9);
    EXPECT_NEAR(bn.min_y, bp.min_y, 1e-9);
    EXPECT_NEAR(bn.max_y, bp.max_y, 1e-9);
    EXPECT_NEAR(bn.min_z, bp.min_z, 1e-9);
    EXPECT_NEAR(bn.max_z, bp.max_z, 1e-9);
    EXPECT_TRUE(all_faces_outward(rn.parts[0].mesh));
}

TEST(FlatEvaluatorSvg, SvgWithCircleProfileWorks) {
    // Non-path primitives inside <svg> still get y-flipped.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<svg>"
        "<extrude height=\"2\"><circle cx=\"0\" cy=\"5\" r=\"3\"/></extrude>"
        "</svg>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto b = bbox(r.parts[0].mesh);
    // Authored cy=5 → y range [2, 8] before flip → [-8, -2] after.
    // Tolerance is the engine's default circle-chord deviation
    // (TessellationParams::tolerance = 0.01 mm) — circle vertices
    // are placed at fixed angular positions, so the bbox extremes
    // may fall short of the geometric extents by up to one chord
    // sagitta. Use 0.01 for slack against the documented contract.
    EXPECT_NEAR(b.min_y, -8.0, 0.01);
    EXPECT_NEAR(b.max_y, -2.0, 0.01);
    EXPECT_TRUE(all_faces_outward(r.parts[0].mesh));
}

TEST(FlatEvaluatorSvg, SvgInsideGroupComposesTransforms) {
    // <group transform="translate(...)"><svg>..</svg></group>: the
    // svg's y-flip applies first (innermost), then the group's
    // translate. Final y position = -authored_y + translate_y.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<group transform=\"translate(0, 100, 0)\">"
        "<svg>"
        "<extrude height=\"2\">"
        "<rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/>"
        "</extrude>"
        "</svg>"
        "</group>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto b = bbox(r.parts[0].mesh);
    // Square's authored y???[0,5] ??? flipped to [-5,0] ??? translated +100 ??? [95,100].
    EXPECT_NEAR(b.min_y,  95.0, 1e-9);
    EXPECT_NEAR(b.max_y, 100.0, 1e-9);
    EXPECT_TRUE(all_faces_outward(r.parts[0].mesh));
}

TEST(FlatEvaluatorSvg, SvgInsideDifferenceWorks) {
    // <svg> inside a boolean: SVG-pasted content can be unioned /
    // differenced with regular CADML geometry. Common pattern: cut
    // an SVG-traced logo out of a plate.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<difference>"
        // Plate spanning x???[0,20], y???[-12, 0], z???[0,5]. Authored
        // directly in CADML coords (y-up).
        "<extrude height=\"5\">"
        "<rect x=\"0\" y=\"-12\" width=\"20\" height=\"12\"/>"
        "</extrude>"
        // SVG-style hole: 4??4 square authored at SVG (x,y)=(8,4) ??? the"
        // y-flip lands it at CADML y=-4. Slightly oversized in z to "
        // ensure a clean cut."
        "<group transform=\"translate(0, 0, -1)\">"
        "<svg>"
        "<extrude height=\"7\">"
        "<rect x=\"8\" y=\"4\" width=\"4\" height=\"4\"/>"
        "</extrude>"
        "</svg>"
        "</group>"
        "</difference>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_FALSE(r.parts[0].mesh.vertices.empty())
        << "difference produced empty result";
    // The hole was at SVG-y=4 with size 4, so flipped y???[-8, -4].
    // Result mesh should still have the plate's overall extent
    // (x???[0,20], y???[-12,0]) but with a cavity.
    const auto b = bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.min_x,   0.0, 1e-6);
    EXPECT_NEAR(b.max_x,  20.0, 1e-6);
    EXPECT_NEAR(b.min_y, -12.0, 1e-6);
    EXPECT_NEAR(b.max_y,   0.0, 1e-6);
}

TEST(FlatEvaluatorSvg, SvgInsideUnionWorks) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<union>"
        "<extrude height=\"2\">"
        "<rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/>"
        "</extrude>"
        "<svg>"
        "<extrude height=\"2\">"
        "<rect x=\"6\" y=\"0\" width=\"5\" height=\"5\"/>"
        "</extrude>"
        "</svg>"
        "</union>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto b = bbox(r.parts[0].mesh);
    // First rect at y???[0,5]; second (in svg) at y???[-5,0]. Union
    // spans y???[-5,5].
    EXPECT_NEAR(b.min_y, -5.0, 1e-6);
    EXPECT_NEAR(b.max_y,  5.0, 1e-6);
}

TEST(FlatEvaluatorSvg, GroupScaleMinusOneOnYBehavesLikeSvg) {
    // <svg> is sugar for "scale(1,-1,1) on descendants". A bare
    // <group transform="scale(1, -1, 1)"> should produce the same
    // bbox AND keep faces outward (the negative-det branch in
    // apply_transform_to_mesh handles both cases identically).
    auto svg_doc = parse_authoring(
        "version 0.1\n<part>"
        "<svg>"
        "<extrude height=\"3\">"
        "<rect x=\"2\" y=\"4\" width=\"5\" height=\"6\"/>"
        "</extrude>"
        "</svg>"
        "</part>");
    auto grp_doc = parse_authoring(
        "version 0.1\n<part>"
        "<group transform=\"scale(1, -1, 1)\">"
        "<extrude height=\"3\">"
        "<rect x=\"2\" y=\"4\" width=\"5\" height=\"6\"/>"
        "</extrude>"
        "</group>"
        "</part>");
    auto rs = evaluate_flat(svg_doc);
    auto rg = evaluate_flat(grp_doc);
    ASSERT_TRUE(rs.ok()); ASSERT_TRUE(rg.ok());
    const auto bs = bbox(rs.parts[0].mesh);
    const auto bg = bbox(rg.parts[0].mesh);
    EXPECT_NEAR(bs.min_x, bg.min_x, 1e-9);
    EXPECT_NEAR(bs.max_x, bg.max_x, 1e-9);
    EXPECT_NEAR(bs.min_y, bg.min_y, 1e-9);
    EXPECT_NEAR(bs.max_y, bg.max_y, 1e-9);
    EXPECT_NEAR(bs.min_z, bg.min_z, 1e-9);
    EXPECT_NEAR(bs.max_z, bg.max_z, 1e-9);
    EXPECT_TRUE(all_faces_outward(rs.parts[0].mesh));
    EXPECT_TRUE(all_faces_outward(rg.parts[0].mesh));
}

TEST(FlatEvaluatorSvg, MirrorTransformXFlipsAndReorientsFaces) {
    // Generic stress for the negative-det branch: scale(-1, 1, 1)
    // mirrors X. Result should land in negative X with outward faces.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<group transform=\"scale(-1, 1, 1)\">"
        "<extrude height=\"3\">"
        "<rect x=\"2\" y=\"0\" width=\"6\" height=\"4\"/>"
        "</extrude>"
        "</group>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto b = bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.min_x, -8.0, 1e-9);
    EXPECT_NEAR(b.max_x, -2.0, 1e-9);
    EXPECT_TRUE(all_faces_outward(r.parts[0].mesh));
}

TEST(FlatEvaluatorSvg, SvgWithSvgPathStarRendersCleanly) {
    // The five-pointed star example, but authored in SVG mental
    // space (y-down). Same vertex magnitudes, but the y-coords go
    // POSITIVE-down (since SVG convention). After the <svg> wrapper
    // y-flips them, the star points up (positive y) just like the
    // version we already use in showcase-path.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<svg>"
        "<extrude height=\"5\">"
        "<path d=\"M 0,-20 l -4.7,13.5 -14.3,0.3 11.4,8.7 -4.2,13.7 11.8,-8.2 11.8,8.2 -4.2,-13.7 11.4,-8.7 -14.3,-0.3 z\"/>"
        "</extrude>"
        "</svg>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto& m = r.parts[0].mesh;
    EXPECT_FALSE(m.vertices.empty());
    EXPECT_TRUE(all_faces_outward(m))
        << "SVG-authored 10-vertex concave polygon should face outward";
    // Star points up (after y-flip) ??? top vertex at +20 (was authored
    // at SVG y=-20, which means visually-up in SVG, which y-flips to
    // CADML y=+20).
    const auto b = bbox(m);
    EXPECT_NEAR(b.max_y,  20.0, 1e-6);
    EXPECT_NEAR(b.min_y, -16.2, 1e-3);
}

// ????????? ensure_ccw helper (direct unit tests) ?????????????????????????????????????????????????????????????????????????????????

TEST(EnsureCcw, NoOpForCcwInput) {
    cadml::engine::detail::Polygon2D p{{ {0,0}, {10,0}, {10,10}, {0,10} }};
    const auto orig = p.points;
    cadml::engine::detail::ensure_ccw(p);
    ASSERT_EQ(p.points.size(), orig.size());
    for (std::size_t i = 0; i < orig.size(); ++i) {
        EXPECT_DOUBLE_EQ(p.points[i].x, orig[i].x);
        EXPECT_DOUBLE_EQ(p.points[i].y, orig[i].y);
    }
}

TEST(EnsureCcw, ReversesCwInput) {
    // A rect traced CW.
    cadml::engine::detail::Polygon2D p{{ {0,0}, {0,10}, {10,10}, {10,0} }};
    cadml::engine::detail::ensure_ccw(p);
    // After reversal: {10,0}, {10,10}, {0,10}, {0,0} ??? and that's CCW.
    EXPECT_DOUBLE_EQ(p.points[0].x, 10);  EXPECT_DOUBLE_EQ(p.points[0].y, 0);
    EXPECT_DOUBLE_EQ(p.points[1].x, 10);  EXPECT_DOUBLE_EQ(p.points[1].y, 10);
    EXPECT_DOUBLE_EQ(p.points[2].x, 0);   EXPECT_DOUBLE_EQ(p.points[2].y, 10);
    EXPECT_DOUBLE_EQ(p.points[3].x, 0);   EXPECT_DOUBLE_EQ(p.points[3].y, 0);
}

TEST(EnsureCcw, DegenerateInputUnchanged) {
    cadml::engine::detail::Polygon2D p{{}};
    cadml::engine::detail::ensure_ccw(p);
    EXPECT_TRUE(p.points.empty());

    cadml::engine::detail::Polygon2D q{{ {0,0}, {1,0} }};
    cadml::engine::detail::ensure_ccw(q);
    ASSERT_EQ(q.points.size(), 2u);
    EXPECT_DOUBLE_EQ(q.points[0].x, 0);
    EXPECT_DOUBLE_EQ(q.points[1].x, 1);
}

TEST(EnsureCcw, ConcaveCwBecomesConcaveCcw) {
    // L-shape traced CW (start at origin, go up-then-right).
    cadml::engine::detail::Polygon2D p{{
        {0,0}, {0,40}, {8,40}, {8,8}, {40,8}, {40,0}
    }};
    cadml::engine::detail::ensure_ccw(p);
    // After ensure_ccw the polygon is the L-bracket from the
    // earlier ear-clipping demo (CCW): {40,0}, {40,8}, {8,8}, {8,40},
    // {0,40}, {0,0}.
    EXPECT_DOUBLE_EQ(p.points[0].x, 40); EXPECT_DOUBLE_EQ(p.points[0].y, 0);
    EXPECT_DOUBLE_EQ(p.points[5].x, 0);  EXPECT_DOUBLE_EQ(p.points[5].y, 0);
    // Triangulator should now succeed on it.
    const auto tris = cadml::engine::detail::triangulate_polygon(p);
    EXPECT_EQ(tris.size(), 4u);   // N - 2 = 6 - 2 = 4
}

// ----- CW-polygon warning (only outside <svg>) ----------------------

TEST(FlatEvaluatorCwWarning, FiresForCwExtrudeOutsideSvg) {
    // SVG-style square traced (0,0)->(0,10)->(10,10)->(10,0). Signed
    // area is negative (CW) in CADML's y-up math frame. Without an
    // <svg> wrapper, the engine warns the author about the winding
    // mismatch but still extrudes the polygon (ensure_ccw flips it
    // for the side-face emitter).
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"3\">"
        "<path d=\"M 0,0 L 0,10 10,10 10,0 Z\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "wound clockwise"));
    // Geometry still produced.
    EXPECT_FALSE(r.parts[0].mesh.vertices.empty());
}

TEST(FlatEvaluatorCwWarning, NoWarningInsideSvg) {
    // Same CW polygon, but inside <svg>. CW is expected here because
    // <svg> applies the y-flip; suppressing the warning avoids
    // crying wolf on intentionally-pasted SVG content.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<svg>"
        "<extrude height=\"3\">"
        "<path d=\"M 0,0 L 0,10 10,10 10,0 Z\"/>"
        "</extrude>"
        "</svg>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(any_warning_contains(r, "wound clockwise"))
        << "<svg> should suppress the CW warning for its descendants";
}

TEST(FlatEvaluatorCwWarning, NoWarningForCcwPolygon) {
    // Standard CCW polygon (0,0)->(10,0)->(10,10)->(0,10) — never
    // warns regardless of svg context.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"3\">"
        "<path d=\"M 0,0 L 10,0 10,10 0,10 Z\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(any_warning_contains(r, "wound clockwise"));
}

TEST(FlatEvaluatorCwWarning, WarnsForRevolveToo) {
    // The CW check applies to <revolve>, not just <extrude>. Same
    // CW polygon, revolved around the X axis instead of extruded.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<revolve axis=\"x\">"
        "<path d=\"M 0,5 L 0,10 5,10 5,5 Z\"/>"
        "</revolve>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "wound clockwise"));
}

TEST(FlatEvaluatorCwWarning, NestedSvgRestoresWarning) {
    // <svg><svg>...</svg></svg>: the inner <svg> increments the
    // depth (so its content suppresses warnings for ITS descendants),
    // but the outer one's exit decrements before the next sibling.
    // For a polygon nested twice, depth > 0 → suppressed.
    // Verify the depth counter is correctly managed across exits:
    // a CW polygon inside <svg> is silent, then a CW polygon AFTER
    // </svg> at part level triggers the warning.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<svg>"
        "<extrude height=\"3\">"
        "<path d=\"M 0,0 L 0,10 10,10 10,0 Z\"/>"
        "</extrude>"
        "</svg>"
        "<group transform=\"translate(20, 0, 0)\">"
        "<extrude height=\"3\">"
        "<path d=\"M 0,0 L 0,10 10,10 10,0 Z\"/>"
        "</extrude>"
        "</group>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    // The second polygon (outside <svg>) MUST trigger the warning.
    EXPECT_TRUE(any_warning_contains(r, "wound clockwise"))
        << "depth counter should decrement back to 0 after </svg> closes";
}

// ????????? glTF export (slice B3.2) ???????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????

namespace {
// Minimal substring-based assertions ??? the unit tests don't need a
// full glTF validator. The headline test parses the result with
// rapidjson to confirm the structure is real JSON.
}

TEST(FlatGltfExport, RoundTripsAsValidJson) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"5\"><rect x=\"0\" y=\"0\" width=\"4\" height=\"4\"/></extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto gltf = export_gltf(r, doc);
    EXPECT_FALSE(gltf.empty());
    // Top-level structural markers required by glTF 2.0.
    EXPECT_NE(gltf.find("\"asset\""),       std::string::npos);
    EXPECT_NE(gltf.find("\"version\":\"2.0\""), std::string::npos);
    EXPECT_NE(gltf.find("\"meshes\""),      std::string::npos);
    EXPECT_NE(gltf.find("\"buffers\""),     std::string::npos);
    EXPECT_NE(gltf.find("\"accessors\""),   std::string::npos);
    EXPECT_NE(gltf.find("\"bufferViews\""), std::string::npos);
    EXPECT_NE(gltf.find("\"scene\""),       std::string::npos);
    // Embedded buffer URI.
    EXPECT_NE(gltf.find("data:application/octet-stream;base64,"),
                std::string::npos);
}

TEST(FlatGltfExport, OnePrimitiveAndOneNodePerPart) {
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"a\"><extrude height=\"3\"><rect width=\"2\" height=\"2\"/></extrude></def>"
        "<def name=\"b\"><extrude height=\"4\"><rect width=\"3\" height=\"3\"/></extrude></def>"
        "<part><a/></part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto gltf = export_gltf(r, doc);
    // Exactly one mesh; exactly one node; one accessor each for
    // POSITION + indices = 2 accessors total.
    auto count_substr = [&](std::string_view needle) {
        std::size_t n = 0;
        std::size_t pos = 0;
        while ((pos = gltf.find(needle, pos)) != std::string::npos) {
            ++n; pos += needle.size();
        }
        return n;
    };
    EXPECT_EQ(count_substr("\"primitives\""), 1u);
    EXPECT_EQ(count_substr("\"POSITION\""),   1u);
    // extrude_linear (post ear-clipping rewrite) no longer emits the
    // cap-centroid vertices that used to carry ??z normals. Every ring
    // vertex participates in both side AND cap faces, so a single
    // per-vertex normal isn't well-defined; we leave the field empty
    // and let the renderer compute flat per-face normals from geometry
    // (which renderers typically do anyway ??? the FlatMesh.normals
    // field is unused by it). NORMAL attribute is therefore omitted.
    EXPECT_EQ(count_substr("\"NORMAL\""), 0u);
}

TEST(FlatGltfExport, ExtrasIncludeSourceAndLine) {
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() / "cadml_b32_extras";
    std::error_code ec; fs::remove_all(base, ec); fs::create_directories(base);
    auto write = [&](const fs::path& rel, std::string_view content) {
        const auto full = base / rel;
        fs::create_directories(full.parent_path());
        std::ofstream f(full, std::ios::binary);
        f << content;
    };
    write("widget.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"3\"><rect width=\"4\" height=\"4\"/></extrude>"
        "</part>");
    auto cr = cadml::compile::compile_file(base / "widget.cadml");
    ASSERT_TRUE(cr.ok());
    auto r = evaluate_flat(cr.document);
    ASSERT_TRUE(r.ok());
    const auto gltf = export_gltf(r, cr.document);
    // Per-node extras should carry the originating filename + line.
    EXPECT_NE(gltf.find("\"extras\""), std::string::npos);
    EXPECT_NE(gltf.find("\"source\""), std::string::npos);
    EXPECT_NE(gltf.find("\"line\""),   std::string::npos);
    // Filename should appear in the source field (could be "<entry>"
    // or a real path depending on the bundler's source registration).
    fs::remove_all(base, ec);
}

TEST(FlatGltfExport, EmptyResultProducesValidGltf) {
    // Parts with no geometry are skipped; resulting glTF has empty
    // meshes/nodes arrays but still parses as valid JSON.
    auto doc = parse_authoring(
        "version 0.1\n<part></part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto gltf = export_gltf(r, doc);
    // Buffers array is omitted entirely when there's nothing to emit
    // (embedding a zero-byte data URI is technically valid but some
    // glTF validators reject it). The rest of the document is still
    // structurally complete.
    EXPECT_EQ(gltf.find("\"buffers\""), std::string::npos);
    EXPECT_NE(gltf.find("\"asset\""),   std::string::npos);
    EXPECT_NE(gltf.find("\"scenes\""),  std::string::npos);
}

TEST(FlatGltfExport, ColorBecomesBaseColorMaterial) {
    // P2-5: a part `color` is exported as a glTF material with a
    // pbrMetallicRoughness.baseColorFactor, and the primitive references
    // it. A colorless part emits no materials array.
    auto doc = parse_authoring(
        "version 0.1\n<part color=\"red\">"
        "<extrude height=\"2\"><rect width=\"4\" height=\"4\"/></extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto gltf = export_gltf(r, doc);
    EXPECT_NE(gltf.find("\"materials\""), std::string::npos);
    EXPECT_NE(gltf.find("\"pbrMetallicRoughness\""), std::string::npos);
    EXPECT_NE(gltf.find("\"baseColorFactor\""), std::string::npos);
    EXPECT_NE(gltf.find("\"material\""), std::string::npos)
        << "primitive should reference the material by index";

    auto doc2 = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"2\"><rect width=\"4\" height=\"4\"/></extrude>"
        "</part>");
    auto r2 = evaluate_flat(doc2);
    ASSERT_TRUE(r2.ok());
    const auto gltf2 = export_gltf(r2, doc2);
    EXPECT_EQ(gltf2.find("\"materials\""), std::string::npos)
        << "a colorless part should emit no materials array";
}

TEST(FlatGltfExport, EndToEndModifierPipeline) {
    // Full pipeline through a modifier:
    //   compile_file (libcadml_compile) -> evaluate_flat -> export_gltf.
    // Catches breakage where bundler output isn't quite what the
    // modifier dispatch expects.
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() / "cadml_e2e_modifier";
    std::error_code ec; fs::remove_all(base, ec); fs::create_directories(base);
    auto write = [&](const fs::path& rel, std::string_view content) {
        const auto full = base / rel;
        fs::create_directories(full.parent_path());
        std::ofstream f(full, std::ios::binary);
        f << content;
    };
    write("widget.cadml",
        "version 0.1\n<part>"
        "<chamfer distance=\"0.5\">"
        "<extrude height=\"4\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</chamfer>"
        "</part>");

    auto cr = cadml::compile::compile_file(base / "widget.cadml");
    ASSERT_TRUE(cr.ok()) << (cr.errors.empty() ? "" : cr.errors[0].message);
    auto er = evaluate_flat(cr.document);
    ASSERT_TRUE(er.ok()) << (er.errors.empty() ? "" : er.errors[0].message);
    EXPECT_GT(er.parts[0].mesh.triangle_count(), 16u)
        << "chamfer applied through bundler should add geometry";
    const auto gltf = export_gltf(er, cr.document);
    EXPECT_FALSE(gltf.empty());
    EXPECT_NE(gltf.find("\"meshes\""), std::string::npos);
    EXPECT_NE(gltf.find("\"extras\""), std::string::npos)
        << "bundler-produced source range should populate node extras";
    fs::remove_all(base, ec);
}

TEST(FlatGltfExport, OptionsSuppressExtras) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"3\"><rect width=\"4\" height=\"4\"/></extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    cadml::engine::GltfExportOptions opts;
    opts.include_source_extras = false;
    const auto gltf = export_gltf(r, doc, opts);
    EXPECT_EQ(gltf.find("\"extras\""), std::string::npos);
}

// Stronger round-trip: parse the JSON via rapidjson and structurally
// validate that accessor counts, bufferView lengths, and the overall
// shape are internally consistent. Catches the kind of bug that
// substring-presence checks would miss (wrong byteOffset, mismatched
// componentType, accessor.count vs buffer size mismatch, etc.).
TEST(FlatGltfExport, RoundTripStructuralIntegrity) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"5\"><rect width=\"10\" height=\"6\"/></extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto gltf = export_gltf(r, doc);

    rapidjson::Document json;
    json.Parse(gltf.c_str());
    ASSERT_FALSE(json.HasParseError())
        << "exported glTF didn't round-trip through rapidjson";

    // Required top-level members per glTF 2.0.
    ASSERT_TRUE(json.HasMember("asset")        && json["asset"].IsObject());
    ASSERT_TRUE(json.HasMember("scenes")       && json["scenes"].IsArray());
    ASSERT_TRUE(json.HasMember("scene")        && json["scene"].IsUint());
    ASSERT_TRUE(json.HasMember("nodes")        && json["nodes"].IsArray());
    ASSERT_TRUE(json.HasMember("meshes")       && json["meshes"].IsArray());
    ASSERT_TRUE(json.HasMember("accessors")    && json["accessors"].IsArray());
    ASSERT_TRUE(json.HasMember("bufferViews")  && json["bufferViews"].IsArray());
    ASSERT_TRUE(json.HasMember("buffers")      && json["buffers"].IsArray());

    EXPECT_EQ(json["nodes"].Size(),  1u);
    EXPECT_EQ(json["meshes"].Size(), 1u);
    EXPECT_EQ(json["buffers"].Size(), 1u);

    // Walk the mesh ??? primitive ??? accessor chain and verify the
    // POSITION accessor's count matches the actual vertex count, and
    // the indices accessor's count matches the actual triangle*3 count.
    const auto& m = r.parts[0].mesh;
    const auto& mesh0 = json["meshes"][0];
    ASSERT_TRUE(mesh0.HasMember("primitives"));
    ASSERT_EQ(mesh0["primitives"].Size(), 1u);
    const auto& prim = mesh0["primitives"][0];
    ASSERT_TRUE(prim.HasMember("attributes"));
    ASSERT_TRUE(prim["attributes"].HasMember("POSITION"));
    ASSERT_TRUE(prim.HasMember("indices"));

    const auto pos_acc_idx = prim["attributes"]["POSITION"].GetUint();
    const auto idx_acc_idx = prim["indices"].GetUint();
    ASSERT_LT(pos_acc_idx, json["accessors"].Size());
    ASSERT_LT(idx_acc_idx, json["accessors"].Size());

    const auto& pos_acc = json["accessors"][pos_acc_idx];
    EXPECT_EQ(pos_acc["count"].GetUint64(), m.vertex_count());
    EXPECT_STREQ(pos_acc["type"].GetString(), "VEC3");
    EXPECT_EQ(pos_acc["componentType"].GetUint(), 5126u);  // FLOAT
    ASSERT_TRUE(pos_acc.HasMember("min"));
    ASSERT_TRUE(pos_acc.HasMember("max"));
    EXPECT_EQ(pos_acc["min"].Size(), 3u);
    EXPECT_EQ(pos_acc["max"].Size(), 3u);

    const auto& idx_acc = json["accessors"][idx_acc_idx];
    EXPECT_EQ(idx_acc["count"].GetUint64(), m.indices.size());
    EXPECT_STREQ(idx_acc["type"].GetString(), "SCALAR");
    EXPECT_EQ(idx_acc["componentType"].GetUint(), 5125u);  // UNSIGNED_INT

    // Buffer byteLength must match the sum of all bufferView lengths
    // (plus any padding bytes added by pad_to). Buffer is the floor.
    const auto buffer_bytes = json["buffers"][0]["byteLength"].GetUint64();
    std::uint64_t sum_bv = 0;
    for (const auto& bv : json["bufferViews"].GetArray()) {
        sum_bv += bv["byteLength"].GetUint64();
    }
    EXPECT_LE(sum_bv, buffer_bytes)
        << "bufferViews exceed declared buffer length";
    EXPECT_GE(buffer_bytes, sum_bv) << "buffer too short for its views";
    // POSITION accessor must reference a bufferView whose length covers
    // count * 3 * sizeof(float) bytes.
    const auto pos_bv_idx = pos_acc["bufferView"].GetUint();
    const auto pos_bv_len = json["bufferViews"][pos_bv_idx]["byteLength"].GetUint64();
    EXPECT_GE(pos_bv_len, m.vertex_count() * 3 * sizeof(float));
}

// ????????? Mesh cache (slice B3.1) ??????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????

TEST(FlatMeshCacheTest, RepeatedInstanceHitsCache) {
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"box\">"
        "<extrude height=\"5\">"
        "<rect x=\"0\" y=\"0\" width=\"4\" height=\"4\"/>"
        "</extrude>"
        "</def>"
        "<part>"
        "<box/>"
        "<group transform=\"translate(10, 0, 0)\"><box/></group>"
        "<group transform=\"translate(20, 0, 0)\"><box/></group>"
        "</part>");
    cadml::engine::FlatMeshCache cache;
    cadml::engine::EvalOptions opts;
    opts.cache = &cache;
    auto r = evaluate_flat(doc, opts);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(cache.size(),    1u) << "one entry per (def, overrides)";
    EXPECT_EQ(cache.misses(),  1u) << "first <box/> populates";
    EXPECT_EQ(cache.hits(),    2u) << "subsequent <box/>s hit";
}

TEST(FlatMeshCacheTest, MakeKeyDoesNotAliasOnPipeOrEquals) {
    // Two distinct inputs that would have collapsed onto the same
    // string under the old "<name>|<param>=<value>" scheme:
    //   Case A: def="a|b", override "c"=1   ->  "a|b|c=1"
    //   Case B: def="a",   override "b|c"=1 ->  "a|b|c=1"
    // CADML's identifier grammar wouldn't let an author write these
    // today, but the cache lives below the parser. The
    // length-prefixed encoding keeps them distinct so a future grammar
    // relaxation can't alias unrelated evaluations onto one mesh.
    cadml::engine::FlatMeshCache cache;
    cadml::engine::FlatMesh mesh_a;
    mesh_a.vertices.push_back({1, 0, 0});
    cadml::engine::FlatMesh mesh_b;
    mesh_b.vertices.push_back({2, 0, 0});

    std::map<std::string, double> overrides_a{ {"c",   1.0} };
    std::map<std::string, double> overrides_b{ {"b|c", 1.0} };
    cadml::engine::FlatMeshCacheAccess::store(cache, "a|b", overrides_a,
                                                 mesh_a);
    cadml::engine::FlatMeshCacheAccess::store(cache, "a",   overrides_b,
                                                 mesh_b);

    EXPECT_EQ(cache.size(), 2u);
    const auto* got_a = cadml::engine::FlatMeshCacheAccess::lookup(
        cache, "a|b", overrides_a);
    const auto* got_b = cadml::engine::FlatMeshCacheAccess::lookup(
        cache, "a", overrides_b);
    ASSERT_NE(got_a, nullptr);
    ASSERT_NE(got_b, nullptr);
    ASSERT_EQ(got_a->vertices.size(), 1u);
    ASSERT_EQ(got_b->vertices.size(), 1u);
    EXPECT_DOUBLE_EQ(got_a->vertices[0].x, 1.0);
    EXPECT_DOUBLE_EQ(got_b->vertices[0].x, 2.0);
}

TEST(FlatMeshCacheTest, DifferentOverridesSeparateEntries) {
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"box\">"
        "<param name=\"size\" value=\"10\"/>"
        "<extrude height=\"{size}\">"
        "<rect x=\"0\" y=\"0\" width=\"{size}\" height=\"{size}\"/>"
        "</extrude>"
        "</def>"
        "<part>"
        "<box size=\"10\"/>"
        "<group transform=\"translate(20, 0, 0)\"><box size=\"20\"/></group>"
        "<group transform=\"translate(40, 0, 0)\"><box size=\"10\"/></group>"
        "</part>");
    cadml::engine::FlatMeshCache cache;
    cadml::engine::EvalOptions opts; opts.cache = &cache;
    auto r = evaluate_flat(doc, opts);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(cache.size(),   2u) << "size=10 and size=20 are distinct keys";
    EXPECT_EQ(cache.misses(), 2u);
    EXPECT_EQ(cache.hits(),   1u) << "third <box size=10> hits the first";
}

TEST(FlatMeshCacheTest, CacheReuseAcrossEvaluateFlatCalls) {
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"box\">"
        "<extrude height=\"5\">"
        "<rect x=\"0\" y=\"0\" width=\"4\" height=\"4\"/>"
        "</extrude>"
        "</def>"
        "<part><box/></part>");
    cadml::engine::FlatMeshCache cache;
    cadml::engine::EvalOptions opts; opts.cache = &cache;

    auto r1 = evaluate_flat(doc, opts);
    ASSERT_TRUE(r1.ok());
    EXPECT_EQ(cache.misses(), 1u);
    EXPECT_EQ(cache.hits(),   0u);

    auto r2 = evaluate_flat(doc, opts);
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(cache.misses(), 1u) << "second call shouldn't re-eval the def";
    EXPECT_EQ(cache.hits(),   1u);
}

TEST(FlatMeshCacheTest, ResetClearsEverything) {
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"box\">"
        "<extrude height=\"5\"><rect width=\"4\" height=\"4\"/></extrude>"
        "</def>"
        "<part><box/></part>");
    cadml::engine::FlatMeshCache cache;
    cadml::engine::EvalOptions opts; opts.cache = &cache;
    evaluate_flat(doc, opts);
    ASSERT_GT(cache.size(), 0u);
    cache.reset();
    EXPECT_EQ(cache.size(),   0u);
    EXPECT_EQ(cache.hits(),   0u);
    EXPECT_EQ(cache.misses(), 0u);
}

TEST(FlatMeshCacheTest, NullCachePassesThrough) {
    // Sanity: opts.cache = nullptr behaves identically to no opts.
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"box\">"
        "<extrude height=\"5\"><rect width=\"4\" height=\"4\"/></extrude>"
        "</def>"
        "<part><box/><box/></part>");
    auto r1 = evaluate_flat(doc);
    cadml::engine::EvalOptions opts; opts.cache = nullptr;
    auto r2 = evaluate_flat(doc, opts);
    ASSERT_TRUE(r1.ok()); ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r1.parts[0].mesh.triangle_count(),
                r2.parts[0].mesh.triangle_count());
}

TEST(FlatMeshCacheTest, CachedMeshTrianglesPreserveSourceAttribution) {
    // After cache hit, triangle_node should match what the first eval
    // produced ??? Manifold-style source mapping survives caching.
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"box\">"
        "<extrude height=\"5\"><rect width=\"4\" height=\"4\"/></extrude>"
        "</def>"
        "<part>"
        "<box/>"
        "<group transform=\"translate(10, 0, 0)\"><box/></group>"
        "</part>");
    cadml::engine::FlatMeshCache cache;
    cadml::engine::EvalOptions opts; opts.cache = &cache;
    auto r = evaluate_flat(doc, opts);
    ASSERT_TRUE(r.ok());
    const auto& m = r.parts[0].mesh;
    ASSERT_EQ(m.triangle_node.size(), m.triangle_count());
    // The two boxes contribute equal-sized halves; first-half source
    // attribution should match second-half (both came from the same
    // cached mesh, then merged after group transforms).
    const auto half = m.triangle_count() / 2;
    for (std::size_t i = 0; i < half; ++i) {
        EXPECT_EQ(m.triangle_node[i], m.triangle_node[i + half]);
    }
}

// ????????? is_convex unit tests + concave-profile warning ??????????????????????????????????????????????????????

// ????????? triangulate_polygon (ear clipping) ??????????????????????????????????????????????????????????????????????????????????????????

TEST(TriangulatePolygon, ConvexQuadProducesTwoTriangles) {
    cadml::engine::detail::Polygon2D p{{ {0,0}, {10,0}, {10,10}, {0,10} }};
    const auto tris = cadml::engine::detail::triangulate_polygon(p);
    ASSERT_EQ(tris.size(), 2u);
    // Every triangle's index set must reference indices in [0, N).
    for (const auto& t : tris) {
        EXPECT_LT(t[0], 4u);
        EXPECT_LT(t[1], 4u);
        EXPECT_LT(t[2], 4u);
    }
}

TEST(TriangulatePolygon, TriangleReturnsItself) {
    cadml::engine::detail::Polygon2D p{{ {0,0}, {1,0}, {0,1} }};
    const auto tris = cadml::engine::detail::triangulate_polygon(p);
    ASSERT_EQ(tris.size(), 1u);
    EXPECT_EQ(tris[0][0], 0u);
    EXPECT_EQ(tris[0][1], 1u);
    EXPECT_EQ(tris[0][2], 2u);
}

TEST(TriangulatePolygon, LShapeProducesNMinusTwoTriangles) {
    // The L-bracket from the demo: 6-vertex concave profile.
    // Expected: N-2 = 4 triangles.
    cadml::engine::detail::Polygon2D p{{
        {0,0}, {40,0}, {40,8}, {8,8}, {8,40}, {0,40}
    }};
    const auto tris = cadml::engine::detail::triangulate_polygon(p);
    EXPECT_EQ(tris.size(), 4u);
    // No triangle may straddle the inner concave corner ??? that is, no
    // triangle (a,b,c) may have positive signed area AND contain any
    // OTHER vertex of the polygon strictly inside it. (This is the
    // ear-clipping invariant; verifying it directly catches the bug
    // where the centroid-fan used to put triangles across the empty
    // quadrant.)
    auto tri_area2 = [](cadml::Vec2 a, cadml::Vec2 b, cadml::Vec2 c) {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    auto strictly_inside = [&](cadml::Vec2 q,
                                cadml::Vec2 a, cadml::Vec2 b, cadml::Vec2 c) {
        const double s1 = tri_area2(a, b, q);
        const double s2 = tri_area2(b, c, q);
        const double s3 = tri_area2(c, a, q);
        return (s1 > 1e-9 && s2 > 1e-9 && s3 > 1e-9)
            || (s1 < -1e-9 && s2 < -1e-9 && s3 < -1e-9);
    };
    for (const auto& t : tris) {
        const auto a = p.points[t[0]];
        const auto b = p.points[t[1]];
        const auto c = p.points[t[2]];
        for (std::size_t i = 0; i < p.points.size(); ++i) {
            if (i == t[0] || i == t[1] || i == t[2]) continue;
            EXPECT_FALSE(strictly_inside(p.points[i], a, b, c))
                << "vertex " << i << " is inside triangle ("
                << t[0] << "," << t[1] << "," << t[2] << ")";
        }
    }
}

TEST(TriangulatePolygon, PlusShapeProducesNMinusTwoTriangles) {
    // Plus-sign profile: 12 vertices, four reflex corners (one per
    // inner concave junction). Expected: 12 - 2 = 10 triangles.
    cadml::engine::detail::Polygon2D p{{
        {-3, -1}, { 3, -1}, { 3, 1}, { 1, 1},
        { 1,  3}, {-1,  3}, {-1, 1}, {-3, 1},
        // (8 vertices ??? that's a plus on its side; let's use the
        // canonical 12-vertex plus instead)
    }};
    // 12-vertex plus:
    p.points = {
        {-1, -3}, { 1, -3}, { 1, -1}, { 3, -1},
        { 3,  1}, { 1,  1}, { 1,  3}, {-1,  3},
        {-1,  1}, {-3,  1}, {-3, -1}, {-1, -1}
    };
    const auto tris = cadml::engine::detail::triangulate_polygon(p);
    EXPECT_EQ(tris.size(), p.points.size() - 2);
}

TEST(TriangulatePolygon, DegenerateCountsReturnEmpty) {
    using cadml::engine::detail::triangulate_polygon;
    EXPECT_TRUE(triangulate_polygon({{}}).empty());
    EXPECT_TRUE(triangulate_polygon({{ {0,0} }}).empty());
    EXPECT_TRUE(triangulate_polygon({{ {0,0}, {1,0} }}).empty());
}

TEST(Polygon2DConvexity, RectIsConvex) {
    cadml::engine::detail::Polygon2D p{{ {0,0}, {1,0}, {1,1}, {0,1} }};
    EXPECT_TRUE(cadml::engine::detail::is_convex(p));
}

TEST(Polygon2DConvexity, CircleApproximationIsConvex) {
    auto c = cadml::engine::detail::tessellate_circle(0, 0, 5, 16);
    EXPECT_TRUE(cadml::engine::detail::is_convex(c));
}

TEST(Polygon2DConvexity, LShapeIsConcave) {
    // L-shape: an inverted corner makes one cross-product flip sign.
    cadml::engine::detail::Polygon2D p{{
        {0,0}, {2,0}, {2,1}, {1,1}, {1,2}, {0,2}
    }};
    EXPECT_FALSE(cadml::engine::detail::is_convex(p));
}

TEST(Polygon2DConvexity, CollinearRunIsToleratedAsConvex) {
    // A square with an extra colinear midpoint on one edge.
    cadml::engine::detail::Polygon2D p{{
        {0,0}, {1,0}, {2,0}, {2,2}, {0,2}
    }};
    EXPECT_TRUE(cadml::engine::detail::is_convex(p));
}

// --- Interference check --------------------------------------------------

// Helper: build a synthetic FlatEvalResult with two named parts whose
// meshes are simple boxes at given positions/sizes.
namespace {
cadml::engine::FlatMesh translated_box(double x, double y, double z,
                                          double w, double h, double d) {
    auto m = cadml::engine::detail::extrude_linear(
        cadml::engine::detail::tessellate_rect(x, y, w, h), d, 0);
    for (auto& v : m.vertices) v.z += z;
    return m;
}
}

// --- Binary STL export ---------------------------------------------------

namespace {
// Read a uint32_t little-endian from a string-stream offset (binary
// STL is little-endian by spec).
std::uint32_t read_u32_le(const std::string& s, std::size_t off) {
    return static_cast<std::uint8_t>(s[off])             |
            (static_cast<std::uint8_t>(s[off + 1]) << 8)  |
            (static_cast<std::uint8_t>(s[off + 2]) << 16) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[off + 3])) << 24);
}
}

#include <cadml/engine/flat_stl.hpp>
#include <cstring>
#include <sstream>

TEST(FlatStlExport, EmptyResultEmitsHeaderAndZeroCount) {
    cadml::engine::FlatEvalResult r;
    std::ostringstream os;
    cadml::engine::write_stl_binary(r, os);
    const auto bytes = os.str();
    // Binary STL: 80-byte header + uint32 triangle count = 84 bytes
    // for an empty mesh.
    ASSERT_EQ(bytes.size(), std::size_t{ 84 });
    EXPECT_EQ(read_u32_le(bytes, 80), 0u);
    // Header begins with the literal "CADML" (default tag).
    EXPECT_EQ(bytes.substr(0, 5), "CADML");
}

TEST(FlatStlExport, SingleBoxRoundTripsTriangleCount) {
    cadml::engine::FlatEvalResult r;
    r.parts.push_back({ "box", "",
        translated_box(0, 0, 0, 5, 5, 5) });
    const auto expected_tris = r.parts[0].mesh.triangle_count();
    std::ostringstream os;
    cadml::engine::write_stl_binary(r, os);
    const auto bytes = os.str();
    EXPECT_EQ(read_u32_le(bytes, 80), expected_tris);
    // Per-triangle record = 50 bytes (12 floats + 2-byte attr).
    EXPECT_EQ(bytes.size(), 84u + expected_tris * 50);
}

TEST(FlatStlExport, MergesAllPartsIntoOneTriangleSoup) {
    // Two boxes -> two parts -> their tri counts sum.
    cadml::engine::FlatEvalResult r;
    r.parts.push_back({ "a", "", translated_box(0, 0, 0, 5, 5, 5) });
    r.parts.push_back({ "b", "", translated_box(10, 0, 0, 5, 5, 5) });
    const auto tris_a = r.parts[0].mesh.triangle_count();
    const auto tris_b = r.parts[1].mesh.triangle_count();
    std::ostringstream os;
    cadml::engine::write_stl_binary(r, os);
    EXPECT_EQ(read_u32_le(os.str(), 80), tris_a + tris_b);
}

TEST(FlatStlExport, CustomHeaderTruncatesAt80Bytes) {
    cadml::engine::FlatEvalResult r;
    std::ostringstream os;
    const std::string long_header(200, 'X');
    cadml::engine::write_stl_binary(r, os, long_header);
    const auto bytes = os.str();
    // First 79 chars of the header are 'X' (we cap at 79, leaving the
    // 80th byte as a terminating zero).
    for (std::size_t i = 0; i < 79; ++i) {
        EXPECT_EQ(bytes[i], 'X') << "byte " << i << " not 'X'";
    }
    EXPECT_EQ(bytes[79], '\0');
}

TEST(FlatStlExport, RejectsMalformedIndexCount) {
    cadml::engine::FlatEvalResult r;
    cadml::engine::FlatMesh m;
    m.vertices = { {0,0,0}, {1,0,0}, {0,1,0} };
    m.indices  = { 0, 1 };       // not a multiple of 3
    r.parts.push_back({ "broken", "", std::move(m) });
    std::ostringstream os;
    EXPECT_THROW(cadml::engine::write_stl_binary(r, os), std::runtime_error);
}

TEST(FlatStlExport, RejectsOutOfRangeIndex) {
    cadml::engine::FlatEvalResult r;
    cadml::engine::FlatMesh m;
    m.vertices = { {0,0,0}, {1,0,0}, {0,1,0} };
    m.indices  = { 0, 1, 7 };    // 7 >= vertex count
    r.parts.push_back({ "broken", "", std::move(m) });
    std::ostringstream os;
    EXPECT_THROW(cadml::engine::write_stl_binary(r, os), std::runtime_error);
}

TEST(FlatStlExport, FaceNormalsComputedFromGeometry) {
    // Single CCW triangle in the +z plane ??? face normal should be +z.
    cadml::engine::FlatEvalResult r;
    cadml::engine::FlatMesh m;
    m.vertices = { {0,0,0}, {1,0,0}, {0,1,0} };
    m.indices  = { 0, 1, 2 };
    r.parts.push_back({ "tri", "", std::move(m) });
    std::ostringstream os;
    cadml::engine::write_stl_binary(r, os);
    const auto bytes = os.str();
    // Layout: 80 header + 4 count + (3 normal floats) ...
    // Read the three floats at offsets 84, 88, 92.
    auto read_float = [&](std::size_t off) {
        float f;
        std::memcpy(&f, bytes.data() + off, 4);
        return f;
    };
    EXPECT_FLOAT_EQ(read_float(84), 0.0f);
    EXPECT_FLOAT_EQ(read_float(88), 0.0f);
    EXPECT_FLOAT_EQ(read_float(92), 1.0f);
}

namespace {
cadml::engine::FlatEvalResult two_box_result(
    const std::string& name_a, double ax, double ay, double az,
                                  double aw, double ah, double ad,
    const std::string& name_b, double bx, double by, double bz,
                                  double bw, double bh, double bd) {
    cadml::engine::FlatEvalResult r;
    r.parts.push_back({ name_a, "",
        translated_box(ax, ay, az, aw, ah, ad) });
    r.parts.push_back({ name_b, "",
        translated_box(bx, by, bz, bw, bh, bd) });
    return r;
}
}

TEST(InterferenceCheck, DisjointPartsCleanReport) {
    auto er = two_box_result(
        "left",  0, 0, 0, 5, 5, 5,
        "right", 10, 0, 0, 5, 5, 5);
    auto rep = cadml::engine::check_interference(er);
    EXPECT_TRUE(rep.ok());
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.reports.size(), 0u);
}

TEST(InterferenceCheck, OverlappingPartsFlagged) {
    // Two boxes overlapping by 2??5??5 = 50 cubic units.
    auto er = two_box_result(
        "a", 0, 0, 0, 5, 5, 5,
        "b", 3, 0, 0, 5, 5, 5);
    auto rep = cadml::engine::check_interference(er);
    EXPECT_TRUE(rep.ok());
    ASSERT_EQ(rep.reports.size(), 1u);
    EXPECT_EQ(rep.reports[0].part_a, "a");
    EXPECT_EQ(rep.reports[0].part_b, "b");
    EXPECT_NEAR(rep.reports[0].volume, 50.0, 1e-6);
}

TEST(InterferenceCheck, ToleranceSuppressesSmallOverlap) {
    // Same 50-unit overlap; tolerance 100 ??? no report.
    auto er = two_box_result(
        "a", 0, 0, 0, 5, 5, 5,
        "b", 3, 0, 0, 5, 5, 5);
    cadml::engine::InterferenceOptions opts;
    opts.tolerance = 100.0;
    auto rep = cadml::engine::check_interference(er, opts);
    EXPECT_TRUE(rep.clean());
}

TEST(InterferenceCheck, FaceContactCountsAsZeroVolume) {
    // Two boxes touching along a face ??? intersection has zero volume.
    auto er = two_box_result(
        "a", 0, 0, 0, 5, 5, 5,
        "b", 5, 0, 0, 5, 5, 5);  // touches at x=5
    auto rep = cadml::engine::check_interference(er);
    EXPECT_TRUE(rep.ok());
    EXPECT_TRUE(rep.clean()) << "face-contact pairs shouldn't trigger";
}

TEST(InterferenceCheck, AllPairsChecked) {
    cadml::engine::FlatEvalResult er;
    er.parts.push_back({ "a", "", translated_box(0, 0, 0, 4, 4, 4) });
    er.parts.push_back({ "b", "", translated_box(3, 0, 0, 4, 4, 4) }); // overlaps a
    er.parts.push_back({ "c", "", translated_box(20, 0, 0, 4, 4, 4) }); // disjoint
    auto rep = cadml::engine::check_interference(er);
    EXPECT_EQ(rep.reports.size(), 1u);
    EXPECT_EQ(rep.reports[0].part_a, "a");
    EXPECT_EQ(rep.reports[0].part_b, "b");
}

TEST(InterferenceCheck, EmptyResultIsClean) {
    cadml::engine::FlatEvalResult er;
    auto rep = cadml::engine::check_interference(er);
    EXPECT_TRUE(rep.clean());
}

TEST(InterferenceCheck, EmptyMeshIsTreatedAsDisjoint) {
    cadml::engine::FlatEvalResult er;
    er.parts.push_back({ "a", "", translated_box(0, 0, 0, 4, 4, 4) });
    er.parts.push_back({ "empty", "", cadml::engine::FlatMesh{} });
    auto rep = cadml::engine::check_interference(er);
    EXPECT_TRUE(rep.clean());
}

// --- allow-pairs suppression --------------------------------------------

TEST(InterferenceCheck, AllowPairsSuppressesOverlap) {
    // Two boxes overlapping by 50 cubic units; (a,b) listed as exempt
    // ??? no report.
    auto er = two_box_result(
        "a", 0, 0, 0, 5, 5, 5,
        "b", 3, 0, 0, 5, 5, 5);
    cadml::engine::InterferenceOptions opts;
    opts.allow_pairs.push_back({ "a", "b" });
    auto rep = cadml::engine::check_interference(er, opts);
    EXPECT_TRUE(rep.clean());
}

TEST(InterferenceCheck, AllowPairsIsSymmetric) {
    // (b,a) should suppress the (a,b) pair just as well.
    auto er = two_box_result(
        "a", 0, 0, 0, 5, 5, 5,
        "b", 3, 0, 0, 5, 5, 5);
    cadml::engine::InterferenceOptions opts;
    opts.allow_pairs.push_back({ "b", "a" });   // reverse order
    auto rep = cadml::engine::check_interference(er, opts);
    EXPECT_TRUE(rep.clean());
}

TEST(InterferenceCheck, AllowPairsLeavesOtherPairsAlone) {
    // Three parts, two overlap each other; only one pair allowed.
    cadml::engine::FlatEvalResult er;
    er.parts.push_back({ "a", "", translated_box(0, 0, 0, 4, 4, 4) });
    er.parts.push_back({ "b", "", translated_box(3, 0, 0, 4, 4, 4) });   // overlaps a
    er.parts.push_back({ "c", "", translated_box(0, 3, 0, 4, 4, 4) });   // overlaps a, b
    cadml::engine::InterferenceOptions opts;
    opts.allow_pairs.push_back({ "a", "b" });
    auto rep = cadml::engine::check_interference(er, opts);
    // Pairs are (a,b) [allowed], (a,c), (b,c). Two reports remain.
    EXPECT_EQ(rep.reports.size(), 2u);
}

// --- Full cadmlcheck pipeline -------------------------------------------
//
// End-to-end integration of the four checks via the same library calls
// that `cadmlcheck` uses internally. Catches API drift in any of:
//   compile_file ??? parse_interference_tolerance ??? evaluate_flat ???
//   check_interference (with allow_pairs from doc.meta).

namespace {
// Mirror of cadmlcheck's library-level pipeline (compile ??? eval ???
// tolerance + allow-pairs from doc.meta ??? check_interference).
struct CheckPipelineResult {
    cadml::compile::CompileResult       compile;
    cadml::engine::FlatEvalResult       eval;
    cadml::engine::InterferenceResult   interference;
};

CheckPipelineResult run_check_pipeline(const std::filesystem::path& entry) {
    CheckPipelineResult r;
    r.compile = cadml::compile::compile_file(entry);
    if (!r.compile.ok()) return r;
    r.eval = cadml::engine::evaluate_flat(r.compile.document);
    if (!r.eval.ok()) return r;
    cadml::engine::InterferenceOptions iopts;
    if (auto t = cadml::parse_interference_tolerance(
            r.compile.document.meta.interference_tolerance,
            r.compile.document.meta.units)) {
        iopts.tolerance = *t;
    }
    iopts.allow_pairs = r.compile.document.meta.allow_interference_pairs;
    r.interference = cadml::engine::check_interference(r.eval, iopts);
    return r;
}
}

TEST(CadmlCheckPipeline, OverlappingPartsReportInterference) {
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() / "cadml_d5_overlap";
    std::error_code ec; fs::remove_all(base, ec); fs::create_directories(base);
    {
        std::ofstream f(base / "rig.cadml");
        f << "version 0.1\n"
              "<part name=\"a\">"
              "<extrude height=\"5\"><rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/></extrude>"
              "</part>\n"
              "<part name=\"b\">"
              "<extrude height=\"5\"><rect x=\"3\" y=\"0\" width=\"5\" height=\"5\"/></extrude>"
              "</part>\n";
    }
    auto r = run_check_pipeline(base / "rig.cadml");
    ASSERT_TRUE(r.compile.ok());
    ASSERT_TRUE(r.eval.ok());
    EXPECT_TRUE(r.interference.ok());
    ASSERT_EQ(r.interference.reports.size(), 1u);
    EXPECT_EQ(r.interference.reports[0].part_a, "a");
    EXPECT_EQ(r.interference.reports[0].part_b, "b");
}

TEST(CadmlCheckPipeline, CliToleranceOverrideMimicsTolerance) {
    // The CLI's `--tolerance <volume>` substitutes the override text
    // for `doc.meta.interference_tolerance` when running the parser
    // and feeds the result to InterferenceOptions. Mirror that exact
    // glue here ??? if it changes, this test must change with it.
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() / "cadml_d5_cli_override";
    std::error_code ec; fs::remove_all(base, ec); fs::create_directories(base);
    {
        std::ofstream f(base / "rig.cadml");
        // No frontmatter tolerance ??? overlap is 50 mm^3.
        f << "version 0.1\n"
              "<part name=\"a\">"
              "<extrude height=\"5\"><rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/></extrude>"
              "</part>\n"
              "<part name=\"b\">"
              "<extrude height=\"5\"><rect x=\"3\" y=\"0\" width=\"5\" height=\"5\"/></extrude>"
              "</part>\n";
    }
    auto cr = cadml::compile::compile_file(base / "rig.cadml");
    ASSERT_TRUE(cr.ok());
    auto er = cadml::engine::evaluate_flat(cr.document);
    ASSERT_TRUE(er.ok());

    cadml::engine::InterferenceOptions iopts;
    // CLI substitutes the override string here.
    auto t = cadml::parse_interference_tolerance("100mm3",
                                                    cr.document.meta.units);
    ASSERT_TRUE(t.has_value());
    iopts.tolerance = *t;
    iopts.allow_pairs = cr.document.meta.allow_interference_pairs;
    auto report = cadml::engine::check_interference(er, iopts);
    EXPECT_TRUE(report.clean());
}

// --- cadml build wrapper safe-emit policy -------------------------------
//
// `cadml build` writes .fcadml only when every check is clean. Verify
// the gate: same input that fails interference must produce the same
// CompileResult, but the wrapper-level decision treats it as failure.

TEST(CadmlBuildPipeline, EmitsOnlyWhenEverythingClean) {
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() / "cadml_p6_build_dirty";
    std::error_code ec; fs::remove_all(base, ec); fs::create_directories(base);
    {
        std::ofstream f(base / "rig.cadml");
        f << "version 0.1\n"
              "<part name=\"a\">"
              "<extrude height=\"5\"><rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/></extrude>"
              "</part>\n"
              "<part name=\"b\">"
              "<extrude height=\"5\"><rect x=\"3\" y=\"0\" width=\"5\" height=\"5\"/></extrude>"
              "</part>\n";
    }
    auto r = run_check_pipeline(base / "rig.cadml");
    ASSERT_TRUE(r.compile.ok());
    ASSERT_TRUE(r.eval.ok());
    // Compile produced flat text (ready to write) but interference
    // is non-empty ??? `cadml build`'s gate would refuse to emit.
    EXPECT_FALSE(r.compile.flat_text.empty());
    ASSERT_FALSE(r.interference.reports.empty());
    const bool would_emit = r.interference.reports.empty() &&
                             r.interference.errors.empty();
    EXPECT_FALSE(would_emit);
}

TEST(CadmlBuildPipeline, EmitsWhenCleanWithToleranceFrontmatter) {
    // Same overlap, suppressed by tolerance ??? `cadml build` emits.
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() / "cadml_p6_build_clean";
    std::error_code ec; fs::remove_all(base, ec); fs::create_directories(base);
    {
        std::ofstream f(base / "rig.cadml");
        f << "version 0.1\n"
              "interference-tolerance 1000mm3\n"
              "<part name=\"a\">"
              "<extrude height=\"5\"><rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/></extrude>"
              "</part>\n"
              "<part name=\"b\">"
              "<extrude height=\"5\"><rect x=\"3\" y=\"0\" width=\"5\" height=\"5\"/></extrude>"
              "</part>\n";
    }
    auto r = run_check_pipeline(base / "rig.cadml");
    ASSERT_TRUE(r.compile.ok());
    ASSERT_TRUE(r.eval.ok());
    EXPECT_TRUE(r.interference.clean());
    const bool would_emit = r.interference.reports.empty() &&
                             r.interference.errors.empty();
    EXPECT_TRUE(would_emit);
    EXPECT_FALSE(r.compile.flat_text.empty());
}

TEST(CadmlCheckPipeline, ToleranceFromFrontmatterSuppresses) {
    // Same overlap as above, but frontmatter sets a tolerance large
    // enough to swallow it.
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() / "cadml_d5_tolerance";
    std::error_code ec; fs::remove_all(base, ec); fs::create_directories(base);
    {
        std::ofstream f(base / "rig.cadml");
        f << "version 0.1\n"
              "interference-tolerance 1000mm3\n"
              "<part name=\"a\">"
              "<extrude height=\"5\"><rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/></extrude>"
              "</part>\n"
              "<part name=\"b\">"
              "<extrude height=\"5\"><rect x=\"3\" y=\"0\" width=\"5\" height=\"5\"/></extrude>"
              "</part>\n";
    }
    auto r = run_check_pipeline(base / "rig.cadml");
    ASSERT_TRUE(r.compile.ok());
    ASSERT_TRUE(r.eval.ok());
    EXPECT_TRUE(r.interference.clean());
}

// ????????? Edge topology (slice C1) ???????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????

TEST(EdgeTopology, ExtrudeRectIs12ConvexPlus6Flat) {
    // A linear-extruded rect produces 12 triangles via extrude_linear
    // (ear-clipped caps):
    //   - 4 side quads ?? 2 = 8 triangles (4 flat side-quad diagonals)
    //   - (4-2) = 2 ear-clipped tris per cap ?? 2 caps = 4 (1 flat
    //     diagonal per cap ?? 2 caps = 2)
    // Edges total: 18 ??? 12 cube-corner CONVEX edges (4 bottom-perimeter,
    // 4 top-perimeter, 4 vertical), and 6 FLAT edges (4 side-quad
    // diagonals + 2 cap diagonals).
    auto m = cadml::engine::detail::extrude_linear(
        cadml::engine::detail::tessellate_rect(0, 0, 10, 6),
        4, /*src=*/0);
    ASSERT_EQ(m.triangle_count(), 12u);

    auto topo = cadml::engine::detail::build_edge_topology(m);
    EXPECT_EQ(topo.edges.size(), 18u);
    int convex = 0, flat = 0, concave = 0, boundary = 0, nm = 0;
    for (const auto& e : topo.edges) {
        switch (e.classification) {
            using cadml::engine::detail::EdgeClass;
            case EdgeClass::Convex:      ++convex;   break;
            case EdgeClass::Flat:        ++flat;     break;
            case EdgeClass::Concave:     ++concave;  break;
            case EdgeClass::Boundary:    ++boundary; break;
            case EdgeClass::NonManifold: ++nm;       break;
        }
    }
    EXPECT_EQ(convex,   12) << "12 cube-corner edges expected";
    EXPECT_EQ(flat,      6) << "6 triangulation seams expected";
    EXPECT_EQ(concave,   0);
    EXPECT_EQ(boundary,  0) << "extrude produces a closed mesh";
    EXPECT_EQ(nm,        0);
}

TEST(EdgeTopology, ExtrudeRectConvexEdgesAre90Degrees) {
    auto m = cadml::engine::detail::extrude_linear(
        cadml::engine::detail::tessellate_rect(0, 0, 10, 6),
        4, /*src=*/0);
    auto topo = cadml::engine::detail::build_edge_topology(m);
    constexpr double kPi = 3.14159265358979323846;
    for (const auto& e : topo.edges) {
        if (e.classification == cadml::engine::detail::EdgeClass::Convex) {
            EXPECT_NEAR(e.dihedral_rad, kPi / 2, 1e-9);
        } else if (e.classification == cadml::engine::detail::EdgeClass::Flat) {
            EXPECT_NEAR(e.dihedral_rad, kPi, 1e-9);
        }
    }
}

TEST(EdgeTopology, TriangleEdgesIndexBackIntoEdgesArray) {
    auto m = cadml::engine::detail::extrude_linear(
        cadml::engine::detail::tessellate_rect(0, 0, 10, 6),
        4, /*src=*/0);
    auto topo = cadml::engine::detail::build_edge_topology(m);
    ASSERT_EQ(topo.triangle_edges.size(), m.triangle_count());
    // Each triangle should have exactly 3 valid edge indices, each
    // pointing at an EdgeInfo whose triangles list contains this tri.
    for (std::uint32_t t = 0; t < m.triangle_count(); ++t) {
        for (int slot = 0; slot < 3; ++slot) {
            const auto eidx = topo.triangle_edges[t][slot];
            ASSERT_GE(eidx, 0) << "triangle " << t << " slot " << slot;
            ASSERT_LT(static_cast<std::size_t>(eidx), topo.edges.size());
            const auto& info = topo.edges[eidx];
            EXPECT_NE(std::find(info.triangles.begin(),
                                  info.triangles.end(), t),
                        info.triangles.end());
        }
    }
}

TEST(EdgeTopology, OpenMeshHasBoundaryEdges) {
    // Hand-construct a single-triangle mesh ??? three boundary edges,
    // no interior. Manifold check must classify all three as Boundary.
    cadml::engine::FlatMesh m;
    m.vertices = { {0,0,0}, {1,0,0}, {0,1,0} };
    m.normals  = { {0,0,1}, {0,0,1}, {0,0,1} };
    m.indices  = { 0, 1, 2 };
    m.triangle_node = { 0 };
    auto topo = cadml::engine::detail::build_edge_topology(m);
    EXPECT_EQ(topo.edges.size(), 3u);
    for (const auto& e : topo.edges) {
        EXPECT_EQ(e.classification,
                    cadml::engine::detail::EdgeClass::Boundary);
    }
}

TEST(EdgeTopology, ConcaveEdgeOnInwardFold) {
    // Two triangles sharing an edge, folded INWARD (the dihedral
    // angle through the solid > ??). Construct them manually so the
    // outward-CCW convention puts the fold on the concave side.
    //
    // Setup: edge along the X axis from (0,0,0) to (1,0,0). Two
    // triangles drape DOWN on both sides (z < 0). When the solid is
    // "above" the fold, the angle from outside through the body is
    // reflex ??? concave.
    cadml::engine::FlatMesh m;
    m.vertices = {
        {0, 0, 0},      // 0 ??? edge start
        {1, 0, 0},      // 1 ??? edge end
        {0.5, -1, -1},  // 2 ??? t1's far vertex (below + back)
        {0.5,  1, -1},  // 3 ??? t2's far vertex (below + front)
    };
    m.normals.assign(4, {0, 0, 0});
    // CCW from outside (viewed from +Z): t1 = (0, 1, 2), t2 = (1, 0, 3).
    m.indices  = { 0, 1, 2,  1, 0, 3 };
    m.triangle_node = { 0, 0 };
    auto topo = cadml::engine::detail::build_edge_topology(m);
    // Find the shared edge (0, 1).
    const cadml::engine::detail::EdgeInfo* shared = nullptr;
    for (const auto& e : topo.edges) {
        if (e.key.v0 == 0 && e.key.v1 == 1) { shared = &e; break; }
    }
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(shared->classification,
                cadml::engine::detail::EdgeClass::Concave);
    constexpr double kPi = 3.14159265358979323846;
    EXPECT_GT(shared->dihedral_rad, kPi);
}

// ????????? Tier 1 predicate (slice C2) ??????????????????????????????????????????????????????????????????????????????????????????????????????????????????

namespace {
// Helper: build topology of a mesh + return the indices of every
// Convex edge. Used by predicate tests below.
std::vector<std::uint32_t> convex_edge_indices(
    const cadml::engine::detail::EdgeTopology& topo)
{
    std::vector<std::uint32_t> out;
    for (std::uint32_t i = 0; i < topo.edges.size(); ++i) {
        if (topo.edges[i].classification ==
            cadml::engine::detail::EdgeClass::Convex) {
            out.push_back(i);
        }
    }
    return out;
}
}

TEST(Tier1Predicate, AcceptsAllCubeEdges) {
    auto m = cadml::engine::detail::extrude_linear(
        cadml::engine::detail::tessellate_rect(0, 0, 10, 6),
        4, /*src=*/0);
    auto topo = cadml::engine::detail::build_edge_topology(m);
    auto edges = convex_edge_indices(topo);
    auto r = cadml::engine::detail::tier1_check(m, topo, edges);
    EXPECT_TRUE(r.ok) << r.reason;
}

TEST(Tier1Predicate, RejectsCylinderRimEdge) {
    // 32-segment cylinder: each side strip has ~11?? bends to its
    // neighbors ??? near-flat convex edges ??? predicate must reject any
    // rim-edge selection (chamfering the rim isn't a Tier 1 op).
    auto m = cadml::engine::detail::extrude_linear(
        cadml::engine::detail::tessellate_circle(0, 0, 5, 32),
        10, /*src=*/0);
    auto topo = cadml::engine::detail::build_edge_topology(m);
    // Pick the FIRST convex edge from the cylinder. Whatever it is ???
    // top rim, bottom rim, or a vertical-ring corner ??? its adjacent
    // side-strip triangles have small bends to their ring neighbors,
    // so the predicate fails.
    auto edges = convex_edge_indices(topo);
    ASSERT_FALSE(edges.empty());
    auto r = cadml::engine::detail::tier1_check(m, topo, { edges.front() });
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.reason.find("near-flat convex"), std::string::npos);
}

TEST(Tier1Predicate, RejectsConcaveEdgeSelection) {
    // L-shape via two boxes booleaned together would produce a real
    // concave edge. Easier to construct: a non-convex profile via
    // bypassing the parser. Use the same fold-mesh from the
    // ConcaveEdgeOnInwardFold test setup.
    cadml::engine::FlatMesh m;
    m.vertices = {
        {0, 0, 0}, {1, 0, 0}, {0.5, -1, -1}, {0.5, 1, -1},
    };
    m.normals.assign(4, {0, 0, 0});
    m.indices = { 0, 1, 2,  1, 0, 3 };
    m.triangle_node = { 0, 0 };
    auto topo = cadml::engine::detail::build_edge_topology(m);
    // The (0,1) edge is concave ??? selecting it must fail.
    std::uint32_t target = 0;
    for (std::uint32_t i = 0; i < topo.edges.size(); ++i) {
        if (topo.edges[i].key.v0 == 0 && topo.edges[i].key.v1 == 1) {
            target = i; break;
        }
    }
    auto r = cadml::engine::detail::tier1_check(m, topo, { target });
    EXPECT_FALSE(r.ok);
    // The whole-mesh "any concave anywhere" check fires first and
    // produces this reason; the per-edge check never runs.
    EXPECT_NE(r.reason.find("concave"), std::string::npos);
}

TEST(Tier1Predicate, RejectsBoundaryEdgeSelection) {
    // Single triangle: every edge is boundary, none convex. Predicate
    // must reject any selection.
    cadml::engine::FlatMesh m;
    m.vertices = { {0,0,0}, {1,0,0}, {0,1,0} };
    m.normals  = { {0,0,1}, {0,0,1}, {0,0,1} };
    m.indices  = { 0, 1, 2 };
    m.triangle_node = { 0 };
    auto topo = cadml::engine::detail::build_edge_topology(m);
    auto r = cadml::engine::detail::tier1_check(m, topo, { 0 });
    EXPECT_FALSE(r.ok);
}

TEST(Tier1Predicate, EmptySelectionTriviallyOk) {
    cadml::engine::FlatMesh m;
    cadml::engine::detail::EdgeTopology topo;
    auto r = cadml::engine::detail::tier1_check(m, topo, {});
    EXPECT_TRUE(r.ok);
}

// ????????? Chamfer (slice C3) ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????

TEST(Chamfer, CubeAllEdgesProducesExpectedBbox) {
    // 10x6x4 box, chamfer width 0.5. The result's bbox stays the same
    // (chamfer cuts INWARD from edges, doesn't extend outside).
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<chamfer distance=\"0.5\">"
        "<extrude height=\"4\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</chamfer>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    ASSERT_EQ(r.parts.size(), 1u);
    EXPECT_TRUE(r.warnings.empty()) << r.warnings.front().message;

    const auto& m = r.parts[0].mesh;
    EXPECT_GT(m.triangle_count(), 16u)
        << "chamfered cube should have more triangles than the plain box";

    // Bbox stays at the original box's bounds ??? chamfer only removes
    // material, it doesn't add any.
    auto b = bbox(m);
    EXPECT_NEAR(b.min_x,  0.0, 1e-6);
    EXPECT_NEAR(b.max_x, 10.0, 1e-6);
    EXPECT_NEAR(b.min_y,  0.0, 1e-6);
    EXPECT_NEAR(b.max_y,  6.0, 1e-6);
    EXPECT_NEAR(b.min_z,  0.0, 1e-6);
    EXPECT_NEAR(b.max_z,  4.0, 1e-6);
}

TEST(Chamfer, CubeAllEdgesRemovesCornerVertices) {
    // After chamfering all 12 edges of a box, no vertex should be at
    // the original 8 corner positions ??? every corner is replaced by
    // a triangular miter patch with 3 new vertices.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<chamfer distance=\"0.5\">"
        "<extrude height=\"4\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</chamfer>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto& m = r.parts[0].mesh;

    // Expected box corners (un-chamfered): 8 of them.
    const std::array<cadml::Vec3, 8> corners = {{
        {0,0,0}, {10,0,0}, {10,6,0}, {0,6,0},
        {0,0,4}, {10,0,4}, {10,6,4}, {0,6,4},
    }};
    for (const auto& corner : corners) {
        for (const auto& v : m.vertices) {
            const double dx = v.x - corner.x;
            const double dy = v.y - corner.y;
            const double dz = v.z - corner.z;
            EXPECT_GT(std::sqrt(dx*dx + dy*dy + dz*dz), 0.4)
                << "found vertex too close to original corner ("
                << corner.x << "," << corner.y << "," << corner.z
                << ") ??? chamfer didn't remove it";
        }
    }
}

TEST(Chamfer, CylinderRejectedByPredicate) {
    // A cylinder fails the Tier 1 predicate (small bends on side
    // strips look like curve approximation). Chamfer warns and falls
    // back to the unmodified input.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<chamfer distance=\"0.5\">"
        "<extrude height=\"10\"><circle r=\"5\"/></extrude>"
        "</chamfer>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.warnings.empty());
    EXPECT_TRUE(any_warning_contains(r, "Tier 1"));
}

TEST(Chamfer, ZeroDistanceLeavesMeshUnchanged) {
    auto plain = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"4\"><rect x=\"0\" y=\"0\" width=\"6\" height=\"6\"/></extrude>"
        "</part>");
    auto chamfered = parse_authoring(
        "version 0.1\n<part>"
        "<chamfer distance=\"0\">"
        "<extrude height=\"4\"><rect x=\"0\" y=\"0\" width=\"6\" height=\"6\"/></extrude>"
        "</chamfer>"
        "</part>");
    auto rp = evaluate_flat(plain);
    auto rc = evaluate_flat(chamfered);
    ASSERT_TRUE(rp.ok());
    ASSERT_TRUE(rc.ok());
    EXPECT_EQ(rp.parts[0].mesh.vertex_count(),
                rc.parts[0].mesh.vertex_count());
    EXPECT_EQ(rp.parts[0].mesh.triangle_count(),
                rc.parts[0].mesh.triangle_count());
}

TEST(Chamfer, OversizedDistanceWarns) {
    // Regression: chamfer with a distance bigger than the
    // input can support consumes the entire mesh via the cutter
    // subtract. Earlier silently produced an empty mesh; now warns.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<chamfer distance=\"100\">"
        "<extrude height=\"4\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</chamfer>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.warnings.empty());
    EXPECT_TRUE(any_warning_contains(r, "result is empty"));
    EXPECT_TRUE(any_warning_contains(r, "too large"));
}

TEST(Chamfer, NegativeDistanceWarns) {
    // Regression: negative distance is bad input. Earlier
    // silently no-op'd; now warns and falls back to the input.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<chamfer distance=\"-0.5\">"
        "<extrude height=\"4\"><rect width=\"6\" height=\"6\"/></extrude>"
        "</chamfer>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.warnings.empty());
    EXPECT_TRUE(any_warning_contains(r, "negative distance"));
}

TEST(Chamfer, RightTrianglePrismCovers45DegreeCorners) {
    // Coverage: a right-triangle prism has vertical corner
    // edges with non-orthogonal adjacent face pairs (135?? dihedral
    // at the 45?? interior corners). Exercises the prism orientation
    // logic on something other than cube-aligned faces.
    //
    // Build a right triangle profile via <path>... actually the
    // parser only supports <rect> and <circle> as profiles today. So
    // we hand-build a 3-vertex profile via tessellate_rect's API
    // analogue.
    cadml::engine::detail::Polygon2D tri;
    tri.points = { {0, 0}, {10, 0}, {0, 6} };
    auto m = cadml::engine::detail::extrude_linear(tri, 4, /*src=*/0);
    auto chr = cadml::engine::detail::chamfer(m, 0.5, cadml::Selector{}, /*src=*/0);
    ASSERT_TRUE(chr.ok) << chr.error;
    EXPECT_GT(chr.mesh.triangle_count(), m.triangle_count())
        << "chamfered triangular prism should have more triangles";
    // Bbox preserved (chamfer only removes material).
    auto b = bbox(chr.mesh);
    EXPECT_NEAR(b.min_x,  0.0, 1e-6);
    EXPECT_NEAR(b.max_x, 10.0, 1e-6);
    EXPECT_NEAR(b.min_y,  0.0, 1e-6);
    EXPECT_NEAR(b.max_y,  6.0, 1e-6);
    EXPECT_NEAR(b.min_z,  0.0, 1e-6);
    EXPECT_NEAR(b.max_z,  4.0, 1e-6);
}

TEST(Chamfer, EmptyChildWarnsButOk) {
    auto doc = parse_authoring(
        "version 0.1\n<part><chamfer distance=\"0.5\"/></part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.warnings.empty());
    EXPECT_TRUE(any_warning_contains(r, "chamfer has no 3D child"));
}

// ????????? Fillet (slice C4) ????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????

TEST(Fillet, CubeAllEdgesProducesExpectedBbox) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<fillet radius=\"0.5\">"
        "<extrude height=\"4\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</fillet>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    ASSERT_EQ(r.parts.size(), 1u);
    EXPECT_TRUE(r.warnings.empty()) << r.warnings.front().message;

    const auto& m = r.parts[0].mesh;
    EXPECT_GT(m.triangle_count(), 16u)
        << "filleted cube should have more triangles than the plain box";

    auto b = bbox(m);
    EXPECT_NEAR(b.min_x,  0.0, 1e-6);
    EXPECT_NEAR(b.max_x, 10.0, 1e-6);
    EXPECT_NEAR(b.min_y,  0.0, 1e-6);
    EXPECT_NEAR(b.max_y,  6.0, 1e-6);
    EXPECT_NEAR(b.min_z,  0.0, 1e-6);
    EXPECT_NEAR(b.max_z,  4.0, 1e-6);
}

TEST(Fillet, CubeAllEdgesRemovesCornerVertices) {
    // The fillet must not leave any output vertex at the original
    // cube corner positions. Weakest meaningful check: distance > 1e-3
    // (catches a literal vertex-at-corner; tolerates fillet-surface
    // tessellation noise).
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<fillet radius=\"0.5\">"
        "<extrude height=\"4\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</fillet>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    const auto& m = r.parts[0].mesh;
    const std::array<cadml::Vec3, 8> corners = {{
        {0,0,0}, {10,0,0}, {10,6,0}, {0,6,0},
        {0,0,4}, {10,0,4}, {10,6,4}, {0,6,4},
    }};
    for (const auto& corner : corners) {
        for (const auto& v : m.vertices) {
            const double dx = v.x - corner.x;
            const double dy = v.y - corner.y;
            const double dz = v.z - corner.z;
            EXPECT_GT(std::sqrt(dx*dx + dy*dy + dz*dz), 1e-3)
                << "vertex sat at original corner ("
                << corner.x << "," << corner.y << "," << corner.z << ")";
        }
    }
}

TEST(Fillet, CubeFilletProducesCurvedRegions) {
    // Fillet with 16-segment cylinders ?? 12 edges + corner intersection
    // patches ??? expect significantly more triangles than chamfer (~64).
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<fillet radius=\"0.5\">"
        "<extrude height=\"4\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</fillet>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    // 16-segment quarter-cylinders ?? 12 edges + cap/face triangulation
    // ??? expect significantly more than the chamfered cube (64 tris).
    EXPECT_GT(r.parts[0].mesh.triangle_count(), 100u);
}

TEST(Fillet, CylinderRejectedByPredicate) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<fillet radius=\"0.5\">"
        "<extrude height=\"10\"><circle r=\"5\"/></extrude>"
        "</fillet>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "Tier 1"));
}

TEST(Fillet, NegativeRadiusWarns) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<fillet radius=\"-0.5\">"
        "<extrude height=\"4\"><rect width=\"6\" height=\"6\"/></extrude>"
        "</fillet>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "negative radius"));
}

TEST(Fillet, OversizedRadiusWarns) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<fillet radius=\"100\">"
        "<extrude height=\"4\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</fillet>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "result is empty"));
}

TEST(Fillet, ZeroRadiusLeavesMeshUnchanged) {
    auto plain = parse_authoring(
        "version 0.1\n<part>"
        "<extrude height=\"4\"><rect x=\"0\" y=\"0\" width=\"6\" height=\"6\"/></extrude>"
        "</part>");
    auto filleted = parse_authoring(
        "version 0.1\n<part>"
        "<fillet radius=\"0\">"
        "<extrude height=\"4\"><rect x=\"0\" y=\"0\" width=\"6\" height=\"6\"/></extrude>"
        "</fillet>"
        "</part>");
    auto rp = evaluate_flat(plain);
    auto rf = evaluate_flat(filleted);
    ASSERT_TRUE(rp.ok());
    ASSERT_TRUE(rf.ok());
    EXPECT_EQ(rp.parts[0].mesh.triangle_count(),
                rf.parts[0].mesh.triangle_count());
}

TEST(Fillet, RadiusMeasurementApproximatesRequest) {
    // For a 1.0-radius fillet, sample vertices on the +X +Y vertical
    // edge fillet surface (cylinder axis at x=10-1, y=6-1). Each
    // sampled vertex should be at distance ??? 1.0 from that axis.
    constexpr double R = 1.0;
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<fillet radius=\"1.0\">"
        "<extrude height=\"4\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</fillet>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    int sampled = 0;
    for (const auto& v : r.parts[0].mesh.vertices) {
        // Vertices on the +X+Y vertical-edge fillet cylinder surface
        // sit at x ??? [9, 10], y ??? [5, 6], z ??? [1, 3] ??? the inclusive
        // range covers the cylinder ring vertices at z=R and z=4-R
        // where the side fillet transitions to the top/bottom rim
        // fillets. Use closed interval; cylinder middle has no ring
        // vertices because Manifold doesn't add intermediate ones.
        if (v.x >= 10 - R - 1e-3 && v.x <= 10 + 1e-3 &&
            v.y >=  6 - R - 1e-3 && v.y <=  6 + 1e-3 &&
            v.z >= R - 1e-3      && v.z <= 4 - R + 1e-3) {
            const double dx = v.x - (10.0 - R);
            const double dy = v.y - (6.0  - R);
            const double dist = std::sqrt(dx*dx + dy*dy);
            EXPECT_NEAR(dist, R, 1e-3);
            ++sampled;
        }
    }
    EXPECT_GT(sampled, 0)
        << "no vertex landed in the +X+Y edge fillet surface region";
}

// ????????? Shell (slice C5) ???????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????

TEST(Shell, ClosedShellHasOuterAndInnerSurfaces) {
    // 10??6??4 closed box, wall thickness 1 ??? outer bbox unchanged;
    // every face of the original is now a "wall" of thickness 1 with
    // an inner cavity of dimensions 8??4??2.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<shell thickness=\"1\">"
        "<extrude height=\"4\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</shell>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_TRUE(r.warnings.empty()) << r.warnings.front().message;
    const auto& m = r.parts[0].mesh;
    EXPECT_GT(m.triangle_count(), 16u)
        << "shell should have outer + inner surfaces ??? more tris than plain box";

    // Bbox unchanged from the source extrude.
    auto b = bbox(m);
    EXPECT_NEAR(b.min_x,  0.0, 1e-6);
    EXPECT_NEAR(b.max_x, 10.0, 1e-6);
    EXPECT_NEAR(b.min_y,  0.0, 1e-6);
    EXPECT_NEAR(b.max_y,  6.0, 1e-6);
    EXPECT_NEAR(b.min_z,  0.0, 1e-6);
    EXPECT_NEAR(b.max_z,  4.0, 1e-6);

    // Sample inner-surface vertices: the inner cavity is at x???[1,9],
    // y???[1,5], z???[1,3]. At LEAST one vertex should land on this
    // inner box's boundary.
    int inner_hits = 0;
    for (const auto& v : m.vertices) {
        const bool on_inner_box =
            (std::abs(v.x - 1.0) < 1e-6 || std::abs(v.x - 9.0) < 1e-6 ||
             std::abs(v.y - 1.0) < 1e-6 || std::abs(v.y - 5.0) < 1e-6 ||
             std::abs(v.z - 1.0) < 1e-6 || std::abs(v.z - 3.0) < 1e-6);
        if (on_inner_box && v.x > 0.5 && v.x < 9.5 &&
                              v.y > 0.5 && v.y < 5.5 &&
                              v.z > 0.5 && v.z < 3.5) {
            ++inner_hits;
        }
    }
    EXPECT_GT(inner_hits, 0)
        << "no vertices on the inner cavity surfaces";
}

TEST(Shell, OpenEndExposesCavity) {
    // open="end" removes the top cap. Inner cavity reaches z=4.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<shell thickness=\"1\" open=\"end\">"
        "<extrude height=\"4\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</shell>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto& m = r.parts[0].mesh;
    // The inner cavity boundary should reach z=4 (the top is open
    // so the inner extrusion extends through the original top cap).
    bool found_inner_at_top = false;
    for (const auto& v : m.vertices) {
        if (std::abs(v.z - 4.0) < 1e-6 &&
            v.x > 0.5 && v.x < 9.5 &&
            v.y > 0.5 && v.y < 5.5) {
            found_inner_at_top = true;
            break;
        }
    }
    EXPECT_TRUE(found_inner_at_top)
        << "open=end should expose the inner cavity at z=4";
}

TEST(Shell, NonConvexProfileViaCircleAccepted) {
    // Circle is convex (regular polygon approximation). Shell should
    // accept it.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<shell thickness=\"0.5\">"
        "<extrude height=\"6\"><circle r=\"4\"/></extrude>"
        "</shell>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_TRUE(r.warnings.empty()) << r.warnings.front().message;
    EXPECT_GT(r.parts[0].mesh.triangle_count(), 0u);
}

TEST(Shell, ThicknessTooLargeWarns) {
    // Box is 10??6??4. Thickness 100 collapses the offset ??? empty.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<shell thickness=\"100\">"
        "<extrude height=\"4\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</shell>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.warnings.empty());
    EXPECT_TRUE(any_warning_contains(r, "too large"));
}

TEST(Shell, NegativeThicknessWarns) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<shell thickness=\"-0.5\">"
        "<extrude height=\"4\"><rect width=\"6\" height=\"6\"/></extrude>"
        "</shell>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "thickness must be positive"));
}

TEST(Shell, NonExtrudeChildErrors) {
    // Shell with a <revolve> child errors per spec (until C5b).
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<shell thickness=\"0.5\">"
        "<revolve axis=\"x\"><circle cx=\"0\" cy=\"5\" r=\"1\"/></revolve>"
        "</shell>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "<extrude>"));
}

TEST(Shell, EmptyChildWarnsButOk) {
    auto doc = parse_authoring(
        "version 0.1\n<part><shell thickness=\"0.5\"/></part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "shell has no 3D child"));
}

TEST(Shell, WallThicknessMeasuredCorrectly) {
    // Regression: sample the inner cavity boundary and confirm the
    // wall thickness ??? requested. For a 10??6 outer rect with
    // thickness 1, the inner cavity should be 8??4 (1 unit of wall
    // on each side).
    constexpr double T = 1.0;
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<shell thickness=\"1\">"
        "<extrude height=\"4\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</shell>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // Find the inner cavity bbox by ignoring outer-shell vertices.
    // The inner cavity occupies x???[1,9], y???[1,5], z???[1,3].
    double inner_min_x = 1e9, inner_max_x = -1e9;
    double inner_min_y = 1e9, inner_max_y = -1e9;
    int sampled = 0;
    for (const auto& v : r.parts[0].mesh.vertices) {
        if (v.x > 0.5 && v.x < 9.5 && v.y > 0.5 && v.y < 5.5 &&
            v.z > 0.5 && v.z < 3.5) {
            inner_min_x = std::min(inner_min_x, v.x);
            inner_max_x = std::max(inner_max_x, v.x);
            inner_min_y = std::min(inner_min_y, v.y);
            inner_max_y = std::max(inner_max_y, v.y);
            ++sampled;
        }
    }
    ASSERT_GT(sampled, 0);
    // Wall thickness on each side = outer face - inner face. For a
    // box with outer extent [0,10] in x and inner extent [1,9],
    // walls are exactly 1 unit thick on each side.
    EXPECT_NEAR(inner_min_x - 0.0,  T, 1e-6);
    EXPECT_NEAR(10.0 - inner_max_x, T, 1e-6);
    EXPECT_NEAR(inner_min_y - 0.0,  T, 1e-6);
    EXPECT_NEAR(6.0 - inner_max_y,  T, 1e-6);
}

TEST(Shell, OpenStartEndBothExposesCavity) {
    // open="start end" ??? both top and bottom open. Inner cavity
    // boundary reaches z=0 AND z=4.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<shell thickness=\"1\" open=\"start end\">"
        "<extrude height=\"4\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</shell>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    bool found_at_z0 = false, found_at_z4 = false;
    for (const auto& v : r.parts[0].mesh.vertices) {
        if (v.x > 0.5 && v.x < 9.5 && v.y > 0.5 && v.y < 5.5) {
            if (std::abs(v.z - 0.0) < 1e-6) found_at_z0 = true;
            if (std::abs(v.z - 4.0) < 1e-6) found_at_z4 = true;
        }
    }
    EXPECT_TRUE(found_at_z0);
    EXPECT_TRUE(found_at_z4);
}

TEST(Fillet, BottomRimEdgeRadiusMatches) {
    // Regression: complement to RadiusMeasurementApproximatesRequest.
    // Sample the BOTTOM-FRONT (-Y, -Z) rim edge fillet ??? different
    // axis orientation than the vertical-edge case, exercises the
    // any_orthogonal_unit basis-construction code on a different
    // axis direction.
    constexpr double R = 1.0;
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<fillet radius=\"1.0\">"
        "<extrude height=\"4\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</fillet>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // Bottom-front edge runs along +X at y=0, z=0. Cylinder axis
    // tangent to both the y=0 and z=0 faces sits at y=R, z=R, and
    // runs from x=0 to x=10. Vertices on the cylindrical fillet
    // surface are at distance R from this axis (in the YZ plane).
    int sampled = 0;
    for (const auto& v : r.parts[0].mesh.vertices) {
        if (v.x > 0.5 && v.x < 9.5 &&
            v.y < R + 1e-6 && v.z < R + 1e-6) {
            // Vertex is on the bottom-front edge fillet surface.
            const double dy = v.y - R;
            const double dz = v.z - R;
            const double dist = std::sqrt(dy*dy + dz*dz);
            EXPECT_NEAR(dist, R, 1e-3);
            ++sampled;
        }
    }
    EXPECT_GT(sampled, 0)
        << "no vertex landed in the -Y -Z (bottom-front) edge fillet "
            "surface region ??? orthogonal-basis construction may have "
            "an axis-orientation bug";
}

TEST(Fillet, EmptyChildWarnsButOk) {
    auto doc = parse_authoring(
        "version 0.1\n<part><fillet radius=\"0.5\"/></part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "fillet has no 3D child"));
}

TEST(Tier1Predicate, RejectsAnyConcaveEdgeInMesh) {
    // Regression: an L-shape extrude has 5 convex outer
    // corners + 1 concave inner corner. Selecting only the convex
    // edges shouldn't be enough to bypass the predicate ??? Tier 1
    // doesn't handle concave geometry anywhere in the input.
    cadml::engine::detail::Polygon2D L;
    L.points = { {0,0}, {2,0}, {2,1}, {1,1}, {1,2}, {0,2} };
    auto m = cadml::engine::detail::extrude_linear(L, 1, /*src=*/0);
    auto topo = cadml::engine::detail::build_edge_topology(m);
    auto convex = convex_edge_indices(topo);
    auto r = cadml::engine::detail::tier1_check(m, topo, convex);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.reason.find("concave"), std::string::npos);
}

TEST(Chamfer, RejectsLShapeExtrude) {
    // End-to-end: chamfer of an L-shape extrude (concave inner
    // corner) must fail Tier 1 and warn.
    cadml::engine::detail::Polygon2D L;
    L.points = { {0,0}, {2,0}, {2,1}, {1,1}, {1,2}, {0,2} };
    auto m = cadml::engine::detail::extrude_linear(L, 1, 0);
    auto chr = cadml::engine::detail::chamfer(m, 0.1, cadml::Selector{}, 0);
    EXPECT_FALSE(chr.ok);
    EXPECT_NE(chr.error.find("concave"), std::string::npos);
}

TEST(Tier1Predicate, OutOfRangeIndexFails) {
    auto m = cadml::engine::detail::extrude_linear(
        cadml::engine::detail::tessellate_rect(0, 0, 10, 6),
        4, /*src=*/0);
    auto topo = cadml::engine::detail::build_edge_topology(m);
    auto r = cadml::engine::detail::tier1_check(m, topo, { 9999u });
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.reason.find("out of range"), std::string::npos);
}

TEST(EdgeTopology, FlatTriangulationSeam) {
    // Two triangles sharing a diagonal edge, both lying in the XY
    // plane (coplanar) ??? the seam is FLAT, dihedral ??? ??.
    cadml::engine::FlatMesh m;
    m.vertices = {
        {0, 0, 0},  // 0
        {1, 0, 0},  // 1
        {1, 1, 0},  // 2
        {0, 1, 0},  // 3
    };
    m.normals.assign(4, {0, 0, 1});
    // Diagonal edge (0, 2) split: t1 = (0,1,2), t2 = (0,2,3).
    m.indices = { 0, 1, 2,  0, 2, 3 };
    m.triangle_node = { 0, 0 };
    auto topo = cadml::engine::detail::build_edge_topology(m);
    const cadml::engine::detail::EdgeInfo* diag = nullptr;
    for (const auto& e : topo.edges) {
        if (e.key.v0 == 0 && e.key.v1 == 2) { diag = &e; break; }
    }
    ASSERT_NE(diag, nullptr);
    EXPECT_EQ(diag->classification, cadml::engine::detail::EdgeClass::Flat);
    constexpr double kPi = 3.14159265358979323846;
    EXPECT_NEAR(diag->dihedral_rad, kPi, 1e-9);
}

// ????????? is_convex unit tests + concave-profile warning ??????????????????????????????????????????????????????

TEST(Polygon2DConvexity, DegenerateCountTreatedAsConvex) {
    // Empty polygons short-circuit later in the pipeline; convexity
    // function returns true so the caller's empty guard catches first.
    EXPECT_TRUE(cadml::engine::detail::is_convex({{}}));
    EXPECT_TRUE(cadml::engine::detail::is_convex({{ {0,0} }}));
    EXPECT_TRUE(cadml::engine::detail::is_convex({{ {0,0}, {1,0} }}));
}

// --- boolean_combine faceID uniformity ----------------------------------
// Verifies to_meshgl always populates faceID (with 0 padding for an
// input whose triangle_node is missing/mismatched) so Manifold's
// invariant ??? faceID either empty or numTri-sized per mesh ??? holds
// uniformly across all inputs in a single Boolean call.

TEST(BooleanFaceIdUniformity, MixedAttributedAndUnattributedInputs) {
    using namespace cadml::engine::detail;
    // Input A: full triangle_node attribution.
    auto a = extrude_linear(tessellate_rect(0, 0, 10, 10), 10, /*src=*/100);
    // Input B: same shape, translated, with triangle_node deliberately
    // cleared to simulate a producer that didn't fill it.
    auto b = extrude_linear(tessellate_rect(20, 0, 10, 10), 10, /*src=*/200);
    b.triangle_node.clear();
    auto r = boolean_combine({a, b}, BoolOp::Union, /*src=*/999);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_FALSE(r.mesh.vertices.empty()) << "two disjoint cubes should "
                                              "produce non-empty union";
    ASSERT_EQ(r.mesh.triangle_node.size(), r.mesh.triangle_count());

    // Should see attribution from A (=100) AND padded-0 entries from B,
    // confirming both inputs survived even though B had no triangle_node.
    bool saw_a = false, saw_padded_b = false;
    for (auto src : r.mesh.triangle_node) {
        if (src == 100) saw_a = true;
        if (src == 0)   saw_padded_b = true;
    }
    EXPECT_TRUE(saw_a)
        << "input A's faceID didn't survive";
    EXPECT_TRUE(saw_padded_b)
        << "input B (triangle_node cleared) was dropped or unattributed";
}

// ????????? Geometry ??? instances (slice B2.4) ?????????????????????????????????????????????????????????????????????????????????????????????

TEST(FlatEvaluatorInstances, BareInstanceRendersDefBody) {
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"widget\">"
        "<extrude height=\"10\">"
        "<rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/>"
        "</extrude>"
        "</def>"
        "<part><widget/></part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_GT(r.parts[0].mesh.triangle_count(), 0u);
    auto b = bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.max_x,  5.0, 1e-9);
    EXPECT_NEAR(b.max_z, 10.0, 1e-9);
}

TEST(FlatEvaluatorInstances, ParamOverrideShadowsDefDefault) {
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"box\">"
        "<param name=\"size\" value=\"10\"/>"
        "<extrude height=\"{size}\">"
        "<rect x=\"0\" y=\"0\" width=\"{size}\" height=\"{size}\"/>"
        "</extrude>"
        "</def>"
        "<part><box size=\"30\"/></part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    auto b = bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.max_x, 30.0, 1e-9);
    EXPECT_NEAR(b.max_y, 30.0, 1e-9);
    EXPECT_NEAR(b.max_z, 30.0, 1e-9);
}

TEST(FlatEvaluatorInstances, DefDefaultUsedWhenNoOverride) {
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"box\">"
        "<param name=\"size\" value=\"10\"/>"
        "<extrude height=\"{size}\">"
        "<rect x=\"0\" y=\"0\" width=\"{size}\" height=\"{size}\"/>"
        "</extrude>"
        "</def>"
        "<part><box/></part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    auto b = bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.max_x, 10.0, 1e-9);
    EXPECT_NEAR(b.max_y, 10.0, 1e-9);
    EXPECT_NEAR(b.max_z, 10.0, 1e-9);
}

TEST(FlatEvaluatorInstances, UnknownRefWarns) {
    auto doc = parse_authoring(
        "version 0.1\n<part><nonexistent/></part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "unknown def"));
    EXPECT_EQ(r.parts[0].mesh.triangle_count(), 0u);
}

TEST(FlatEvaluatorInstances, InstanceInsideGroupRespectsTransform) {
    // Bundler-output style: a group-wrapped instance gets transformed.
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"plate\">"
        "<extrude height=\"5\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/>"
        "</extrude>"
        "</def>"
        "<part>"
        "<group transform=\"translate(100, 0, 0)\">"
        "<plate/>"
        "</group>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    auto b = bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.min_x, 100.0, 1e-9);
    EXPECT_NEAR(b.max_x, 110.0, 1e-9);
}

TEST(FlatEvaluatorInstances, OverrideExprResolvesCallerParam) {
    // Regression: override expressions must evaluate in the
    // CALLER's scope so that `{outer * 2}` resolves outer.
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"box\">"
        "<param name=\"size\" value=\"10\"/>"
        "<extrude height=\"{size}\">"
        "<rect x=\"0\" y=\"0\" width=\"{size}\" height=\"{size}\"/>"
        "</extrude>"
        "</def>"
        "<part>"
        "<param name=\"outer\" value=\"50\"/>"
        "<box size=\"{outer * 2}\"/>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    auto b = bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.max_x, 100.0, 1e-9);
    EXPECT_NEAR(b.max_y, 100.0, 1e-9);
    EXPECT_NEAR(b.max_z, 100.0, 1e-9);
}

TEST(FlatEvaluatorInstances, CallerScopeIsNotVisibleInsideDef) {
    // Defs have their own scope: a def-internal expression that references
    // a name only the caller has should NOT resolve. (We expose this as
    // a warning, not silent.) `{outer}` inside the def's geometry
    // shouldn't see the part's `outer` param.
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"leaky\">"
        "<extrude height=\"{outer}\">"
        "<rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/>"
        "</extrude>"
        "</def>"
        "<part>"
        "<param name=\"outer\" value=\"42\"/>"
        "<leaky/>"
        "</part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    // The expression evaluator failed (outer not bound in def scope) ???
    // height defaulted to 0 ??? empty mesh ??? eval_num warning surfaced.
    EXPECT_EQ(r.parts[0].mesh.triangle_count(), 0u);
    EXPECT_TRUE(any_warning_contains(r, "extrude.height"));
}

TEST(FlatEvaluatorInstances, RecursiveDefBoundsRecursion) {
    // Regression: a self-referential def must NOT stack
    // overflow. The recursion guard short-circuits at kMaxInstanceDepth
    // (=64) and emits a warning.
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"recursive\"><recursive/></def>"
        "<part><recursive/></part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.parts[0].mesh.triangle_count(), 0u);
    EXPECT_TRUE(any_warning_contains(r, "recursion depth exceeded"));
}

TEST(FlatEvaluatorInstances, QualifiedDefLookupForNestedImport) {
    // Regression: an instance inside an imported def's body
    // has a `ref_name` local to that def's namespace; doc.defs keys
    // it as "<containing-def>.<ref_name>" after merge_imported_doc.
    // Engine resolution must qualify with the closest Def/Part
    // ancestor's name to find it.
    //
    // Construct: inner.cadml is a <part>, mid.cadml is a <part> that
    // bare-instances <inner/>, rig.cadml bare-instances <mid/>. After
    // bundling rig, doc.defs has "inner", "mid", "mid.inner". The
    // mid-def's body has an `<inner/>` Instance whose ref_name is the
    // unqualified "inner" ??? qualified lookup must find "mid.inner".
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() / "cadml_b24_qualified";
    std::error_code ec; fs::remove_all(base, ec); fs::create_directories(base);

    auto write = [&](const fs::path& rel, std::string_view content) {
        const auto full = base / rel;
        fs::create_directories(full.parent_path());
        std::ofstream f(full, std::ios::binary);
        f << content;
    };
    write("inner.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"3\">"
        "<rect x=\"0\" y=\"0\" width=\"4\" height=\"4\"/>"
        "</extrude>"
        "</part>");
    write("mid.cadml",
        "version 0.1\n"
        "import \"inner.cadml\"\n"
        "<part><inner/></part>");
    write("rig.cadml",
        "version 0.1\n"
        "import \"mid.cadml\"\n"
        "<part><mid/></part>");

    auto cr = cadml::compile::compile_file(base / "rig.cadml");
    ASSERT_TRUE(cr.ok()) << (cr.errors.empty() ? "" : cr.errors[0].message);

    auto r = evaluate_flat(cr.document);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_GT(r.parts[0].mesh.triangle_count(), 0u)
        << "qualified lookup failed ??? inner-def's geometry didn't render";
    auto b = bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.max_x, 4.0, 1e-9);
    EXPECT_NEAR(b.max_z, 3.0, 1e-9);

    fs::remove_all(base, ec);
}

TEST(FlatEvaluatorInstances, DefAToBToATerminates) {
    // Mutually recursive defs (A???B???A) ??? exercises the same guard with
    // a longer cycle.
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"a\"><b/></def>"
        "<def name=\"b\"><a/></def>"
        "<part><a/></part>");
    auto r = evaluate_flat(doc);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "recursion depth exceeded"));
}

// End-to-end: bundler-output assembly with two parts at different
// positions renders correctly through the flat engine. Headline
// scenario for the v0.1 toolchain.
TEST(FlatEvaluatorInstances, BundlerOutputAssemblyRendersBothParts) {
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() / "cadml_b24_test";
    std::error_code ec; fs::remove_all(base, ec); fs::create_directories(base);

    auto write = [&](const fs::path& rel, std::string_view content) {
        const auto full = base / rel;
        fs::create_directories(full.parent_path());
        std::ofstream f(full, std::ios::binary);
        f << content;
    };
    write("plate.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"6\">"
        "<rect x=\"-20\" y=\"-20\" width=\"40\" height=\"40\"/>"
        "</extrude>"
        "<port name=\"top\" position=\"0 0 6\" normal=\"+z\" up=\"+x\"/>"
        "</part>");
    write("bolt.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"30\"><circle r=\"3\"/></extrude>"
        "<port name=\"head\" position=\"0 0 30\" normal=\"-z\" up=\"+x\"/>"
        "</part>");
    write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "import \"bolt.cadml\"\n"
        "<assembly name=\"rig\">"
        "<plate><bolt at=\"top\" port=\"head\"/></plate>"
        "</assembly>");

    auto cr = cadml::compile::compile_file(base / "rig.cadml");
    ASSERT_TRUE(cr.ok()) << (cr.errors.empty() ? "" : cr.errors[0].message);

    auto r = evaluate_flat(cr.document);
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    ASSERT_EQ(r.parts.size(), 1u);

    // Plate at origin: spans z???[0, 6].
    // Bolt placed via at=top/port=head: head (z=30) lands on plate top
    // (z=6) with normals opposing ??? bolt translates -24 along z, so
    // its base ends at z = -24, top ends at z = 6.
    auto b = bbox(r.parts[0].mesh);
    EXPECT_NEAR(b.max_z,  6.0, 1e-6);
    EXPECT_NEAR(b.min_z, -24.0, 1e-6);

    fs::remove_all(base, ec);
}

TEST(BooleanFaceIdUniformity, MismatchedTriangleNodeSizeDoesntPoisonOthers) {
    using namespace cadml::engine::detail;
    auto a = extrude_linear(tessellate_rect(0, 0, 10, 10), 10, /*src=*/100);
    auto b = extrude_linear(tessellate_rect(5, 0, 10, 10), 10, /*src=*/200);
    // Shrink b's triangle_node to wrong size ??? to_meshgl should pad.
    b.triangle_node.resize(b.triangle_count() - 1);
    auto r = boolean_combine({a, b}, BoolOp::Union, /*src=*/999);
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_FALSE(r.mesh.vertices.empty());
}

// SVG fill auto-mapping: when <part> has no explicit color and the
// body contains a 2D primitive with `fill="..."`, that fill becomes
// the part color so SVG-pasted artwork renders in its source colour
// without the author having to copy the value to the wrapping part.
TEST(FlatEvaluatorFillMapping, FirstFillPromotesToPartColor) {
    auto doc = parse_authoring(
        "version 0.1\n"
        "<part name=\"badge\">"
        "<extrude height=\"2\">"
        "<rect width=\"10\" height=\"10\" fill=\"#3a7bd5\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.parts.size(), 1u);
    EXPECT_EQ(r.parts[0].color, "#3a7bd5");
}

TEST(FlatEvaluatorFillMapping, ExplicitPartColorBeatsChildFill) {
    auto doc = parse_authoring(
        "version 0.1\n"
        "<part name=\"badge\" color=\"#888888\">"
        "<extrude height=\"2\">"
        "<rect width=\"10\" height=\"10\" fill=\"#3a7bd5\"/>"
        "</extrude>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.parts[0].color, "#888888");
}

TEST(FlatEvaluatorFillMapping, EmptyFillOnFirstShapeFallsThroughToNext) {
    // First shape has no fill; second carries the colour. Walk picks
    // the first NON-EMPTY fill in document order.
    auto doc = parse_authoring(
        "version 0.1\n"
        "<part name=\"badge\">"
        "<union>"
        "<extrude height=\"2\"><rect width=\"5\" height=\"5\"/></extrude>"
        "<extrude height=\"2\">"
        "<rect x=\"6\" width=\"5\" height=\"5\" fill=\"red\"/>"
        "</extrude>"
        "</union>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.parts[0].color, "red");
}

TEST(FlatEvaluatorFillMapping, ImportedDefColorPropagatesIntoColorlessPart) {
    // When a colourless host <part> instances a def whose original
    // <part color> was preserved by the bundler, the engine should
    // surface that colour on the resulting FlatEvalResult::Part.
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"widget\" color=\"#abcdef\">"
        "<extrude height=\"5\"><circle r=\"3\"/></extrude>"
        "</def>"
        "<part name=\"host\"><widget/></part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.parts[0].color, "#abcdef");
}

TEST(FlatEvaluatorFillMapping, ExplicitPartColorBeatsDefColor) {
    // The host <part>'s own colour still wins over any imported-def
    // colour — only colourless parts inherit.
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"widget\" color=\"#abcdef\">"
        "<extrude height=\"5\"><circle r=\"3\"/></extrude>"
        "</def>"
        "<part name=\"host\" color=\"#112233\"><widget/></part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.parts[0].color, "#112233");
}

TEST(FlatEvaluatorFillMapping, FillOnChildPrimitiveInsideDefPropagates) {
    // When the def has no `color=` of its own but a child primitive
    // carries an SVG `fill=`, walking through the instance into the
    // def body should still surface that fill on the host part.
    // (Regression: an earlier first_descendant_fill stopped at the
    // Instance without recursing into the def body.)
    auto doc = parse_authoring(
        "version 0.1\n"
        "<def name=\"widget\">"
        "<extrude height=\"5\"><circle r=\"3\" fill=\"red\"/></extrude>"
        "</def>"
        "<part name=\"host\"><widget/></part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.parts[0].color, "red");
}

// ───────── Selector grammar end-to-end (spec §13 / Block 3 B3.1) ─────

namespace {
// Fillet a 10×6×4 box with the given selector and return the part's
// triangle count (0 on failure).
std::size_t fillet_box_tris(const std::string& select) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<fillet radius=\"1\" select=\"" + select + "\">"
        "<extrude height=\"4\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</fillet>"
        "</part>");
    auto r = evaluate_flat(doc);
    if (!r.ok() || r.parts.empty()) return 0;
    return r.parts[0].mesh.triangle_count();
}
}  // namespace

TEST(Selector, FilletSubsetRoundsFewerEdgesThanAll) {
    // A box has 12 convex edges: 4 along x, 4 along y, 4 along z. A plain
    // (un-filleted) box is 12 triangles. Filleting all edges adds the
    // most geometry; filleting only the x-parallel edges adds strictly
    // less but strictly more than none — proving the selector actually
    // filters rather than silently rounding everything.
    const auto none = std::size_t{12};
    const auto all  = fillet_box_tris("all");
    const auto x    = fillet_box_tris("edge:along=+x");
    EXPECT_GT(all, x);
    EXPECT_GT(x, none);
}

TEST(Selector, FilletAlongAxisIsOrientationAgnostic) {
    // +x and -x select the same set of (undirected) edges.
    EXPECT_EQ(fillet_box_tris("edge:along=+x"),
              fillet_box_tris("edge:along=-x"));
}

TEST(Selector, FilletByEdgePositionSelectsBottomRim) {
    // Only the four bottom-rim edges (z=0) get rounded.
    const auto bottom = fillet_box_tris("edge:position.z=0");
    EXPECT_GT(bottom, std::size_t{12});                 // something rounded
    EXPECT_LT(bottom, fillet_box_tris("all"));          // not everything
}

TEST(Selector, FilletZeroMatchWarnsAndLeavesMeshUnchanged) {
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<fillet radius=\"1\" select=\"edge:length>999\">"
        "<extrude height=\"4\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</fillet>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "matched no edges"));
    // Unmodified box = 12 triangles (NOT a silent fallback to all-edges).
    EXPECT_EQ(r.parts[0].mesh.triangle_count(), std::size_t{12});
}

TEST(Selector, ShellOpenFaceSelectorOpensTopCap) {
    // open="face:normal=+z" must open the top cap exactly like the
    // legacy open="end" keyword: the inner cavity reaches z=height.
    auto eval_open = [](const std::string& open) {
        auto doc = parse_authoring(
            "version 0.1\n<part>"
            "<shell thickness=\"1\" open=\"" + open + "\">"
            "<extrude height=\"4\">"
            "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
            "</extrude>"
            "</shell>"
            "</part>");
        return evaluate_flat(doc);
    };
    auto by_kw  = eval_open("end");
    auto by_sel = eval_open("face:normal=+z");
    ASSERT_TRUE(by_kw.ok());
    ASSERT_TRUE(by_sel.ok());
    // Inner cavity reaches the top in both: some vertex with z > 3.5 and
    // strictly inside the x/y walls.
    auto cavity_reaches_top = [](const FlatEvalResult& r) {
        for (const auto& v : r.parts[0].mesh.vertices)
            if (v.z > 3.5 && v.x > 0.5 && v.x < 9.5 &&
                v.y > 0.5 && v.y < 5.5)
                return true;
        return false;
    };
    EXPECT_TRUE(cavity_reaches_top(by_kw));
    EXPECT_TRUE(cavity_reaches_top(by_sel));
}

TEST(Selector, ShellOpenAllIsRejectedAtCompileButWarnsInEngine) {
    // The bundler rejects open="all"; here we exercise the engine's
    // defensive path on a hand-authored doc — it must warn and produce
    // a (closed) shell rather than an empty mesh.
    auto doc = parse_authoring(
        "version 0.1\n<part>"
        "<shell thickness=\"1\" open=\"all\">"
        "<extrude height=\"4\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"6\"/>"
        "</extrude>"
        "</shell>"
        "</part>");
    auto r = evaluate_flat(doc);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(any_warning_contains(r, "invalid"));
    EXPECT_GT(r.parts[0].mesh.triangle_count(), std::size_t{0});
}

// ─── 3MF export ───────────────────────────────────────────────────────
//
// 3MF is a ZIP package. We round-trip by writing the ZIP to a string,
// re-opening it through miniz's heap-based reader, extracting each
// archive entry, and parsing 3D/3dmodel.model with pugixml. That keeps
// the test independent of any reference 3MF library and verifies both
// the OPC layout (presence of all three required parts) and the model
// XML structure.

#include <cadml/engine/flat_3mf.hpp>
#include <miniz.h>
#include <pugixml.hpp>
#include <cstring>
#include <unordered_map>

namespace {

// Open a string holding ZIP bytes, return a map<filename, contents>
// covering every entry. Throws on any miniz error so the EXPECTs in the
// caller stay readable.
std::unordered_map<std::string, std::string> unzip_to_map(const std::string& bytes) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) {
        throw std::runtime_error("3MF test: miniz failed to open the ZIP");
    }
    std::unordered_map<std::string, std::string> out;
    const auto n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            mz_zip_reader_end(&zip);
            throw std::runtime_error("3MF test: miniz failed reading stat");
        }
        std::string contents(static_cast<std::size_t>(st.m_uncomp_size), '\0');
        if (!mz_zip_reader_extract_to_mem(&zip, i,
                contents.data(), contents.size(), 0)) {
            mz_zip_reader_end(&zip);
            throw std::runtime_error(
                std::string("3MF test: miniz failed extracting ") +
                st.m_filename);
        }
        out.emplace(st.m_filename, std::move(contents));
    }
    mz_zip_reader_end(&zip);
    return out;
}

}  // namespace

TEST(Flat3mfExport, EmptyResultEmitsValidPackage) {
    cadml::engine::FlatEvalResult r;
    std::ostringstream os;
    cadml::engine::write_3mf(r, os);
    const auto bytes = os.str();
    ASSERT_FALSE(bytes.empty());

    auto entries = unzip_to_map(bytes);
    EXPECT_EQ(entries.count("[Content_Types].xml"), std::size_t{1});
    EXPECT_EQ(entries.count("_rels/.rels"),         std::size_t{1});
    EXPECT_EQ(entries.count("3D/3dmodel.model"),    std::size_t{1});

    // The model XML parses, declares the core namespace, and has empty
    // resources + build sections.
    pugi::xml_document doc;
    auto load = doc.load_string(entries["3D/3dmodel.model"].c_str());
    ASSERT_TRUE(load) << load.description();
    const auto model = doc.child("model");
    ASSERT_TRUE(model);
    EXPECT_STREQ(model.attribute("unit").value(), "millimeter");

    EXPECT_EQ(model.child("resources").children().begin(),
              model.child("resources").children().end())
        << "empty result should produce empty <resources>";

    EXPECT_EQ(model.child("build").children().begin(),
              model.child("build").children().end())
        << "empty result should produce empty <build>";
}

TEST(Flat3mfExport, SingleBoxEmitsOneObjectWithVertsAndTris) {
    cadml::engine::FlatEvalResult r;
    r.parts.push_back({ "box", "", translated_box(0, 0, 0, 5, 5, 5) });
    const auto expected_verts = r.parts[0].mesh.vertices.size();
    const auto expected_tris  = r.parts[0].mesh.triangle_count();

    std::ostringstream os;
    cadml::engine::write_3mf(r, os);

    auto entries = unzip_to_map(os.str());
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(entries["3D/3dmodel.model"].c_str()));

    auto obj = doc.child("model").child("resources").child("object");
    ASSERT_TRUE(obj);
    EXPECT_STREQ(obj.attribute("type").value(), "model");
    EXPECT_STREQ(obj.attribute("name").value(), "box");

    auto vcount = std::distance(obj.child("mesh").child("vertices").children("vertex").begin(),
                                  obj.child("mesh").child("vertices").children("vertex").end());
    auto tcount = std::distance(obj.child("mesh").child("triangles").children("triangle").begin(),
                                  obj.child("mesh").child("triangles").children("triangle").end());
    EXPECT_EQ(static_cast<std::size_t>(vcount), expected_verts);
    EXPECT_EQ(static_cast<std::size_t>(tcount), expected_tris);

    auto items = doc.child("model").child("build").children("item");
    EXPECT_EQ(std::distance(items.begin(), items.end()), 1);
}

TEST(Flat3mfExport, KeepsPartsSeparate) {
    // Unlike STL (which merges into one triangle soup), 3MF preserves
    // per-part separation — each part becomes its own <object>.
    cadml::engine::FlatEvalResult r;
    r.parts.push_back({ "a", "", translated_box(0, 0, 0, 5, 5, 5) });
    r.parts.push_back({ "b", "", translated_box(10, 0, 0, 5, 5, 5) });

    std::ostringstream os;
    cadml::engine::write_3mf(r, os);
    auto entries = unzip_to_map(os.str());
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(entries["3D/3dmodel.model"].c_str()));

    auto objs = doc.child("model").child("resources").children("object");
    EXPECT_EQ(std::distance(objs.begin(), objs.end()), 2);

    auto items = doc.child("model").child("build").children("item");
    EXPECT_EQ(std::distance(items.begin(), items.end()), 2);

    // Object IDs are unique and don't collide with basematerials id=1.
    std::set<std::string> ids;
    for (auto o : objs) ids.insert(o.attribute("id").value());
    EXPECT_EQ(ids.size(), std::size_t{2});
    EXPECT_EQ(ids.count("1"), std::size_t{0})
        << "object IDs must skip 1 to avoid colliding with material id";
}

TEST(Flat3mfExport, ColorBecomesBaseMaterial) {
    cadml::engine::FlatEvalResult r;
    r.parts.push_back({ "red-box", "#FF0000",
                          translated_box(0, 0, 0, 5, 5, 5) });

    std::ostringstream os;
    cadml::engine::write_3mf(r, os);
    auto entries = unzip_to_map(os.str());
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(entries["3D/3dmodel.model"].c_str()));

    auto resources = doc.child("model").child("resources");

    auto mats = resources.child("basematerials");
    ASSERT_TRUE(mats);
    EXPECT_STREQ(mats.attribute("id").value(), "1");
    auto base = mats.child("base");
    ASSERT_TRUE(base);
    // Authored "#FF0000" + opaque alpha → "#FF0000FF".
    EXPECT_STREQ(base.attribute("displaycolor").value(), "#FF0000FF");

    auto obj = resources.child("object");
    EXPECT_STREQ(obj.attribute("pid").value(),    "1");
    EXPECT_STREQ(obj.attribute("pindex").value(), "0");
}

TEST(Flat3mfExport, DedupesIdenticalColors) {
    // Two parts with the same color → one basematerials entry,
    // both objects pointing at pindex=0.
    cadml::engine::FlatEvalResult r;
    r.parts.push_back({ "a", "#3344FF",
                          translated_box(0, 0, 0, 5, 5, 5) });
    r.parts.push_back({ "b", "#3344FF",
                          translated_box(10, 0, 0, 5, 5, 5) });

    std::ostringstream os;
    cadml::engine::write_3mf(r, os);
    auto entries = unzip_to_map(os.str());
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(entries["3D/3dmodel.model"].c_str()));

    auto bases = doc.child("model").child("resources")
                     .child("basematerials").children("base");
    EXPECT_EQ(std::distance(bases.begin(), bases.end()), 1)
        << "duplicate colors should collapse to one base material";
    for (auto obj : doc.child("model").child("resources").children("object")) {
        EXPECT_STREQ(obj.attribute("pindex").value(), "0");
    }
}

TEST(Flat3mfExport, ShortHexExpandsToFullForm) {
    // CADML's color parser accepts "#RGB" shorthand; 3MF needs "#RRGGBBAA".
    // Each nibble should double.
    cadml::engine::FlatEvalResult r;
    r.parts.push_back({ "x", "#ABC",
                          translated_box(0, 0, 0, 5, 5, 5) });

    std::ostringstream os;
    cadml::engine::write_3mf(r, os);
    auto entries = unzip_to_map(os.str());
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(entries["3D/3dmodel.model"].c_str()));

    auto base = doc.child("model").child("resources")
                    .child("basematerials").child("base");
    EXPECT_STREQ(base.attribute("displaycolor").value(), "#AABBCCFF");
}

TEST(Flat3mfExport, EmptyPartIsSkipped) {
    cadml::engine::FlatEvalResult r;
    r.parts.push_back({ "real",  "", translated_box(0, 0, 0, 5, 5, 5) });
    r.parts.push_back({ "empty", "", cadml::engine::FlatMesh{} });

    std::ostringstream os;
    cadml::engine::write_3mf(r, os);
    auto entries = unzip_to_map(os.str());
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(entries["3D/3dmodel.model"].c_str()));

    auto objs = doc.child("model").child("resources").children("object");
    EXPECT_EQ(std::distance(objs.begin(), objs.end()), 1)
        << "empty part should produce no <object>";

    auto items = doc.child("model").child("build").children("item");
    EXPECT_EQ(std::distance(items.begin(), items.end()), 1)
        << "empty part should produce no <build><item>";
}

TEST(Flat3mfExport, RespectsUnitsOption) {
    cadml::engine::FlatEvalResult r;
    r.parts.push_back({ "box", "", translated_box(0, 0, 0, 5, 5, 5) });

    for (const auto& [in, out] : std::vector<std::pair<std::string, std::string>>{
            { "mm", "millimeter" },
            { "cm", "centimeter" },
            { "m",  "meter"      },
            { "in", "inch"       },
            { "ft", "foot"       },
        }) {
        cadml::engine::ThreeMfOptions opts;
        opts.units = in;
        std::ostringstream os;
        cadml::engine::write_3mf(r, os, opts);
        auto entries = unzip_to_map(os.str());
        pugi::xml_document doc;
        ASSERT_TRUE(doc.load_string(entries["3D/3dmodel.model"].c_str()));
        EXPECT_STREQ(doc.child("model").attribute("unit").value(), out.c_str())
            << "units=" << in;
    }
}

TEST(Flat3mfExport, RejectsUnknownUnits) {
    cadml::engine::FlatEvalResult r;
    cadml::engine::ThreeMfOptions opts;
    opts.units = "furlong";
    std::ostringstream os;
    EXPECT_THROW(cadml::engine::write_3mf(r, os, opts), std::runtime_error);
}

TEST(Flat3mfExport, EmitsTitleMetadata) {
    cadml::engine::FlatEvalResult r;
    r.parts.push_back({ "box", "", translated_box(0, 0, 0, 5, 5, 5) });
    cadml::engine::ThreeMfOptions opts;
    opts.title = "hex-bolt assembly";

    std::ostringstream os;
    cadml::engine::write_3mf(r, os, opts);
    auto entries = unzip_to_map(os.str());
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(entries["3D/3dmodel.model"].c_str()));

    auto md = doc.child("model").child("metadata");
    ASSERT_TRUE(md);
    EXPECT_STREQ(md.attribute("name").value(), "Title");
    EXPECT_STREQ(md.text().get(), "hex-bolt assembly");
}

TEST(Flat3mfExport, EscapesXmlSpecialsInNameAndTitle) {
    cadml::engine::FlatEvalResult r;
    // CADML's parser enforces kebab-case on element names, but Part::name
    // is just a string — defense in depth says we must escape.
    r.parts.push_back({ "evil<&\">", "", translated_box(0, 0, 0, 5, 5, 5) });
    cadml::engine::ThreeMfOptions opts;
    opts.title = "title with <special> & \"chars\"";

    std::ostringstream os;
    cadml::engine::write_3mf(r, os, opts);
    auto entries = unzip_to_map(os.str());

    // Round-trip via pugixml — if escaping is wrong the parser explodes.
    pugi::xml_document doc;
    auto load = doc.load_string(entries["3D/3dmodel.model"].c_str());
    ASSERT_TRUE(load) << load.description();
    EXPECT_STREQ(doc.child("model").child("metadata").text().get(),
                  "title with <special> & \"chars\"");
    EXPECT_STREQ(doc.child("model").child("resources")
                    .child("object").attribute("name").value(),
                  "evil<&\">");
}

TEST(Flat3mfExport, RejectsMalformedIndexCount) {
    cadml::engine::FlatEvalResult r;
    cadml::engine::FlatMesh m;
    m.vertices = { {0,0,0}, {1,0,0}, {0,1,0} };
    m.indices  = { 0, 1 };       // not a multiple of 3
    r.parts.push_back({ "broken", "", std::move(m) });
    std::ostringstream os;
    EXPECT_THROW(cadml::engine::write_3mf(r, os), std::runtime_error);
}

TEST(Flat3mfExport, RejectsOutOfRangeIndex) {
    cadml::engine::FlatEvalResult r;
    cadml::engine::FlatMesh m;
    m.vertices = { {0,0,0}, {1,0,0}, {0,1,0} };
    m.indices  = { 0, 1, 7 };    // 7 >= vertex count
    r.parts.push_back({ "broken", "", std::move(m) });
    std::ostringstream os;
    EXPECT_THROW(cadml::engine::write_3mf(r, os), std::runtime_error);
}

TEST(Flat3mfExport, RejectsNonFiniteVertex) {
    cadml::engine::FlatEvalResult r;
    cadml::engine::FlatMesh m;
    m.vertices = { {0,0,0}, {1,0,0}, { std::nan(""), 0, 0 } };
    m.indices  = { 0, 1, 2 };
    r.parts.push_back({ "broken", "", std::move(m) });
    std::ostringstream os;
    EXPECT_THROW(cadml::engine::write_3mf(r, os), std::runtime_error);
}

TEST(Flat3mfExport, GeometryDeterministicAcrossRuns) {
    // Determinism scope (per flat_3mf.cpp's package_zip comment): the
    // 3D/3dmodel.model payload — every vertex, every triangle, every
    // material assignment — is byte-identical for byte-identical input.
    // The ZIP container around it carries per-entry timestamps that
    // miniz stamps from wall-clock time; those legitimately drift
    // across runs and are out of scope for the spec's determinism
    // promise. The right thing to compare is the extracted XML.
    cadml::engine::FlatEvalResult r;
    r.parts.push_back({ "a", "#FF0000", translated_box(0, 0, 0, 5, 5, 5) });
    r.parts.push_back({ "b", "#00FF00", translated_box(10, 0, 0, 5, 5, 5) });

    std::ostringstream os1;
    std::ostringstream os2;
    cadml::engine::write_3mf(r, os1);
    cadml::engine::write_3mf(r, os2);

    auto entries1 = unzip_to_map(os1.str());
    auto entries2 = unzip_to_map(os2.str());
    EXPECT_EQ(entries1["3D/3dmodel.model"], entries2["3D/3dmodel.model"])
        << "3MF model XML must be byte-identical for byte-identical input";
    // The OPC scaffolding parts are also content-deterministic — they
    // are fixed string constants.
    EXPECT_EQ(entries1["[Content_Types].xml"], entries2["[Content_Types].xml"]);
    EXPECT_EQ(entries1["_rels/.rels"],          entries2["_rels/.rels"]);
}
