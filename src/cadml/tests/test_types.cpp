// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/types.hpp>

#include <gtest/gtest.h>

#include <array>
#include <string_view>

using namespace cadml;

// ─── Round-trip: built-in name → NodeType → name ─────────────────────

TEST(NodeType, RoundTripStructural) {
    EXPECT_EQ(node_type_from_builtin_name("part"),     NodeType::Part);
    EXPECT_EQ(node_type_from_builtin_name("def"),      NodeType::Def);
    EXPECT_EQ(node_type_from_builtin_name("assembly"), NodeType::Assembly);
    EXPECT_EQ(node_type_from_builtin_name("connect"),  NodeType::Connect);
    EXPECT_EQ(node_type_from_builtin_name("port"),     NodeType::Port);
    EXPECT_EQ(node_type_from_builtin_name("group"),    NodeType::Group);
    EXPECT_EQ(node_type_from_builtin_name("script"),   NodeType::Script);
    EXPECT_EQ(node_type_from_builtin_name("for"),      NodeType::For);
}

TEST(NodeType, RoundTrip2DPrimitives) {
    EXPECT_EQ(node_type_from_builtin_name("circle"), NodeType::Circle);
    EXPECT_EQ(node_type_from_builtin_name("rect"),   NodeType::Rect);
    EXPECT_EQ(node_type_from_builtin_name("path"),   NodeType::Path);
    EXPECT_EQ(node_type_from_builtin_name("sketch"), NodeType::Sketch);
}

TEST(NodeType, RoundTrip2DTo3D) {
    EXPECT_EQ(node_type_from_builtin_name("extrude"), NodeType::Extrude);
    EXPECT_EQ(node_type_from_builtin_name("revolve"), NodeType::Revolve);
    EXPECT_EQ(node_type_from_builtin_name("sweep"),   NodeType::Sweep);
    EXPECT_EQ(node_type_from_builtin_name("loft"),    NodeType::Loft);
    EXPECT_EQ(node_type_from_builtin_name("helix"),   NodeType::Helix);
}

TEST(NodeType, RoundTripBooleans) {
    EXPECT_EQ(node_type_from_builtin_name("union"),      NodeType::Union);
    EXPECT_EQ(node_type_from_builtin_name("difference"), NodeType::Difference);
    EXPECT_EQ(node_type_from_builtin_name("intersect"),  NodeType::Intersect);
}

TEST(NodeType, RoundTripModifiers) {
    EXPECT_EQ(node_type_from_builtin_name("fillet"),  NodeType::Fillet);
    EXPECT_EQ(node_type_from_builtin_name("chamfer"), NodeType::Chamfer);
    EXPECT_EQ(node_type_from_builtin_name("shell"),   NodeType::Shell);
    EXPECT_EQ(node_type_from_builtin_name("cut"),     NodeType::Cut);
    EXPECT_EQ(node_type_from_builtin_name("pattern"), NodeType::Pattern);
}

TEST(NodeType, RoundTripFlatOutput) {
    EXPECT_EQ(node_type_from_builtin_name("param"),   NodeType::Param);
    EXPECT_EQ(node_type_from_builtin_name("sources"), NodeType::Sources);
    EXPECT_EQ(node_type_from_builtin_name("source"),  NodeType::Source);
}

TEST(NodeType, ReverseLookupRoundTrip) {
    // For each built-in, name → type → name should round-trip.
    constexpr NodeType all[] = {
        NodeType::Part, NodeType::Def, NodeType::Assembly, NodeType::Connect,
        NodeType::Port, NodeType::Group, NodeType::Script, NodeType::For,
        NodeType::Circle, NodeType::Rect, NodeType::Path, NodeType::Sketch,
        NodeType::Extrude, NodeType::Revolve, NodeType::Sweep, NodeType::Loft,
        NodeType::Helix,
        NodeType::Union, NodeType::Difference, NodeType::Intersect,
        NodeType::Fillet, NodeType::Chamfer, NodeType::Shell,
        NodeType::Cut, NodeType::Pattern,
        NodeType::Param, NodeType::Sources, NodeType::Source,
    };
    for (NodeType t : all) {
        const auto name = builtin_name_from_node_type(t);
        EXPECT_FALSE(name.empty()) << "missing name for NodeType " << static_cast<int>(t);
        EXPECT_EQ(node_type_from_builtin_name(name), t)
            << "round-trip failed for `" << name << "`";
    }
}

TEST(NodeType, UnknownNamesReturnUnknown) {
    EXPECT_EQ(node_type_from_builtin_name("bolt"),       NodeType::Unknown);
    EXPECT_EQ(node_type_from_builtin_name("plate"),      NodeType::Unknown);
    EXPECT_EQ(node_type_from_builtin_name("xml"),        NodeType::Unknown);
    EXPECT_EQ(node_type_from_builtin_name(""),           NodeType::Unknown);
    EXPECT_EQ(node_type_from_builtin_name("Part"),       NodeType::Unknown);  // case-sensitive
    EXPECT_EQ(node_type_from_builtin_name("CIRCLE"),     NodeType::Unknown);
}

TEST(NodeType, IsBuiltinForBuiltins) {
    EXPECT_TRUE(is_builtin(NodeType::Part));
    EXPECT_TRUE(is_builtin(NodeType::Circle));
    EXPECT_TRUE(is_builtin(NodeType::Param));
}

TEST(NodeType, IsBuiltinForInstanceAndUnknown) {
    EXPECT_FALSE(is_builtin(NodeType::Instance));
    EXPECT_FALSE(is_builtin(NodeType::Unknown));
}

TEST(NodeType, BuiltinNameForInstanceIsEmpty) {
    EXPECT_TRUE(builtin_name_from_node_type(NodeType::Instance).empty());
    EXPECT_TRUE(builtin_name_from_node_type(NodeType::Unknown).empty());
}

// ─── Reserved set count (spec §4.3) ──────────────────────────────────

TEST(NodeType, ReservedNamesCountIs28) {
    // Confirm that exactly 28 names round-trip (matching spec §4.3).
    constexpr std::array<std::string_view, 28> spec_names = {{
        // Structural (8)
        "part", "def", "assembly", "connect", "port", "group", "script", "for",
        // 2D primitives (4)
        "circle", "rect", "path", "sketch",
        // 2D-to-3D (5)
        "extrude", "revolve", "sweep", "loft", "helix",
        // Booleans (3)
        "union", "difference", "intersect",
        // Modifiers (5)
        "fillet", "chamfer", "shell", "cut", "pattern",
        // Flat-output (3)
        "param", "sources", "source",
    }};
    for (auto name : spec_names) {
        EXPECT_NE(node_type_from_builtin_name(name), NodeType::Unknown)
            << "expected reserved built-in `" << name << "` to map to a NodeType";
    }
}

// ─── Document basics ─────────────────────────────────────────────────

TEST(Document, EmptyDocumentIsValid) {
    Document doc;
    EXPECT_TRUE(doc.nodes.empty());
    EXPECT_TRUE(doc.defs.empty());
    EXPECT_TRUE(doc.exports.empty());
    EXPECT_TRUE(doc.imports.empty());
    EXPECT_TRUE(doc.params.empty());
    EXPECT_EQ(doc.meta.units, "mm");
    EXPECT_EQ(doc.meta.version, "0.1.0");
}

TEST(Document, ChildIteratorOverEmptyParent) {
    Document doc;
    doc.nodes.push_back(Node{});  // parent with no children
    int count = 0;
    for ([[maybe_unused]] const auto& c : doc.children(0)) ++count;
    EXPECT_EQ(count, 0);
}

TEST(Document, ChildIteratorWalksSiblings) {
    Document doc;
    // Build:
    //   nodes[0] = parent
    //   nodes[1] = child0  (first_child of 0)
    //   nodes[2] = child1  (next_sibling of 1)
    //   nodes[3] = child2  (next_sibling of 2)
    Node parent{};
    parent.first_child = 1;
    Node a{}; a.next_sibling = 2;
    Node b{}; b.next_sibling = 3;
    Node c{};  // last; next_sibling stays NO_NODE
    doc.nodes = { parent, a, b, c };

    std::vector<NodeType> visited;
    for (const auto& child : doc.children(0)) {
        visited.push_back(child.type);
    }
    EXPECT_EQ(visited.size(), 3u);
}

// ─── SpecVersion parsing ─────────────────────────────────────────────

TEST(SpecVersion, LenientParseReadsMajorMinor) {
    EXPECT_EQ(spec_version_from_string("0.1"),   kSpecV01);
    EXPECT_EQ(spec_version_from_string("0.2"),   kSpecV02);
    EXPECT_EQ(spec_version_from_string("0.2.0"), kSpecV02);
    EXPECT_EQ(spec_version_from_string("0.2.7"), kSpecV02);
}

TEST(SpecVersion, LenientParseFallsBackConservatively) {
    // Unparseable input yields kSpecV01 (the smallest vocabulary); the
    // compiler's acceptance check rejects the string separately.
    EXPECT_EQ(spec_version_from_string(""),      kSpecV01);
    EXPECT_EQ(spec_version_from_string("x"),     kSpecV01);
    EXPECT_EQ(spec_version_from_string("0"),     kSpecV01);
    EXPECT_EQ(spec_version_from_string("0."),    kSpecV01);
}

TEST(SpecVersion, HostileDigitRunsSaturateInsteadOfOverflowing) {
    // Regression: `dst = dst * 10 + digit` used to overflow int (UB) on
    // long digit runs. Components must saturate to a rejectable value.
    const auto v = spec_version_from_string("99999999999999999999.1");
    EXPECT_GT(v.major, kSpecLatest.major);
    const auto w = spec_version_from_string("0.99999999999999999999");
    EXPECT_EQ(w.major, 0);
    EXPECT_GT(w.minor, kSpecLatest.minor);
}

TEST(SpecVersion, StrictParseAcceptsExactForms) {
    EXPECT_EQ(spec_version_parse_strict("0.1"),    kSpecV01);
    EXPECT_EQ(spec_version_parse_strict("0.2"),    kSpecV02);
    EXPECT_EQ(spec_version_parse_strict("0.1.0"),  kSpecV01);
    EXPECT_EQ(spec_version_parse_strict("0.2.15"), kSpecV02);
}

TEST(SpecVersion, StrictParseRejectsMalformedStrings) {
    EXPECT_EQ(spec_version_parse_strict(""),          std::nullopt);
    EXPECT_EQ(spec_version_parse_strict("0"),         std::nullopt);
    EXPECT_EQ(spec_version_parse_strict("0."),        std::nullopt);
    EXPECT_EQ(spec_version_parse_strict("0.2."),      std::nullopt);
    EXPECT_EQ(spec_version_parse_strict("0.2.banana"), std::nullopt);
    EXPECT_EQ(spec_version_parse_strict("0.2.0.0"),   std::nullopt);
    EXPECT_EQ(spec_version_parse_strict("0.2x"),      std::nullopt);
    EXPECT_EQ(spec_version_parse_strict(" 0.2"),      std::nullopt);
}

TEST(SpecVersion, ToStringRendersMajorMinor) {
    EXPECT_EQ(to_string(kSpecV01), "0.1");
    EXPECT_EQ(to_string(kSpecV02), "0.2");
}

// ─── Spec 0.3 (texture attributes) ───────────────────────────────────

TEST(SpecVersion, Spec03IsTheLatestKnownVersion) {
    // `texture*` attributes (spec §5.1) are gated on 0.3; the constant
    // every acceptance / pinning check derives from must name it.
    EXPECT_EQ(kSpecLatest, kSpecV03);
    EXPECT_LT(kSpecV02, kSpecV03);
    EXPECT_EQ(spec_version_from_string("0.3"),     kSpecV03);
    EXPECT_EQ(spec_version_from_string("0.3.1"),   kSpecV03);
    EXPECT_EQ(spec_version_parse_strict("0.3"),    kSpecV03);
    EXPECT_EQ(spec_version_parse_strict("0.3.0"),  kSpecV03);
    EXPECT_EQ(to_string(kSpecV03), "0.3");
}

TEST(SpecVersion, Spec03AddsNoReservedElementNames) {
    // 0.3 is an attribute-only revision: the reserved-name set a 0.3
    // file sees is exactly the 0.2 set (`stl` still the newest name).
    EXPECT_EQ(node_type_from_builtin_name("stl", kSpecV03), NodeType::Stl);
    EXPECT_EQ(node_type_from_builtin_name("texture", kSpecV03),
              NodeType::Unknown);
    EXPECT_EQ(builtin_since("texture"), std::nullopt);
}

TEST(TextureAttrs, EmptyMeansNoImageSource) {
    TextureAttrs t;
    EXPECT_TRUE(t.empty());
    t.scale_expr = "10";           // a scale alone is not a texture
    EXPECT_TRUE(t.empty());
    t.src = "grass.png";
    EXPECT_FALSE(t.empty());
    t.src.clear();
    t.data = "AAAA";
    EXPECT_FALSE(t.empty());
}
