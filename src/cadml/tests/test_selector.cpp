// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/selector.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace cadml;

namespace {

Selector ok_parse(std::string_view s) {
    auto r = parse_selector(s);
    EXPECT_TRUE(r.ok) << "expected `" << s << "` to parse: " << r.error;
    return r.selector;
}

void bad_parse(std::string_view s, std::string_view needle) {
    auto r = parse_selector(s);
    EXPECT_FALSE(r.ok) << "expected `" << s << "` to be rejected";
    if (!r.ok)
        EXPECT_NE(r.error.find(std::string(needle)), std::string::npos)
            << "message `" << r.error << "` lacked `" << needle << "`";
}

}  // namespace

// ─── Parsing: the happy paths ───────────────────────────────────────

TEST(Selector, EmptyAndAllAreAllScope) {
    EXPECT_TRUE(ok_parse("").is_all());
    EXPECT_TRUE(ok_parse("all").is_all());
    EXPECT_TRUE(ok_parse("   all  ").is_all());
}

TEST(Selector, EdgeFieldsParse) {
    EXPECT_TRUE(ok_parse("edge:along=+x").is_edge());
    EXPECT_EQ(ok_parse("edge:along=+x").field, SelectorField::Along);
    EXPECT_EQ(ok_parse("edge:dihedral>90").field, SelectorField::Dihedral);
    EXPECT_EQ(ok_parse("edge:dihedral>90").cmp, SelectorCmp::Gt);
    EXPECT_EQ(ok_parse("edge:position.z=0").field, SelectorField::EdgePosZ);
    EXPECT_EQ(ok_parse("edge:length>=10").field, SelectorField::Length);
    EXPECT_EQ(ok_parse("edge:length>=10").cmp, SelectorCmp::Ge);
}

TEST(Selector, FaceFieldsParse) {
    EXPECT_TRUE(ok_parse("face:normal=+z").is_face());
    EXPECT_EQ(ok_parse("face:normal=+z").field, SelectorField::Normal);
    EXPECT_EQ(ok_parse("face:position.x<=5").field, SelectorField::FacePosX);
    EXPECT_EQ(ok_parse("face:position.x<=5").cmp, SelectorCmp::Le);
    EXPECT_EQ(ok_parse("face:area>100").field, SelectorField::Area);
}

TEST(Selector, WhitespaceInPredicateIsOptional) {
    auto a = ok_parse("edge:dihedral>90");
    auto b = ok_parse("edge:dihedral > 90");
    EXPECT_EQ(a.field, b.field);
    EXPECT_EQ(a.cmp, b.cmp);
    EXPECT_DOUBLE_EQ(a.number, b.number);
}

TEST(Selector, NormalAcceptsAxisAliasAndVector) {
    auto a = ok_parse("face:normal=+z");
    EXPECT_TRUE(a.value_is_vector);
    auto b = ok_parse("face:normal=0,0,1");
    EXPECT_TRUE(b.value_is_vector);
}

// ─── Parsing: the error paths (loud, no silent fallback) ────────────

TEST(Selector, RejectsUnknownScope) {
    bad_parse("vertex:foo=1", "unknown selector scope");
}

TEST(Selector, RejectsUnknownEdgeField) {
    bad_parse("edge:z=0", "unknown edge field");
}

TEST(Selector, RejectsUnknownFaceField) {
    // Probe 18's case: `face:z` is not a field (it's `face:position.z`).
    bad_parse("face:z=foo", "unknown face field");
}

TEST(Selector, RejectsMalformedNumber) {
    bad_parse("edge:length>foo", "malformed value");
    bad_parse("face:area=12abc", "malformed value");
}

TEST(Selector, RejectsMalformedAxis) {
    bad_parse("edge:along=q", "malformed value");
    bad_parse("edge:along=5", "malformed value");
}

TEST(Selector, RejectsIllegalComparatorForAxisFields) {
    bad_parse("edge:along>+x", "only supports the `=` comparator");
    bad_parse("face:normal>+z", "only supports the `=` comparator");
}

TEST(Selector, RejectsMissingComparator) {
    bad_parse("edge:dihedral", "no comparator");
}

TEST(Selector, RejectsMissingColon) {
    bad_parse("dihedral>90", "must be");
}

// ─── Matching: edges ────────────────────────────────────────────────

TEST(Selector, AllMatchesEveryEdge) {
    EdgeProps e{ {1,0,0}, 90.0, {5,0,0}, 10.0 };
    EXPECT_TRUE(ok_parse("all").matches(e));
}

TEST(Selector, EdgeAlongMatchesParallelEitherOrientation) {
    auto sel = ok_parse("edge:along=+x");
    EXPECT_TRUE(sel.matches(EdgeProps{ {1,0,0}, 90, {0,0,0}, 1 }));
    EXPECT_TRUE(sel.matches(EdgeProps{ {-1,0,0}, 90, {0,0,0}, 1 }))  // undirected
        << "an edge along -x is the same line as +x";
    EXPECT_FALSE(sel.matches(EdgeProps{ {0,1,0}, 90, {0,0,0}, 1 }));
    EXPECT_FALSE(sel.matches(EdgeProps{ {0,0,1}, 90, {0,0,0}, 1 }));
}

TEST(Selector, EdgeDihedralComparators) {
    EdgeProps cube{ {1,0,0}, 90.0, {0,0,0}, 1 };
    EXPECT_TRUE (ok_parse("edge:dihedral=90").matches(cube));
    EXPECT_FALSE(ok_parse("edge:dihedral>90").matches(cube));   // exactly 90
    EXPECT_TRUE (ok_parse("edge:dihedral>=90").matches(cube));
    EXPECT_FALSE(ok_parse("edge:dihedral<90").matches(cube));
    EXPECT_TRUE (ok_parse("edge:dihedral<=90").matches(cube));
    EdgeProps gentle{ {1,0,0}, 170.0, {0,0,0}, 1 };
    EXPECT_TRUE (ok_parse("edge:dihedral>90").matches(gentle));
}

TEST(Selector, EdgePositionAndLength) {
    EdgeProps e{ {1,0,0}, 90.0, {3,4,0}, 7.0 };
    EXPECT_TRUE (ok_parse("edge:position.z=0").matches(e));
    EXPECT_TRUE (ok_parse("edge:position.x>2").matches(e));
    EXPECT_FALSE(ok_parse("edge:position.x>3").matches(e));
    EXPECT_TRUE (ok_parse("edge:length>=7").matches(e));
    EXPECT_FALSE(ok_parse("edge:length>7").matches(e));
}

TEST(Selector, FaceSelectorDoesNotMatchEdge) {
    EdgeProps e{ {1,0,0}, 90.0, {0,0,0}, 1 };
    EXPECT_FALSE(ok_parse("face:normal=+x").matches(e));
}

// ─── Matching: faces ────────────────────────────────────────────────

TEST(Selector, FaceNormalAngleTolerance) {
    auto sel = ok_parse("face:normal=+z");
    // Faces are matched as undirected: both +z and -z face normals
    // satisfy `face:normal=+z`. Authors who want signed selection
    // use position.* fields instead.
    EXPECT_TRUE (sel.matches(FaceProps{ {0,0, 1}, {0,0,0}, 1 }));
    EXPECT_TRUE (sel.matches(FaceProps{ {0,0,-1}, {0,0,0}, 1 }));
    EXPECT_FALSE(sel.matches(FaceProps{ {1,0, 0}, {0,0,0}, 1 }));
    // Within 1° should still match.
    const double tiny = 0.3 * 3.14159265358979323846 / 180.0;  // 0.3°
    EXPECT_TRUE(sel.matches(FaceProps{ {std::sin(tiny),0,std::cos(tiny)}, {0,0,0}, 1 }));
}

TEST(Selector, EdgeAlongUndirectedMatchesEitherOrientation) {
    auto sel = ok_parse("edge:along=+x");
    EXPECT_TRUE(sel.matches(EdgeProps{ { 1,0,0}, 0, {0,0,0}, 1 }));
    EXPECT_TRUE(sel.matches(EdgeProps{ {-1,0,0}, 0, {0,0,0}, 1 }));
    EXPECT_FALSE(sel.matches(EdgeProps{ { 0,1,0}, 0, {0,0,0}, 1 }));
}

TEST(Selector, LengthEqualityIsScaleRelative) {
    // The old absolute 1e-6 tolerance would have failed on
    // `length=1e7` vs an edge of length 1e7 + 5 — even though the
    // relative gap is ~5e-7. Scale-relative tolerance accepts it.
    auto sel = ok_parse("edge:length=1e7");
    EdgeProps e{ {1,0,0}, 0, {0,0,0}, 1e7 + 5 };
    EXPECT_TRUE(sel.matches(e));
    // A 2x edge must still fail (relative gap is enormous).
    EdgeProps far{ {1,0,0}, 0, {0,0,0}, 2e7 };
    EXPECT_FALSE(sel.matches(far));
}

TEST(Selector, FacePositionAndArea) {
    FaceProps f{ {0,0,1}, {1,2,5}, 50.0 };
    EXPECT_TRUE (ok_parse("face:position.z=5").matches(f));
    EXPECT_TRUE (ok_parse("face:area<100").matches(f));
    EXPECT_FALSE(ok_parse("face:area>100").matches(f));
}

TEST(Selector, EdgeSelectorDoesNotMatchFace) {
    FaceProps f{ {0,0,1}, {0,0,0}, 1 };
    EXPECT_FALSE(ok_parse("edge:along=+z").matches(f));
}
