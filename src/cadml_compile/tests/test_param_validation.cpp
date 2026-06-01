// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace cadml::compile;

namespace {

struct TempProject {
    fs::path root;
    TempProject() {
        const auto base = fs::temp_directory_path() / "cadml_pvalid_test_";
        for (int i = 0; i < 100; ++i) {
            const auto candidate = base.string() + std::to_string(i);
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) { root = candidate; return; }
        }
        throw std::runtime_error("could not create temp dir");
    }
    ~TempProject() {
        std::error_code ec; fs::remove_all(root, ec);
    }
    void write(const fs::path& rel, std::string_view contents) {
        const auto full = root / rel;
        fs::create_directories(full.parent_path());
        std::ofstream f(full, std::ios::binary);
        f << contents;
    }
};

bool has_validation_error(const CompileResult& r, std::string_view needle) {
    return std::any_of(r.errors.begin(), r.errors.end(),
        [&](const CompileError& e) {
            return e.category == CompileError::Validation
                && e.message.find(needle) != std::string::npos;
        });
}

}  // namespace

// ─── Below-min override ────────────────────────────────────────────────

TEST(ParamValidation, RejectsBelowMin) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n"
        "param thickness = 5 (min=1, max=20)\n"
        "<part><circle r=\"5\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "<part><plate thickness=\"0.5\"/></part>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(has_validation_error(r, "below declared min"));
    EXPECT_TRUE(has_validation_error(r, "thickness"));
}

// ─── Above-max override ────────────────────────────────────────────────

TEST(ParamValidation, RejectsAboveMax) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n"
        "param thickness = 5 (min=1, max=20)\n"
        "<part><circle r=\"5\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "<part><plate thickness=\"100\"/></part>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(has_validation_error(r, "above declared max"));
}

// ─── In-range override accepted ───────────────────────────────────────

TEST(ParamValidation, AcceptsInRange) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n"
        "param thickness = 5 (min=1, max=20)\n"
        "<part><circle r=\"5\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "<part><plate thickness=\"10\"/></part>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
}

// ─── Boundary values are accepted ────────────────────────────────────

TEST(ParamValidation, AcceptsExactlyAtMin) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n"
        "param thickness = 5 (min=1, max=20)\n"
        "<part><circle r=\"5\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "<part><plate thickness=\"1\"/></part>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
}

TEST(ParamValidation, AcceptsExactlyAtMax) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n"
        "param thickness = 5 (min=1, max=20)\n"
        "<part><circle r=\"5\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "<part><plate thickness=\"20\"/></part>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
}

// ─── min-only / max-only constraints ─────────────────────────────────

TEST(ParamValidation, MinOnlyConstraint) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n"
        "param thickness = 5 (min=1)\n"
        "<part><circle r=\"5\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "<part><plate thickness=\"0\"/></part>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(has_validation_error(r, "below declared min"));
}

TEST(ParamValidation, MaxOnlyConstraint) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n"
        "param thickness = 5 (max=20)\n"
        "<part><circle r=\"5\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "<part><plate thickness=\"30\"/></part>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(has_validation_error(r, "above declared max"));
}

// ─── Param without bounds is silently accepted ────────────────────────

TEST(ParamValidation, NoBoundsMeansNoValidation) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n"
        "param thickness = 5\n"
        "<part><circle r=\"5\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "<part><plate thickness=\"-9999\"/></part>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
}

// ─── Override expression referencing entry-file param ─────────────────

TEST(ParamValidation, ResolvesEntryParamsInOverrideExpr) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n"
        "param thickness = 5 (min=1, max=20)\n"
        "<part><circle r=\"5\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "param scale = 3\n"
        "<part><plate thickness=\"{scale * 4}\"/></part>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
}

TEST(ParamValidation, EntryParamExprViolatesMax) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n"
        "param thickness = 5 (min=1, max=20)\n"
        "<part><circle r=\"5\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "param scale = 30\n"
        "<part><plate thickness=\"{scale * 4}\"/></part>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(has_validation_error(r, "above declared max"));
}

// ─── Multiple violations on the same instance ────────────────────────

TEST(ParamValidation, ReportsMultipleViolationsOnSameInstance) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n"
        "param thickness = 5 (min=1, max=20)\n"
        "param length = 100 (min=10, max=500)\n"
        "<part><circle r=\"5\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "<part><plate thickness=\"100\" length=\"5\"/></part>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(has_validation_error(r, "thickness"));
    EXPECT_TRUE(has_validation_error(r, "length"));
}

// ─── Violation inside an imported sub-assembly is still caught ───────
// Regression for namespace-qualification bug: an Instance whose ref_name
// is local to an imported sub-assembly's namespace must still resolve
// to its def for validation.

TEST(ParamValidation, CatchesViolationInsideImportedSubAssembly) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n"
        "param thickness = 5 (min=1, max=20)\n"
        "<part><circle r=\"5\"/></part>");
    p.write("bip.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "<assembly name=\"bip\">"
        "<plate id=\"plate-bottom\" thickness=\"100\"/>"
        "</assembly>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"bip.cadml\"\n"
        "<part><bip/></part>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(has_validation_error(r, "above declared max"));
    EXPECT_TRUE(has_validation_error(r, "thickness"));
}

// ─── Unknown override is ignored (matches engine convention) ──────────

TEST(ParamValidation, UnknownOverrideIsAnError) {
    // Per spec §6.7: an override naming a param the def doesn't
    // declare is a compile-time error. Silent swallow used to mask
    // typos like <my-hole d="3"/> when the def declared `diameter`.
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n"
        "param thickness = 5 (min=1, max=20)\n"
        "<part><circle r=\"5\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "<part><plate not-a-param=\"-9999\"/></part>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_FALSE(r.ok());
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors[0].message.find("not-a-param"), std::string::npos)
        << "expected the offender's name in the diagnostic";
}
