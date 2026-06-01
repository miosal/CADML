// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/parser.hpp>

#include <gtest/gtest.h>

using namespace cadml;

namespace {

BodyResult parse_body_str(std::string_view body, std::uint32_t line_offset = 1) {
    return parse_body(body, /*file_id=*/0, line_offset);
}

}  // namespace

// ─── Empty + whitespace ──────────────────────────────────────────────

TEST(BodyParser, EmptyInput) {
    auto r = parse_body_str("");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.nodes.size(), 0u);
}

TEST(BodyParser, WhitespaceOnlyInput) {
    auto r = parse_body_str("\n  \n");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.nodes.size(), 0u);
}

// ─── Built-in dispatch ────────────────────────────────────────────────

TEST(BodyParser, ExtrudeWithCircleChild) {
    auto r = parse_body_str(R"(<extrude height="10"><circle r="5"/></extrude>)");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.nodes.size(), 2u);
    EXPECT_EQ(r.nodes[0].type, NodeType::Extrude);
    EXPECT_EQ(r.nodes[1].type, NodeType::Circle);
    EXPECT_EQ(r.nodes[0].first_child, 1u);

    const auto& ext = std::get<ExtrudeAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(ext.height_expr, "10");
    EXPECT_EQ(ext.scale_expr, "1");      // default
    EXPECT_EQ(ext.direction_expr, "+z"); // default
    EXPECT_FALSE(ext.symmetric);

    const auto& cir = std::get<CircleAttrs>(r.nodes[1].attrs);
    EXPECT_EQ(cir.r_expr, "5");
}

TEST(BodyParser, RectAttributes) {
    auto r = parse_body_str(
        R"(<rect x="-10" y="-5" width="20" height="10" rx="2"/>)");
    EXPECT_TRUE(r.ok());
    const auto& rect = std::get<RectAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(rect.x_expr, "-10");
    EXPECT_EQ(rect.y_expr, "-5");
    EXPECT_EQ(rect.width_expr, "20");
    EXPECT_EQ(rect.height_expr, "10");
    EXPECT_EQ(rect.rx_expr, "2");
}

TEST(BodyParser, PathPreservesD) {
    auto r = parse_body_str(R"(<path d="M 0,0 L 10,0 L 10,10 Z"/>)");
    EXPECT_TRUE(r.ok());
    const auto& p = std::get<PathAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(p.d, "M 0,0 L 10,0 L 10,10 Z");
}

TEST(BodyParser, SketchWithAllAttrs) {
    auto r = parse_body_str(
        R"(<sketch plane="xz" origin="0 5 0" rotation="45" normal="0 1 0"/>)");
    EXPECT_TRUE(r.ok());
    const auto& s = std::get<SketchAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(s.plane, "xz");
    EXPECT_EQ(s.origin_expr, "0 5 0");
    EXPECT_EQ(s.rotation_expr, "45");
    EXPECT_EQ(s.normal_expr, "0 1 0");
}

TEST(BodyParser, SketchDefaults) {
    auto r = parse_body_str(R"(<sketch/>)");
    EXPECT_TRUE(r.ok());
    const auto& s = std::get<SketchAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(s.plane, "xy");
    EXPECT_EQ(s.origin_expr, "0 0 0");
    EXPECT_EQ(s.rotation_expr, "0");
    EXPECT_EQ(s.normal_expr, "");
}

TEST(BodyParser, RevolveAndAxis) {
    auto r = parse_body_str(R"(<revolve axis="+y" angle="180"/>)");
    EXPECT_TRUE(r.ok());
    const auto& rv = std::get<RevolveAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(rv.axis, "+y");
    EXPECT_EQ(rv.angle_expr, "180");
}

TEST(BodyParser, HelixAttrs) {
    auto r = parse_body_str(
        R"(<helix radius="5" pitch="2" turns="10" taper="0.1" direction="cw"/>)");
    EXPECT_TRUE(r.ok());
    const auto& h = std::get<HelixAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(h.radius_expr, "5");
    EXPECT_EQ(h.pitch_expr, "2");
    EXPECT_EQ(h.turns_expr, "10");
    EXPECT_EQ(h.taper_expr, "0.1");
    EXPECT_EQ(h.direction, "cw");
}

TEST(BodyParser, BooleansHaveNoAttrs) {
    auto r = parse_body_str(
        "<union><circle r=\"1\"/><circle r=\"2\"/></union>");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.nodes[0].type, NodeType::Union);
    EXPECT_TRUE(std::holds_alternative<UnionAttrs>(r.nodes[0].attrs));
}

TEST(BodyParser, DifferenceAndIntersect) {
    auto r1 = parse_body_str("<difference/>");
    EXPECT_EQ(r1.nodes[0].type, NodeType::Difference);
    auto r2 = parse_body_str("<intersect/>");
    EXPECT_EQ(r2.nodes[0].type, NodeType::Intersect);
}

TEST(BodyParser, ModifiersWithDefaults) {
    auto r = parse_body_str(R"(<fillet radius="2"/>)");
    EXPECT_TRUE(r.ok());
    const auto& f = std::get<FilletAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(f.radius_expr, "2");
    EXPECT_EQ(f.select, "all");
}

TEST(BodyParser, ChamferAttrs) {
    auto r = parse_body_str(R"(<chamfer distance="1" angle="60" select="edge:along=+x"/>)");
    const auto& c = std::get<ChamferAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(c.distance_expr, "1");
    EXPECT_EQ(c.angle_expr, "60");
    EXPECT_EQ(c.select, "edge:along=+x");
}

TEST(BodyParser, ShellAttrs) {
    auto r = parse_body_str(R"(<shell thickness="3" open="face:normal=+z"/>)");
    const auto& s = std::get<ShellAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(s.thickness_expr, "3");
    EXPECT_EQ(s.open, "face:normal=+z");
}

TEST(BodyParser, CutAttrs) {
    auto r = parse_body_str(R"(<cut face="end" type="miter" angle="-45"/>)");
    const auto& c = std::get<CutAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(c.face, "end");
    EXPECT_EQ(c.type, "miter");
    EXPECT_EQ(c.angle_expr, "-45");
}

TEST(BodyParser, CompoundCut) {
    auto r = parse_body_str(R"(<cut face="start" type="compound" miter="30" bevel="15"/>)");
    const auto& c = std::get<CutAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(c.miter_expr, "30");
    EXPECT_EQ(c.bevel_expr, "15");
}

TEST(BodyParser, PatternLinear) {
    auto r = parse_body_str(R"(<pattern type="linear" count="5" axis="+x" spacing="10"/>)");
    const auto& p = std::get<PatternAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(p.type, "linear");
    EXPECT_EQ(p.count_expr, "5");
    EXPECT_EQ(p.axis, "+x");
    EXPECT_EQ(p.spacing_expr, "10");
}

TEST(BodyParser, PatternCircular) {
    auto r = parse_body_str(R"(<pattern type="circular" count="3" axis="z"/>)");
    const auto& p = std::get<PatternAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(p.type, "circular");
    EXPECT_EQ(p.angle_expr, "360");  // default
}

TEST(BodyParser, GroupWithTransform) {
    auto r = parse_body_str(R"XML(<group transform="translate(5, 0, 0)" color="#aabbcc"/>)XML");
    const auto& g = std::get<GroupAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(g.transform, "translate(5, 0, 0)");
    EXPECT_EQ(g.color, "#aabbcc");
    EXPECT_EQ(g.id, "");
}

TEST(BodyParser, GroupWithExplicitId) {
    auto r = parse_body_str(R"(<group id="my-group"/>)");
    const auto& g = std::get<GroupAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(g.id, "my-group");
}

TEST(BodyParser, ScriptCapturesBody) {
    auto r = parse_body_str(R"(<script lang="lua">function f() return 42 end</script>)");
    const auto& s = std::get<ScriptAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(s.lang, "lua");
    EXPECT_EQ(s.source, "function f() return 42 end");
}

TEST(BodyParser, ForUniformRange) {
    auto r = parse_body_str(R"(<for var="i" from="0" to="10" steps="11"/>)");
    const auto& f = std::get<ForAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(f.var, "i");
    EXPECT_EQ(f.from_expr, "0");
    EXPECT_EQ(f.to_expr, "10");
    EXPECT_EQ(f.steps_expr, "11");
}

TEST(BodyParser, ForExplicitValues) {
    auto r = parse_body_str(R"(<for var="c" values="nw ne sw se"/>)");
    const auto& f = std::get<ForAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(f.var, "c");
    EXPECT_EQ(f.values, "nw ne sw se");
}

TEST(BodyParser, PortAttrs) {
    auto r = parse_body_str(
        R"(<port name="hole" position="0 0 6" normal="+z" up="+x"/>)");
    const auto& p = std::get<PortAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(p.name, "hole");
    EXPECT_EQ(p.position_expr, "0 0 6");
    EXPECT_EQ(p.normal_expr, "+z");
    EXPECT_EQ(p.up_expr, "+x");
}

// ─── Synthetic root ─────────────────────────────────────────────────

TEST(BodyParser, MultipleTopLevelSiblings) {
    auto r = parse_body_str(
        R"(<def name="blade"><circle r="1"/></def>)"
        R"(<part name="propeller"><circle r="5"/></part>)");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.nodes[0].type, NodeType::Def);
    // First node's next_sibling should point at the part.
    EXPECT_NE(r.nodes[0].next_sibling, NO_NODE);
    EXPECT_EQ(r.nodes[r.nodes[0].next_sibling].type, NodeType::Part);
}

// ─── Instance dispatch ──────────────────────────────────────────────

TEST(BodyParser, BareInstance) {
    auto r = parse_body_str(R"(<bolt/>)");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.nodes[0].type, NodeType::Instance);
    const auto& inst = std::get<InstanceAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(inst.ref_name, "bolt");
    EXPECT_EQ(inst.id, "");
    EXPECT_EQ(inst.at, "");
    EXPECT_EQ(inst.port, "");
    EXPECT_TRUE(inst.param_overrides.empty());
}

TEST(BodyParser, MatingInstance) {
    auto r = parse_body_str(R"(<bolt id="b1" at="hole" port="head"/>)");
    EXPECT_TRUE(r.ok());
    const auto& inst = std::get<InstanceAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(inst.id, "b1");
    EXPECT_EQ(inst.at, "hole");
    EXPECT_EQ(inst.port, "head");
}

TEST(BodyParser, InstanceWithParamOverrides) {
    auto r = parse_body_str(R"(<bolt length="20" d="8"/>)");
    EXPECT_TRUE(r.ok());
    const auto& inst = std::get<InstanceAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(inst.param_overrides.size(), 2u);
    EXPECT_EQ(inst.param_overrides.at("length"), "20");
    EXPECT_EQ(inst.param_overrides.at("d"), "8");
}

TEST(BodyParser, InstanceMixedReservedAndOverrides) {
    auto r = parse_body_str(R"(<bolt id="b1" at="hole" port="head" length="20"/>)");
    EXPECT_TRUE(r.ok());
    const auto& inst = std::get<InstanceAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(inst.id, "b1");
    EXPECT_EQ(inst.at, "hole");
    EXPECT_EQ(inst.port, "head");
    EXPECT_EQ(inst.param_overrides.size(), 1u);
    EXPECT_EQ(inst.param_overrides.at("length"), "20");
}

// ─── Param min/max strict numeric validation ────────────────────────

TEST(BodyParser, ParamMinIsValidatedStrict) {
    // The bundler used to silently treat min="abc" as min=0 (strtod
    // returns 0 on no parse). Reject it as a Schema error so the
    // author sees the real problem.
    auto r = parse_body_str(R"(<part><param name="x" value="1" min="abc"/></part>)");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.errors[0].category, ParseError::Schema);
    EXPECT_NE(r.errors[0].message.find("min"), std::string::npos);
}

TEST(BodyParser, ParamMaxIsValidatedStrict) {
    auto r = parse_body_str(R"(<part><param name="x" value="1" max="1e9999"/></part>)");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.errors[0].category, ParseError::Schema);
    EXPECT_NE(r.errors[0].message.find("max"), std::string::npos);
}

TEST(BodyParser, ParamMinMaxLiteralsParseClean) {
    auto r = parse_body_str(R"(<part><param name="x" value="1" min="0" max="10"/></part>)");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
}

// ─── Def + export tracking ──────────────────────────────────────────

TEST(BodyParser, DefIndexed) {
    auto r = parse_body_str(R"(<def name="blade"><circle r="1"/></def>)");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.defs.size(), 1u);
    EXPECT_EQ(r.defs.at("blade"), 0u);
}

TEST(BodyParser, DefMissingNameErrors) {
    auto r = parse_body_str(R"(<def><circle r="1"/></def>)");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.errors[0].category, ParseError::Schema);
}

TEST(BodyParser, DefNameCollidesWithBuiltinErrors) {
    auto r = parse_body_str(R"(<def name="circle"/>)");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.errors[0].category, ParseError::Vocabulary);
}

TEST(BodyParser, DuplicateDefErrors) {
    auto r = parse_body_str(
        R"(<def name="blade"/><def name="blade"/>)");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.errors[0].category, ParseError::Vocabulary);
}

TEST(BodyParser, PartExportTracked) {
    auto r = parse_body_str(R"(<part name="bolt"><circle r="5"/></part>)");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.exports.count("bolt"), 1u);
}

TEST(BodyParser, AssemblyExportTracked) {
    auto r = parse_body_str(R"(<assembly name="rig"/>)");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.exports.count("rig"), 1u);
}

TEST(BodyParser, AnonymousPartTrackedAsAnonymous) {
    auto r = parse_body_str(R"(<part><circle r="1"/></part>)");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.exports.count("<anonymous>"), 1u);
}

TEST(BodyParser, DuplicateExportErrors) {
    auto r = parse_body_str(R"(<part name="x"/><part name="x"/>)");
    EXPECT_FALSE(r.ok());
}

// ─── Source ranges ──────────────────────────────────────────────────

TEST(BodyParser, SourceRangeOnFirstLine) {
    auto r = parse_body_str(R"(<part/>)");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.nodes[0].source.line, 1u);
    EXPECT_GE(r.nodes[0].source.column, 1u);
}

TEST(BodyParser, SourceRangeWithLineOffset) {
    // Body parser invoked with body_line_offset=5, simulating a
    // frontmatter that consumed lines 1-4.
    auto r = parse_body("<part/>", /*file_id=*/0, /*body_line_offset=*/5);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.nodes[0].source.line, 5u);
}

TEST(BodyParser, SourceRangeOnDeeperLine) {
    auto r = parse_body_str("<part>\n  <circle r=\"5\"/>\n</part>");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.nodes.size(), 2u);
    // <circle> is on the 2nd line of the body.
    EXPECT_EQ(r.nodes[1].source.line, 2u);
}

// ─── Tree structure ─────────────────────────────────────────────────

TEST(BodyParser, ChildLinking) {
    auto r = parse_body_str(R"(<union><circle r="1"/><rect width="2" height="2"/><sketch/></union>)");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.nodes.size(), 4u);
    // node 0 = union, parent NO_NODE, first_child = 1
    EXPECT_EQ(r.nodes[0].type, NodeType::Union);
    EXPECT_EQ(r.nodes[0].first_child, 1u);
    // siblings: 1 → 2 → 3
    EXPECT_EQ(r.nodes[1].next_sibling, 2u);
    EXPECT_EQ(r.nodes[2].next_sibling, 3u);
    EXPECT_EQ(r.nodes[3].next_sibling, NO_NODE);
    // parents
    EXPECT_EQ(r.nodes[1].parent, 0u);
    EXPECT_EQ(r.nodes[2].parent, 0u);
    EXPECT_EQ(r.nodes[3].parent, 0u);
}

// ─── Malformed XML ──────────────────────────────────────────────────

TEST(BodyParser, MalformedXMLProducesParseError) {
    auto r = parse_body_str(R"(<extrude height="10")");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.errors[0].category, ParseError::Parse);
}

// ─── XML comments (H4) ─────────────────────────────────────────────

TEST(BodyParser, XMLCommentsAtTopLevelIgnored) {
    auto r = parse_body_str(
        "<!-- file header -->"
        "<part><circle r=\"5\"/></part>");
    EXPECT_TRUE(r.ok());
    // Only the part + circle nodes; comment is discarded.
    ASSERT_EQ(r.nodes.size(), 2u);
    EXPECT_EQ(r.nodes[0].type, NodeType::Part);
    EXPECT_EQ(r.nodes[1].type, NodeType::Circle);
}

TEST(BodyParser, XMLCommentsInsideElementsIgnored) {
    auto r = parse_body_str(
        "<part>"
        "<!-- a comment between children -->"
        "<circle r=\"5\"/>"
        "<!-- and another -->"
        "<rect width=\"1\" height=\"1\"/>"
        "</part>");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.nodes.size(), 3u);
    EXPECT_EQ(r.nodes[0].type, NodeType::Part);
    EXPECT_EQ(r.nodes[1].type, NodeType::Circle);
    EXPECT_EQ(r.nodes[2].type, NodeType::Rect);
}

// ─── <for> with element children (M1) ──────────────────────────────

TEST(BodyParser, ForWithChildElements) {
    auto r = parse_body_str(R"XML(<for var="i" from="0" to="3" steps="4"><group transform="translate({i}, 0, 0)"><circle r="1"/></group></for>)XML");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    ASSERT_EQ(r.nodes.size(), 3u);
    EXPECT_EQ(r.nodes[0].type, NodeType::For);
    EXPECT_EQ(r.nodes[0].first_child, 1u);
    EXPECT_EQ(r.nodes[1].type, NodeType::Group);
    EXPECT_EQ(r.nodes[1].first_child, 2u);
    EXPECT_EQ(r.nodes[2].type, NodeType::Circle);
}

TEST(BodyParser, ForExplicitValuesWithChildren) {
    auto r = parse_body_str(
        R"(<for var="c" values="nw ne sw se">)"
        R"(<bolt at="hole-{c}" port="head" length="20"/>)"
        R"(</for>)");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.nodes.size(), 2u);
    const auto& f = std::get<ForAttrs>(r.nodes[0].attrs);
    EXPECT_EQ(f.values, "nw ne sw se");
    EXPECT_EQ(r.nodes[1].type, NodeType::Instance);
}

// ─── <script> with realistic Lua (M2) ──────────────────────────────

TEST(BodyParser, ScriptWithSimpleLua) {
    auto r = parse_body_str(
        R"(<script lang="lua">)"
        R"(function f(x) return x * 2 end)"
        R"(</script>)");
    EXPECT_TRUE(r.ok());
    const auto& s = std::get<ScriptAttrs>(r.nodes[0].attrs);
    EXPECT_NE(s.source.find("function f(x)"), std::string::npos);
}

TEST(BodyParser, ScriptWithCDATAPreservesLessThan) {
    // Per spec §5.1: scripts containing `<` or `>` MUST be wrapped in
    // CDATA. This test confirms the wrapping survives parsing.
    auto r = parse_body_str(
        "<script lang=\"lua\"><![CDATA[\n"
        "function naca(c, t, n)\n"
        "  for i = 0, n do\n"
        "    if i < 5 then return i end\n"
        "  end\n"
        "end\n"
        "]]></script>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    const auto& s = std::get<ScriptAttrs>(r.nodes[0].attrs);
    EXPECT_NE(s.source.find("if i < 5 then"), std::string::npos);
    EXPECT_NE(s.source.find("for i = 0, n do"), std::string::npos);
}

TEST(BodyParser, MultipleScriptBlocks) {
    auto r = parse_body_str(
        "<script lang=\"lua\">function a() return 1 end</script>"
        "<script lang=\"lua\">function b() return 2 end</script>");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.nodes.size(), 2u);
    EXPECT_EQ(r.nodes[0].type, NodeType::Script);
    EXPECT_EQ(r.nodes[1].type, NodeType::Script);
}

// ─── <sources>/<source> in body (M3) ───────────────────────────────

TEST(BodyParser, FlatOutputSourcesElement) {
    auto r = parse_body_str(
        R"(<sources>)"
        R"(<source id="0" path="bolt.cadml" hash="abc123"/>)"
        R"(<source id="1" path="plate.cadml" hash="def456"/>)"
        R"(</sources>)");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.nodes.size(), 3u);
    EXPECT_EQ(r.nodes[0].type, NodeType::Sources);
    EXPECT_EQ(r.nodes[1].type, NodeType::Source);
    const auto& s0 = std::get<SourceAttrs>(r.nodes[1].attrs);
    EXPECT_EQ(s0.id, 0u);
    EXPECT_EQ(s0.path, "bolt.cadml");
    EXPECT_EQ(s0.hash, "abc123");
}

// ─── Nested instances with at/port (M4) ────────────────────────────

TEST(BodyParser, NestedInstanceWithAtPort) {
    auto r = parse_body_str(
        R"(<assembly>)"
        R"(<plate>)"
        R"(<bolt at="hole" port="head" length="20"/>)"
        R"(</plate>)"
        R"(</assembly>)");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.nodes.size(), 3u);
    EXPECT_EQ(r.nodes[0].type, NodeType::Assembly);
    EXPECT_EQ(r.nodes[1].type, NodeType::Instance);
    EXPECT_EQ(r.nodes[2].type, NodeType::Instance);

    // Plate (instance under assembly) — no at/port.
    const auto& plate = std::get<InstanceAttrs>(r.nodes[1].attrs);
    EXPECT_EQ(plate.ref_name, "plate");
    EXPECT_EQ(plate.at, "");
    EXPECT_EQ(plate.port, "");

    // Bolt (mating instance under plate).
    const auto& bolt = std::get<InstanceAttrs>(r.nodes[2].attrs);
    EXPECT_EQ(bolt.ref_name, "bolt");
    EXPECT_EQ(bolt.at, "hole");
    EXPECT_EQ(bolt.port, "head");
    EXPECT_EQ(bolt.param_overrides.at("length"), "20");

    // Tree linkage.
    EXPECT_EQ(r.nodes[1].parent, 0u);
    EXPECT_EQ(r.nodes[2].parent, 1u);
}

TEST(BodyParser, ConnectElementParsedInsideAssembly) {
    auto r = parse_body_str(
        R"(<assembly>)"
        R"(<plate id="p"/>)"
        R"(<bolt id="b"/>)"
        R"(<connect a="b.head" b="p.hole"/>)"
        R"(</assembly>)");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.nodes.size(), 4u);
    EXPECT_EQ(r.nodes[3].type, NodeType::Connect);
    const auto& c = std::get<ConnectAttrs>(r.nodes[3].attrs);
    EXPECT_EQ(c.a, "b.head");
    EXPECT_EQ(c.b, "p.hole");
    EXPECT_FALSE(c.allow_interference);
}

TEST(BodyParser, ConnectAllowInterferenceParsed) {
    auto r = parse_body_str(
        R"(<assembly>)"
        R"(<plate id="p"/>)"
        R"(<bolt id="b"/>)"
        R"(<connect a="b.head" b="p.hole" allow-interference="true"/>)"
        R"(</assembly>)");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.nodes.size(), 4u);
    const auto& c = std::get<ConnectAttrs>(r.nodes[3].attrs);
    EXPECT_TRUE(c.allow_interference);
}

TEST(BodyParser, ConnectAllowInterferenceFalseStringIsFalse) {
    // Only the literal "true" sets the flag — anything else stays false.
    auto r = parse_body_str(
        R"(<assembly>)"
        R"(<plate id="p"/>)"
        R"(<bolt id="b"/>)"
        R"(<connect a="b.head" b="p.hole" allow-interference="yes"/>)"
        R"(</assembly>)");
    EXPECT_TRUE(r.ok());
    const auto& c = std::get<ConnectAttrs>(r.nodes[3].attrs);
    EXPECT_FALSE(c.allow_interference);
}

// ─── Deep nesting (M5) ─────────────────────────────────────────────

TEST(BodyParser, DeeplyNestedTree) {
    auto r = parse_body_str(
        R"(<part>)"
        R"(<group>)"
        R"(<group>)"
        R"(<difference>)"
        R"(<extrude height="10"><circle r="5"/></extrude>)"
        R"(<extrude height="11"><circle r="3"/></extrude>)"
        R"(</difference>)"
        R"(</group>)"
        R"(</group>)"
        R"(</part>)");
    EXPECT_TRUE(r.ok());
    // Walk parents to count depth from circle r=5 back to part.
    std::uint32_t depth = 0;
    std::uint32_t idx = NO_NODE;
    for (std::uint32_t i = 0; i < r.nodes.size(); ++i) {
        if (r.nodes[i].type == NodeType::Circle) {
            const auto& c = std::get<CircleAttrs>(r.nodes[i].attrs);
            if (c.r_expr == "5") { idx = i; break; }
        }
    }
    ASSERT_NE(idx, NO_NODE);
    while (r.nodes[idx].parent != NO_NODE) {
        idx = r.nodes[idx].parent;
        ++depth;
    }
    // circle → extrude → difference → group → group → part = 5 steps
    EXPECT_EQ(depth, 5u);
}
