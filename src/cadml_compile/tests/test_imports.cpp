// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace cadml::compile;

namespace {

// Scratch-directory helper: writes files, cleans up on destruction.
struct TempProject {
    fs::path root;

    TempProject() {
        const auto base = fs::temp_directory_path() / "cadml_compile_test_";
        for (int i = 0; i < 100; ++i) {
            const auto candidate = base.string() + std::to_string(i);
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) {
                root = candidate;
                return;
            }
        }
        throw std::runtime_error("could not create temp dir");
    }

    ~TempProject() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void write(const fs::path& rel, std::string_view contents) {
        const auto full = root / rel;
        fs::create_directories(full.parent_path());
        std::ofstream f(full, std::ios::binary);
        f << contents;
    }
};

}  // namespace

// ─── Single-file via compile_file ───────────────────────────────────

TEST(Imports, CompileFileNoImports) {
    TempProject p;
    p.write("entry.cadml",
        "version 0.1\n"
        "<part name=\"x\"><circle r=\"5\"/></part>");
    auto r = compile_file(p.root / "entry.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_NE(r.flat_text.find("entry.cadml"), std::string::npos);
}

TEST(Imports, CompileFileMissingPathErrors) {
    auto r = compile_file("nonexistent-cadml-file.cadml");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.errors[0].category, CompileError::Parse);
}

// ─── Single-level import ────────────────────────────────────────────

TEST(Imports, ImportProducesDef) {
    TempProject p;
    p.write("bolt.cadml",
        "version 0.1\n"
        "param length = 30\n"
        "<part><extrude height=\"{length}\"><circle r=\"5\"/></extrude></part>");
    p.write("entry.cadml",
        "version 0.1\n"
        "import \"bolt.cadml\"\n"
        "<part>\n"
        "  <bolt/>\n"
        "</part>");

    auto r = compile_file(p.root / "entry.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);

    // Imported bolt should appear as a <def name="bolt"> in flat output.
    EXPECT_NE(r.flat_text.find("<def name=\"bolt\""), std::string::npos);
    // The import directive itself does NOT appear in flat output.
    EXPECT_EQ(r.flat_text.find("import \""), std::string::npos);
    // Imported file's params hoisted into the def.
    EXPECT_NE(r.flat_text.find("<param name=\"length\""), std::string::npos);
}

TEST(Imports, OversizedImportYieldsImportErrorWithPath) {
    // Files larger than cadml::kMaxSourceBytes (64 MiB) must be
    // rejected before the read so a hostile import can't OOM the
    // bundler. Synthesise a 65 MiB CADML file (just a long XML
    // comment — the byte count is what matters, not the shape).
    TempProject p;
    {
        const auto path = p.root / "huge.cadml";
        std::ofstream f(path, std::ios::binary);
        f << "version 0.1\n<!-- ";
        const std::string chunk(64ull * 1024, 'x');
        constexpr std::size_t cap = 64ull * 1024 * 1024;
        std::size_t written = 0;
        while (written < cap + 1024) {
            f.write(chunk.data(), chunk.size());
            written += chunk.size();
        }
        f << " -->\n<part><circle r=\"1\"/></part>\n";
    }
    p.write("entry.cadml",
        "version 0.1\n"
        "import \"huge.cadml\"\n"
        "<part><huge/></part>");

    auto r = compile_file(p.root / "entry.cadml");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.errors[0].category, CompileError::Import);
    EXPECT_NE(r.errors[0].message.find("huge.cadml"), std::string::npos);
    EXPECT_NE(r.errors[0].message.find("source-size limit"),
              std::string::npos);
}

TEST(Imports, ImportedPartColorPreservedOnDef) {
    // Regression: bundler used to discard `<part color>` when converting
    // an imported `<part>` into a `<def>`. The colour should survive
    // onto the synthesised DefAttrs so consumers see the original.
    TempProject p;
    p.write("widget.cadml",
        "version 0.1\n"
        "<part color=\"#abcdef\">"
        "<extrude height=\"5\"><circle r=\"3\"/></extrude>"
        "</part>");
    p.write("entry.cadml",
        "version 0.1\n"
        "import \"widget.cadml\"\n"
        "<part><widget/></part>");

    auto r = compile_file(p.root / "entry.cadml");
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // The colour must appear on the synthesised <def>.
    EXPECT_NE(r.flat_text.find("<def name=\"widget\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("color=\"#abcdef\""), std::string::npos);
}

TEST(Imports, ImportedDerivedParamsPreserveDeclarationOrder) {
    // Regression: merge_imported_doc used to splice each new param as
    // the first child of the def, which reversed declaration order and
    // broke subfile derived params (`param y = {x / 2}` would evaluate
    // before x was bound). The imported-file hoist now mirrors the
    // entry-file hoist and chains nodes in order.
    TempProject p;
    p.write("widget.cadml",
        "version 0.1\n"
        "param base = 10\n"
        "param double = {base * 2}\n"
        "param quad = {double * 2}\n"
        "<part>\n"
        "  <extrude height=\"{quad}\">\n"
        "    <circle r=\"{double}\"/>\n"
        "  </extrude>\n"
        "</part>");
    p.write("entry.cadml",
        "version 0.1\n"
        "import \"widget.cadml\"\n"
        "<part><widget/></part>");

    auto r = compile_file(p.root / "entry.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);

    // Hoisted params should appear in declaration order: base, double, quad.
    const auto base_pos   = r.flat_text.find("name=\"base\"");
    const auto double_pos = r.flat_text.find("name=\"double\"");
    const auto quad_pos   = r.flat_text.find("name=\"quad\"");
    ASSERT_NE(base_pos,   std::string::npos);
    ASSERT_NE(double_pos, std::string::npos);
    ASSERT_NE(quad_pos,   std::string::npos);
    EXPECT_LT(base_pos,   double_pos);
    EXPECT_LT(double_pos, quad_pos);
}

TEST(Imports, ExplicitAliasUsedInFlatOutput) {
    TempProject p;
    p.write("bolt.cadml",
        "version 0.1\n"
        "<part><circle r=\"5\"/></part>");
    p.write("entry.cadml",
        "version 0.1\n"
        "import \"bolt.cadml\" as small-bolt\n"
        "<part><small-bolt/></part>");

    auto r = compile_file(p.root / "entry.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_NE(r.flat_text.find("<def name=\"small-bolt\""), std::string::npos);
}

// ─── Multiple imports ───────────────────────────────────────────────

TEST(Imports, MultipleImports) {
    TempProject p;
    p.write("bolt.cadml",
        "version 0.1\n<part><circle r=\"5\"/></part>");
    p.write("plate.cadml",
        "version 0.1\n<part><rect width=\"40\" height=\"40\"/></part>");
    p.write("entry.cadml",
        "version 0.1\n"
        "import \"bolt.cadml\"\n"
        "import \"plate.cadml\"\n"
        "<part><bolt/><plate/></part>");

    auto r = compile_file(p.root / "entry.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_NE(r.flat_text.find("<def name=\"bolt\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("<def name=\"plate\""), std::string::npos);
}

// ─── Catalogue (ctl/) prefix ───────────────────────────────────────

TEST(Imports, CtlPrefixResolvesUnderEntryDir) {
    TempProject p;
    p.write("ctl/fasteners/hex-bolt.cadml",
        "version 0.1\n"
        "<part><circle r=\"5\"/></part>");
    p.write("entry.cadml",
        "version 0.1\n"
        "import \"ctl/fasteners/hex-bolt.cadml\"\n"
        "<part><hex-bolt/></part>");

    auto r = compile_file(p.root / "entry.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_NE(r.flat_text.find("<def name=\"hex-bolt\""), std::string::npos);
}

// ─── Cycle detection ───────────────────────────────────────────────

TEST(Imports, DirectCycleDetected) {
    TempProject p;
    p.write("a.cadml",
        "version 0.1\n"
        "import \"b.cadml\"\n"
        "<part><circle r=\"1\"/></part>");
    p.write("b.cadml",
        "version 0.1\n"
        "import \"a.cadml\"\n"
        "<part><circle r=\"2\"/></part>");

    auto r = compile_file(p.root / "a.cadml");
    EXPECT_FALSE(r.ok());
    bool found_circular = false;
    for (const auto& e : r.errors) {
        if (e.category == CompileError::Import &&
            e.message.find("circular") != std::string::npos) {
            found_circular = true;
        }
    }
    EXPECT_TRUE(found_circular);
}

// ─── Missing import ────────────────────────────────────────────────

TEST(Imports, MissingImportFileErrors) {
    TempProject p;
    p.write("entry.cadml",
        "version 0.1\n"
        "import \"does-not-exist.cadml\"\n"
        "<part/>");
    auto r = compile_file(p.root / "entry.cadml");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.errors[0].category, CompileError::Import);
}

// ─── Source files table ────────────────────────────────────────────

TEST(Imports, SourcesTableLogsAllFiles) {
    TempProject p;
    p.write("a.cadml",
        "version 0.1\n<part><circle r=\"1\"/></part>");
    p.write("b.cadml",
        "version 0.1\n<part><circle r=\"2\"/></part>");
    p.write("entry.cadml",
        "version 0.1\n"
        "import \"a.cadml\"\n"
        "import \"b.cadml\"\n"
        "<part><a/><b/></part>");

    auto r = compile_file(p.root / "entry.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_NE(r.flat_text.find("path=\"entry.cadml\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("path=\"a.cadml\""),     std::string::npos);
    EXPECT_NE(r.flat_text.find("path=\"b.cadml\""),     std::string::npos);
}

// ─── Imported file errors propagate ─────────────────────────────────

TEST(Imports, ImportedFileWithErrorsPropagates) {
    TempProject p;
    p.write("broken.cadml",
        "version 0.1\n"
        "<part><circle></part>");   // malformed
    p.write("entry.cadml",
        "version 0.1\n"
        "import \"broken.cadml\"\n"
        "<part/>");

    auto r = compile_file(p.root / "entry.cadml");
    EXPECT_FALSE(r.ok());
}
