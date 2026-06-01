// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>

#include <gtest/gtest.h>

using namespace cadml;
using namespace cadml::compile;

namespace {

CompileResult cs(std::string_view src) {
    return compile_string(src);
}

std::size_t count_substr(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return 0;
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

}  // namespace

// ─── Linear pattern ─────────────────────────────────────────────────

TEST(PatternUnroller, LinearAlongPlusX) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<pattern type=\"linear\" count=\"3\" axis=\"+x\" spacing=\"10\">"
        "<circle r=\"1\"/>"
        "</pattern>"
        "</part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(count_substr(r.flat_text, "<circle"), 3u);
    // 3 wrapper groups, each with a translate transform.
    EXPECT_NE(r.flat_text.find("translate(0, 0, 0)"),  std::string::npos);
    EXPECT_NE(r.flat_text.find("translate(10, 0, 0)"), std::string::npos);
    EXPECT_NE(r.flat_text.find("translate(20, 0, 0)"), std::string::npos);
    // <pattern> itself is gone.
    EXPECT_EQ(r.flat_text.find("<pattern"), std::string::npos);
}

TEST(PatternUnroller, LinearAlongMinusZ) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<pattern type=\"linear\" count=\"2\" axis=\"-z\" spacing=\"5\">"
        "<circle r=\"1\"/>"
        "</pattern>"
        "</part>");
    EXPECT_TRUE(r.ok());
    // -z * 0 = 0, -z * 5 = -5
    EXPECT_NE(r.flat_text.find("translate(0, 0, 0)"),  std::string::npos);
    EXPECT_NE(r.flat_text.find("translate(0, 0, -5)"), std::string::npos);
}

TEST(PatternUnroller, LinearWithParamSpacing) {
    auto r = cs(
        "version 0.1\n"
        "param spacing = 8\n"
        "<part>"
        "<pattern type=\"linear\" count=\"4\" axis=\"+y\" spacing=\"{spacing}\">"
        "<circle r=\"1\"/>"
        "</pattern>"
        "</part>");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(count_substr(r.flat_text, "translate(0,"), 4u);
    EXPECT_NE(r.flat_text.find("translate(0, 0, 0)"),  std::string::npos);
    EXPECT_NE(r.flat_text.find("translate(0, 24, 0)"), std::string::npos);
}

// ─── Circular pattern ───────────────────────────────────────────────

TEST(PatternUnroller, Circular3OnZ) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<pattern type=\"circular\" count=\"3\" axis=\"z\">"
        "<circle r=\"1\"/>"
        "</pattern>"
        "</part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(count_substr(r.flat_text, "<circle"), 3u);
    // Default angle is 360°, divided by 3 = 120° steps.
    EXPECT_NE(r.flat_text.find("rotate(0, 0, 0, 1)"),   std::string::npos);
    EXPECT_NE(r.flat_text.find("rotate(120, 0, 0, 1)"), std::string::npos);
    EXPECT_NE(r.flat_text.find("rotate(240, 0, 0, 1)"), std::string::npos);
}

TEST(PatternUnroller, CircularPartialAngle) {
    // count=4 over 180° → 0, 45, 90, 135
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<pattern type=\"circular\" count=\"4\" axis=\"z\" angle=\"180\">"
        "<circle r=\"1\"/>"
        "</pattern>"
        "</part>");
    EXPECT_TRUE(r.ok());
    EXPECT_NE(r.flat_text.find("rotate(0, 0, 0, 1)"),   std::string::npos);
    EXPECT_NE(r.flat_text.find("rotate(45, 0, 0, 1)"),  std::string::npos);
    EXPECT_NE(r.flat_text.find("rotate(90, 0, 0, 1)"),  std::string::npos);
    EXPECT_NE(r.flat_text.find("rotate(135, 0, 0, 1)"), std::string::npos);
}

TEST(PatternUnroller, CircularOnX) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<pattern type=\"circular\" count=\"2\" axis=\"+x\">"
        "<circle r=\"1\"/>"
        "</pattern>"
        "</part>");
    EXPECT_TRUE(r.ok());
    EXPECT_NE(r.flat_text.find("rotate(0, 1, 0, 0)"),   std::string::npos);
    EXPECT_NE(r.flat_text.find("rotate(180, 1, 0, 0)"), std::string::npos);
}

// ─── Iteration metadata ────────────────────────────────────────────

TEST(PatternUnroller, EachIterationHasIterationAttribute) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<pattern type=\"linear\" count=\"3\" axis=\"+x\" spacing=\"5\">"
        "<circle r=\"1\"/>"
        "</pattern>"
        "</part>");
    EXPECT_TRUE(r.ok());
    EXPECT_NE(r.flat_text.find("iteration=\"0\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("iteration=\"2\""), std::string::npos);
}

// ─── Multiple body children ────────────────────────────────────────

TEST(PatternUnroller, MultipleChildrenPerIteration) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<pattern type=\"linear\" count=\"2\" axis=\"+x\" spacing=\"10\">"
        "<circle r=\"1\"/>"
        "<rect width=\"1\" height=\"1\"/>"
        "</pattern>"
        "</part>");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(count_substr(r.flat_text, "<circle"), 2u);
    EXPECT_EQ(count_substr(r.flat_text, "<rect"),   2u);
}

// ─── Nested <pattern> ──────────────────────────────────────────────

TEST(PatternUnroller, NestedPatterns) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<pattern type=\"linear\" count=\"2\" axis=\"+x\" spacing=\"10\">"
        "<pattern type=\"linear\" count=\"3\" axis=\"+y\" spacing=\"5\">"
        "<circle r=\"1\"/>"
        "</pattern>"
        "</pattern>"
        "</part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // 2 × 3 = 6 circles.
    EXPECT_EQ(count_substr(r.flat_text, "<circle"), 6u);
    // No <pattern> remains.
    EXPECT_EQ(r.flat_text.find("<pattern"), std::string::npos);
}

// ─── Pattern using a def ───────────────────────────────────────────

TEST(PatternUnroller, CircularPatternOfImportedPart) {
    auto r = cs(
        "version 0.1\n"
        "<def name=\"blade\"><circle r=\"3\"/></def>"
        "<part>"
        "<pattern type=\"circular\" count=\"3\" axis=\"z\">"
        "<blade/>"
        "</pattern>"
        "</part>");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(count_substr(r.flat_text, "<blade"), 3u);
}

// ─── Validation errors ─────────────────────────────────────────────

TEST(PatternUnroller, MissingCountErrors) {
    auto r = cs(
        "version 0.1\n"
        "<part><pattern type=\"linear\" axis=\"+x\" spacing=\"5\"><circle r=\"1\"/></pattern></part>");
    EXPECT_FALSE(r.ok());
}

TEST(PatternUnroller, ZeroCountErrors) {
    auto r = cs(
        "version 0.1\n"
        "<part><pattern type=\"linear\" count=\"0\" axis=\"+x\" spacing=\"5\"><circle r=\"1\"/></pattern></part>");
    EXPECT_FALSE(r.ok());
}

TEST(PatternUnroller, UnknownTypeErrors) {
    auto r = cs(
        "version 0.1\n"
        "<part><pattern type=\"spiral\" count=\"3\" axis=\"+x\"><circle r=\"1\"/></pattern></part>");
    EXPECT_FALSE(r.ok());
}

TEST(PatternUnroller, LinearWithoutSpacingErrors) {
    auto r = cs(
        "version 0.1\n"
        "<part><pattern type=\"linear\" count=\"3\" axis=\"+x\"><circle r=\"1\"/></pattern></part>");
    EXPECT_FALSE(r.ok());
}

// ─── Realistic propeller pattern ──────────────────────────────────

TEST(PatternUnroller, PropellerWithThreeBlades) {
    auto r = cs(
        "version 0.1\n"
        "<def name=\"blade\">"
        "<extrude height=\"50\"><circle r=\"1\"/></extrude>"
        "</def>"
        "<part>"
        "<union>"
        "<extrude height=\"6\"><circle r=\"6\"/></extrude>"
        "<pattern type=\"circular\" count=\"3\" axis=\"z\">"
        "<blade/>"
        "</pattern>"
        "</union>"
        "</part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(count_substr(r.flat_text, "<blade"),  3u);
    EXPECT_EQ(count_substr(r.flat_text, "rotate("), 3u);
}
