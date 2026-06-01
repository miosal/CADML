// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/engine/flat_analysis.hpp>
#include <cadml/engine/flat_evaluator.hpp>

#include <cadml/compile/bundler.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using cadml::compile::compile_string;
using cadml::engine::evaluate_flat;
using cadml::engine::flat_bounds;
using cadml::engine::flat_diff;
using cadml::engine::flat_mass_properties;
using cadml::engine::flat_measure;
using cadml::engine::flat_topology;
using cadml::engine::FlatEvalResult;
using cadml::engine::MeasureKind;
using cadml::engine::MeasureProbe;

namespace {

FlatEvalResult eval(std::string_view src) {
    auto cr = compile_string(src);
    if (!cr.ok()) {
        FlatEvalResult er;
        for (const auto& e : cr.errors)
            er.errors.push_back({ e.message, cadml::SourceRange{} });
        return er;
    }
    return evaluate_flat(cr.document);
}

constexpr double kPi = 3.14159265358979323846;

}  // namespace

// ── Mass properties ──────────────────────────────────────────────────

TEST(FlatMass, CubeVolumeMatchesAnalytic) {
    // 10 mm cube centred at the origin, extruded in +Z.
    auto er = eval(
        "version 0.1\n<part name=\"cube\">"
        "<extrude height=\"10\"><rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/></extrude>"
        "</part>");
    ASSERT_TRUE(er.ok()) << (er.errors.empty() ? "" : er.errors[0].message);
    ASSERT_EQ(er.parts.size(), 1u);

    auto mp = flat_mass_properties(er.parts[0].mesh);
    EXPECT_NEAR(mp.volume_mm3, 1000.0, 1e-6);
    EXPECT_NEAR(mp.surface_area_mm2, 600.0, 1e-6);
    // Centre of mass at (5, 5, 5).
    EXPECT_NEAR(mp.center_of_mass[0], 5.0, 1e-6);
    EXPECT_NEAR(mp.center_of_mass[1], 5.0, 1e-6);
    EXPECT_NEAR(mp.center_of_mass[2], 5.0, 1e-6);
    EXPECT_TRUE(mp.is_watertight);
}

TEST(FlatMass, CubeInertiaMatchesAnalytic) {
    // 10 mm cube at origin (corner at 0,0,0). Inertia tensor about the
    // origin (NOT the centre) for a uniform-density cube of side a is:
    //   Ixx = (2/3)*M*a^2 - M*a*(yc + zc)
    // Easier: derive each diagonal directly from the volume integrals.
    //   integral (y^2+z^2) dV over [0,a]^3 = a^3 * (a^2/3 + a^2/3) = (2/3)a^5
    // For a=10, density=1: I = (2/3) * 100000 = 66666.667.
    auto er = eval(
        "version 0.1\n<part name=\"cube\">"
        "<extrude height=\"10\"><rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/></extrude>"
        "</part>");
    ASSERT_TRUE(er.ok());
    auto mp = flat_mass_properties(er.parts[0].mesh);
    EXPECT_NEAR(mp.inertia_origin[0], 66666.6667, 1e-2);  // Ixx
    EXPECT_NEAR(mp.inertia_origin[4], 66666.6667, 1e-2);  // Iyy
    EXPECT_NEAR(mp.inertia_origin[8], 66666.6667, 1e-2);  // Izz
    // Off-diagonals: Ixy = -integral(xy dV) = -(a^2/2)*(a^2/2)*a = -25000.
    EXPECT_NEAR(mp.inertia_origin[1], -25000.0, 1e-2);
    EXPECT_NEAR(mp.inertia_origin[2], -25000.0, 1e-2);
    EXPECT_NEAR(mp.inertia_origin[5], -25000.0, 1e-2);
}

TEST(FlatMass, DensityYieldsMass) {
    // 10x10x10 cube, density 1000 kg/m^3 (water): mass = 0.001 kg = 1 g.
    auto er = eval(
        "version 0.1\n<part>"
        "<extrude height=\"10\"><rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/></extrude>"
        "</part>");
    auto mp = flat_mass_properties(er.parts[0].mesh, 1000.0);
    EXPECT_NEAR(mp.mass_kg, 0.001, 1e-9);
}

TEST(FlatMass, InertiaComMatchesParallelAxisShift) {
    // Same 10 mm cube at the origin (corner at 0,0,0). With density
    // 1000 kg/m^3 the mass is 0.001 kg and the centre of mass is at
    // (5, 5, 5) mm. For a uniform-density cube of side a the COM
    // inertia diagonal is M·(a² + a²)/12:
    //     Ixx_com = 0.001 · (100 + 100) / 12 ≈ 1.667e-5 kg·mm².
    // The parallel-axis shift between the two tensors is M·(cy² + cz²)
    // along each diagonal — verify the relationship explicitly so a
    // regression in either branch shows up.
    auto er = eval(
        "version 0.1\n<part>"
        "<extrude height=\"10\"><rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/></extrude>"
        "</part>");
    auto mp = flat_mass_properties(er.parts[0].mesh, 1000.0);
    ASSERT_NEAR(mp.mass_kg, 0.001, 1e-9);

    const double Ixx_com_expected = mp.mass_kg * (10.0 * 10.0 + 10.0 * 10.0) / 12.0;
    EXPECT_NEAR(mp.inertia_com[0], Ixx_com_expected, 1e-6);
    EXPECT_NEAR(mp.inertia_com[4], Ixx_com_expected, 1e-6);
    EXPECT_NEAR(mp.inertia_com[8], Ixx_com_expected, 1e-6);
    // Off-diagonals at the COM should be zero (cube is symmetric).
    EXPECT_NEAR(mp.inertia_com[1], 0.0, 1e-6);
    EXPECT_NEAR(mp.inertia_com[2], 0.0, 1e-6);
    EXPECT_NEAR(mp.inertia_com[5], 0.0, 1e-6);

    // Parallel-axis identity: I_origin[xx] - I_com[xx] == M * (cy² + cz²).
    const double cy = mp.center_of_mass[1];
    const double cz = mp.center_of_mass[2];
    EXPECT_NEAR(mp.inertia_origin[0] - mp.inertia_com[0],
                mp.mass_kg * (cy * cy + cz * cz), 1e-6);
}

TEST(FlatMass, InertiaWithoutDensityIsGeometricMm5) {
    // Same 10 mm cube, no density. The inertia integral is then a pure
    // mm⁵ geometric moment, not kg·mm². Verify by checking it matches
    // the analytic ∫(y² + z²) dV = (2/3)·a⁵ = 66666.67 mm⁵.
    auto er = eval(
        "version 0.1\n<part>"
        "<extrude height=\"10\"><rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/></extrude>"
        "</part>");
    auto mp = flat_mass_properties(er.parts[0].mesh);
    EXPECT_NEAR(mp.inertia_origin[0], 66666.6667, 1e-2);
    EXPECT_DOUBLE_EQ(mp.mass_kg, 0.0);
    // COM inertia diagonal for a unit-density 10-cube about its centre:
    //   ∫(y² + z²) dV over centred [-5,5]³ = 2 * a · (a³/12) · a = (a⁵)/6
    //                                      = 100000/6 ≈ 16666.67 mm⁵.
    EXPECT_NEAR(mp.inertia_com[0], 16666.6667, 1e-2);
}

// ── Bounding shapes ──────────────────────────────────────────────────

TEST(FlatBounds, CubeAabbAndObbAgree) {
    // Cube → AABB and OBB extents should match (axes are X/Y/Z up to
    // permutation/sign).
    auto er = eval(
        "version 0.1\n<part>"
        "<extrude height=\"10\"><rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/></extrude>"
        "</part>");
    auto b = flat_bounds(er.parts[0].mesh);
    EXPECT_NEAR(b.aabb_min[0],  0.0, 1e-6);
    EXPECT_NEAR(b.aabb_max[0], 10.0, 1e-6);
    EXPECT_NEAR(b.aabb_max[1] - b.aabb_min[1], 10.0, 1e-6);
    EXPECT_NEAR(b.aabb_max[2] - b.aabb_min[2], 10.0, 1e-6);
    // Each OBB extent is 5 (half-width) for a 10-cube.
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(b.principal_box_extents[k], 5.0, 1e-3);
    // Sphere radius >= half body diagonal = 5*sqrt(3) ≈ 8.66.
    EXPECT_GT(b.sphere_radius, 8.5);
    EXPECT_LT(b.sphere_radius, 10.0);   // Ritter loose by <15%
}

TEST(FlatBounds, SmallScaleLongBoxOBBLongestAxisAlignsWithLongDimension) {
    // The old jacobi_3x3 used absolute 1e-12 / 1e-15 thresholds.
    // For a part whose extents are well below 1 mm the covariance
    // entries are O(extents²) — too small to clear the absolute
    // threshold, so the sweep quit immediately with near-identity
    // eigenvectors and the OBB axes were nonsense.
    //
    // Scale-relative thresholds (Frobenius-norm fraction) keep the
    // decomposition correct at any scale. Verify by extruding a
    // long, thin block at sub-millimetre scale and checking that
    // the OBB's largest extent does indeed match the long axis.
    auto er = eval(
        "version 0.1\n<part>"
        "<extrude height=\"0.1\">"
        "<rect x=\"0\" y=\"0\" width=\"1\" height=\"0.1\"/>"
        "</extrude></part>");
    ASSERT_TRUE(er.ok());
    auto b = flat_bounds(er.parts[0].mesh);
    // OBB extents should be sorted longest-first by the new jacobi.
    EXPECT_GE(b.principal_box_extents[0], b.principal_box_extents[1]);
    EXPECT_GE(b.principal_box_extents[1], b.principal_box_extents[2]);
    // The long extent (half-width) must be ~0.5 mm to within
    // tessellation tolerance.
    EXPECT_NEAR(b.principal_box_extents[0], 0.5, 5e-3);
}

TEST(FlatBounds, CylinderHasMinimalRadiusAlongOwnAxis) {
    // Cylinder extruded along +Z, radius 5, height 20. The Z-axis
    // bounding cylinder should have radius 5 and height 20; X and Y
    // axis cylinders should be much larger.
    auto er = eval(
        "version 0.1\n<part>"
        "<extrude height=\"20\"><circle r=\"5\"/></extrude>"
        "</part>");
    auto b = flat_bounds(er.parts[0].mesh);
    EXPECT_NEAR(b.cyl_radius[2], 5.0, 0.05);   // tessellation tolerance
    EXPECT_NEAR(b.cyl_height[2], 20.0, 1e-6);
    EXPECT_GT(b.cyl_radius[0], b.cyl_radius[2]);
    EXPECT_GT(b.cyl_radius[1], b.cyl_radius[2]);
}

// ── Topology ─────────────────────────────────────────────────────────

TEST(FlatTopology, CubeAggregatesMatchAnalytic) {
    auto er = eval(
        "version 0.1\n<part>"
        "<extrude height=\"10\"><rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/></extrude>"
        "</part>");
    auto cr = compile_string(
        "version 0.1\n<part>"
        "<extrude height=\"10\"><rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/></extrude>"
        "</part>");
    ASSERT_TRUE(cr.ok());
    auto t = flat_topology(er.parts[0].mesh, cr.document);
    EXPECT_GT(t.triangles, 0u);
    EXPECT_NEAR(t.volume_mm3, 1000.0, 1e-6);
    EXPECT_NEAR(t.surface_area_mm2, 600.0, 1e-6);
    EXPECT_FALSE(t.elements.empty());
    // The single extrude is the only contributor.
    bool saw_extrude = false;
    for (const auto& e : t.elements) {
        if (e.tag == "extrude") {
            saw_extrude = true;
            EXPECT_NEAR(e.volume_mm3, 1000.0, 1e-6);
        }
    }
    EXPECT_TRUE(saw_extrude);
}

// ── Diff ─────────────────────────────────────────────────────────────

TEST(FlatDiff, IdenticalDocumentsAreAllEqual) {
    constexpr const char* src =
        "version 0.1\n<part name=\"a\">"
        "<extrude height=\"10\"><rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/></extrude>"
        "</part>";
    auto a = eval(src);
    auto b = eval(src);
    auto d = flat_diff(a, b);
    ASSERT_EQ(d.entries.size(), 1u);
    EXPECT_TRUE(d.entries[0].in_a);
    EXPECT_TRUE(d.entries[0].in_b);
    EXPECT_NEAR(d.entries[0].volume_a, d.entries[0].volume_b, 1e-9);
    EXPECT_NEAR(d.entries[0].center_shift_mm, 0.0, 1e-6);
}

TEST(FlatDiff, AddedAndRemovedPartsTracked) {
    auto a = eval(
        "version 0.1\n"
        "<part name=\"only-in-a\"><extrude height=\"5\"><rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/></extrude></part>"
        "<part name=\"shared\"><extrude height=\"3\"><rect x=\"0\" y=\"0\" width=\"3\" height=\"3\"/></extrude></part>");
    auto b = eval(
        "version 0.1\n"
        "<part name=\"only-in-b\"><extrude height=\"7\"><rect x=\"0\" y=\"0\" width=\"7\" height=\"7\"/></extrude></part>"
        "<part name=\"shared\"><extrude height=\"3\"><rect x=\"0\" y=\"0\" width=\"3\" height=\"3\"/></extrude></part>");
    auto d = flat_diff(a, b);
    int added = 0, removed = 0, both = 0;
    for (const auto& e : d.entries) {
        if (e.in_a && !e.in_b)      ++removed;
        else if (!e.in_a && e.in_b) ++added;
        else                        ++both;
    }
    EXPECT_EQ(added,   1);
    EXPECT_EQ(removed, 1);
    EXPECT_EQ(both,    1);
}

TEST(FlatDiff, ResizeShowsVolumeDelta) {
    auto a = eval(
        "version 0.1\n<part name=\"box\">"
        "<extrude height=\"10\"><rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/></extrude>"
        "</part>");
    auto b = eval(
        "version 0.1\n<part name=\"box\">"
        "<extrude height=\"20\"><rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/></extrude>"
        "</part>");
    auto d = flat_diff(a, b);
    ASSERT_EQ(d.entries.size(), 1u);
    EXPECT_NEAR(d.entries[0].volume_a, 1000.0, 1e-6);
    EXPECT_NEAR(d.entries[0].volume_b, 2000.0, 1e-6);
}

// ── Cylindrical-feature inventory ────────────────────────────────────

TEST(FlatHoles, DrilledHoleInBoxIsDetected) {
    // 10x10x10 box with a 2 mm dia hole drilled through the +Z face
    // for 10 mm. The <difference>'s second child is the drill cylinder.
    auto cr = cadml::compile::compile_string(
        "version 0.1\n"
        "param hole-d = 2\n"
        "param hole-h = 10\n"
        "<part name=\"box\">"
        "<difference>"
        "  <extrude height=\"10\"><rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/></extrude>"
        "  <group transform=\"translate(5, 5, 0)\">"
        "    <extrude height=\"{hole-h}\"><circle r=\"{hole-d / 2}\"/></extrude>"
        "  </group>"
        "</difference>"
        "</part>");
    ASSERT_TRUE(cr.ok()) << (cr.errors.empty() ? "" : cr.errors[0].message);
    auto rep = cadml::engine::flat_holes(cr.document);
    ASSERT_EQ(rep.entries.size(), 1u);
    EXPECT_EQ(rep.entries[0].role, "drilled");
    EXPECT_NEAR(rep.entries[0].diameter_mm, 2.0,  1e-6);
    EXPECT_NEAR(rep.entries[0].depth_mm,    10.0, 1e-6);
    EXPECT_TRUE(rep.entries[0].error_hint.empty());
}

// ── Wall thickness ───────────────────────────────────────────────────

TEST(FlatWalls, HollowBoxThicknessMatchesWall) {
    // 20x20x20 outer box minus a 16x16x16 inner box (centred). Walls
    // are 2 mm on every face. Median thickness should land near 2.
    auto er = eval(
        "version 0.1\n<part>"
        "<difference>"
        "  <extrude height=\"20\"><rect x=\"0\" y=\"0\" width=\"20\" height=\"20\"/></extrude>"
        "  <group transform=\"translate(2, 2, 2)\">"
        "    <extrude height=\"16\"><rect x=\"0\" y=\"0\" width=\"16\" height=\"16\"/></extrude>"
        "  </group>"
        "</difference>"
        "</part>");
    ASSERT_TRUE(er.ok());
    auto r = cadml::engine::flat_wall_thickness(er.parts[0].mesh);
    EXPECT_GT(r.samples_with_hit, 0u);
    // Wall is 2 mm perpendicular. An axis-aligned rect+extrude has
    // only corner / edge vertices in its Manifold representation
    // (no face-interior vertices) so per-vertex normals are diagonal
    // averages of incident face normals. The inward ray travels at
    // an angle and the path length depends on which wall it hits
    // first — values can range from 2 (perpendicular wall hit) to
    // ~2*sqrt(3) (cube-diagonal corner hit) up to a full body span
    // when the ray crosses the cavity. Just sanity-check that the
    // numbers are positive and finite, and that the minimum isn't
    // smaller than the actual wall thickness.
    EXPECT_GE(r.min_mm, 1.99);
    EXPECT_GT(r.median_mm, 0);
    EXPECT_GT(r.max_mm, 0);
    EXPECT_LT(r.max_mm, 30.0);   // sanity — no infinities
}

TEST(FlatHoles, AxisFollowsAncestorRotation) {
    // Local +z extrude wrapped in a group that rotates -90° about X.
    // World-frame axis must come out as +y, NOT the local +z.
    auto cr = cadml::compile::compile_string(
        "version 0.1\n<part>"
        "<difference>"
        "  <extrude height=\"10\"><rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/></extrude>"
        "  <group transform=\"rotate(-90, 1, 0, 0)\">"
        "    <extrude height=\"5\"><circle r=\"1\"/></extrude>"
        "  </group>"
        "</difference>"
        "</part>");
    ASSERT_TRUE(cr.ok()) << (cr.errors.empty() ? "" : cr.errors[0].message);
    auto rep = cadml::engine::flat_holes(cr.document);
    ASSERT_EQ(rep.entries.size(), 1u);
    EXPECT_EQ(rep.entries[0].axis, "+y");
}

TEST(FlatHoles, AxisIsInvariantUnderPatternAxisRotation) {
    // Circular pattern about Y wrapping a rotate-X-90 + +z extrude.
    // Every unrolled instance's axis should remain +y in world (Y
    // rotation preserves the Y axis itself). The pre-fix tool put the
    // 4 instances at ±x/±y because it composed transforms outer-first
    // instead of inner-first.
    auto cr = cadml::compile::compile_string(
        "version 0.1\n<part>"
        "<difference>"
        "  <extrude height=\"10\"><rect x=\"0\" y=\"0\" width=\"40\" height=\"40\"/></extrude>"
        "  <pattern type=\"circular\" count=\"4\" axis=\"y\">"
        "    <group transform=\"translate(15, 5, 0) rotate(-90, 1, 0, 0)\">"
        "      <extrude height=\"5\"><circle r=\"1\"/></extrude>"
        "    </group>"
        "  </pattern>"
        "</difference>"
        "</part>");
    ASSERT_TRUE(cr.ok()) << (cr.errors.empty() ? "" : cr.errors[0].message);
    auto rep = cadml::engine::flat_holes(cr.document);
    EXPECT_EQ(rep.entries.size(), 4u);
    for (const auto& e : rep.entries) {
        EXPECT_EQ(e.axis, "+y");
    }
}

TEST(FlatBounds, ObbDoesNotExceedAabb) {
    // L-bracket profile — the canonical asymmetric distribution where
    // PCA finds 45°-rotated max-variance axes whose projected extents
    // exceed the AABB. The post-fix code clamps OBB to AABB whenever
    // PCA inflates, so this invariant should hold for every input.
    auto er = eval(
        "version 0.1\n<part>"
        "<extrude height=\"6\">"
        "<path d=\"M 0,0 L 40,0 40,8 8,8 8,40 0,40 Z\"/>"
        "</extrude>"
        "</part>");
    ASSERT_TRUE(er.ok());
    auto b = cadml::engine::flat_bounds(er.parts[0].mesh);
    const double aabb_vol =
        (b.aabb_max[0] - b.aabb_min[0]) *
        (b.aabb_max[1] - b.aabb_min[1]) *
        (b.aabb_max[2] - b.aabb_min[2]);
    EXPECT_LE(b.principal_box_volume_mm3, aabb_vol + 1e-6);
}

TEST(FlatHoles, BodyCircleIsNotAHole) {
    // First child of <difference> = the body the drill cuts FROM.
    // The body's circle isn't itself a hole.
    auto cr = cadml::compile::compile_string(
        "version 0.1\n<part>"
        "<difference>"
        "  <extrude height=\"10\"><circle r=\"5\"/></extrude>"   // body
        "  <extrude height=\"10\"><circle r=\"1\"/></extrude>"   // drill
        "</difference>"
        "</part>");
    ASSERT_TRUE(cr.ok());
    auto rep = cadml::engine::flat_holes(cr.document);
    ASSERT_EQ(rep.entries.size(), 1u);   // only the drill, not the body
    EXPECT_NEAR(rep.entries[0].diameter_mm, 2.0, 1e-6);
}

// ───────── Parity coverage ported from the retired cadml::analysis ──────
//
// The standalone cadml::analysis library (topology + measure) was a 0.1-era
// duplicate of what flat_analysis now provides; it and its tests were
// removed. These cases reproduce what its suite checked, so flat_analysis
// demonstrably encompasses the old coverage:
//   old Topology.EmptyMeshReturnsZeroSummary     -> TopologyEmptyMeshReturnsZero
//   old Topology.ExtrudedCubeAggregatesMatch...  -> (already) FlatTopology.CubeAggregatesMatchAnalytic
//   old Topology.UnionOfTwoBoxesGivesTwoElements -> TopologyUnionGivesTwoElements
//   old Topology.ElementSourceRangeMatchesParser -> TopologyElementMapsToSourceNode
//   old Measure.BboxOfCubeExtrudeMatches         -> MeasureElementBbox
//   old Measure.DistanceBetweenTwoBoxesMatches   -> MeasureElementDistance
//   old Measure.MissingElementReturnsError       -> MeasureMissingElementErrors

TEST(FlatTopology, TopologyEmptyMeshReturnsZero) {
    cadml::engine::FlatMesh empty;
    cadml::Document doc;
    const auto t = flat_topology(empty, doc);
    EXPECT_EQ(t.triangles, 0u);
    EXPECT_EQ(t.vertices, 0u);
    EXPECT_EQ(t.volume_mm3, 0.0);
    EXPECT_TRUE(t.elements.empty());
}

TEST(FlatTopology, TopologyUnionGivesTwoElements) {
    // Two disjoint 5mm boxes (the second translated +10 in x). The per-
    // element breakdown must show both extrudes, each 125 mm^3, summing
    // to the whole-mesh 250 mm^3 — i.e. source attribution survives a
    // boolean union.
    auto cr = compile_string(
        "version 0.1\n<part name=\"pair\"><union>"
        "<extrude height=\"5\"><rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/></extrude>"
        "<group transform=\"translate(10, 0, 0)\">"
        "<extrude height=\"5\"><rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/></extrude>"
        "</group></union></part>");
    ASSERT_TRUE(cr.ok()) << (cr.errors.empty() ? "" : cr.errors[0].message);
    auto er = evaluate_flat(cr.document);
    ASSERT_TRUE(er.ok());
    ASSERT_FALSE(er.parts.empty());
    const auto t = flat_topology(er.parts[0].mesh, cr.document);
    EXPECT_NEAR(t.volume_mm3, 250.0, 1e-6);

    int extrudes = 0;
    double per_element_sum = 0;
    for (const auto& e : t.elements) {
        per_element_sum += e.volume_mm3;
        if (e.tag == "extrude") {
            ++extrudes;
            EXPECT_NEAR(e.volume_mm3, 125.0, 1e-6);
        }
    }
    EXPECT_EQ(extrudes, 2);
    EXPECT_NEAR(per_element_sum, t.volume_mm3, 1e-6);
}

TEST(FlatTopology, TopologyElementMapsToSourceNode) {
    // The old per-element result pre-joined a SourceRange; flat_topology
    // carries node_id, and the source is recovered by indexing the
    // Document. Verify that join lands on the right node.
    auto cr = compile_string(
        "version 0.1\n<part name=\"p\"><extrude height=\"5\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/></extrude></part>");
    ASSERT_TRUE(cr.ok());
    auto er = evaluate_flat(cr.document);
    ASSERT_TRUE(er.ok());
    ASSERT_FALSE(er.parts.empty());
    const auto t = flat_topology(er.parts[0].mesh, cr.document);

    bool checked = false;
    for (const auto& e : t.elements) {
        if (e.tag != "extrude") continue;
        ASSERT_LT(e.node_id, cr.document.nodes.size());
        EXPECT_EQ(cr.document.nodes[e.node_id].type, cadml::NodeType::Extrude);
        checked = true;
    }
    EXPECT_TRUE(checked);
}

TEST(FlatMeasure, MeasureElementBbox) {
    auto cr = compile_string(
        "version 0.1\n<part name=\"cube\"><extrude height=\"10\">"
        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/></extrude></part>");
    ASSERT_TRUE(cr.ok());
    auto er = evaluate_flat(cr.document);
    ASSERT_TRUE(er.ok());
    ASSERT_FALSE(er.parts.empty());
    const auto& mesh = er.parts[0].mesh;

    const auto t = flat_topology(mesh, cr.document);
    std::uint32_t extrude_id = 0;
    bool found = false;
    for (const auto& e : t.elements)
        if (e.tag == "extrude") { extrude_id = e.node_id; found = true; break; }
    ASSERT_TRUE(found);

    const std::vector<MeasureProbe> probes = {
        { MeasureKind::Bbox, extrude_id, 0 }
    };
    const auto r = flat_measure(mesh, probes);
    ASSERT_EQ(r.items.size(), 1u);
    ASSERT_TRUE(r.items[0].ok) << r.items[0].error;
    EXPECT_NEAR(r.items[0].size[0], 10.0, 1e-6);
    EXPECT_NEAR(r.items[0].size[1], 10.0, 1e-6);
    EXPECT_NEAR(r.items[0].size[2], 10.0, 1e-6);
}

TEST(FlatMeasure, MeasureElementDistance) {
    // Box A x∈[0,5], box B x∈[15,20] → near faces 10 mm apart.
    auto cr = compile_string(
        "version 0.1\n<part name=\"pair\"><union>"
        "<extrude height=\"5\"><rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/></extrude>"
        "<group transform=\"translate(15, 0, 0)\">"
        "<extrude height=\"5\"><rect x=\"0\" y=\"0\" width=\"5\" height=\"5\"/></extrude>"
        "</group></union></part>");
    ASSERT_TRUE(cr.ok());
    auto er = evaluate_flat(cr.document);
    ASSERT_TRUE(er.ok());
    ASSERT_FALSE(er.parts.empty());
    const auto& mesh = er.parts[0].mesh;

    const auto t = flat_topology(mesh, cr.document);
    std::vector<std::uint32_t> extrude_ids;
    for (const auto& e : t.elements)
        if (e.tag == "extrude") extrude_ids.push_back(e.node_id);
    ASSERT_GE(extrude_ids.size(), 2u);

    const std::vector<MeasureProbe> probes = {
        { MeasureKind::DistanceMin, extrude_ids[0], extrude_ids[1] }
    };
    const auto r = flat_measure(mesh, probes);
    ASSERT_EQ(r.items.size(), 1u);
    ASSERT_TRUE(r.items[0].ok) << r.items[0].error;
    EXPECT_NEAR(r.items[0].distance, 10.0, 1e-6);
}

TEST(FlatMeasure, MeasureMissingElementErrors) {
    auto cr = compile_string(
        "version 0.1\n<part name=\"c\"><extrude height=\"1\">"
        "<rect x=\"0\" y=\"0\" width=\"1\" height=\"1\"/></extrude></part>");
    ASSERT_TRUE(cr.ok());
    auto er = evaluate_flat(cr.document);
    ASSERT_TRUE(er.ok());
    ASSERT_FALSE(er.parts.empty());

    const std::vector<MeasureProbe> probes = {
        { MeasureKind::Bbox, 99999u, 0u }   // no such element
    };
    const auto r = flat_measure(er.parts[0].mesh, probes);
    ASSERT_EQ(r.items.size(), 1u);
    EXPECT_FALSE(r.items[0].ok);
    EXPECT_FALSE(r.items[0].error.empty());
}
