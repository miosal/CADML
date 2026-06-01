// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/parser.hpp>
#include <cadml/serializer.hpp>

#include <gtest/gtest.h>

using namespace cadml;

namespace {

// Round-trip: parse → serialize → parse, compare semantic content.
struct RoundTrip {
    Document a;
    Document b;
    std::string serialized;
};

RoundTrip round_trip(std::string_view src) {
    auto first = parse(src);
    EXPECT_TRUE(first.ok()) << (first.errors.empty() ? "" : first.errors[0].message);
    auto serialized = serialize(first.document);
    auto second = parse(serialized);
    EXPECT_TRUE(second.ok())
        << "second parse failed:\n" << serialized
        << "\nerror: " << (second.errors.empty() ? "" : second.errors[0].message);
    return { std::move(first.document), std::move(second.document), serialized };
}

void expect_meta_equiv(const Document& a, const Document& b) {
    EXPECT_EQ(a.meta.version,            b.meta.version);
    EXPECT_EQ(a.meta.units,              b.meta.units);
    EXPECT_EQ(a.meta.description,        b.meta.description);
    EXPECT_EQ(a.meta.tags,               b.meta.tags);
    EXPECT_EQ(a.meta.catalogue_version,  b.meta.catalogue_version);
}

void expect_imports_equiv(const Document& a, const Document& b) {
    ASSERT_EQ(a.imports.size(), b.imports.size());
    for (std::size_t i = 0; i < a.imports.size(); ++i) {
        EXPECT_EQ(a.imports[i].path,         b.imports[i].path);
        EXPECT_EQ(a.imports[i].alias,        b.imports[i].alias);
        EXPECT_EQ(a.imports[i].is_catalogue, b.imports[i].is_catalogue);
        EXPECT_EQ(a.imports[i].is_lua,       b.imports[i].is_lua);
    }
}

void expect_params_equiv(const Document& a, const Document& b) {
    ASSERT_EQ(a.params.size(), b.params.size());
    for (std::size_t i = 0; i < a.params.size(); ++i) {
        EXPECT_EQ(a.params[i].name,       b.params[i].name);
        EXPECT_EQ(a.params[i].value_expr, b.params[i].value_expr);
        EXPECT_EQ(a.params[i].min.has_value(), b.params[i].min.has_value());
        EXPECT_EQ(a.params[i].max.has_value(), b.params[i].max.has_value());
    }
}

void expect_node_count(const Document& a, const Document& b) {
    EXPECT_EQ(a.nodes.size(), b.nodes.size());
}

}  // namespace

// ─── Frontmatter round-trip ─────────────────────────────────────────

TEST(Serializer, MinimalDocument) {
    auto rt = round_trip("version 0.1\n<part/>");
    expect_meta_equiv(rt.a, rt.b);
    expect_node_count(rt.a, rt.b);
}

TEST(Serializer, AllFrontmatterSettings) {
    auto rt = round_trip(
        "version 0.1\n"
        "units cm\n"
        "description \"hello world\"\n"
        "tags \"alpha beta gamma\"\n"
        "catalogue-version 1.0.0\n"
        "<part/>\n");
    expect_meta_equiv(rt.a, rt.b);
}

TEST(Serializer, ImportsRoundTrip) {
    auto rt = round_trip(
        "version 0.1\n"
        "import \"shared/bolt.cadml\"\n"
        "import \"ctl/aero/airfoils.lua\" as airfoils\n"
        "<part/>\n");
    expect_imports_equiv(rt.a, rt.b);
}

TEST(Serializer, ParamsRoundTrip) {
    auto rt = round_trip(
        "version 0.1\n"
        "param chord = 100\n"
        "param length = 30 (min=5, max=200)\n"
        "param thickness = 0.12 (min=0.05)\n"
        "<part/>\n");
    expect_params_equiv(rt.a, rt.b);
    EXPECT_DOUBLE_EQ(*rt.b.params[1].min, 5.0);
    EXPECT_DOUBLE_EQ(*rt.b.params[1].max, 200.0);
}

// ─── Body round-trip ────────────────────────────────────────────────

TEST(Serializer, SimpleBody) {
    auto rt = round_trip(
        "version 0.1\n"
        "<part>\n"
        "  <extrude height=\"10\">\n"
        "    <circle r=\"5\"/>\n"
        "  </extrude>\n"
        "</part>\n");
    expect_node_count(rt.a, rt.b);
    EXPECT_EQ(rt.a.nodes[0].type, rt.b.nodes[0].type);
    EXPECT_EQ(rt.a.nodes[1].type, rt.b.nodes[1].type);
    EXPECT_EQ(rt.a.nodes[2].type, rt.b.nodes[2].type);
}

TEST(Serializer, AssemblyWithMatingInstance) {
    auto rt = round_trip(
        "version 0.1\n"
        "import \"shared/bolt.cadml\"\n"
        "import \"./plate.cadml\"\n"
        "<assembly>\n"
        "  <plate>\n"
        "    <bolt at=\"hole\" port=\"head\" length=\"20\"/>\n"
        "  </plate>\n"
        "</assembly>\n");
    expect_meta_equiv(rt.a, rt.b);
    expect_imports_equiv(rt.a, rt.b);
    expect_node_count(rt.a, rt.b);

    // Find the bolt instance and verify its mating attrs survived.
    bool found = false;
    for (const auto& n : rt.b.nodes) {
        if (n.type != NodeType::Instance) continue;
        const auto& inst = std::get<InstanceAttrs>(n.attrs);
        if (inst.ref_name == "bolt") {
            EXPECT_EQ(inst.at, "hole");
            EXPECT_EQ(inst.port, "head");
            EXPECT_EQ(inst.param_overrides.at("length"), "20");
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(Serializer, ConnectElement) {
    auto rt = round_trip(
        "version 0.1\n"
        "import \"x.cadml\"\n"
        "<assembly>\n"
        "  <x id=\"a\"/>\n"
        "  <x id=\"b\"/>\n"
        "  <connect a=\"a.p\" b=\"b.q\"/>\n"
        "</assembly>\n");
    bool found_connect = false;
    for (const auto& n : rt.b.nodes) {
        if (n.type == NodeType::Connect) {
            const auto& c = std::get<ConnectAttrs>(n.attrs);
            EXPECT_EQ(c.a, "a.p");
            EXPECT_EQ(c.b, "b.q");
            EXPECT_FALSE(c.allow_interference);
            found_connect = true;
        }
    }
    EXPECT_TRUE(found_connect);
    // Default-false flag must NOT serialise — empty attrs only.
    EXPECT_EQ(rt.serialized.find("allow-interference"), std::string::npos);
}

TEST(Serializer, ConnectAllowInterferenceRoundTrips) {
    auto rt = round_trip(
        "version 0.1\n"
        "import \"x.cadml\"\n"
        "<assembly>\n"
        "  <x id=\"a\"/>\n"
        "  <x id=\"b\"/>\n"
        "  <connect a=\"a.p\" b=\"b.q\" allow-interference=\"true\"/>\n"
        "</assembly>\n");
    bool found = false;
    for (const auto& n : rt.b.nodes) {
        if (n.type == NodeType::Connect) {
            const auto& c = std::get<ConnectAttrs>(n.attrs);
            EXPECT_TRUE(c.allow_interference);
            found = true;
        }
    }
    EXPECT_TRUE(found);
    EXPECT_NE(rt.serialized.find("allow-interference=\"true\""),
                std::string::npos);
}

TEST(Serializer, FullPropellerLikeShape) {
    auto rt = round_trip(
        "version 0.1\n"
        "units mm\n"
        "description \"3-blade prop\"\n"
        "param hub-r = 6\n"
        "<def name=\"blade\">\n"
        "  <loft>\n"
        "    <sketch plane=\"xy\" origin=\"0 0 6\"><circle r=\"3\"/></sketch>\n"
        "    <sketch plane=\"xy\" origin=\"0 0 30\"><circle r=\"2\"/></sketch>\n"
        "  </loft>\n"
        "</def>\n"
        "<part name=\"propeller\">\n"
        "  <union>\n"
        "    <extrude height=\"6\" symmetric=\"true\"><circle r=\"{hub-r}\"/></extrude>\n"
        "    <pattern type=\"circular\" count=\"3\" axis=\"z\">\n"
        "      <blade/>\n"
        "    </pattern>\n"
        "  </union>\n"
        "</part>\n");
    expect_meta_equiv(rt.a, rt.b);
    expect_params_equiv(rt.a, rt.b);
    expect_node_count(rt.a, rt.b);
    EXPECT_EQ(rt.b.exports.count("propeller"), 1u);
    EXPECT_EQ(rt.b.defs.count("blade"), 1u);
}

// ─── Idempotence ────────────────────────────────────────────────────

TEST(Serializer, IdempotentOnSecondRoundTrip) {
    const std::string src =
        "version 0.1\n"
        "param x = 10 (min=1, max=100)\n"
        "<part name=\"p\">\n"
        "  <extrude height=\"{x}\"><circle r=\"5\"/></extrude>\n"
        "</part>\n";
    auto p1 = parse(src);
    auto s1 = serialize(p1.document);
    auto p2 = parse(s1);
    auto s2 = serialize(p2.document);
    EXPECT_EQ(s1, s2);  // serialize → parse → serialize is byte-identical
}

// ─── Flat-form serialization (.fcadml) ──────────────────────────────

TEST(Serializer, FlatFormIncludesSourcesTable) {
    Document doc;
    doc.meta.version = "0.1.0";
    SourceFile sf;
    sf.id = 0; sf.path = "test.cadml"; sf.hash = "abc123";
    doc.source_files.push_back(sf);

    SerializeOptions opts;
    opts.include_source_map = true;
    auto s = serialize(doc, opts);
    EXPECT_NE(s.find("<sources>"), std::string::npos);
    EXPECT_NE(s.find("<source id=\"0\""), std::string::npos);
    EXPECT_NE(s.find("hash=\"abc123\""), std::string::npos);
}

TEST(Serializer, FlatFormEmitsSrcAttributes) {
    auto p = parse("version 0.1\n<part><circle r=\"5\"/></part>");
    ASSERT_TRUE(p.ok());
    SerializeOptions opts;
    opts.include_source_map = true;
    auto s = serialize(p.document, opts);
    EXPECT_NE(s.find("src="), std::string::npos);
}

// ─── XML escaping ───────────────────────────────────────────────────

TEST(Serializer, AmpersandInValuesEscaped) {
    auto p = parse(
        "version 0.1\n"
        "<part name=\"a&amp;b\"/>");
    ASSERT_TRUE(p.ok());
    auto s = serialize(p.document);
    EXPECT_NE(s.find("a&amp;b"), std::string::npos);
}
