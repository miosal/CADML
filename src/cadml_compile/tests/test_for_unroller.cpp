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

// Count occurrences of a substring in a string.
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

// ─── Uniform-range form ─────────────────────────────────────────────

TEST(ForUnroller, BasicUniformRange) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<for var=\"i\" from=\"0\" to=\"4\" steps=\"5\">"
        "<circle r=\"{i}\"/>"
        "</for>"
        "</part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // 5 iterations → 5 circles, each wrapped in a group.
    EXPECT_EQ(count_substr(r.flat_text, "<circle"), 5u);
    // Each iteration produces a group with iteration="N".
    EXPECT_NE(r.flat_text.find("iteration=\"0\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("iteration=\"4\""), std::string::npos);
    // Variable substitution: {i} → {0}, {1}, ... braces preserved.
    EXPECT_NE(r.flat_text.find("r=\"0\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("r=\"4\""), std::string::npos);
    // The original <for> is gone.
    EXPECT_EQ(r.flat_text.find("<for "), std::string::npos);
}

TEST(ForUnroller, SingleStep) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<for var=\"i\" from=\"5\" to=\"5\" steps=\"1\">"
        "<circle r=\"{i}\"/>"
        "</for>"
        "</part>");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(count_substr(r.flat_text, "<circle"), 1u);
    EXPECT_NE(r.flat_text.find("r=\"5\""), std::string::npos);
}

TEST(ForUnroller, FractionalRange) {
    // from=0 to=1 steps=3 → values 0, 0.5, 1
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<for var=\"t\" from=\"0\" to=\"1\" steps=\"3\">"
        "<circle r=\"{t}\"/>"
        "</for>"
        "</part>");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(count_substr(r.flat_text, "<circle"), 3u);
    EXPECT_NE(r.flat_text.find("r=\"0\""),   std::string::npos);
    EXPECT_NE(r.flat_text.find("r=\"0.5\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("r=\"1\""),   std::string::npos);
}

TEST(ForUnroller, BoundsReferenceParam) {
    auto r = cs(
        "version 0.1\n"
        "param n = 4\n"
        "<part>"
        "<for var=\"i\" from=\"0\" to=\"{n - 1}\" steps=\"{n}\">"
        "<circle r=\"{i + 1}\"/>"
        "</for>"
        "</part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(count_substr(r.flat_text, "<circle"), 4u);
}

// ─── Explicit-values form ───────────────────────────────────────────

TEST(ForUnroller, NumericValuesList) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<for var=\"v\" values=\"1 3 7 15\">"
        "<circle r=\"{v}\"/>"
        "</for>"
        "</part>");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(count_substr(r.flat_text, "<circle"), 4u);
    EXPECT_NE(r.flat_text.find("r=\"1\""),  std::string::npos);
    EXPECT_NE(r.flat_text.find("r=\"15\""), std::string::npos);
}

TEST(ForUnroller, StringValuesList) {
    auto r = cs(
        "version 0.1\n"
        "<def name=\"hex-bolt\"><circle r=\"5\"/></def>"
        "<part>"
        "<for var=\"c\" values=\"nw ne sw se\">"
        "<hex-bolt id=\"bolt-{c}\"/>"
        "</for>"
        "</part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_NE(r.flat_text.find("id=\"bolt-nw\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("id=\"bolt-ne\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("id=\"bolt-sw\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("id=\"bolt-se\""), std::string::npos);
}

TEST(ForUnroller, EmptyValuesListErrors) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<for var=\"i\" values=\"\"><circle r=\"1\"/></for>"
        "</part>");
    EXPECT_FALSE(r.ok());
}

// ─── Loop variable substitution edge cases ─────────────────────────

TEST(ForUnroller, VariableInsideLargerIdentifierNotReplaced) {
    auto r = cs(
        "version 0.1\n"
        "param index = 100\n"
        "<part>"
        "<for var=\"i\" values=\"5\">"
        "<circle r=\"{index}\"/>"  // {index} should NOT become {1005}
        "</for>"
        "</part>");
    EXPECT_TRUE(r.ok());
    EXPECT_NE(r.flat_text.find("r=\"{index}\""), std::string::npos);
}

TEST(ForUnroller, LoopVariableInExpression) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<for var=\"i\" values=\"3\">"
        "<circle r=\"{i * 2 + 1}\"/>"
        "</for>"
        "</part>");
    EXPECT_TRUE(r.ok());
    EXPECT_NE(r.flat_text.find("r=\"{3 * 2 + 1}\""), std::string::npos);
}

TEST(ForUnroller, LoopVariableInTransform) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<for var=\"i\" values=\"0 10 20\">"
        "<group transform=\"translate({i}, 0, 0)\">"
        "<circle r=\"1\"/>"
        "</group>"
        "</for>"
        "</part>");
    EXPECT_TRUE(r.ok());
    // Whole-block {i} → bare value (no braces).
    EXPECT_NE(r.flat_text.find("translate(0, 0, 0)"),  std::string::npos);
    EXPECT_NE(r.flat_text.find("translate(10, 0, 0)"), std::string::npos);
    EXPECT_NE(r.flat_text.find("translate(20, 0, 0)"), std::string::npos);
}

// ─── Multiple body children ────────────────────────────────────────

TEST(ForUnroller, MultipleChildrenPerIteration) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<for var=\"i\" values=\"0 1\">"
        "<circle r=\"{i}\"/>"
        "<rect width=\"{i}\" height=\"{i}\"/>"
        "</for>"
        "</part>");
    EXPECT_TRUE(r.ok());
    // 2 iterations × 2 children = 4 elements (under 2 group wrappers).
    EXPECT_EQ(count_substr(r.flat_text, "<circle"), 2u);
    EXPECT_EQ(count_substr(r.flat_text, "<rect"),   2u);
}

TEST(ForUnroller, EmptyBodyOK) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<for var=\"i\" values=\"1 2 3\"></for>"
        "</part>");
    EXPECT_TRUE(r.ok());
    // No iteration groups since body is empty.
    EXPECT_EQ(r.flat_text.find("<for"), std::string::npos);
}

// ─── Nested <for> ──────────────────────────────────────────────────

TEST(ForUnroller, NestedFor) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<for var=\"i\" values=\"0 1\">"
        "<for var=\"j\" values=\"0 1 2\">"
        "<circle r=\"{i + j * 10}\"/>"
        "</for>"
        "</for>"
        "</part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // 2 × 3 = 6 circles total.
    EXPECT_EQ(count_substr(r.flat_text, "<circle"), 6u);
    // Sample expected values.
    EXPECT_NE(r.flat_text.find("r=\"{0 + 0 * 10}\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("r=\"{1 + 2 * 10}\""), std::string::npos);
}

// ─── Validation errors ─────────────────────────────────────────────

TEST(ForUnroller, MissingVarErrors) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<for from=\"0\" to=\"3\" steps=\"4\"><circle r=\"1\"/></for>"
        "</part>");
    EXPECT_FALSE(r.ok());
}

TEST(ForUnroller, MissingBoundsErrors) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<for var=\"i\"><circle r=\"1\"/></for>"
        "</part>");
    EXPECT_FALSE(r.ok());
}

TEST(ForUnroller, NonResolvableBoundsErrors) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<for var=\"i\" from=\"0\" to=\"{undefined}\" steps=\"4\">"
        "<circle r=\"1\"/>"
        "</for>"
        "</part>");
    EXPECT_FALSE(r.ok());
}

TEST(ForUnroller, ZeroStepsErrors) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<for var=\"i\" from=\"0\" to=\"3\" steps=\"0\">"
        "<circle r=\"1\"/>"
        "</for>"
        "</part>");
    EXPECT_FALSE(r.ok());
}

// ─── Iteration metadata ────────────────────────────────────────────

TEST(ForUnroller, EachIterationHasIterationAttribute) {
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<for var=\"i\" values=\"a b c\"><circle r=\"1\"/></for>"
        "</part>");
    EXPECT_TRUE(r.ok());
    EXPECT_NE(r.flat_text.find("iteration=\"0\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("iteration=\"1\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("iteration=\"2\""), std::string::npos);
}

// ─── Realistic CADML pattern ────────────────────────────────────────

TEST(ForUnroller, BlueprintHolesPattern) {
    // Drawn from the existing skill: array of cooling holes via <for>.
    // Param names use kebab-case (spec §2.8 forbids underscores).
    auto r = cs(
        "version 0.1\n"
        "param wall = 5\n"
        "param spacing = 12\n"
        "param hole-r = 2\n"
        "param overshoot = 1\n"
        "<part>"
        "<difference>"
        "<extrude height=\"{wall}\">"
        "<rect x=\"0\" y=\"0\" width=\"60\" height=\"20\"/>"
        "</extrude>"
        "<for var=\"i\" from=\"0\" to=\"4\" steps=\"5\">"
        "<group transform=\"translate({spacing * i}, 10, {-overshoot})\">"
        "<extrude height=\"{wall + overshoot * 2}\">"
        "<circle r=\"{hole-r}\"/>"
        "</extrude>"
        "</group>"
        "</for>"
        "</difference>"
        "</part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // Outer rect-extrude + 5 hole-extrudes = 6 total extrudes.
    EXPECT_EQ(count_substr(r.flat_text, "<extrude"), 6u);
    // Each hole has its iteration's spacing baked in (substitution
    // preserves braces because expr is not whole-block match).
    EXPECT_NE(r.flat_text.find("translate({spacing * 0}"), std::string::npos);
    EXPECT_NE(r.flat_text.find("translate({spacing * 4}"), std::string::npos);
}

// ─── Shadow detection ───────────────────────────────────────────────

TEST(ForUnroller, LoopVariableShadowingParamErrors) {
    // The textual `{var}` substitution would silently capture the
    // matching param name. Force authors to rename one of the two.
    auto r = cs(
        "version 0.1\n"
        "param r = 5\n"
        "<part>"
        "<for var=\"r\" from=\"0\" to=\"3\" steps=\"3\">"
        "<circle r=\"{r}\"/>"
        "</for>"
        "</part>");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.errors[0].category, CompileError::Composition);
    EXPECT_NE(r.errors[0].message.find("shadows"), std::string::npos);
    EXPECT_NE(r.errors[0].message.find("`r`"), std::string::npos);
}

TEST(ForUnroller, LoopVariableDistinctFromParamCompilesClean) {
    auto r = cs(
        "version 0.1\n"
        "param r = 5\n"
        "<part>"
        "<for var=\"i\" from=\"0\" to=\"3\" steps=\"3\">"
        "<circle r=\"{r}\"/>"
        "</for>"
        "</part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
}
