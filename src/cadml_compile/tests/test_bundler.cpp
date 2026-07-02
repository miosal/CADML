// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace cadml;
using namespace cadml::compile;

namespace {

CompileResult cs(std::string_view src) {
    return compile_string(src);
}

}  // namespace

// ─── Single-file pass-through ───────────────────────────────────────

TEST(Bundler, MinimalPart) {
    auto r = cs(
        "version 0.1\n"
        "<part name=\"x\"><circle r=\"5\"/></part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_FALSE(r.flat_text.empty());
    // Flat output starts with version, then sources.
    EXPECT_NE(r.flat_text.find("version 0.1.0"), std::string::npos);
    EXPECT_NE(r.flat_text.find("<sources>"), std::string::npos);
    EXPECT_NE(r.flat_text.find("<part"), std::string::npos);
    EXPECT_NE(r.flat_text.find("circle"), std::string::npos);
}

TEST(Bundler, EmptyInputErrors) {
    // Per spec: empty file is OK at parser level but bundler should
    // probably warn about no exports. Today we accept it.
    auto r = cs("");
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.flat_text.empty());  // at least the version line
}

TEST(Bundler, MalformedXMLPropagatesError) {
    auto r = cs(
        "version 0.1\n"
        "<part><circle r=\"5\">");   // missing close tag
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.errors[0].category, CompileError::Parse);
}

// ─── Param hoisting ─────────────────────────────────────────────────

TEST(Bundler, FrontmatterParamsHoistedIntoPart) {
    auto r = cs(
        "version 0.1\n"
        "param chord = 100\n"
        "param thickness = 0.12 (min=0.05, max=0.3)\n"
        "<part>\n"
        "  <extrude height=\"{chord}\"><circle r=\"5\"/></extrude>\n"
        "</part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);

    // Params should appear in the AST inside the <part>.
    const auto& doc = r.document;
    EXPECT_TRUE(doc.params.empty());  // moved into the body

    // Find the <part>, verify its first children are <param>s.
    std::uint32_t part_idx = NO_NODE;
    for (std::uint32_t i = 0; i < doc.nodes.size(); ++i) {
        if (doc.nodes[i].type == NodeType::Part) { part_idx = i; break; }
    }
    ASSERT_NE(part_idx, NO_NODE);

    int param_count = 0;
    for (const auto& child : doc.children(part_idx)) {
        if (child.type == NodeType::Param) ++param_count;
    }
    EXPECT_EQ(param_count, 2);

    // Flat text shows them.
    EXPECT_NE(r.flat_text.find("<param name=\"chord\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("<param name=\"thickness\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("min=\"0.05\""), std::string::npos);
}

TEST(Bundler, ParamsAppearBeforeGeometry) {
    auto r = cs(
        "version 0.1\n"
        "param chord = 100\n"
        "<part><extrude height=\"{chord}\"><circle r=\"5\"/></extrude></part>");
    EXPECT_TRUE(r.ok());
    // Param should appear before extrude in the flat output.
    const auto pp = r.flat_text.find("<param");
    const auto ep = r.flat_text.find("<extrude");
    ASSERT_NE(pp, std::string::npos);
    ASSERT_NE(ep, std::string::npos);
    EXPECT_LT(pp, ep);
}

// ─── Source-files table ─────────────────────────────────────────────

TEST(Bundler, SingleFileEmitsOneSource) {
    auto r = cs("version 0.1\n<part/>");
    EXPECT_TRUE(r.ok());
    EXPECT_NE(r.flat_text.find("<source id=\"0\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("hash=\""), std::string::npos);
}

TEST(Bundler, StripSourcesOmitsSourcesAndSrcAttrs) {
    CompileOptions opts;
    opts.include_source_map = false;
    auto r = compile_string(
        "version 0.1\n<part><circle r=\"5\"/></part>",
        {}, opts);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.flat_text.find("<sources>"), std::string::npos);
    EXPECT_EQ(r.flat_text.find(" src=\""), std::string::npos);
}

// ─── Composition detection ──────────────────────────────────────────

// <assembly> is compiled by the assembly compiler — see
// test_assembly_compiler.cpp. An empty assembly (no instances)
// compiles to an empty <part>.
TEST(Bundler, EmptyAssemblyCompilesToEmptyPart) {
    auto r = cs(
        "version 0.1\n"
        "<assembly name=\"x\"/>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_NE(r.flat_text.find("<part name=\"x\""), std::string::npos);
    EXPECT_EQ(r.flat_text.find("<assembly"), std::string::npos);
}

// <for> and <pattern> are unrolled by the bundler — see
// test_for_unroller.cpp and test_pattern_unroller.cpp for behaviour.

// ─── Determinism ─────────────────────────────────────────────────────

TEST(Bundler, BundlingSameSourceTwiceProducesByteEqualOutput) {
    // The spec promises byte-stable .fcadml output for a given input
    // (language.md §10.7, flat-ir.md §7). Compile the same source
    // twice in this process and assert byte equality — catches any
    // surviving hash-order iteration in the bundler.
    const std::string src =
        "version 0.1\n"
        "param a = 10\n"
        "param b = 20\n"
        "<def name=\"box\"><extrude height=\"{a}\">"
        "<rect width=\"{b}\" height=\"{b}\"/></extrude></def>"
        "<def name=\"hole\"><extrude height=\"5\">"
        "<circle r=\"2\"/></extrude></def>"
        "<part name=\"x\">"
        "<difference><box/><hole/></difference>"
        "</part>";
    auto r1 = compile_string(src);
    auto r2 = compile_string(src);
    ASSERT_TRUE(r1.ok());
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r1.flat_text, r2.flat_text);
}

// ─── Reserved <extrude> attributes ──────────────────────────────────

TEST(Bundler, ExtrudeScaleRejected) {
    auto r = cs(
        "version 0.1\n"
        "<part><extrude height=\"5\" scale=\"0.5\">"
        "<circle r=\"3\"/></extrude></part>");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.errors[0].message.find("scale"), std::string::npos);
    EXPECT_EQ(r.errors[0].category, CompileError::Schema);
}

TEST(Bundler, ExtrudeDraftRejected) {
    auto r = cs(
        "version 0.1\n"
        "<part><extrude height=\"5\" draft=\"5\">"
        "<circle r=\"3\"/></extrude></part>");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.errors[0].message.find("draft"), std::string::npos);
}

TEST(Bundler, ExtrudeDirectionRejected) {
    auto r = cs(
        "version 0.1\n"
        "<part><extrude height=\"5\" direction=\"-z\">"
        "<circle r=\"3\"/></extrude></part>");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.errors[0].message.find("direction"), std::string::npos);
}

TEST(Bundler, ExtrudeDefaultsAccepted) {
    // Explicitly writing the defaults must not trip the reserved-attr
    // check (parser stores the defaults verbatim).
    auto r = cs(
        "version 0.1\n"
        "<part><extrude height=\"5\" scale=\"1\" draft=\"0\""
        " direction=\"+z\"><circle r=\"3\"/></extrude></part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
}

TEST(Bundler, ExtrudeDefaultsAcceptedSemanticallyEquivalent) {
    // `scale="1.0"` ≡ `scale="1"`; `draft="0.0"` ≡ `draft="0"`;
    // `direction="z"` ≡ `direction="+z"`. None of these change the
    // default geometry, so none of them should trip the check.
    auto r = cs(
        "version 0.1\n"
        "<part><extrude height=\"5\" scale=\"1.0\" draft=\"0.0\""
        " direction=\"z\"><circle r=\"3\"/></extrude></part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
}

// ─── Spec-version gating (§15) ──────────────────────────────────────

TEST(Bundler, Spec02VersionAccepted) {
    auto r = cs("version 0.2\n<part name=\"p\"><circle r=\"5\"/></part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // The flat output carries the declared version, normalised.
    EXPECT_NE(r.flat_text.find("version 0.2.0"), std::string::npos);
}

TEST(Bundler, UnknownSpecVersionRejected) {
    auto r = cs("version 0.3\n<part name=\"p\"/>");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.errors[0].message.find("unrecognized spec version"),
              std::string::npos);
}

TEST(Bundler, StlNotReservedInSpec01) {
    // §15.2 pinning: `stl` joined the reserved set in 0.2. A 0.1 file's
    // namespace is unaffected — it may keep using the name for its own
    // defs and references.
    auto r = cs(
        "version 0.1\n"
        "<def name=\"stl\"><extrude height=\"1\"><circle r=\"1\"/></extrude></def>\n"
        "<part name=\"p\"><stl/></part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
}

TEST(Bundler, DefNamedStlRejectedInSpec02) {
    auto r = cs(
        "version 0.2\n"
        "<def name=\"stl\"><circle r=\"1\"/></def>\n"
        "<part name=\"p\"/>");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.errors[0].message.find("collides with a built-in"),
              std::string::npos);
}

// ─── <stl> source validation ────────────────────────────────────────

TEST(Bundler, StlBothSourcesRejected) {
    // Spec: the mesh comes from exactly one source. Both set previously
    // compiled clean and eval silently ignored `src`.
    auto r = cs(
        "version 0.2\n"
        "<part><stl src=\"cube.stl\" data=\"AAAA\"/></part>");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.errors[0].message.find("both `src` and `data`"),
              std::string::npos);
    EXPECT_EQ(r.errors[0].category, CompileError::Schema);
}

TEST(Bundler, StlNoSourceRejected) {
    // Bare <stl/> previously compiled clean and only warned at eval with
    // an empty mesh.
    auto r = cs("version 0.2\n<part><stl/></part>");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.errors[0].message.find("no mesh source"), std::string::npos);
    EXPECT_EQ(r.errors[0].category, CompileError::Schema);
}

TEST(Bundler, StlSrcInSingleFileModeRejected) {
    // compile_string with no base_dir (the WASM bindings path) cannot
    // resolve external references: an unresolvable import is a compile
    // error, and <stl src> must behave the same rather than compiling
    // clean and rendering an empty mesh at eval.
    auto r = cs("version 0.2\n<part><stl src=\"cube.stl\"/></part>");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.errors[0].message.find("no base directory"),
              std::string::npos);
    EXPECT_EQ(r.errors[0].category, CompileError::Import);
}

TEST(Bundler, CutSurvivesIntoFlatOutput) {
    // <cut> stays in the flat output (engine resolves at evaluate time
    // because pivot-edge positioning needs target bbox).
    auto r = cs(
        "version 0.1\n"
        "<part>"
        "<cut face=\"end\" type=\"miter\" angle=\"45\">"
        "<extrude height=\"10\"><rect width=\"5\" height=\"5\"/></extrude>"
        "</cut>"
        "</part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_NE(r.flat_text.find("<cut"), std::string::npos);
}

TEST(Bundler, BareInstanceOK) {
    // Bare instance (no at/port) is allowed inside <part>; it's the
    // mating instance with at/port that's not yet supported.
    auto r = cs(
        "version 0.1\n"
        "<def name=\"blade\"><circle r=\"3\"/></def>"
        "<part><blade/></part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
}

TEST(Bundler, MatingInstanceErrorsAsUnsupported) {
    auto r = cs(
        "version 0.1\n"
        "<assembly>"
        "<plate><bolt at=\"hole\" port=\"head\"/></plate>"
        "</assembly>");
    EXPECT_FALSE(r.ok());
    // Both <assembly> AND mating instance trigger errors; one is enough.
    bool found_composition = false;
    for (const auto& e : r.errors) {
        if (e.category == CompileError::Composition) found_composition = true;
    }
    EXPECT_TRUE(found_composition);
}

// ─── Realistic single-file CADML ───────────────────────────────────

TEST(Bundler, BoltCatalogueFile) {
    auto r = cs(
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

    // Confirm key elements survived to the flat output.
    EXPECT_NE(r.flat_text.find("version 0.1.0"), std::string::npos);
    EXPECT_NE(r.flat_text.find("description"), std::string::npos);
    EXPECT_NE(r.flat_text.find("Hex bolt"), std::string::npos);
    EXPECT_NE(r.flat_text.find("<param name=\"length\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("<extrude"), std::string::npos);
    EXPECT_NE(r.flat_text.find("<port"), std::string::npos);
}

TEST(Bundler, LuaScriptPreservedThroughBundling) {
    auto r = cs(
        "version 0.1\n"
        "<script lang=\"lua\"><![CDATA[\n"
        "function naca(c, t, n)\n"
        "  if n < 1 then return \"\" end\n"
        "  return cadml.path({{0,0}, {c,0}})\n"
        "end\n"
        "]]></script>"
        "<part>\n"
        "  <extrude height=\"10\">\n"
        "    <path d=\"{naca(50, 0.12, 40)}\"/>\n"
        "  </extrude>\n"
        "</part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // The <script> block is preserved in the flat output for source-map
    // provenance, even though its functions have already been invoked
    // eagerly at bundle time (spec §9.2; compiler.md §2.10).
    EXPECT_NE(r.flat_text.find("<script"), std::string::npos);
    EXPECT_NE(r.flat_text.find("naca"), std::string::npos);
    // Lua-call expressions are evaluated eagerly: the {naca(...)} body
    // has been substituted with the function's return value
    // (`cadml.path` produces an SVG-style "M ... L ... Z" string).
    EXPECT_EQ(r.flat_text.find("{naca("), std::string::npos)
        << "Lua-call expressions must NOT survive symbolic per spec §9.2";
    EXPECT_NE(r.flat_text.find("M 0,0 L 50,0 Z"), std::string::npos)
        << "the substituted path data must appear in the d= attribute";
}

// ─── Idempotence on .fcadml input (spec §10.7) ──────────────────────

// Spec §10.7: cadml_compile is idempotent on flat input — running it
// on a .fcadml produces the same .fcadml modulo source paths.
// Regression: `<recurser src="0:8:6"/>` was being treated as a param
// override on re-parse, exploding validation.
TEST(Bundler, IdempotentOnFlatInputWithLocalDefRef) {
    const std::string src =
        "version 0.1\n"
        "<def name=\"d\">\n"
        "  <rect width=\"1\" height=\"1\"/>\n"
        "</def>\n"
        "<part name=\"x\">\n"
        "  <d/>\n"
        "</part>";
    auto r1 = cs(src);
    ASSERT_TRUE(r1.ok()) << (r1.errors.empty() ? "" : r1.errors[0].message);

    // Re-compile the .fcadml. Before the fix this failed with
    // "instance `d` overrides param `src` …" because src= on <d/>
    // wasn't on the reserved-attr list.
    auto r2 = cs(r1.flat_text);
    EXPECT_TRUE(r2.ok())
        << (r2.errors.empty() ? "" : r2.errors[0].message);
    EXPECT_FALSE(r2.flat_text.empty());

    // No double <sources>: parse() extracts the <sources> block into
    // Document.source_files and marks the body nodes dead, so the
    // serializer re-emits exactly one block at the top.
    auto first  = r2.flat_text.find("<sources>");
    auto second = r2.flat_text.find("<sources>", first + 1);
    EXPECT_NE(first, std::string::npos);
    EXPECT_EQ(second, std::string::npos)
        << "duplicate <sources> block on round-trip";

    // src= back-references on body nodes preserve the original line
    // numbers from r1 (not r2's own physical positions). The <d/>
    // instance ref was at source line 6 in `src`.
    EXPECT_NE(r2.flat_text.find("<d src=\"0:6:"), std::string::npos)
        << "src= attributes should survive round-trip with original lines";
}

// Cycle-stable: compile(compile(x)) == compile(compile(compile(x)))
// modulo the synthetic source path. Verifies extraction populates
// source_files cleanly so the second-cycle output is byte-identical
// to the first-cycle output (apart from the `path=` field, which the
// bundler always rewrites to the current entry).
TEST(Bundler, IdempotentTwoCycleStability) {
    const std::string src =
        "version 0.1\n"
        "<part name=\"p\">\n"
        "  <extrude height=\"10\"><circle r=\"3\"/></extrude>\n"
        "</part>";
    auto r1 = cs(src);
    ASSERT_TRUE(r1.ok());
    auto r2 = cs(r1.flat_text);
    ASSERT_TRUE(r2.ok());
    auto r3 = cs(r2.flat_text);
    ASSERT_TRUE(r3.ok());

    // Compare r2 and r3 ignoring the path= field on <source>.
    auto strip_source_path = [](std::string s) {
        const std::string needle = "path=\"";
        auto i = s.find(needle);
        if (i == std::string::npos) return s;
        i += needle.size();
        const auto j = s.find('"', i);
        if (j == std::string::npos) return s;
        return s.replace(i, j - i, "<entry>");
    };
    EXPECT_EQ(strip_source_path(r2.flat_text),
              strip_source_path(r3.flat_text))
        << "second-cycle re-bundle must be stable modulo source path";
}

// ─── Deep-recursion DoS regression ──────────────────────────────────
//
// The original cycle detector was a recursive std::function DFS; a long
// linear chain of un-nested defs (a0→a1→…→aN) drove it N deep and
// overflowed the call stack (~26k–30k on a 1 MB stack) — crashing
// inside the very check meant to guard against pathological def graphs.
// It is now an explicit-stack iterative DFS. This test drives a chain
// far past the old crash threshold; it must terminate cleanly (no
// crash), and since a linear chain is acyclic, it must NOT report a
// cycle. The prior remediation's sweep only tested 1024-deep nesting —
// below the crash threshold — which is why it slipped through.
TEST(Bundler, DeepDefChainDoesNotOverflow) {
    constexpr int kChain = 50000;   // well past the ~26k–30k crash point
    std::string src = "version 0.1\n";
    for (int i = 0; i < kChain; ++i) {
        if (i < kChain - 1) {
            src += "<def name=\"a" + std::to_string(i) + "\"><a" +
                   std::to_string(i + 1) + "/></def>\n";
        } else {
            src += "<def name=\"a" + std::to_string(i) +
                   "\"><rect width=\"1\" height=\"1\"/></def>\n";
        }
    }
    src += "<part name=\"p\"><a0/></part>\n";

    auto r = cs(src);   // must return, not crash
    // A linear chain is acyclic → no cyclic-def error.
    for (const auto& e : r.errors) {
        EXPECT_EQ(e.message.find("cyclic <def>"), std::string::npos)
            << "linear def chain wrongly flagged as a cycle: " << e.message;
    }
}

// Companion: a genuine def cycle must still be DETECTED by the iterative
// DFS (the fix must not lose the feature). a→b→a is the smallest cycle.
TEST(Bundler, DefCycleStillDetected) {
    auto r = cs(
        "version 0.1\n"
        "<def name=\"a\"><b/></def>\n"
        "<def name=\"b\"><a/></def>\n"
        "<part name=\"p\"><a/></part>\n");
    bool found = false;
    for (const auto& e : r.errors) {
        if (e.message.find("cyclic <def>") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found) << "mutual def cycle a→b→a must be reported";
}

// ─── <def> cycle detection (Block 3 B3.6) ───────────────────────────

// Direct self-reference: <def name="r"><r/></def>. The engine has a
// runtime depth guard, but the bundler should reject this at compile
// time with a clear, source-located error.
TEST(Bundler, DefDirectSelfReferenceRejected) {
    auto r = cs(
        "version 0.1\n"
        "<def name=\"r\">\n"
        "  <union>\n"
        "    <rect width=\"1\" height=\"1\"/>\n"
        "    <r/>\n"
        "  </union>\n"
        "</def>\n"
        "<part name=\"x\"><r/></part>");
    EXPECT_FALSE(r.ok());
    ASSERT_FALSE(r.errors.empty());
    EXPECT_EQ(r.errors[0].category, CompileError::Composition);
    EXPECT_NE(r.errors[0].message.find("cyclic <def>"), std::string::npos)
        << r.errors[0].message;
    EXPECT_NE(r.errors[0].message.find("`r`"), std::string::npos)
        << r.errors[0].message;
}

// Transitive (mutual) recursion: ping → pong → ping. Must also be
// caught — a depth-1 self-reference check would miss this.
TEST(Bundler, DefMutualRecursionRejected) {
    auto r = cs(
        "version 0.1\n"
        "<def name=\"ping\"><union><rect width=\"1\" height=\"1\"/><pong/></union></def>\n"
        "<def name=\"pong\"><union><rect width=\"1\" height=\"1\"/><ping/></union></def>\n"
        "<part name=\"x\"><ping/></part>");
    EXPECT_FALSE(r.ok());
    ASSERT_FALSE(r.errors.empty());
    EXPECT_EQ(r.errors[0].category, CompileError::Composition);
    EXPECT_NE(r.errors[0].message.find("cyclic <def>"), std::string::npos)
        << r.errors[0].message;
}

// Negative case — guards against false positives. An ACYCLIC def chain
// (leaf ← branch ← part) is legitimate composition and must compile.
TEST(Bundler, DefAcyclicChainAccepted) {
    auto r = cs(
        "version 0.1\n"
        "<def name=\"leaf\"><rect width=\"1\" height=\"1\"/></def>\n"
        "<def name=\"branch\"><union><leaf/><leaf/></union></def>\n"
        "<part name=\"x\"><extrude height=\"2\"><branch/></extrude></part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
}

// A def referencing itself only from a <part> use-site (NOT from inside
// the def body) is not a cycle — the same def name appearing twice in a
// part is ordinary repeated instantiation.
TEST(Bundler, DefReusedFromPartIsNotACycle) {
    auto r = cs(
        "version 0.1\n"
        "<def name=\"hole\"><circle r=\"1\"/></def>\n"
        "<part name=\"x\">\n"
        "  <extrude height=\"2\">\n"
        "    <rect width=\"10\" height=\"10\"/>\n"
        "  </extrude>\n"
        "  <hole/>\n"
        "  <hole/>\n"
        "</part>");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
}
