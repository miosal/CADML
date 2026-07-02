// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>
#include <cadml/constants.hpp>          // kMaxSourceBytes

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace cadml::compile;

namespace {

std::vector<InMemoryFile> files(std::initializer_list<InMemoryFile> fs) {
    return std::vector<InMemoryFile>(fs);
}

}  // namespace

// ─── Single file ────────────────────────────────────────────────────

TEST(InMemory, SingleFileNoImports) {
    auto r = compile_in_memory(
        files({ { "main.cadml",
                  "version 0.1\n<part name=\"x\"><circle r=\"5\"/></part>" } }),
        "main.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_NE(r.flat_text.find("<part"), std::string::npos);
    EXPECT_NE(r.flat_text.find("circle"), std::string::npos);
    // The entry's <source path="..."> is the supplied key, not "<entry>".
    EXPECT_NE(r.flat_text.find("path=\"main.cadml\""), std::string::npos);
}

TEST(InMemory, MissingEntryErrors) {
    auto r = compile_in_memory(
        files({ { "main.cadml", "version 0.1\n<part name=\"x\"/>" } }),
        "not-there.cadml");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.errors[0].category, CompileError::Import);
}

// ─── Multi-file imports by map key ──────────────────────────────────

TEST(InMemory, ImportProducesDef) {
    auto r = compile_in_memory(
        files({
            { "bolt.cadml",
              "version 0.1\nparam length = 30\n"
              "<part><extrude height=\"{length}\"><circle r=\"5\"/></extrude></part>" },
            { "main.cadml",
              "version 0.1\nimport \"bolt.cadml\"\n<part><bolt/></part>" },
        }),
        "main.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_NE(r.flat_text.find("<def name=\"bolt\""), std::string::npos);
    EXPECT_EQ(r.flat_text.find("import \""), std::string::npos);
    EXPECT_NE(r.flat_text.find("<param name=\"length\""), std::string::npos);
}

TEST(InMemory, SubdirRelativeImportResolves) {
    // An entry in a subdir importing a sibling, plus a parent-relative
    // import that normalizes back to the root — both must resolve to the
    // right map keys.
    auto r = compile_in_memory(
        files({
            { "lib/helper.cadml",
              "version 0.1\n<part><circle r=\"2\"/></part>" },
            { "root.cadml",
              "version 0.1\n<part><circle r=\"9\"/></part>" },
            { "sub/main.cadml",
              "version 0.1\n"
              "import \"helper.cadml\" as h\n"          // sub/helper? no — see below
              "<part><h/></part>" },
        }),
        "sub/main.cadml");
    // sub/main imports "helper.cadml" → key "sub/helper.cadml", which is
    // NOT in the map → expect a clean import error (proves keys are
    // resolved relative to the importing file's dir).
    EXPECT_FALSE(r.ok());
    bool found = false;
    for (const auto& e : r.errors)
        if (e.message.find("cannot find imported file: sub/helper.cadml")
            != std::string::npos) found = true;
    EXPECT_TRUE(found) << "expected sub/-relative key in the not-found error";
}

TEST(InMemory, ParentRelativeImportResolvesToRoot) {
    auto r = compile_in_memory(
        files({
            { "shared/box.cadml",
              "version 0.1\n<part><rect width=\"4\" height=\"4\"/></part>" },
            { "sub/main.cadml",
              "version 0.1\n"
              "import \"../shared/box.cadml\" as box\n"   // → key "shared/box.cadml"
              "<part><box/></part>" },
        }),
        "sub/main.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_NE(r.flat_text.find("<def name=\"box\""), std::string::npos);
    // The shared file is recorded under its normalized root-relative key.
    EXPECT_NE(r.flat_text.find("path=\"shared/box.cadml\""), std::string::npos);
}

TEST(InMemory, LuaModuleImport) {
    auto r = compile_in_memory(
        files({
            { "helpers.lua",
              "function dbl(x) return x * 2 end\n" },
            { "main.cadml",
              "version 0.1\nimport \"helpers.lua\"\n"
              "<part><extrude height=\"{helpers.dbl(5)}\">"
              "<rect width=\"1\" height=\"1\"/></extrude></part>" },
        }),
        "main.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // The Lua call is evaluated eagerly at bundle time → 10 lands literal.
    EXPECT_NE(r.flat_text.find("height=\"10\""), std::string::npos);
}

// ─── Security: traversal is structurally rejected ───────────────────

TEST(InMemory, ParentEscapeRejected) {
    auto r = compile_in_memory(
        files({
            { "main.cadml",
              "version 0.1\nimport \"../../etc/passwd\"\n<part/>" },
        }),
        "main.cadml");
    EXPECT_FALSE(r.ok());
    bool found = false;
    for (const auto& e : r.errors)
        if (e.message.find("outside the project root") != std::string::npos)
            found = true;
    EXPECT_TRUE(found) << "../ escape must be refused";
}

TEST(InMemory, RootAnchoredImportRejected) {
    auto r = compile_in_memory(
        files({
            { "main.cadml",
              "version 0.1\nimport \"/etc/passwd\"\n<part/>" },
        }),
        "main.cadml");
    EXPECT_FALSE(r.ok());
}

// ─── Security: <stl src> reuses the import containment guards ───────
// Regression pins for resolve_stl_imports — these all work by reusing
// resolve_import_key / key_escapes_root and the provider size cap, and
// must keep failing closed if that plumbing is ever refactored.

TEST(InMemory, StlAbsolutePathRejected) {
    auto r = compile_in_memory(
        files({
            { "main.cadml",
              "version 0.2\n<part><stl src=\"/etc/passwd\"/></part>" },
        }),
        "main.cadml");
    EXPECT_FALSE(r.ok());
    bool found = false;
    for (const auto& e : r.errors)
        if (e.category == CompileError::Import &&
            e.message.find("absolute paths are not permitted")
                != std::string::npos) found = true;
    EXPECT_TRUE(found) << "absolute <stl src> must be refused before any read";
}

TEST(InMemory, StlParentEscapeRejected) {
    auto r = compile_in_memory(
        files({
            { "main.cadml",
              "version 0.2\n<part><stl src=\"../../escape.stl\"/></part>" },
        }),
        "main.cadml");
    EXPECT_FALSE(r.ok());
    bool found = false;
    for (const auto& e : r.errors)
        if (e.category == CompileError::Import &&
            e.message.find("outside the project root") != std::string::npos)
            found = true;
    EXPECT_TRUE(found) << "../ escape in <stl src> must be refused";
}

TEST(InMemory, StlOversizeSourceRejected) {
    // One byte over kMaxSourceBytes: the provider reports too_large and
    // the bundler surfaces the size limit instead of embedding 64 MiB.
    auto r = compile_in_memory(
        files({
            { "big.stl", std::string(cadml::kMaxSourceBytes + 1, 's') },
            { "main.cadml",
              "version 0.2\n<part><stl src=\"big.stl\"/></part>" },
        }),
        "main.cadml");
    EXPECT_FALSE(r.ok());
    bool found = false;
    for (const auto& e : r.errors)
        if (e.category == CompileError::Import &&
            e.message.find("size limit") != std::string::npos) found = true;
    EXPECT_TRUE(found) << "oversize <stl src> must be refused, not embedded";
}

TEST(InMemory, StlFlatOutputRecompilesClean) {
    // The bundler emits <stl data=… src="file:line:col"> where `src` is
    // the source-map back-reference every flat element carries — the
    // mesh source was already lowered into `data`. Re-compiling the flat
    // output must not read the back-reference as a second mesh source
    // (spec §10.7 idempotence; the parser strips back-reference-shaped
    // values from StlAttrs.src).
    auto r = compile_in_memory(
        files({
            { "cube.stl", "opaque bytes; resolution just embeds them" },
            { "main.cadml",
              "version 0.2\n<part><stl src=\"cube.stl\"/></part>" },
        }),
        "main.cadml");
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    auto r2 = compile_string(r.flat_text);
    EXPECT_TRUE(r2.ok()) << (r2.errors.empty() ? "" : r2.errors[0].message);
}

// ─── Spec versions across imports (§15.3) ───────────────────────────

TEST(InMemory, ImportRequiringNewerSpecRejected) {
    // A 0.1 entry importing a 0.2 file: the flat output would declare
    // 0.1 yet contain 0.2 vocabulary, so it must be refused at the
    // import site with a pointer at the fix.
    auto r = compile_in_memory(
        files({
            { "mesh.cadml",
              "version 0.2\n<part name=\"m\"><stl data=\"AAAA\"/></part>" },
            { "main.cadml",
              "version 0.1\nimport \"mesh.cadml\"\n"
              "<part name=\"p\"><mesh/></part>" },
        }),
        "main.cadml");
    ASSERT_FALSE(r.ok());
    bool found = false;
    for (const auto& e : r.errors)
        if (e.message.find("newer spec") != std::string::npos) found = true;
    EXPECT_TRUE(found) << "0.1 entry must not import a 0.2 file";
}

TEST(InMemory, ImportOfOlderSpecFileAccepted) {
    // A 0.2 entry importing a 0.1 library is fine — 0.1 vocabulary is a
    // strict subset of 0.2's.
    auto r = compile_in_memory(
        files({
            { "lib.cadml",
              "version 0.1\n<part name=\"l\"><circle r=\"2\"/></part>" },
            { "main.cadml",
              "version 0.2\nimport \"lib.cadml\" as lib\n"
              "<part name=\"p\"><lib/></part>" },
        }),
        "main.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
}

// ─── Cycle detection works through the in-memory provider ───────────

TEST(InMemory, CircularImportDetected) {
    auto r = compile_in_memory(
        files({
            { "a.cadml", "version 0.1\nimport \"b.cadml\"\n<part><b/></part>" },
            { "b.cadml", "version 0.1\nimport \"a.cadml\"\n<part><a/></part>" },
        }),
        "a.cadml");
    EXPECT_FALSE(r.ok());
    bool found = false;
    for (const auto& e : r.errors)
        if (e.message.find("circular import") != std::string::npos) found = true;
    EXPECT_TRUE(found);
}

// ─── Parity with compile_string on the single-file case ─────────────

TEST(InMemory, ParityWithCompileStringSingleFile) {
    const std::string src =
        "version 0.1\n"
        "param r = 4\n"
        "<part name=\"p\"><extrude height=\"10\"><circle r=\"{r}\"/></extrude></part>";

    // compile_string (no base_dir) and compile_in_memory should produce
    // identical geometry. The <sources> path differs ("<entry>" vs the
    // map key), so compare the body after the sources block.
    auto a = compile_string(src);
    auto b = compile_in_memory(files({ { "p.cadml", src } }), "p.cadml");
    ASSERT_TRUE(a.ok()) << (a.errors.empty() ? "" : a.errors[0].message);
    ASSERT_TRUE(b.ok()) << (b.errors.empty() ? "" : b.errors[0].message);

    auto body = [](const std::string& s) {
        const auto pos = s.find("<part");
        return pos == std::string::npos ? s : s.substr(pos);
    };
    EXPECT_EQ(body(a.flat_text), body(b.flat_text));
}
