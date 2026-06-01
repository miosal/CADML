// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace cadml;
using namespace cadml::compile;

namespace {

CompileResult cs(std::string_view src) { return compile_string(src); }

std::size_t count_substr(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return 0;
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count; pos += needle.size();
    }
    return count;
}

struct TempProject {
    fs::path root;
    TempProject() {
        const auto base = fs::temp_directory_path() / "cadml_assembly_test_";
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

}  // namespace

// ─── Simplest assembly: two parts, one mating ──────────────────────

TEST(AssemblyCompiler, SimpleNestedMating) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n"
        "param h = 6\n"
        "<part>"
        "<extrude height=\"{h}\"><rect x=\"-20\" y=\"-20\" width=\"40\" height=\"40\"/></extrude>"
        "<port name=\"hole\" position=\"0 0 {h}\" normal=\"+z\" up=\"+x\"/>"
        "</part>");
    p.write("bolt.cadml",
        "version 0.1\n"
        "param length = 30\n"
        "<part>"
        "<extrude height=\"{length}\"><circle r=\"5\"/></extrude>"
        "<port name=\"head\" position=\"0 0 {length}\" normal=\"-z\" up=\"+x\"/>"
        "</part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "import \"bolt.cadml\"\n"
        "<assembly name=\"rig\">"
        "<plate>"
        "<bolt at=\"hole\" port=\"head\" length=\"20\"/>"
        "</plate>"
        "</assembly>");

    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);

    // The <assembly> should be gone, replaced by a <part>.
    EXPECT_EQ(r.flat_text.find("<assembly"), std::string::npos);
    EXPECT_NE(r.flat_text.find("<part name=\"rig\""), std::string::npos);

    // Two groups: one for plate (identity), one for bolt (translated).
    EXPECT_EQ(count_substr(r.flat_text, "<group id=\"plate\""), 1u);
    EXPECT_EQ(count_substr(r.flat_text, "<group id=\"bolt\""),  1u);

    // The bolt's transform should include the expected translation
    // (plate.h=6, bolt.length=20 (overridden) → bolt at z=-14).
    EXPECT_NE(r.flat_text.find("translate(0, 0, -14"), std::string::npos);

    // Both bare instances inside their groups.
    EXPECT_NE(r.flat_text.find("<plate"), std::string::npos);
    EXPECT_NE(r.flat_text.find("<bolt"), std::string::npos);
}

// ─── Explicit <connect> form ───────────────────────────────────────

TEST(AssemblyCompiler, ExplicitConnect) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n"
        "<part>"
        "<extrude height=\"6\"><circle r=\"20\"/></extrude>"
        "<port name=\"top\" position=\"0 0 6\" normal=\"+z\" up=\"+x\"/>"
        "</part>");
    p.write("bolt.cadml",
        "version 0.1\n"
        "<part>"
        "<extrude height=\"30\"><circle r=\"5\"/></extrude>"
        "<port name=\"head\" position=\"0 0 30\" normal=\"-z\" up=\"+x\"/>"
        "</part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "import \"bolt.cadml\"\n"
        "<assembly>"
        "<plate id=\"p\"/>"
        "<bolt id=\"b\"/>"
        "<connect a=\"b.head\" b=\"p.top\"/>"
        "</assembly>");

    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_NE(r.flat_text.find("<group id=\"p\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("<group id=\"b\""), std::string::npos);
    // Bolt translated to z = 6 - 30 = -24.
    EXPECT_NE(r.flat_text.find("translate(0, 0, -24"), std::string::npos);
}

// ─── Top-level free-floating instance (no mating) ──────────────────

TEST(AssemblyCompiler, FreeFloatingTopLevelInstance) {
    TempProject p;
    p.write("widget.cadml",
        "version 0.1\n<part><circle r=\"5\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"widget.cadml\"\n"
        "<assembly name=\"rig\">"
        "<widget/>"  // no at/port — placed at identity
        "</assembly>");

    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_NE(r.flat_text.find("<group id=\"widget\""), std::string::npos);
    // Identity transform — no translate or rotate emitted.
    const auto group_pos = r.flat_text.find("<group id=\"widget\"");
    const auto next_close = r.flat_text.find('>', group_pos);
    const auto group_open_tag = r.flat_text.substr(group_pos, next_close - group_pos);
    EXPECT_EQ(group_open_tag.find("translate"), std::string::npos);
    EXPECT_EQ(group_open_tag.find("rotate"),    std::string::npos);
}

// ─── Auto-id generation ────────────────────────────────────────────

TEST(AssemblyCompiler, AutoIdsForUnnamedInstances) {
    TempProject p;
    p.write("widget.cadml",
        "version 0.1\n<part><circle r=\"5\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"widget.cadml\"\n"
        "<assembly>"
        "<widget/>"
        "<widget/>"
        "<widget/>"
        "</assembly>");

    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // First gets the bare alias, subsequent get suffixed.
    EXPECT_NE(r.flat_text.find("<group id=\"widget\""),    std::string::npos);
    EXPECT_NE(r.flat_text.find("<group id=\"widget#2\""),  std::string::npos);
    EXPECT_NE(r.flat_text.find("<group id=\"widget#3\""),  std::string::npos);
}

// ─── Param overrides survive into flat output ──────────────────────

TEST(AssemblyCompiler, ParamOverridesPreserved) {
    TempProject p;
    p.write("bolt.cadml",
        "version 0.1\n"
        "param length = 30\n"
        "<part>"
        "<extrude height=\"{length}\"><circle r=\"5\"/></extrude>"
        "<port name=\"head\" position=\"0 0 {length}\" normal=\"-z\" up=\"+x\"/>"
        "</part>");
    p.write("plate.cadml",
        "version 0.1\n"
        "<part>"
        "<extrude height=\"6\"><circle r=\"20\"/></extrude>"
        "<port name=\"top\" position=\"0 0 6\" normal=\"+z\" up=\"+x\"/>"
        "</part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "import \"bolt.cadml\"\n"
        "<assembly>"
        "<plate><bolt at=\"top\" port=\"head\" length=\"42\"/></plate>"
        "</assembly>");

    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // The bolt instance retains the length=42 override.
    EXPECT_NE(r.flat_text.find("length=\"42\""), std::string::npos);
}

// ─── Validation errors ─────────────────────────────────────────────

TEST(AssemblyCompiler, UnknownInstanceInConnectErrors) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n<part><circle r=\"5\"/>"
        "<port name=\"top\" position=\"0 0 6\" normal=\"+z\" up=\"+x\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "<assembly>"
        "<plate id=\"p\"/>"
        "<connect a=\"missing.x\" b=\"p.top\"/>"
        "</assembly>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_FALSE(r.ok());
}

TEST(AssemblyCompiler, UnknownPortErrors) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n<part><circle r=\"5\"/>"
        "<port name=\"top\" position=\"0 0 6\" normal=\"+z\" up=\"+x\"/></part>");
    p.write("bolt.cadml",
        "version 0.1\n<part><circle r=\"3\"/>"
        "<port name=\"head\" position=\"0 0 30\" normal=\"-z\" up=\"+x\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "import \"bolt.cadml\"\n"
        "<assembly>"
        "<plate><bolt at=\"nonexistent\" port=\"head\"/></plate>"
        "</assembly>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_FALSE(r.ok());
}

TEST(AssemblyCompiler, MatingInstanceAtTopLevelErrors) {
    TempProject p;
    p.write("widget.cadml",
        "version 0.1\n<part><circle r=\"5\"/>"
        "<port name=\"p\" position=\"0 0 0\" normal=\"+z\" up=\"+x\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"widget.cadml\"\n"
        "<assembly>"
        "<widget at=\"p\" port=\"p\"/>"  // no parent to mate against
        "</assembly>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_FALSE(r.ok());
}

TEST(AssemblyCompiler, MultipleBareInstancesAtTopLevelOK) {
    // Spec §6.2: bare instances at top of <assembly> are allowed —
    // they form a forest of unconnected sub-trees, each at identity.
    // Compilation succeeds (.ok()) but the disconnected-instance check
    // emits a warning about the multi-component assembly so cadml_check
    // can flag it.
    TempProject p;
    p.write("widget.cadml",
        "version 0.1\n<part><circle r=\"5\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"widget.cadml\"\n"
        "<assembly>"
        "<widget id=\"a\"/>"
        "<widget id=\"b\"/>"
        "</assembly>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_NE(r.flat_text.find("<group id=\"a\""), std::string::npos);
    EXPECT_NE(r.flat_text.find("<group id=\"b\""), std::string::npos);
    // D2 warning: 2 disconnected sub-trees.
    EXPECT_FALSE(r.warnings.empty());
    bool found = false;
    for (const auto& w : r.warnings) {
        if (w.message.find("disconnected") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found);
}

// ─── Disconnected-instance detection ────────────────────────────────

TEST(AssemblyCompiler, DisconnectedSubtreesEmitWarning) {
    // 3 bare top-level instances → 3 disconnected components.
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"3\"><rect width=\"5\" height=\"5\"/></extrude>"
        "<port name=\"top\" position=\"0 0 3\" normal=\"+z\" up=\"+x\"/>"
        "</part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "<assembly name=\"rig\">"
        "<plate id=\"a\"/>"
        "<plate id=\"b\"/>"
        "<plate id=\"c\"/>"
        "</assembly>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    ASSERT_FALSE(r.warnings.empty());
    bool found = false;
    for (const auto& w : r.warnings) {
        if (w.message.find("disconnected sub-trees") != std::string::npos &&
            w.message.find("3 disconnected") != std::string::npos &&
            w.message.find("`a`") != std::string::npos &&
            w.message.find("`b`") != std::string::npos &&
            w.message.find("`c`") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(AssemblyCompiler, ConnectedAssemblyHasNoDisconnectWarning) {
    // Plate + bolt linked via at/port mate → 1 component → no D2 warn.
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"3\"><rect width=\"10\" height=\"10\"/></extrude>"
        "<port name=\"top\" position=\"0 0 3\" normal=\"+z\" up=\"+x\"/>"
        "</part>");
    p.write("bolt.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"5\"><circle r=\"1\"/></extrude>"
        "<port name=\"head\" position=\"0 0 5\" normal=\"-z\" up=\"+x\"/>"
        "</part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "import \"bolt.cadml\"\n"
        "<assembly name=\"rig\">"
        "<plate><bolt at=\"top\" port=\"head\"/></plate>"
        "</assembly>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    for (const auto& w : r.warnings) {
        EXPECT_EQ(w.message.find("disconnected"), std::string::npos)
            << "unexpected disconnect warning: " << w.message;
    }
}

// ─── Over-constrained mate detection ────────────────────────────────
//
// "Under-constrained" in CADML's rigid 6-DoF port-mate model is the
// same condition D2 already catches (disconnected components). The
// meaningful inverse-of-D2 check is OVER-constraint detection:
// when the connect graph has a cycle whose closing edge implies a
// transform that disagrees with the BFS-computed placement.

TEST(AssemblyCompiler, InconsistentCycleEmitsOverConstraintWarning) {
    // Two parts mated face-to-face by at/port AND a separate
    // <connect> linking the SAME two parts via mismatched ports —
    // creates a cycle whose closing constraint contradicts the
    // primary mate.
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"3\"><rect width=\"10\" height=\"10\"/></extrude>"
        "<port name=\"top\"    position=\"0 0 3\" normal=\"+z\" up=\"+x\"/>"
        "<port name=\"bottom\" position=\"0 0 0\" normal=\"-z\" up=\"+x\"/>"
        "<port name=\"side\"   position=\"5 0 1.5\" normal=\"+x\" up=\"+z\"/>"
        "</part>");
    p.write("widget.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"5\"><circle r=\"1\"/></extrude>"
        "<port name=\"head\"   position=\"0 0 5\" normal=\"-z\" up=\"+x\"/>"
        "<port name=\"shaft\"  position=\"0 0 2.5\" normal=\"+x\" up=\"+z\"/>"
        "</part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "import \"widget.cadml\"\n"
        "<assembly name=\"rig\">"
        // Primary mate places widget's head on plate's top.
        "<plate id=\"pl\">"
        "<widget id=\"wid\" at=\"top\" port=\"head\"/>"
        "</plate>"
        // Redundant connect places widget's shaft on plate's side —
        // a different relative pose. Cycle closes inconsistently.
        "<connect a=\"pl.side\" b=\"wid.shaft\"/>"
        "</assembly>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    bool found = false;
    for (const auto& w : r.warnings) {
        if (w.message.find("closes a cycle") != std::string::npos &&
            w.message.find("over-constrained") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found)
        << "redundant inconsistent connect should warn about over-constraint";
}

TEST(AssemblyCompiler, ConsistentCycleHasNoOverConstraintWarning) {
    // Two redundant connects whose ports describe the SAME placement —
    // the cycle closes consistently and D3 must NOT warn.
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"3\"><rect width=\"10\" height=\"10\"/></extrude>"
        "<port name=\"top1\" position=\"0 0 3\" normal=\"+z\" up=\"+x\"/>"
        "<port name=\"top2\" position=\"0 0 3\" normal=\"+z\" up=\"+x\"/>"
        "</part>");
    p.write("bolt.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"5\"><circle r=\"1\"/></extrude>"
        "<port name=\"head1\" position=\"0 0 5\" normal=\"-z\" up=\"+x\"/>"
        "<port name=\"head2\" position=\"0 0 5\" normal=\"-z\" up=\"+x\"/>"
        "</part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "import \"bolt.cadml\"\n"
        "<assembly name=\"rig\">"
        "<plate id=\"pl\">"
        "<bolt id=\"bo\" at=\"top1\" port=\"head1\"/>"
        "</plate>"
        "<connect a=\"pl.top2\" b=\"bo.head2\"/>"
        "</assembly>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    for (const auto& w : r.warnings) {
        EXPECT_EQ(w.message.find("over-constrained"), std::string::npos)
            << "consistent cycle should not warn: " << w.message;
        EXPECT_EQ(w.message.find("closes a cycle"), std::string::npos)
            << "consistent cycle should not warn: " << w.message;
    }
}

TEST(AssemblyCompiler, SelfMateConnectErrors) {
    // <connect a="x.p" b="x.q"/> is nonsense in the rigid model and
    // would otherwise silently no-op (both adj entries collapse and
    // the cur<other dedup never fires). Reject it explicitly.
    TempProject p;
    p.write("widget.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"3\"><rect width=\"5\" height=\"5\"/></extrude>"
        "<port name=\"a\" position=\"0 0 3\" normal=\"+z\" up=\"+x\"/>"
        "<port name=\"b\" position=\"0 0 0\" normal=\"-z\" up=\"+x\"/>"
        "</part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"widget.cadml\"\n"
        "<assembly>"
        "<widget id=\"w\"/>"
        "<connect a=\"w.a\" b=\"w.b\"/>"
        "</assembly>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_FALSE(r.ok());
    bool found = false;
    for (const auto& e : r.errors) {
        if (e.message.find("cannot mate instance") != std::string::npos &&
            e.message.find("to itself") != std::string::npos) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found) << "self-mate connect should produce an explicit error";
}

// ─── allow-interference ─────────────────────────────────────────────

TEST(AssemblyCompiler, AllowInterferenceOnConnectAggregatedToMeta) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"3\"><rect width=\"10\" height=\"10\"/></extrude>"
        "<port name=\"top\" position=\"0 0 3\" normal=\"+z\" up=\"+x\"/>"
        "</part>");
    p.write("bolt.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"5\"><circle r=\"1\"/></extrude>"
        "<port name=\"head\" position=\"0 0 5\" normal=\"-z\" up=\"+x\"/>"
        "</part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "import \"bolt.cadml\"\n"
        "<assembly name=\"rig\">"
        "<plate id=\"pl\"/>"
        "<bolt id=\"bo\"/>"
        "<connect a=\"bo.head\" b=\"pl.top\" allow-interference=\"true\"/>"
        "</assembly>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    ASSERT_EQ(r.document.meta.allow_interference_pairs.size(), 1u);
    const auto& [a, b] = r.document.meta.allow_interference_pairs[0];
    // Pair recorded as (def-name of inst_a, def-name of inst_b);
    // explicit <connect> stores a as written, so order is bolt, plate.
    EXPECT_TRUE((a == "bolt" && b == "plate") ||
                  (a == "plate" && b == "bolt"));
}

TEST(AssemblyCompiler, NoAllowInterferenceLeavesMetaEmpty) {
    TempProject p;
    p.write("widget.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"3\"><rect width=\"5\" height=\"5\"/></extrude>"
        "<port name=\"top\" position=\"0 0 3\" normal=\"+z\" up=\"+x\"/>"
        "<port name=\"bot\" position=\"0 0 0\" normal=\"-z\" up=\"+x\"/>"
        "</part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"widget.cadml\"\n"
        "<assembly>"
        "<widget id=\"a\"/>"
        "<widget id=\"b\"/>"
        "<connect a=\"a.top\" b=\"b.bot\"/>"
        "</assembly>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.document.meta.allow_interference_pairs.empty());
}

TEST(AssemblyCompiler, AllowInterferencePairsDedup) {
    // Two connects between the same def pair, both allow-interference —
    // should land as a single entry in meta (de-duplicated).
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"3\"><rect width=\"10\" height=\"10\"/></extrude>"
        "<port name=\"a1\" position=\"0 0 3\" normal=\"+z\" up=\"+x\"/>"
        "<port name=\"a2\" position=\"0 0 0\" normal=\"-z\" up=\"+x\"/>"
        "</part>");
    p.write("bolt.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"5\"><circle r=\"1\"/></extrude>"
        "<port name=\"b1\" position=\"0 0 5\" normal=\"-z\" up=\"+x\"/>"
        "<port name=\"b2\" position=\"0 0 0\" normal=\"+z\" up=\"+x\"/>"
        "</part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "import \"bolt.cadml\"\n"
        "<assembly>"
        "<plate id=\"pl\"/>"
        "<bolt id=\"bo\"/>"
        "<connect a=\"bo.b1\" b=\"pl.a1\" allow-interference=\"true\"/>"
        "<connect a=\"pl.a2\" b=\"bo.b2\" allow-interference=\"true\"/>"
        "</assembly>");
    auto r = compile_file(p.root / "rig.cadml");
    // Compile may warn over-constrained but the meta should still be
    // populated; what we care about here is the dedup.
    ASSERT_EQ(r.document.meta.allow_interference_pairs.size(), 1u);
}

TEST(AssemblyCompiler, SingleInstanceAssemblyHasNoDisconnectWarning) {
    TempProject p;
    p.write("widget.cadml",
        "version 0.1\n<part><circle r=\"5\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"widget.cadml\"\n"
        "<assembly><widget id=\"only\"/></assembly>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok());
    for (const auto& w : r.warnings) {
        EXPECT_EQ(w.message.find("disconnected"), std::string::npos);
    }
}

// ─── Empty assembly ────────────────────────────────────────────────

TEST(AssemblyCompiler, EmptyAssemblyEmitsEmptyPart) {
    TempProject p;
    p.write("rig.cadml",
        "version 0.1\n"
        "<assembly name=\"rig\"/>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_EQ(r.flat_text.find("<assembly"), std::string::npos);
    EXPECT_NE(r.flat_text.find("<part name=\"rig\""), std::string::npos);
}

// ─── Realistic 3-part assembly ─────────────────────────────────────

TEST(AssemblyCompiler, BoltOnPlateWithPlateOnTable) {
    TempProject p;
    p.write("table.cadml",
        "version 0.1\n"
        "<part>"
        "<extrude height=\"50\"><circle r=\"100\"/></extrude>"
        "<port name=\"top\" position=\"0 0 50\" normal=\"+z\" up=\"+x\"/>"
        "</part>");
    p.write("plate.cadml",
        "version 0.1\n"
        "<part>"
        "<extrude height=\"6\"><circle r=\"20\"/></extrude>"
        "<port name=\"bottom\" position=\"0 0 0\" normal=\"-z\" up=\"+x\"/>"
        "<port name=\"hole\" position=\"0 0 6\" normal=\"+z\" up=\"+x\"/>"
        "</part>");
    p.write("bolt.cadml",
        "version 0.1\n"
        "<part>"
        "<extrude height=\"30\"><circle r=\"5\"/></extrude>"
        "<port name=\"head\" position=\"0 0 30\" normal=\"-z\" up=\"+x\"/>"
        "</part>");
    p.write("stack.cadml",
        "version 0.1\n"
        "import \"table.cadml\"\n"
        "import \"plate.cadml\"\n"
        "import \"bolt.cadml\"\n"
        "<assembly name=\"stack\">"
        "<table>"
        "<plate at=\"top\" port=\"bottom\">"
        "<bolt at=\"hole\" port=\"head\"/>"
        "</plate>"
        "</table>"
        "</assembly>");

    auto r = compile_file(p.root / "stack.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);

    // Three groups, three transforms.
    EXPECT_EQ(count_substr(r.flat_text, "<group id=\"table\""), 1u);
    EXPECT_EQ(count_substr(r.flat_text, "<group id=\"plate\""), 1u);
    EXPECT_EQ(count_substr(r.flat_text, "<group id=\"bolt\""),  1u);
    // Plate stacks on table top (z=50).
    EXPECT_NE(r.flat_text.find("translate(0, 0, 50"), std::string::npos);
}
