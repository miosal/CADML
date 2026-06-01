// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/parser.hpp>

#include <gtest/gtest.h>

using namespace cadml;

namespace {

FrontmatterResult parse_fm(std::string_view src) {
    return parse_frontmatter(src, /*file_id=*/0);
}

}  // namespace

// ─── Empty + whitespace ──────────────────────────────────────────────

TEST(Frontmatter, EmptyInput) {
    auto r = parse_fm("");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.body_offset, 0u);
}

TEST(Frontmatter, WhitespaceOnlyInput) {
    std::string_view src = "\n\n  \n";
    auto r = parse_fm(src);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.body_offset, src.size());  // consumed all input, no body found
}

TEST(Frontmatter, BodyOnlyInputBodyOffsetIsZeroWhenStartingWithLT) {
    auto r = parse_fm("<part></part>");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.body_offset, 0u);
}

TEST(Frontmatter, BodyOnlyAfterWhitespace) {
    // Frontmatter parser is permissive about missing version — the
    // version-required check lives in parse() (see ParserFull tests).
    auto r = parse_fm("   \n  <part/>");
    EXPECT_TRUE(r.ok());
    // body offset points at the `<`, not before.
    EXPECT_EQ(r.body_offset, 6u);
    EXPECT_FALSE(r.version_explicitly_set);
    EXPECT_EQ(r.meta.version, "0.1.0");  // default initial value
}

// ─── Settings ────────────────────────────────────────────────────────

TEST(Frontmatter, VersionShortForm) {
    auto r = parse_fm("version 0.1\n");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.meta.version, "0.1.0");
}

TEST(Frontmatter, VersionFullForm) {
    auto r = parse_fm("version 0.1.0\n");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.meta.version, "0.1.0");
}

TEST(Frontmatter, VersionPatchForm) {
    auto r = parse_fm("version 0.1.5\n");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.meta.version, "0.1.5");
}

TEST(Frontmatter, UnitsValid) {
    for (auto u : {"mm", "cm", "m", "in", "ft"}) {
        auto r = parse_fm(std::string("units ") + u + "\n");
        EXPECT_TRUE(r.ok()) << "failed for units `" << u << "`";
        EXPECT_EQ(r.meta.units, u);
    }
}

TEST(Frontmatter, UnitsInvalidErrors) {
    auto r = parse_fm("units leagues\n");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.errors.size(), 1u);
    EXPECT_NE(r.errors[0].message.find("units"), std::string::npos);
}

TEST(Frontmatter, DescriptionQuoted) {
    auto r = parse_fm(R"(description "a quick wing test")"
                      "\n");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.meta.description, "a quick wing test");
}

TEST(Frontmatter, DescriptionUnquotedErrors) {
    auto r = parse_fm("description my-wing\n");
    EXPECT_FALSE(r.ok());
}

TEST(Frontmatter, DescriptionWithEscapedQuote) {
    auto r = parse_fm(R"(description "with \"quotes\" inside")"
                      "\n");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.meta.description, R"(with "quotes" inside)");
}

TEST(Frontmatter, TagsAndCatalogueVersion) {
    auto r = parse_fm(
        "version 0.1\n"
        "tags \"fastener bolt metric\"\n"
        "catalogue-version 1.0.0\n");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.meta.tags, "fastener bolt metric");
    EXPECT_EQ(r.meta.catalogue_version, "1.0.0");
}

TEST(Frontmatter, InterferenceToleranceValueWithUnitSuffix) {
    auto r = parse_fm("interference-tolerance 0.01mm³\n");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.meta.interference_tolerance, "0.01mm³");
}

TEST(Frontmatter, InterferenceToleranceMalformedRejected) {
    // Multiple dots — std::stod silently parses "1.2", but our
    // post-validation requires the entire number be consumed.
    auto r = parse_fm("interference-tolerance 1.2.3mm3\n");
    EXPECT_FALSE(r.ok());
}

TEST(Frontmatter, InterferenceToleranceNegativeRejected) {
    auto r = parse_fm("interference-tolerance -1mm3\n");
    EXPECT_FALSE(r.ok());
}

TEST(Frontmatter, InterferenceToleranceUnknownUnitRejected) {
    auto r = parse_fm("interference-tolerance 1km3\n");
    EXPECT_FALSE(r.ok());
}

TEST(Frontmatter, InterferenceToleranceMissingCubeMarkerRejected) {
    // `1mm` is a length, not a volume — must reject at parse time
    // so the user catches the typo before the file ships.
    auto r = parse_fm("interference-tolerance 1mm\n");
    EXPECT_FALSE(r.ok());
}

// ─── Imports ─────────────────────────────────────────────────────────

TEST(Frontmatter, ImportDefaultAlias) {
    auto r = parse_fm(
        "version 0.1\n"
        "import \"shared/bolt.cadml\"\n");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.imports.size(), 1u);
    EXPECT_EQ(r.imports[0].path, "shared/bolt.cadml");
    EXPECT_EQ(r.imports[0].alias, "bolt");
    EXPECT_FALSE(r.imports[0].is_catalogue);
    EXPECT_FALSE(r.imports[0].is_lua);
}

TEST(Frontmatter, ImportExplicitAlias) {
    auto r = parse_fm(
        "version 0.1\n"
        "import \"shared/bolt.cadml\" as small-bolt\n");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.imports.size(), 1u);
    EXPECT_EQ(r.imports[0].alias, "small-bolt");
}

TEST(Frontmatter, ImportCtlPrefixSetsCatalogueFlag) {
    auto r = parse_fm(
        "version 0.1\n"
        "import \"ctl/stock/square-tube.cadml\"\n");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.imports.size(), 1u);
    EXPECT_TRUE(r.imports[0].is_catalogue);
    EXPECT_EQ(r.imports[0].alias, "square-tube");
}

TEST(Frontmatter, ImportLuaSetsLuaFlag) {
    auto r = parse_fm(
        "version 0.1\n"
        "import \"ctl/aero/airfoils.lua\" as airfoils\n");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.imports.size(), 1u);
    EXPECT_TRUE(r.imports[0].is_lua);
    EXPECT_TRUE(r.imports[0].is_catalogue);
    EXPECT_EQ(r.imports[0].alias, "airfoils");
}

TEST(Frontmatter, ImportRelativePath) {
    auto r = parse_fm(
        "version 0.1\n"
        "import \"./local-file.cadml\"\n"
        "import \"../sibling.cadml\"\n");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.imports.size(), 2u);
    EXPECT_EQ(r.imports[0].alias, "local-file");
    EXPECT_EQ(r.imports[1].alias, "sibling");
}

TEST(Frontmatter, ImportMissingQuoteErrors) {
    auto r = parse_fm("import shared/bolt.cadml\n");
    EXPECT_FALSE(r.ok());
}

TEST(Frontmatter, ImportInvalidAliasErrors) {
    // Underscores not allowed (kebab-case only per spec §2.8).
    auto r = parse_fm("import \"x.cadml\" as my_bolt\n");
    EXPECT_FALSE(r.ok());
}

TEST(Frontmatter, ImportTrailingTextErrors) {
    auto r = parse_fm("import \"x.cadml\" as bolt extra-token\n");
    EXPECT_FALSE(r.ok());
}

// ─── Params ──────────────────────────────────────────────────────────

TEST(Frontmatter, ParamSimpleNumber) {
    auto r = parse_fm(
        "version 0.1\n"
        "param chord = 100\n");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.params.size(), 1u);
    EXPECT_EQ(r.params[0].name, "chord");
    EXPECT_EQ(r.params[0].value_expr, "100");
    EXPECT_FALSE(r.params[0].min.has_value());
    EXPECT_FALSE(r.params[0].max.has_value());
}

TEST(Frontmatter, ParamWithExpressionValue) {
    auto r = parse_fm(
        "version 0.1\n"
        "param chord = 100\n"
        "param half-chord = chord / 2\n");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.params.size(), 2u);
    EXPECT_EQ(r.params[1].value_expr, "chord / 2");
}

TEST(Frontmatter, ParamWithMinMax) {
    auto r = parse_fm(
        "version 0.1\n"
        "param length = 30 (min=5, max=200)\n");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.params.size(), 1u);
    EXPECT_EQ(r.params[0].name, "length");
    EXPECT_EQ(r.params[0].value_expr, "30");
    ASSERT_TRUE(r.params[0].min.has_value());
    EXPECT_DOUBLE_EQ(*r.params[0].min, 5.0);
    ASSERT_TRUE(r.params[0].max.has_value());
    EXPECT_DOUBLE_EQ(*r.params[0].max, 200.0);
}

TEST(Frontmatter, ParamWithMinOnly) {
    auto r = parse_fm(
        "version 0.1\n"
        "param d = 10 (min=3)\n");
    EXPECT_TRUE(r.ok());
    ASSERT_EQ(r.params.size(), 1u);
    EXPECT_DOUBLE_EQ(*r.params[0].min, 3.0);
    EXPECT_FALSE(r.params[0].max.has_value());
}

TEST(Frontmatter, ParamWithMaxOnly) {
    auto r = parse_fm(
        "version 0.1\n"
        "param d = 10 (max=30)\n");
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.params[0].min.has_value());
    EXPECT_DOUBLE_EQ(*r.params[0].max, 30.0);
}

TEST(Frontmatter, ParamConstraintsInverseOrder) {
    auto r = parse_fm(
        "version 0.1\n"
        "param d = 10 (max=30, min=3)\n");
    EXPECT_TRUE(r.ok());
    EXPECT_DOUBLE_EQ(*r.params[0].min, 3.0);
    EXPECT_DOUBLE_EQ(*r.params[0].max, 30.0);
}

TEST(Frontmatter, ParamWithLuaCallExpression) {
    auto r = parse_fm(
        "version 0.1\n"
        "import \"ctl/aero/airfoils.lua\" as airfoils\n"
        "param chord = airfoils.default_chord()\n");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.params[0].value_expr, "airfoils.default_chord()");
}

TEST(Frontmatter, ParamUnknownConstraintErrors) {
    auto r = parse_fm("param x = 10 (foo=5)\n");
    EXPECT_FALSE(r.ok());
}

TEST(Frontmatter, ParamUnderscoreNameErrors) {
    auto r = parse_fm("param max_thickness = 0.12\n");
    EXPECT_FALSE(r.ok());
}

TEST(Frontmatter, ParamMissingEqualsErrors) {
    auto r = parse_fm("param chord 100\n");
    EXPECT_FALSE(r.ok());
}

TEST(Frontmatter, ParamEmptyValueErrors) {
    auto r = parse_fm("param chord = \n");
    EXPECT_FALSE(r.ok());
}

// ─── Order rules ─────────────────────────────────────────────────────

TEST(Frontmatter, ProperOrder) {
    auto r = parse_fm(
        "version 0.1\n"
        "units mm\n"
        "import \"x.cadml\"\n"
        "param a = 10\n");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.imports.size(), 1u);
    EXPECT_EQ(r.params.size(), 1u);
}

// The settings → imports → params ordering is a style rule the
// parser surfaces as a warning, not an error — out-of-order
// statements still parse cleanly so authoring tools don't fail a
// build on the difference between `units mm` written before vs.
// after an import.
TEST(Frontmatter, SettingAfterImportWarns) {
    auto r = parse_fm(
        "import \"x.cadml\"\n"
        "version 0.1\n");
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.warnings.empty());
    EXPECT_NE(r.warnings[0].message.find("ordering"),
              std::string::npos);
}

TEST(Frontmatter, SettingAfterParamWarns) {
    auto r = parse_fm(
        "param a = 10\n"
        "version 0.1\n");
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.warnings.empty());
}

TEST(Frontmatter, ImportAfterParamWarns) {
    auto r = parse_fm(
        "param a = 10\n"
        "import \"x.cadml\"\n");
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.warnings.empty());
}

// ─── Comments + whitespace + blank lines ────────────────────────────

TEST(Frontmatter, LineComments) {
    auto r = parse_fm(
        "# top of file\n"
        "version 0.1  # this is the version\n"
        "\n"
        "# imports follow\n"
        "import \"x.cadml\"\n");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.meta.version, "0.1.0");
    EXPECT_EQ(r.imports.size(), 1u);
}

TEST(Frontmatter, HashInsideQuotedStringNotComment) {
    auto r = parse_fm(R"(description "issue #42 fix")"
                      "\n");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.meta.description, "issue #42 fix");
}

TEST(Frontmatter, BlankLinesIgnored) {
    auto r = parse_fm(
        "\n\n"
        "version 0.1\n"
        "\n\n"
        "param a = 1\n");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.params.size(), 1u);
}

// ─── BOM + CRLF ──────────────────────────────────────────────────────

TEST(Frontmatter, UTF8BOMStripped) {
    std::string src = "\xEF\xBB\xBFversion 0.1\n";
    auto r = parse_fm(src);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.meta.version, "0.1.0");
}

TEST(Frontmatter, CRLFLineEndings) {
    auto r = parse_fm("version 0.1\r\nunits cm\r\n");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.meta.version, "0.1.0");
    EXPECT_EQ(r.meta.units, "cm");
}

TEST(Frontmatter, MixedLineEndings) {
    auto r = parse_fm("version 0.1\r\nunits mm\nparam a = 1\r\n");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.params.size(), 1u);
}

// ─── Body offset ─────────────────────────────────────────────────────

TEST(Frontmatter, BodyOffsetPointsAtFirstLT) {
    std::string_view src =
        "version 0.1\n"
        "<part></part>\n";
    auto r = parse_fm(src);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(src[r.body_offset], '<');
}

TEST(Frontmatter, BodyOffsetSkipsLeadingWhitespaceOfBodyLine) {
    std::string_view src =
        "version 0.1\n"
        "    <part/>\n";
    auto r = parse_fm(src);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(src[r.body_offset], '<');
}

// ─── End-to-end frontmatter examples ─────────────────────────────────

TEST(Frontmatter, RealisticBoltFile) {
    auto r = parse_fm(
        "version 0.1\n"
        "units mm\n"
        "description \"Hex bolt — parametric\"\n"
        "tags \"fastener hex bolt\"\n"
        "catalogue-version 1.0.0\n"
        "\n"
        "param length = 30 (min=5, max=200)\n"
        "param d = 10 (min=3, max=30)\n"
        "param head-h = 6.4\n");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(r.meta.description, "Hex bolt — parametric");
    EXPECT_EQ(r.meta.catalogue_version, "1.0.0");
    EXPECT_EQ(r.params.size(), 3u);
    EXPECT_EQ(r.params[0].name, "length");
    EXPECT_EQ(r.params[1].name, "d");
    EXPECT_EQ(r.params[2].name, "head-h");
}

TEST(Frontmatter, RealisticAssemblyFile) {
    auto r = parse_fm(
        "version 0.1\n"
        "units mm\n"
        "description \"Bolt through plate\"\n"
        "\n"
        "import \"ctl/fasteners/hex-bolt.cadml\" as bolt\n"
        "import \"./plate.cadml\"\n"
        "\n"
        "param plate-h = 6\n"
        "interference-tolerance 0.001mm³\n");
    // interference-tolerance is a setting placed after imports + params;
    // out-of-order is a style warning, not an error.
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_FALSE(r.warnings.empty());
}

TEST(Frontmatter, InterferenceToleranceProperPlacement) {
    auto r = parse_fm(
        "version 0.1\n"
        "interference-tolerance 0.001mm³\n"
        "\n"
        "import \"./plate.cadml\"\n"
        "param x = 1\n");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(r.meta.interference_tolerance, "0.001mm³");
}

// ─── Duplicate setting detection (C1) ───────────────────────────────

TEST(Frontmatter, DuplicateVersionErrors) {
    auto r = parse_fm(
        "version 0.1\n"
        "version 0.1.5\n");
    EXPECT_FALSE(r.ok());
    bool has_dup_msg = false;
    for (const auto& e : r.errors) {
        if (e.message.find("duplicate") != std::string::npos) has_dup_msg = true;
    }
    EXPECT_TRUE(has_dup_msg);
}

TEST(Frontmatter, DuplicateVersionEvenWhenSameValueErrors) {
    auto r = parse_fm(
        "version 0.1\n"
        "version 0.1\n");
    EXPECT_FALSE(r.ok());
}

TEST(Frontmatter, DuplicateUnitsErrors) {
    auto r = parse_fm(
        "version 0.1\n"
        "units mm\n"
        "units cm\n");
    EXPECT_FALSE(r.ok());
}

TEST(Frontmatter, VersionExplicitlySetFlag) {
    auto r = parse_fm("version 0.1\n");
    EXPECT_TRUE(r.version_explicitly_set);

    auto r2 = parse_fm("");
    EXPECT_FALSE(r2.version_explicitly_set);

    auto r3 = parse_fm("units mm\n");  // no version line
    EXPECT_FALSE(r3.version_explicitly_set);
}

// ─── Import alias collision with built-ins (C3, C4) ─────────────────

TEST(Frontmatter, ImportExplicitAliasCollidesWithBuiltinErrors) {
    auto r = parse_fm("import \"x.cadml\" as circle\n");
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.errors[0].message.find("circle"), std::string::npos);
    EXPECT_NE(r.errors[0].message.find("collides"), std::string::npos);
}

TEST(Frontmatter, ImportDefaultAliasCollidesWithBuiltinErrors) {
    // Default alias derived from filename: "circle".
    auto r = parse_fm("import \"shapes/circle.cadml\"\n");
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.errors[0].message.find("circle"), std::string::npos);
}

TEST(Frontmatter, ImportAliasMatchingPartErrors) {
    auto r = parse_fm("import \"thing.cadml\" as part\n");
    EXPECT_FALSE(r.ok());
}

TEST(Frontmatter, ImportAliasMatchingFlatOutputElementErrors) {
    // `param` is reserved as flat-output element.
    auto r = parse_fm("import \"x.cadml\" as param\n");
    EXPECT_FALSE(r.ok());
}

TEST(Frontmatter, ImportNonCollidingAliasOK) {
    auto r = parse_fm(
        "version 0.1\n"
        "import \"shapes/circle.cadml\" as my-circle\n");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.imports[0].alias, "my-circle");
}

TEST(Frontmatter, ErrorPointsAtCorrectLine) {
    auto r = parse_fm(
        "version 0.1\n"
        "units mm\n"
        "param oops = \n");  // empty value on line 3
    EXPECT_FALSE(r.ok());
    ASSERT_FALSE(r.errors.empty());
    EXPECT_EQ(r.errors[0].source.line, 3u);
}
