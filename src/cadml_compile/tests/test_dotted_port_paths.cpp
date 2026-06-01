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
        const auto base = fs::temp_directory_path() / "cadml_dotpath_test_";
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

bool any_error_contains(const CompileResult& r, std::string_view needle) {
    return std::any_of(r.errors.begin(), r.errors.end(),
        [&](const CompileError& e) {
            return e.message.find(needle) != std::string::npos;
        });
}

}  // namespace

// ─── Two-segment dotted at: reaches through bare sub-instance ────────

TEST(DottedPortPath, TwoSegmentReachesIntoSubAssembly) {
    TempProject p;
    // Plate has a `top` port at z=6.
    p.write("plate.cadml",
        "version 0.1\n"
        "<part>"
        "<extrude height=\"6\"><rect x=\"-20\" y=\"-20\" width=\"40\" height=\"40\"/></extrude>"
        "<port name=\"top\" position=\"0 0 6\" normal=\"+z\" up=\"+x\"/>"
        "</part>");
    // Bolt has a `head` port at z=30 (length default).
    p.write("bolt.cadml",
        "version 0.1\n"
        "<part>"
        "<extrude height=\"30\"><circle r=\"5\"/></extrude>"
        "<port name=\"head\" position=\"0 0 30\" normal=\"-z\" up=\"+x\"/>"
        "</part>");
    // Sub-assembly: a single bare plate-bottom.
    p.write("bip.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "<assembly name=\"bip\">"
        "<plate id=\"plate-bottom\"/>"
        "</assembly>");
    // Outer rig: bolt mates onto bip via dotted path.
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"bip.cadml\"\n"
        "import \"bolt.cadml\"\n"
        "<assembly name=\"rig\">"
        "<bip>"
        "<bolt at=\"plate-bottom.top\" port=\"head\"/>"
        "</bip>"
        "</assembly>");

    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // Bolt's head (z=30) lands on plate-bottom.top (z=6), so bolt
    // translates by -24 along z (head→top alignment, opposing normals).
    EXPECT_NE(r.flat_text.find("translate(0, 0, -24"), std::string::npos);
}

// ─── Dotted at via auto-id (no explicit id) ──────────────────────────

TEST(DottedPortPath, AutoIdMatchedByRefName) {
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
    // No explicit id on the inner plate — auto-id derives from ref_name
    // ("plate"), so the dotted path is "plate.top".
    p.write("bip.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "<assembly name=\"bip\">"
        "<plate/>"
        "</assembly>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"bip.cadml\"\n"
        "import \"bolt.cadml\"\n"
        "<assembly name=\"rig\">"
        "<bip>"
        "<bolt at=\"plate.top\" port=\"head\"/>"
        "</bip>"
        "</assembly>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
}

// ─── Unknown sub-instance segment fails clearly ──────────────────────

TEST(DottedPortPath, MissingSubInstanceErrors) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"6\"><circle r=\"20\"/></extrude>"
        "<port name=\"top\" position=\"0 0 6\" normal=\"+z\" up=\"+x\"/></part>");
    p.write("bolt.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"30\"><circle r=\"5\"/></extrude>"
        "<port name=\"head\" position=\"0 0 30\" normal=\"-z\" up=\"+x\"/></part>");
    p.write("bip.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "<assembly name=\"bip\">"
        "<plate id=\"plate-bottom\"/>"
        "</assembly>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"bip.cadml\"\n"
        "import \"bolt.cadml\"\n"
        "<assembly>"
        "<bip>"
        "<bolt at=\"nope.top\" port=\"head\"/>"
        "</bip>"
        "</assembly>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(any_error_contains(r, "cannot find sub-instance"));
}

// ─── Unknown terminal port fails ────────────────────────────────────

TEST(DottedPortPath, MissingTerminalPortErrors) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"6\"><circle r=\"20\"/></extrude>"
        "<port name=\"top\" position=\"0 0 6\" normal=\"+z\" up=\"+x\"/></part>");
    p.write("bolt.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"30\"><circle r=\"5\"/></extrude>"
        "<port name=\"head\" position=\"0 0 30\" normal=\"-z\" up=\"+x\"/></part>");
    p.write("bip.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "<assembly name=\"bip\">"
        "<plate id=\"plate-bottom\"/>"
        "</assembly>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"bip.cadml\"\n"
        "import \"bolt.cadml\"\n"
        "<assembly>"
        "<bip>"
        "<bolt at=\"plate-bottom.no-such-port\" port=\"head\"/>"
        "</bip>"
        "</assembly>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(any_error_contains(r, "not found on terminal def"));
}

// ─── Three-segment path: depth-2 namespace cascade ───────────────────
// inner.cadml exports a part with a `tip` port. mid.cadml imports inner
// and exposes it bare under id "inner-1". outer.cadml imports mid and
// reaches `inner-1.tip` two levels deep via "mid.inner-1.tip" from the
// rig. Exercises qualified_def_lookup at depth (mid → mid.inner).

TEST(DottedPortPath, ThreeSegmentNamespaceCascade) {
    TempProject p;
    p.write("inner.cadml",
        "version 0.1\n"
        "<part>"
        "<extrude height=\"3\"><circle r=\"1\"/></extrude>"
        "<port name=\"tip\" position=\"0 0 3\" normal=\"+z\" up=\"+x\"/>"
        "</part>");
    p.write("mid.cadml",
        "version 0.1\n"
        "import \"inner.cadml\"\n"
        "<assembly name=\"mid\">"
        "<inner id=\"inner-1\"/>"
        "</assembly>");
    p.write("bolt.cadml",
        "version 0.1\n"
        "<part>"
        "<extrude height=\"30\"><circle r=\"5\"/></extrude>"
        "<port name=\"head\" position=\"0 0 30\" normal=\"-z\" up=\"+x\"/>"
        "</part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"mid.cadml\"\n"
        "import \"bolt.cadml\"\n"
        "<assembly name=\"rig\">"
        "<mid id=\"mid\"/>"
        "<bolt id=\"bolt\"/>"
        // inner-1 lives inside mid; tip is its port. Two-segment path
        // relative to mid; three-segment from the connect's PoV.
        "<connect a=\"bolt.head\" b=\"mid.inner-1.tip\"/>"
        "</assembly>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // Bolt's head (z=30) lands on inner-1.tip (z=3), so bolt translates
    // by -27 along z (head→tip alignment, opposing normals).
    EXPECT_NE(r.flat_text.find("translate(0, 0, -27"), std::string::npos);
}

// ─── Dotted path inside an explicit <connect> ────────────────────────

TEST(DottedPortPath, ExplicitConnectAcceptsDottedPort) {
    TempProject p;
    p.write("plate.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"6\"><circle r=\"20\"/></extrude>"
        "<port name=\"top\" position=\"0 0 6\" normal=\"+z\" up=\"+x\"/></part>");
    p.write("bolt.cadml",
        "version 0.1\n<part>"
        "<extrude height=\"30\"><circle r=\"5\"/></extrude>"
        "<port name=\"head\" position=\"0 0 30\" normal=\"-z\" up=\"+x\"/></part>");
    p.write("bip.cadml",
        "version 0.1\n"
        "import \"plate.cadml\"\n"
        "<assembly name=\"bip\">"
        "<plate id=\"plate-bottom\"/>"
        "</assembly>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"bip.cadml\"\n"
        "import \"bolt.cadml\"\n"
        "<assembly>"
        "<bip id=\"bip\"/>"
        "<bolt id=\"bolt\"/>"
        // Connect bolt.head to bip's nested plate-bottom.top.
        "<connect a=\"bolt.head\" b=\"bip.plate-bottom.top\"/>"
        "</assembly>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
}
