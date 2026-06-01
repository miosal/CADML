// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>
#include <cadml/lua.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace cadml::compile;

namespace {

struct TempProject {
    fs::path root;
    TempProject() {
        const auto base = fs::temp_directory_path() / "cadml_lua_test_";
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

// ─── Free-form Lua module ──────────────────────────────────────────

TEST(LuaImports, FreeFormModule) {
    TempProject p;
    p.write("airfoils.lua",
        "function naca(c, t, n) return c * t * n end\n"
        "function clarky(c, n) return c + n end\n");
    p.write("wing.cadml",
        "version 0.1\n"
        "import \"airfoils.lua\"\n"
        "<part><circle r=\"5\"/></part>");
    auto r = compile_file(p.root / "wing.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // A <script> element appears in the flat output containing the
    // alias-binding wrapper.
    EXPECT_NE(r.flat_text.find("<script"), std::string::npos);
    EXPECT_NE(r.flat_text.find("function naca"),  std::string::npos);
    EXPECT_NE(r.flat_text.find("airfoils"),       std::string::npos);
    // Free-form wrapper signature.
    EXPECT_NE(r.flat_text.find("for k, _ in pairs(_G) do __pre[k] = true"),
                std::string::npos);
}

// ─── Module-pattern Lua module ─────────────────────────────────────

TEST(LuaImports, ModulePatternBindsReturnedTable) {
    TempProject p;
    p.write("math2.lua",
        "local M = {}\n"
        "function M.add(a, b) return a + b end\n"
        "function M.mul(a, b) return a * b end\n"
        "return M\n");
    p.write("calc.cadml",
        "version 0.1\n"
        "import \"math2.lua\"\n"
        "<part><circle r=\"5\"/></part>");
    auto r = compile_file(p.root / "calc.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // Module-pattern wrapper signature.
    EXPECT_NE(r.flat_text.find("local _m = (function()"), std::string::npos);
    EXPECT_NE(r.flat_text.find("_G[\"math2\"] = _m"), std::string::npos);
}

// ─── Explicit alias ────────────────────────────────────────────────

TEST(LuaImports, ExplicitAliasUsedInWrapper) {
    TempProject p;
    p.write("airfoils.lua",
        "function naca() return 42 end\n");
    p.write("wing.cadml",
        "version 0.1\n"
        "import \"airfoils.lua\" as foils\n"
        "<part><circle r=\"5\"/></part>");
    auto r = compile_file(p.root / "wing.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // The wrapper binds to the explicit alias, not the filename.
    EXPECT_NE(r.flat_text.find("_G[\"foils\"]"), std::string::npos);
    EXPECT_EQ(r.flat_text.find("_G[\"airfoils\"]"), std::string::npos);
}

// ─── Catalogue prefix ──────────────────────────────────────────────

TEST(LuaImports, CtlPrefixForLuaModules) {
    TempProject p;
    p.write("ctl/aero/airfoils.lua",
        "function naca() return 42 end\n");
    p.write("wing.cadml",
        "version 0.1\n"
        "import \"ctl/aero/airfoils.lua\"\n"
        "<part><circle r=\"5\"/></part>");
    auto r = compile_file(p.root / "wing.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_NE(r.flat_text.find("function naca"), std::string::npos);
    // Default alias = filename without extension = "airfoils".
    EXPECT_NE(r.flat_text.find("_G[\"airfoils\"]"), std::string::npos);
}

// ─── Multiple Lua imports ──────────────────────────────────────────

TEST(LuaImports, MultipleModulesCoexist) {
    TempProject p;
    p.write("airfoils.lua",
        "function naca() return 1 end\n");
    p.write("gears.lua",
        "function involute() return 2 end\n");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"airfoils.lua\"\n"
        "import \"gears.lua\"\n"
        "<part><circle r=\"5\"/></part>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    EXPECT_NE(r.flat_text.find("function naca"),     std::string::npos);
    EXPECT_NE(r.flat_text.find("function involute"), std::string::npos);
    EXPECT_NE(r.flat_text.find("_G[\"airfoils\"]"),  std::string::npos);
    EXPECT_NE(r.flat_text.find("_G[\"gears\"]"),     std::string::npos);
}

// ─── Mixed .cadml + .lua imports ───────────────────────────────────

TEST(LuaImports, MixedCadmlAndLuaImports) {
    TempProject p;
    p.write("airfoils.lua",
        "function naca(c, t, n) return c end\n");
    p.write("plate.cadml",
        "version 0.1\n<part><circle r=\"5\"/></part>");
    p.write("rig.cadml",
        "version 0.1\n"
        "import \"airfoils.lua\"\n"
        "import \"plate.cadml\"\n"
        "<part><plate/></part>");
    auto r = compile_file(p.root / "rig.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // Both produce body content.
    EXPECT_NE(r.flat_text.find("function naca"), std::string::npos);
    EXPECT_NE(r.flat_text.find("<def name=\"plate\""), std::string::npos);
}

// ─── Realistic NACA usage ──────────────────────────────────────────

TEST(LuaImports, NACAModuleEndToEnd) {
    TempProject p;
    p.write("airfoils.lua",
        R"LUA(
local M = {}
function M.naca(c, t, n)
  local pts = {}
  for i = 0, n do
    local x = i / n
    local y = 5*t*(0.2969*math.sqrt(x) - 0.1260*x
                  - 0.3516*x^2 + 0.2843*x^3 - 0.1015*x^4)
    pts[#pts+1] = {x * c, y * c}
  end
  return cadml.path(pts)
end
return M
)LUA");
    p.write("wing.cadml",
        "version 0.1\n"
        "import \"airfoils.lua\"\n"
        "param chord = 100\n"
        "param thickness = 0.12\n"
        "<part>\n"
        "  <extrude height=\"50\">\n"
        "    <path d=\"{airfoils.naca(chord, thickness, 40)}\"/>\n"
        "  </extrude>\n"
        "</part>");
    auto r = compile_file(p.root / "wing.cadml");
    EXPECT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);
    // The imported .lua module is preserved in the flat output as a
    // <script> block (source-map provenance).
    EXPECT_NE(r.flat_text.find("function M.naca"), std::string::npos);
    // Lua-call expressions are evaluated eagerly at bundle time per
    // spec §9.2: the {airfoils.naca(...)} body has been substituted
    // with the function's return value (an SVG path string starting
    // with "M ").
    EXPECT_EQ(r.flat_text.find("{airfoils.naca"), std::string::npos)
        << "Lua-call expressions must NOT survive symbolic per spec §9.2";
    // The path data should now appear as a literal d= attribute.
    // The NACA-4 series at chord=100 starts at (0,0) and walks along
    // the chord — easiest invariant: there's a `d="M 0,0` somewhere.
    EXPECT_NE(r.flat_text.find("d=\"M 0,0"), std::string::npos)
        << "the substituted path must start with M 0,0";
}

// ─── Behavioral: wrapper actually binds the alias at runtime ─────────
// These run the bundler's synthesized <script> through a real Lua
// runtime to verify the alias becomes callable, not just that the
// expected substrings appear in flat_text.

TEST(LuaImports, ModulePatternRuntimeBindsAlias) {
    TempProject p;
    p.write("math2.lua",
        "local M = {}\n"
        "function M.add(a, b) return a + b end\n"
        "function M.mul(a, b) return a * b end\n"
        "return M\n");
    p.write("calc.cadml",
        "version 0.1\n"
        "import \"math2.lua\"\n"
        "<part><circle r=\"5\"/></part>");
    auto r = compile_file(p.root / "calc.cadml");
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);

    cadml::LuaRuntime rt;
    auto load_errs = rt.load_inline_scripts(r.document);
    ASSERT_TRUE(load_errs.empty()) << load_errs[0].message;

    std::vector<cadml::LuaError> call_errs;
    auto sum = rt.try_call_number("math2.add", {3.0, 4.0}, {}, call_errs);
    ASSERT_TRUE(sum.has_value()) << "math2.add not callable";
    EXPECT_DOUBLE_EQ(*sum, 7.0);

    auto prod = rt.try_call_number("math2.mul", {6.0, 7.0}, {}, call_errs);
    ASSERT_TRUE(prod.has_value());
    EXPECT_DOUBLE_EQ(*prod, 42.0);
}

TEST(LuaImports, FreeFormRuntimeBindsAlias) {
    TempProject p;
    // No `return` at the end → free-form. Top-level globals are
    // exposed under BOTH the alias and the global namespace —
    // a deliberate trade-off so modules with inter-function calls
    // (e.g. compressor.lua's hub_tx calling hub_r) keep working
    // even when their helpers are called via the alias. See
    // wrap_lua_module() in bundler.cpp for the rationale.
    p.write("airfoils.lua",
        "function naca(c, t, n) return c * t * n end\n"
        "function clarky(c, n) return c + n end\n");
    p.write("wing.cadml",
        "version 0.1\n"
        "import \"airfoils.lua\"\n"
        "<part><circle r=\"5\"/></part>");
    auto r = compile_file(p.root / "wing.cadml");
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);

    cadml::LuaRuntime rt;
    auto load_errs = rt.load_inline_scripts(r.document);
    ASSERT_TRUE(load_errs.empty()) << load_errs[0].message;

    std::vector<cadml::LuaError> call_errs;
    // The alias-namespaced form must be callable.
    auto v = rt.try_call_number("airfoils.naca", {2.0, 3.0, 4.0}, {}, call_errs);
    ASSERT_TRUE(v.has_value()) << "airfoils.naca not callable";
    EXPECT_DOUBLE_EQ(*v, 24.0);

    // The bare form (in _G) must ALSO be callable — that's the
    // deliberate trade-off documented in wrap_lua_module().
    auto bare = rt.try_call_number("naca", {2.0, 3.0, 4.0}, {}, call_errs);
    ASSERT_TRUE(bare.has_value()) << "free-form should leave bare `naca` in _G "
        "for intra-module inter-function calls (see wrap_lua_module)";
    EXPECT_DOUBLE_EQ(*bare, 24.0);
}

TEST(LuaImports, ExplicitAliasUsedAtRuntime) {
    TempProject p;
    p.write("math2.lua",
        "local M = {}\n"
        "function M.id(x) return x end\n"
        "return M\n");
    p.write("calc.cadml",
        "version 0.1\n"
        "import \"math2.lua\" as mm\n"
        "<part><circle r=\"5\"/></part>");
    auto r = compile_file(p.root / "calc.cadml");
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].message);

    cadml::LuaRuntime rt;
    auto load_errs = rt.load_inline_scripts(r.document);
    ASSERT_TRUE(load_errs.empty()) << load_errs[0].message;

    std::vector<cadml::LuaError> call_errs;
    auto v = rt.try_call_number("mm.id", {123.0}, {}, call_errs);
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 123.0);

    // The filename-derived alias should NOT be bound.
    auto v2 = rt.try_call_number("math2.id", {123.0}, {}, call_errs);
    EXPECT_FALSE(v2.has_value());
}
