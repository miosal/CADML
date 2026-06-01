// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/types.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace cadml;

namespace {

constexpr double kEps = 1e-9;

// Use a function (not a macro) for vec comparison. Macros expand
// `EXPECT_*(Vec3{a, b, c}, ...)` with the preprocessor seeing the
// commas inside braces as macro-arg separators, breaking compilation.
// Same gotcha applies to EXPECT_NEAR with `Vec3{...}.length()` — wrap
// in a local variable first.
void ExpectVec3Near(Vec3 a, Vec3 b) {
    EXPECT_NEAR(a.x, b.x, kEps) << "x component";
    EXPECT_NEAR(a.y, b.y, kEps) << "y component";
    EXPECT_NEAR(a.z, b.z, kEps) << "z component";
}

}  // namespace

// ─── Vec3 basics ─────────────────────────────────────────────────────

TEST(Vec3, Length) {
    Vec3 v345{3, 4, 0};
    Vec3 v122{1, 2, 2};
    Vec3 zero{0, 0, 0};
    EXPECT_NEAR(v345.length(), 5.0, kEps);
    EXPECT_NEAR(v122.length(), 3.0, kEps);
    EXPECT_NEAR(zero.length(), 0.0, kEps);
}

TEST(Vec3, Normalized) {
    ExpectVec3Near(Vec3{3, 4, 0}.normalized(), (Vec3{0.6, 0.8, 0}));
    ExpectVec3Near(Vec3{0, 0, 0}.normalized(), (Vec3{0, 0, 0}));  // safe zero
    ExpectVec3Near(Vec3{0, 0, 5}.normalized(), (Vec3{0, 0, 1}));
}

TEST(Vec3, DotAndCross) {
    Vec3 a{1, 0, 0}, b{0, 1, 0};
    EXPECT_NEAR(a.dot(b), 0.0, kEps);
    EXPECT_NEAR(a.dot(a), 1.0, kEps);
    ExpectVec3Near(a.cross(b), (Vec3{0, 0, 1}));
    ExpectVec3Near(b.cross(a), (Vec3{0, 0, -1}));
}

TEST(Vec3, ArithmeticOperators) {
    Vec3 a{1, 2, 3}, b{4, 5, 6};
    ExpectVec3Near(a + b, (Vec3{5, 7, 9}));
    ExpectVec3Near(a - b, (Vec3{-3, -3, -3}));
    ExpectVec3Near(a * 2.0, (Vec3{2, 4, 6}));
    ExpectVec3Near(b / 2.0, (Vec3{2, 2.5, 3}));
}

// ─── Mat4 basics ─────────────────────────────────────────────────────

TEST(Mat4, IdentityTransformsPointUnchanged) {
    auto m = Mat4::identity();
    ExpectVec3Near(m.transform_point({1, 2, 3}), (Vec3{1, 2, 3}));
}

TEST(Mat4, Translation) {
    auto m = Mat4::translation(1, 2, 3);
    ExpectVec3Near(m.transform_point({0, 0, 0}), (Vec3{1, 2, 3}));
    ExpectVec3Near(m.transform_point({10, 0, 0}), (Vec3{11, 2, 3}));
}

TEST(Mat4, TranslationDoesNotAffectDirection) {
    auto m = Mat4::translation(100, 200, 300);
    ExpectVec3Near(m.transform_direction({1, 0, 0}), (Vec3{1, 0, 0}));
}

TEST(Mat4, Scaling) {
    auto m = Mat4::scaling(2, 3, 4);
    ExpectVec3Near(m.transform_point({1, 1, 1}), (Vec3{2, 3, 4}));
}

TEST(Mat4, RotationZ90Degrees) {
    auto m = Mat4::rotation(90, 0, 0, 1);
    // +X axis rotates 90° around +Z → +Y
    ExpectVec3Near(m.transform_point({1, 0, 0}), (Vec3{0, 1, 0}));
    // +Y rotates → -X
    ExpectVec3Near(m.transform_point({0, 1, 0}), (Vec3{-1, 0, 0}));
    // +Z stays
    ExpectVec3Near(m.transform_point({0, 0, 1}), (Vec3{0, 0, 1}));
}

TEST(Mat4, RotationX90Degrees) {
    auto m = Mat4::rotation(90, 1, 0, 0);
    // +Y rotates 90° around +X → +Z
    ExpectVec3Near(m.transform_point({0, 1, 0}), (Vec3{0, 0, 1}));
    // +Z rotates → -Y
    ExpectVec3Near(m.transform_point({0, 0, 1}), (Vec3{0, -1, 0}));
}

TEST(Mat4, RotationDegenerateAxisReturnsIdentity) {
    auto m = Mat4::rotation(45, 0, 0, 0);  // zero-length axis
    ExpectVec3Near(m.transform_point({1, 2, 3}), (Vec3{1, 2, 3}));
}

TEST(Mat4, MirrorThroughZPlane) {
    auto m = Mat4::mirror(0, 0, 1);
    ExpectVec3Near(m.transform_point({1, 2, 3}),  (Vec3{1, 2, -3}));
    ExpectVec3Near(m.transform_point({1, 2, -5}), (Vec3{1, 2, 5}));
}

TEST(Mat4, ComposingTranslateAndRotate) {
    // Translate then rotate (column-major: M = R * T means T applied first).
    auto t = Mat4::translation(1, 0, 0);
    auto r = Mat4::rotation(90, 0, 0, 1);
    auto m = r * t;
    // Apply T first: {0,0,0} → {1,0,0}; then R: {1,0,0} → {0,1,0}
    ExpectVec3Near(m.transform_point({0, 0, 0}), (Vec3{0, 1, 0}));
}

// ─── Axis aliases (spec §7.4) ────────────────────────────────────────

TEST(AxisAlias, AllSixDirections) {
    ExpectVec3Near(*parse_axis_alias("+x"), (Vec3{ 1,  0,  0}));
    ExpectVec3Near(*parse_axis_alias( "x"), (Vec3{ 1,  0,  0}));
    ExpectVec3Near(*parse_axis_alias("-x"), (Vec3{-1,  0,  0}));
    ExpectVec3Near(*parse_axis_alias("+y"), (Vec3{ 0,  1,  0}));
    ExpectVec3Near(*parse_axis_alias( "y"), (Vec3{ 0,  1,  0}));
    ExpectVec3Near(*parse_axis_alias("-y"), (Vec3{ 0, -1,  0}));
    ExpectVec3Near(*parse_axis_alias("+z"), (Vec3{ 0,  0,  1}));
    ExpectVec3Near(*parse_axis_alias( "z"), (Vec3{ 0,  0,  1}));
    ExpectVec3Near(*parse_axis_alias("-z"), (Vec3{ 0,  0, -1}));
}

TEST(AxisAlias, WhitespaceTolerated) {
    ExpectVec3Near(*parse_axis_alias("  +x  "),  (Vec3{1, 0, 0}));
    ExpectVec3Near(*parse_axis_alias("\t-z\n"), (Vec3{0, 0, -1}));
}

TEST(AxisAlias, RejectsUppercase) {
    EXPECT_FALSE(parse_axis_alias("+X").has_value());
    EXPECT_FALSE(parse_axis_alias("Z").has_value());
}

TEST(AxisAlias, RejectsLiteralVec3) {
    EXPECT_FALSE(parse_axis_alias("0 0 1").has_value());
    EXPECT_FALSE(parse_axis_alias("1, 0, 0").has_value());
}

TEST(AxisAlias, RejectsExpression) {
    EXPECT_FALSE(parse_axis_alias("{a}").has_value());
}

TEST(AxisAlias, RejectsEmpty) {
    EXPECT_FALSE(parse_axis_alias("").has_value());
    EXPECT_FALSE(parse_axis_alias("   ").has_value());
}

TEST(AxisAlias, RejectsRandomString) {
    EXPECT_FALSE(parse_axis_alias("foo").has_value());
    EXPECT_FALSE(parse_axis_alias("xx").has_value());
    EXPECT_FALSE(parse_axis_alias("++x").has_value());
}

// ─── parse_interference_tolerance ────────────────────────────────────

TEST(InterferenceTolerance, EmptyMeansZero) {
    auto v = parse_interference_tolerance("", "mm");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 0.0);
}

TEST(InterferenceTolerance, NoSuffixUsesDocUnits) {
    // "0.01" with doc_units=mm → 0.01 mm³ (no conversion).
    auto v = parse_interference_tolerance("0.01", "mm");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 0.01);
}

TEST(InterferenceTolerance, MmCubedSuffix) {
    auto v = parse_interference_tolerance("0.5mm\xC2\xB3", "mm");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 0.5);
}

TEST(InterferenceTolerance, AsciiCubedSuffix) {
    // ASCII fallback: `mm3` is accepted alongside `mm³`.
    auto v = parse_interference_tolerance("0.5mm3", "mm");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 0.5);
}

TEST(InterferenceTolerance, CmCubedConvertsToMm) {
    // 1 cm³ = 1000 mm³.
    auto v = parse_interference_tolerance("1cm\xC2\xB3", "mm");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 1000.0);
}

TEST(InterferenceTolerance, MmCubedInCmDoc) {
    // 1000 mm³ = 1 cm³ — when doc units are cm, the answer is 1.
    auto v = parse_interference_tolerance("1000mm\xC2\xB3", "cm");
    ASSERT_TRUE(v.has_value());
    EXPECT_NEAR(*v, 1.0, 1e-9);
}

TEST(InterferenceTolerance, InchCubed) {
    // 1 in = 25.4 mm → 1 in³ = 16387.064 mm³.
    auto v = parse_interference_tolerance("1in3", "mm");
    ASSERT_TRUE(v.has_value());
    EXPECT_NEAR(*v, 25.4 * 25.4 * 25.4, 1e-6);
}

TEST(InterferenceTolerance, WhitespaceAroundUnit) {
    auto v = parse_interference_tolerance(" 0.5 mm3 ", "mm");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 0.5);
}

TEST(InterferenceTolerance, ScientificNotation) {
    auto v = parse_interference_tolerance("1e-3mm3", "mm");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 1e-3);
}

TEST(InterferenceTolerance, RejectsNegative) {
    EXPECT_FALSE(parse_interference_tolerance("-0.01", "mm").has_value());
}

TEST(InterferenceTolerance, RejectsUnknownUnit) {
    EXPECT_FALSE(parse_interference_tolerance("1km3", "mm").has_value());
}

TEST(InterferenceTolerance, RejectsSuffixWithoutCubeMarker) {
    // `1mm` is a length, not a volume — reject.
    EXPECT_FALSE(parse_interference_tolerance("1mm", "mm").has_value());
}

TEST(InterferenceTolerance, RejectsGarbage) {
    EXPECT_FALSE(parse_interference_tolerance("abc", "mm").has_value());
    EXPECT_FALSE(parse_interference_tolerance("...", "mm").has_value());
}

TEST(InterferenceTolerance, RejectsUnknownDocUnit) {
    // Document units must be one of the known ones for unit-tagged input.
    EXPECT_FALSE(parse_interference_tolerance("1mm3", "lightyear").has_value());
}

TEST(InterferenceTolerance, RejectsMultipleDots) {
    // std::stod parses "1.2.3" as 1.2 and silently ignores the rest.
    // Our wrapper must verify ALL of the number-text was consumed.
    EXPECT_FALSE(parse_interference_tolerance("1.2.3mm3", "mm").has_value());
    EXPECT_FALSE(parse_interference_tolerance("1.2.3", "mm").has_value());
    EXPECT_FALSE(parse_interference_tolerance("..1mm3", "mm").has_value());
}

TEST(InterferenceTolerance, RejectsOverflow) {
    // 1e308 m^3 = 1e308 * 1e9 mm^3 = 1e317 — overflows double.
    // Tolerance = inf would silently suppress every interference.
    EXPECT_FALSE(parse_interference_tolerance("1e308m3", "mm").has_value());
}

TEST(InterferenceTolerance, AcceptsLargeButFinite) {
    // 1e10 mm^3 is huge but finite — should round-trip.
    auto v = parse_interference_tolerance("1e10mm3", "mm");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 1e10);
}

TEST(InterferenceTolerance, RejectsLoneSign) {
    EXPECT_FALSE(parse_interference_tolerance("+", "mm").has_value());
    EXPECT_FALSE(parse_interference_tolerance("-", "mm").has_value());
    EXPECT_FALSE(parse_interference_tolerance("+mm3", "mm").has_value());
}

TEST(InterferenceTolerance, RejectsHexLiteral) {
    // CADML scalars are decimal; reject hex-style "0x..." even though
    // std::stod accepts hex floats per C99.
    EXPECT_FALSE(parse_interference_tolerance("0x1p3mm3", "mm").has_value());
}

TEST(InterferenceTolerance, AcceptsLeadingZeros) {
    auto v = parse_interference_tolerance("00010mm3", "mm");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 10.0);
}

TEST(InterferenceTolerance, AcceptsExplicitPositiveSign) {
    auto v = parse_interference_tolerance("+1mm3", "mm");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 1.0);
}

// ─── parse_double_strict ────────────────────────────────────────────

TEST(ParseDoubleStrict, AcceptsPlainNumbers) {
    EXPECT_DOUBLE_EQ(*parse_double_strict("0"),     0.0);
    EXPECT_DOUBLE_EQ(*parse_double_strict("1.5"),   1.5);
    EXPECT_DOUBLE_EQ(*parse_double_strict("-2.25"), -2.25);
    EXPECT_DOUBLE_EQ(*parse_double_strict("1e3"),   1000.0);
}

TEST(ParseDoubleStrict, RejectsEmptyAndWhitespaceOnly) {
    EXPECT_FALSE(parse_double_strict("").has_value());
    EXPECT_FALSE(parse_double_strict("   ").has_value());
    EXPECT_FALSE(parse_double_strict("\t\n").has_value());
}

TEST(ParseDoubleStrict, TolerantOfSurroundingWhitespace) {
    auto v = parse_double_strict("  3.14  ");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 3.14);
}

TEST(ParseDoubleStrict, RejectsTrailingJunk) {
    EXPECT_FALSE(parse_double_strict("1.0x").has_value());
    EXPECT_FALSE(parse_double_strict("abc").has_value());
    EXPECT_FALSE(parse_double_strict("1, 2").has_value());
}

TEST(ParseDoubleStrict, RejectsOutOfRangeMagnitudes) {
    EXPECT_FALSE(parse_double_strict("1e9999").has_value());
}

TEST(ParseDoubleStrict, AcceptsExplicitPositiveSign) {
    // SVG path data, transform args, and CLI flags all legally write
    // `+0.5`. std::from_chars rejects bare `+` per [charconv]; the
    // helper strips it so the strtod-era behaviour survives.
    EXPECT_DOUBLE_EQ(*parse_double_strict("+5"),   5.0);
    EXPECT_DOUBLE_EQ(*parse_double_strict("+0.5"), 0.5);
    EXPECT_DOUBLE_EQ(*parse_double_strict("+1e3"), 1000.0);
    // Just `+` is still invalid.
    EXPECT_FALSE(parse_double_strict("+").has_value());
}
