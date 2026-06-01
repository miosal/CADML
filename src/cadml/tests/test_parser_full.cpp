// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/parser.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace cadml;

// Deep-XML DoS regression. The PI/DOCTYPE rejection
// scan (`find_forbidden_xml_node`) was recursive and ran BEFORE the
// Walker's 256-level cap, so deeply-nested input overflowed the call
// stack (~26k–30k on a 1 MB stack) before the cap could fire. The scan
// is now iterative. Input far past the old crash threshold must return
// a clean error (the 256-level cap), never crash. (The earlier
// remediation sweep only tested 1024-deep — below the crash point.)
TEST(ParserFull, DeepNestingDoesNotOverflow) {
    constexpr int kDepth = 50000;   // well past the ~26k–30k crash point
    std::string src = "version 0.1\n<part name=\"x\">\n";
    src.reserve(kDepth * 16);
    for (int i = 0; i < kDepth; ++i) src += "<group>";
    src += "<extrude height=\"1\"><rect width=\"1\" height=\"1\"/></extrude>";
    for (int i = 0; i < kDepth; ++i) src += "</group>";
    src += "\n</part>\n";

    auto r = parse(src);   // must return, not crash
    EXPECT_FALSE(r.ok()) << "50k-deep nesting should be rejected by the cap";
    bool hit_cap = false;
    for (const auto& e : r.errors) {
        if (e.message.find("nesting exceeds") != std::string::npos) hit_cap = true;
    }
    EXPECT_TRUE(hit_cap) << "expected the 256-level nesting-cap error";
}

TEST(ParserFull, MinimalPart) {
    auto r = parse(
        "version 0.1\n"
        "units mm\n"
        "\n"
        "<part>\n"
        "  <extrude height=\"10\">\n"
        "    <circle r=\"5\"/>\n"
        "  </extrude>\n"
        "</part>\n");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(r.document.meta.version, "0.1.0");
    EXPECT_EQ(r.document.meta.units, "mm");
    EXPECT_EQ(r.document.nodes.size(), 3u);
    EXPECT_EQ(r.document.nodes[0].type, NodeType::Part);
    EXPECT_EQ(r.document.nodes[1].type, NodeType::Extrude);
    EXPECT_EQ(r.document.nodes[2].type, NodeType::Circle);
}

TEST(ParserFull, BoltCatalogueFile) {
    auto r = parse(
        "version 0.1\n"
        "units mm\n"
        "description \"Hex bolt — parametric\"\n"
        "tags \"fastener hex bolt\"\n"
        "catalogue-version 1.0.0\n"
        "\n"
        "param length = 30 (min=5, max=200)\n"
        "param d = 10 (min=3, max=30)\n"
        "\n"
        "<part>\n"
        "  <extrude height=\"{length}\">\n"
        "    <circle r=\"{d/2}\"/>\n"
        "  </extrude>\n"
        "  <port name=\"head\" position=\"0 0 {length}\" normal=\"-z\" up=\"+x\"/>\n"
        "</part>\n");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);

    EXPECT_EQ(r.document.params.size(), 2u);
    EXPECT_EQ(r.document.params[0].name, "length");
    ASSERT_TRUE(r.document.params[0].min.has_value());
    EXPECT_DOUBLE_EQ(*r.document.params[0].min, 5.0);

    // 4 nodes: <part>, <extrude>, <circle>, <port>
    EXPECT_EQ(r.document.nodes.size(), 4u);
    EXPECT_EQ(r.document.nodes[0].type, NodeType::Part);
    EXPECT_EQ(r.document.nodes[3].type, NodeType::Port);

    const auto& port = std::get<PortAttrs>(r.document.nodes[3].attrs);
    EXPECT_EQ(port.name, "head");
    EXPECT_EQ(port.position_expr, "0 0 {length}");
    EXPECT_EQ(port.normal_expr, "-z");
    EXPECT_EQ(port.up_expr, "+x");
}

TEST(ParserFull, MultiFileAssembly) {
    auto r = parse(
        "version 0.1\n"
        "units mm\n"
        "description \"Bolt through plate\"\n"
        "\n"
        "import \"ctl/fasteners/hex-bolt.cadml\" as bolt\n"
        "import \"./plate.cadml\"\n"
        "\n"
        "<assembly>\n"
        "  <plate>\n"
        "    <bolt at=\"hole\" port=\"head\" length=\"20\"/>\n"
        "  </plate>\n"
        "</assembly>\n");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);

    EXPECT_EQ(r.document.imports.size(), 2u);
    EXPECT_EQ(r.document.imports[0].alias, "bolt");
    EXPECT_TRUE(r.document.imports[0].is_catalogue);
    EXPECT_EQ(r.document.imports[1].alias, "plate");
    EXPECT_FALSE(r.document.imports[1].is_catalogue);

    // 3 nodes: <assembly>, <plate> (instance), <bolt> (instance)
    ASSERT_EQ(r.document.nodes.size(), 3u);
    EXPECT_EQ(r.document.nodes[0].type, NodeType::Assembly);
    EXPECT_EQ(r.document.nodes[1].type, NodeType::Instance);
    EXPECT_EQ(r.document.nodes[2].type, NodeType::Instance);

    // The bolt instance carries at/port + length override.
    const auto& bolt_inst = std::get<InstanceAttrs>(r.document.nodes[2].attrs);
    EXPECT_EQ(bolt_inst.ref_name, "bolt");
    EXPECT_EQ(bolt_inst.at, "hole");
    EXPECT_EQ(bolt_inst.port, "head");
    EXPECT_EQ(bolt_inst.param_overrides.at("length"), "20");
}

TEST(ParserFull, FullPropellerStructure) {
    // Exercises: defs + script + part + loft + pattern + use of the def.
    auto r = parse(
        "version 0.1\n"
        "units mm\n"
        "description \"3-blade propeller\"\n"
        "\n"
        "param hub-r = 6\n"
        "param hub-h = 6\n"
        "\n"
        "<def name=\"blade\">\n"
        "  <loft>\n"
        "    <sketch plane=\"xy\" origin=\"0 0 6\"><circle r=\"3\"/></sketch>\n"
        "    <sketch plane=\"xy\" origin=\"0 0 30\"><circle r=\"2\"/></sketch>\n"
        "  </loft>\n"
        "</def>\n"
        "\n"
        "<part name=\"propeller\">\n"
        "  <union>\n"
        "    <extrude height=\"{hub-h}\" symmetric=\"true\">\n"
        "      <circle r=\"{hub-r}\"/>\n"
        "    </extrude>\n"
        "    <pattern type=\"circular\" count=\"3\" axis=\"z\">\n"
        "      <blade/>\n"
        "    </pattern>\n"
        "  </union>\n"
        "</part>\n");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);

    // Confirm key structures are in place.
    EXPECT_EQ(r.document.params.size(), 2u);
    EXPECT_EQ(r.document.defs.size(), 1u);
    EXPECT_EQ(r.document.exports.size(), 1u);
    EXPECT_EQ(r.document.exports.count("propeller"), 1u);

    // The blade-instance under <pattern> has ref_name "blade"
    bool found_blade_instance = false;
    for (const auto& n : r.document.nodes) {
        if (n.type == NodeType::Instance) {
            const auto& inst = std::get<InstanceAttrs>(n.attrs);
            if (inst.ref_name == "blade") {
                found_blade_instance = true;
            }
        }
    }
    EXPECT_TRUE(found_blade_instance);
}

TEST(ParserFull, FrontmatterOrderingWarnsButParseSucceeds) {
    // Out-of-order frontmatter is a style warning, not an error.
    // The parse still succeeds and the body is processed normally.
    auto r = parse(
        "param x = 10\n"
        "version 0.1\n"        // out of order — now a warning
        "<part/>\n");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_FALSE(r.warnings.empty());
    EXPECT_NE(r.warnings[0].message.find("ordering"), std::string::npos);
}

TEST(ParserFull, BodyErrorReportedWithCorrectLine) {
    // Body starts on line 4; the unknown element appears on line 5.
    auto r = parse(
        "version 0.1\n"
        "units mm\n"
        "\n"
        "<part>\n"
        "  <NOT_A_VALID_NAME/>\n"
        "</part>\n");
    EXPECT_FALSE(r.ok());
    bool found_line_5 = false;
    for (const auto& e : r.errors) {
        if (e.source.line == 5u) found_line_5 = true;
    }
    EXPECT_TRUE(found_line_5)
        << "expected an error on line 5; got "
        << (r.errors.empty() ? std::string("none")
                              : std::to_string(r.errors[0].source.line));
}

TEST(ParserFull, EmptyFileIsValidButProducesNoExport) {
    auto r = parse("");
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.document.exports.empty());
}

TEST(ParserFull, FrontmatterOnlyFileNoBodyOK) {
    auto r = parse(
        "version 0.1\n"
        "units mm\n");
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.document.nodes.empty());
}

// ─── Required-version enforcement (C2) ──────────────────────────────

TEST(ParserFull, MissingVersionWithBodyErrors) {
    auto r = parse("<part/>");
    EXPECT_FALSE(r.ok());
    bool has_missing_version = false;
    for (const auto& e : r.errors) {
        if (e.message.find("version") != std::string::npos) {
            has_missing_version = true;
        }
    }
    EXPECT_TRUE(has_missing_version);
}

TEST(ParserFull, MissingVersionWithSettingsErrors) {
    auto r = parse(
        "units mm\n"
        "description \"no version line\"\n"
        "<part/>\n");
    EXPECT_FALSE(r.ok());
}

TEST(ParserFull, MissingVersionWithImportErrors) {
    auto r = parse(
        "import \"x.cadml\"\n"
        "<part/>\n");
    EXPECT_FALSE(r.ok());
}

TEST(ParserFull, EmptyFileNoVersionRequiredOK) {
    // Empty / whitespace-only files don't require version (incremental
    // editing scenario).
    EXPECT_TRUE(parse("").ok());
    EXPECT_TRUE(parse("\n\n  \n").ok());
}

TEST(ParserFull, CommentsOnlyNoVersionRequiredOK) {
    auto r = parse("# just a comment\n");
    EXPECT_TRUE(r.ok());
}

TEST(ParserFull, FullVersionFormAlsoOK) {
    auto r = parse("version 0.1.0\n<part/>");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.document.meta.version, "0.1.0");
}

// ─── parse_file() entry point (M6) ──────────────────────────────────

TEST(ParserFull, ParseFileReadsFromDisk) {
    // Write a temp .cadml file, parse it via parse_file.
    auto tmp = std::filesystem::temp_directory_path()
                / "cadml_parser_test_input.cadml";
    {
        std::ofstream f(tmp);
        f << "version 0.1\n";
        f << "units mm\n";
        f << "<part name=\"x\"><circle r=\"5\"/></part>\n";
    }
    auto r = parse_file(tmp);
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(r.document.exports.count("x"), 1u);
    std::filesystem::remove(tmp);
}

TEST(ParserFull, ParseFileMissingPathErrors) {
    auto r = parse_file("definitely-does-not-exist.cadml");
    EXPECT_FALSE(r.ok());
}
