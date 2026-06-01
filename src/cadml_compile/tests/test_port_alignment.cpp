// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/types.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace cadml;

namespace {

constexpr double kEps = 1e-6;

void ExpectVec3Near(Vec3 a, Vec3 b) {
    EXPECT_NEAR(a.x, b.x, kEps);
    EXPECT_NEAR(a.y, b.y, kEps);
    EXPECT_NEAR(a.z, b.z, kEps);
}

}  // namespace

// ─── Identity cases ─────────────────────────────────────────────────

TEST(PortAlignment, OriginToOriginFlippingNormals) {
    // Two ports at origin, A.normal=+z, B.normal=-z, both up=+x.
    // After alignment, A's local origin should be at world origin.
    Vec3 a_pos{0, 0, 0}, a_n{0, 0, 1}, a_u{1, 0, 0};
    Vec3 b_pos{0, 0, 0}, b_n{0, 0, -1}, b_u{1, 0, 0};
    auto t = compute_port_alignment(a_pos, a_n, a_u, b_pos, b_n, b_u);
    ExpectVec3Near(t.transform_point({0, 0, 0}), {0, 0, 0});
    // A's normal (+z) should map to -B.normal (= +z). Identity here.
    ExpectVec3Near(t.transform_direction({0, 0, 1}), {0, 0, 1});
}

TEST(PortAlignment, MatchingNormalsRequiresFlip) {
    // Both ports have normal +z. After mating, A's +z should map to -z
    // in world (so the two normals point at each other).
    Vec3 a_pos{0, 0, 0}, a_n{0, 0, 1}, a_u{1, 0, 0};
    Vec3 b_pos{0, 0, 0}, b_n{0, 0, 1}, b_u{1, 0, 0};
    auto t = compute_port_alignment(a_pos, a_n, a_u, b_pos, b_n, b_u);
    ExpectVec3Near(t.transform_direction({0, 0, 1}), {0, 0, -1});
}

// ─── Translation only (parallel + opposite normals) ─────────────────

TEST(PortAlignment, TranslateBoltOntoPlate) {
    // Bolt port (head): position=(0,0,length=20), normal=-z, up=+x
    // Plate port (hole): position=(0,0,plate_h=6), normal=+z, up=+x
    // After mating: bolt.head sits on plate.hole. Bolt's local origin
    // (thread tip) should end up at z = 6 - 20 = -14.
    const double length = 20;
    const double plate_h = 6;
    Vec3 a_pos{0, 0, length}, a_n{0, 0, -1}, a_u{1, 0, 0};
    Vec3 b_pos{0, 0, plate_h}, b_n{0, 0, 1}, b_u{1, 0, 0};
    auto t = compute_port_alignment(a_pos, a_n, a_u, b_pos, b_n, b_u);
    // Bolt's origin (0,0,0) should land at world (0, 0, plate_h - length).
    ExpectVec3Near(t.transform_point({0, 0, 0}),
                    {0, 0, plate_h - length});
    // The head port itself should land at the plate hole.
    ExpectVec3Near(t.transform_point(a_pos), b_pos);
}

// ─── Rotation around the normal (up vector) ─────────────────────────

TEST(PortAlignment, UpVectorAlignment) {
    // Both ports at origin, opposite normals (+z and -z), but B.up=+y
    // (vs A.up=+x). After mating, A's local +x should align with B's
    // +y direction (since both ups must agree in world space).
    Vec3 a_pos{0, 0, 0}, a_n{0, 0, 1}, a_u{1, 0, 0};
    Vec3 b_pos{0, 0, 0}, b_n{0, 0, -1}, b_u{0, 1, 0};
    auto t = compute_port_alignment(a_pos, a_n, a_u, b_pos, b_n, b_u);
    // A's local up (+x) goes to world +y.
    ExpectVec3Near(t.transform_direction({1, 0, 0}), {0, 1, 0});
}

// ─── Composed transform (b is already placed) ──────────────────────

TEST(PortAlignment, RespectsBPriorTransform) {
    // B placed by transform_b = translate(10, 20, 0). Then A is placed
    // such that its origin lands at world (10, 20, plate_h - length).
    const double length = 20;
    const double plate_h = 6;
    Vec3 a_pos{0, 0, length}, a_n{0, 0, -1}, a_u{1, 0, 0};
    Vec3 b_pos{0, 0, plate_h}, b_n{0, 0, 1}, b_u{1, 0, 0};
    auto transform_b = Mat4::translation(10, 20, 0);
    auto t = compute_port_alignment(a_pos, a_n, a_u, b_pos, b_n, b_u, transform_b);
    ExpectVec3Near(t.transform_point({0, 0, 0}),
                    {10, 20, plate_h - length});
}

// ─── Mat4 ↔ transform-string ───────────────────────────────────────

TEST(Mat4ToTransform, IdentityIsEmpty) {
    EXPECT_EQ(mat4_to_transform_string(Mat4::identity()), "");
}

TEST(Mat4ToTransform, TranslateOnly) {
    auto s = mat4_to_transform_string(Mat4::translation(1, 2, 3));
    EXPECT_NE(s.find("translate(1, 2, 3)"), std::string::npos);
    EXPECT_EQ(s.find("rotate"), std::string::npos);
}

TEST(Mat4ToTransform, RotationOnly) {
    auto m = Mat4::rotation(90, 0, 0, 1);
    auto s = mat4_to_transform_string(m);
    EXPECT_NE(s.find("rotate(90"), std::string::npos);
    EXPECT_EQ(s.find("translate"), std::string::npos);
}

TEST(Mat4ToTransform, TranslateAndRotate) {
    auto m = Mat4::translation(1, 0, 0) * Mat4::rotation(45, 0, 0, 1);
    auto s = mat4_to_transform_string(m);
    EXPECT_NE(s.find("translate"), std::string::npos);
    EXPECT_NE(s.find("rotate"), std::string::npos);
}
