// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include "subtree_ops.hpp"

#include <cadml/parser.hpp>

#include <gtest/gtest.h>

using namespace cadml;
using namespace cadml::compile::detail;

namespace {

Document parse_doc(std::string_view src) {
    auto r = parse(src);
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    return std::move(r.document);
}

}  // namespace

// ─── substitute_var_in_string ───────────────────────────────────────

TEST(SubstituteVar, ReplacesIdentifierInsideExpression) {
    // Identifier inside larger brace expression — braces preserved.
    EXPECT_EQ(substitute_var_in_string("{i + 1}", "i", "5"), "{5 + 1}");
    // Bare identifier outside braces is literal text — unchanged.
    EXPECT_EQ(substitute_var_in_string("i", "i", "42"), "i");
}

TEST(SubstituteVar, WholeBlockSubstitutionStripsBraces) {
    // `{var}` with no other content → value with braces stripped.
    EXPECT_EQ(substitute_var_in_string("{i}",   "i", "5"),  "5");
    EXPECT_EQ(substitute_var_in_string("{c}",   "c", "nw"), "nw");
    EXPECT_EQ(substitute_var_in_string("{ i }", "i", "5"),  "5");  // whitespace OK
}

TEST(SubstituteVar, MixedTextAndWholeBlock) {
    // bolt-{c} → bolt-nw (literal text concatenation works).
    EXPECT_EQ(substitute_var_in_string("bolt-{c}", "c", "nw"), "bolt-nw");
    EXPECT_EQ(substitute_var_in_string("hole-{i}-pin", "i", "3"),
                "hole-3-pin");
}

TEST(SubstituteVar, DoesNotReplaceInsideLargerIdentifier) {
    EXPECT_EQ(substitute_var_in_string("{index}", "i", "5"), "{index}");
    EXPECT_EQ(substitute_var_in_string("{ix}",    "i", "5"), "{ix}");
    EXPECT_EQ(substitute_var_in_string("{abc-i}", "i", "5"), "{abc-i}");
    EXPECT_EQ(substitute_var_in_string("{i-abc}", "i", "5"), "{i-abc}");
}

TEST(SubstituteVar, MultipleOccurrences) {
    EXPECT_EQ(substitute_var_in_string("{i + i * i}", "i", "3"),
                "{3 + 3 * 3}");
}

TEST(SubstituteVar, KebabCaseTarget) {
    EXPECT_EQ(substitute_var_in_string("{my-var * 2}", "my-var", "10"),
                "{10 * 2}");
}

TEST(SubstituteVar, EmptyVarReturnsUnchanged) {
    EXPECT_EQ(substitute_var_in_string("anything", "", "X"), "anything");
}

TEST(SubstituteVar, EmptyStringReturnsEmpty) {
    EXPECT_EQ(substitute_var_in_string("", "i", "5"), "");
}

TEST(SubstituteVar, NoMatch) {
    EXPECT_EQ(substitute_var_in_string("{j + 1}", "i", "5"), "{j + 1}");
}

TEST(SubstituteVar, MixedTextAndIdentifier) {
    // For values like "hole-{i}" with i=2 → "hole-2"
    EXPECT_EQ(substitute_var_in_string("hole-{i}", "i", "2"), "hole-2");
}

// ─── collect_subtree ────────────────────────────────────────────────

TEST(CollectSubtree, SingleNode) {
    auto doc = parse_doc("version 0.1\n<part/>");
    auto indices = collect_subtree(doc, 0);
    ASSERT_EQ(indices.size(), 1u);
    EXPECT_EQ(indices[0], 0u);
}

TEST(CollectSubtree, NestedTree) {
    auto doc = parse_doc(
        "version 0.1\n"
        "<part>"
        "<extrude height=\"10\"><circle r=\"5\"/></extrude>"
        "<rect width=\"1\" height=\"1\"/>"
        "</part>");
    auto indices = collect_subtree(doc, 0);
    // part, extrude, circle, rect = 4 nodes
    EXPECT_EQ(indices.size(), 4u);
    // First is the root.
    EXPECT_EQ(indices[0], 0u);
}

// ─── clone_subtree ──────────────────────────────────────────────────

TEST(CloneSubtree, SimpleClone) {
    auto doc = parse_doc(
        "version 0.1\n"
        "<part><circle r=\"5\"/></part>");
    const auto orig_size = doc.nodes.size();
    const auto new_root = clone_subtree(doc, 1, /*new_parent=*/0);
    EXPECT_GE(doc.nodes.size(), orig_size + 1);
    EXPECT_NE(new_root, NO_NODE);
    EXPECT_EQ(doc.nodes[new_root].type, NodeType::Circle);
    EXPECT_EQ(doc.nodes[new_root].parent, 0u);
}

TEST(CloneSubtree, DeepClonePreservesStructure) {
    auto doc = parse_doc(
        "version 0.1\n"
        "<part>"
        "<extrude height=\"10\"><circle r=\"5\"/></extrude>"
        "</part>");
    const auto extrude_idx = doc.nodes[0].first_child;
    const auto orig_size = doc.nodes.size();
    const auto new_root = clone_subtree(doc, extrude_idx, /*new_parent=*/0);
    // Cloned: extrude + circle = 2 new nodes.
    EXPECT_EQ(doc.nodes.size(), orig_size + 2);
    EXPECT_EQ(doc.nodes[new_root].type, NodeType::Extrude);
    // The cloned extrude has a child.
    const auto cloned_circle = doc.nodes[new_root].first_child;
    ASSERT_NE(cloned_circle, NO_NODE);
    EXPECT_EQ(doc.nodes[cloned_circle].type, NodeType::Circle);
    EXPECT_EQ(doc.nodes[cloned_circle].parent, new_root);
}

TEST(CloneSubtree, IterationFlagSet) {
    auto doc = parse_doc("version 0.1\n<part/>");
    const auto new_root = clone_subtree(doc, 0, NO_NODE, /*iteration=*/3);
    EXPECT_EQ(doc.nodes[new_root].iteration, 3u);
}

TEST(CloneSubtree, OriginalUnchanged) {
    auto doc = parse_doc(
        "version 0.1\n"
        "<part><circle r=\"5\"/></part>");
    const auto orig_circle_idx = doc.nodes[0].first_child;
    const auto orig_first_child = doc.nodes[0].first_child;
    clone_subtree(doc, orig_circle_idx, /*new_parent=*/0);
    // Original part should still have its original child.
    EXPECT_EQ(doc.nodes[0].first_child, orig_first_child);
}

// ─── unlink_from_parent ─────────────────────────────────────────────

TEST(UnlinkFromParent, RemoveOnlyChild) {
    auto doc = parse_doc(
        "version 0.1\n"
        "<part><circle r=\"5\"/></part>");
    const auto circle_idx = doc.nodes[0].first_child;
    const auto prev = unlink_from_parent(doc, circle_idx);
    EXPECT_EQ(prev, NO_NODE);
    EXPECT_EQ(doc.nodes[0].first_child, NO_NODE);
}

TEST(UnlinkFromParent, RemoveMiddleChild) {
    auto doc = parse_doc(
        "version 0.1\n"
        "<part>"
        "<circle r=\"1\"/>"
        "<rect width=\"2\" height=\"2\"/>"
        "<circle r=\"3\"/>"
        "</part>");
    const auto first  = doc.nodes[0].first_child;            // r=1
    const auto middle = doc.nodes[first].next_sibling;       // rect
    const auto last   = doc.nodes[middle].next_sibling;      // r=3
    const auto prev = unlink_from_parent(doc, middle);
    EXPECT_EQ(prev, first);
    // First should now point directly at last.
    EXPECT_EQ(doc.nodes[first].next_sibling, last);
}

TEST(UnlinkFromParent, RemoveFirstOfMany) {
    auto doc = parse_doc(
        "version 0.1\n"
        "<part>"
        "<circle r=\"1\"/>"
        "<rect width=\"2\" height=\"2\"/>"
        "</part>");
    const auto first  = doc.nodes[0].first_child;
    const auto second = doc.nodes[first].next_sibling;
    const auto prev = unlink_from_parent(doc, first);
    EXPECT_EQ(prev, NO_NODE);
    EXPECT_EQ(doc.nodes[0].first_child, second);
}
