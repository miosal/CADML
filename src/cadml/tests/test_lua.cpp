// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/lua.hpp>
#include <cadml/parser.hpp>

#include <gtest/gtest.h>

using namespace cadml;

namespace {

double call_n(LuaRuntime& lua, std::string_view name,
              const std::vector<double>& args = {}) {
    std::vector<LuaError> errs;
    auto v = lua.try_call_number(name, args, {}, errs);
    EXPECT_TRUE(v.has_value()) << (errs.empty() ? "" : errs[0].message);
    return v.value_or(0);
}

std::string call_s(LuaRuntime& lua, std::string_view name,
                    const std::vector<double>& args = {}) {
    std::vector<LuaError> errs;
    auto v = lua.try_call_string(name, args, {}, errs);
    EXPECT_TRUE(v.has_value()) << (errs.empty() ? "" : errs[0].message);
    return v.value_or("");
}

}  // namespace

// ─── Basic execution ────────────────────────────────────────────────

TEST(Lua, EmptyRuntimeHasNoUserFunctions) {
    LuaRuntime lua;
    std::vector<LuaError> errs;
    auto v = lua.try_call_number("missing", {}, {}, errs);
    EXPECT_FALSE(v.has_value());
}

TEST(Lua, InlineScriptDefinesGlobalFunction) {
    LuaRuntime lua;
    Document doc;
    doc.nodes.push_back(Node{
        NodeType::Script, NO_NODE, NO_NODE, NO_NODE,
        ScriptAttrs{ "lua", "function double(x) return x * 2 end" },
        SourceRange{}, UINT32_MAX
    });
    auto errs = lua.load_inline_scripts(doc);
    EXPECT_TRUE(errs.empty());
    EXPECT_NEAR(call_n(lua, "double", {21}), 42.0, 1e-9);
}

TEST(Lua, InlineScriptWithError) {
    LuaRuntime lua;
    Document doc;
    doc.nodes.push_back(Node{
        NodeType::Script, NO_NODE, NO_NODE, NO_NODE,
        ScriptAttrs{ "lua", "this is not valid lua" },
        SourceRange{}, UINT32_MAX
    });
    auto errs = lua.load_inline_scripts(doc);
    EXPECT_FALSE(errs.empty());
}

TEST(Lua, NonLuaLanguageRejected) {
    LuaRuntime lua;
    Document doc;
    doc.nodes.push_back(Node{
        NodeType::Script, NO_NODE, NO_NODE, NO_NODE,
        ScriptAttrs{ "python", "print('hi')" },
        SourceRange{}, UINT32_MAX
    });
    auto errs = lua.load_inline_scripts(doc);
    EXPECT_EQ(errs.size(), 1u);
}

// ─── Parameter binding (kebab→snake) ───────────────────────────────

TEST(Lua, ParamsAvailableAsSnakeCase) {
    LuaRuntime lua;
    lua.set_params({{"max-thickness", 0.12}, {"chord", 100}});
    Document doc;
    doc.nodes.push_back(Node{
        NodeType::Script, NO_NODE, NO_NODE, NO_NODE,
        ScriptAttrs{ "lua",
            "function get_thickness() return max_thickness end\n"
            "function get_chord()     return chord end" },
        SourceRange{}, UINT32_MAX
    });
    EXPECT_TRUE(lua.load_inline_scripts(doc).empty());
    EXPECT_NEAR(call_n(lua, "get_thickness"), 0.12, 1e-9);
    EXPECT_NEAR(call_n(lua, "get_chord"),     100, 1e-9);
}

TEST(Lua, CadmlParamFunctionAcceptsKebabAndSnake) {
    LuaRuntime lua;
    lua.set_params({{"max-thickness", 0.12}});
    Document doc;
    doc.nodes.push_back(Node{
        NodeType::Script, NO_NODE, NO_NODE, NO_NODE,
        ScriptAttrs{ "lua",
            "function via_kebab() return cadml.param(\"max-thickness\") end\n"
            "function via_snake() return cadml.param(\"max_thickness\") end" },
        SourceRange{}, UINT32_MAX
    });
    EXPECT_TRUE(lua.load_inline_scripts(doc).empty());
    EXPECT_NEAR(call_n(lua, "via_kebab"), 0.12, 1e-9);
    EXPECT_NEAR(call_n(lua, "via_snake"), 0.12, 1e-9);
}

TEST(Lua, UndefinedParamRaisesError) {
    // Per spec / lua-embedding.md §3.1: cadml.param() must raise on
    // unknown names rather than silently returning 0. Silent zeros
    // produce subtly wrong geometry that's very hard to debug.
    LuaRuntime lua;
    Document doc;
    doc.nodes.push_back(Node{
        NodeType::Script, NO_NODE, NO_NODE, NO_NODE,
        ScriptAttrs{ "lua",
            "function f() return cadml.param(\"missing\") end" },
        SourceRange{}, UINT32_MAX
    });
    EXPECT_TRUE(lua.load_inline_scripts(doc).empty());

    std::vector<LuaError> errs;
    auto v = lua.try_call_number("f", {}, {}, errs);
    EXPECT_FALSE(v.has_value());
    ASSERT_FALSE(errs.empty());
    EXPECT_NE(errs[0].message.find("cadml.param"), std::string::npos)
        << "expected the Lua error to mention cadml.param";
    EXPECT_NE(errs[0].message.find("missing"), std::string::npos)
        << "expected the Lua error to mention the offending name";
}

// ─── cadml.path() ───────────────────────────────────────────────────

TEST(Lua, CadmlPathBuildsSVGString) {
    LuaRuntime lua;
    Document doc;
    doc.nodes.push_back(Node{
        NodeType::Script, NO_NODE, NO_NODE, NO_NODE,
        ScriptAttrs{ "lua",
            "function tri()\n"
            "  return cadml.path({{0,0}, {10,0}, {5,8}})\n"
            "end" },
        SourceRange{}, UINT32_MAX
    });
    EXPECT_TRUE(lua.load_inline_scripts(doc).empty());
    auto s = call_s(lua, "tri");
    EXPECT_NE(s.find("M 0,0"), std::string::npos);
    EXPECT_NE(s.find("L"), std::string::npos);
    EXPECT_NE(s.find("Z"), std::string::npos);
}

// ─── Sandbox ────────────────────────────────────────────────────────

TEST(Lua, IoLibraryUnavailable) {
    LuaRuntime lua;
    Document doc;
    doc.nodes.push_back(Node{
        NodeType::Script, NO_NODE, NO_NODE, NO_NODE,
        ScriptAttrs{ "lua", "function f() return io and 1 or 0 end" },
        SourceRange{}, UINT32_MAX
    });
    EXPECT_TRUE(lua.load_inline_scripts(doc).empty());
    EXPECT_NEAR(call_n(lua, "f"), 0.0, 1e-9);
}

TEST(Lua, RequireDisabled) {
    LuaRuntime lua;
    Document doc;
    doc.nodes.push_back(Node{
        NodeType::Script, NO_NODE, NO_NODE, NO_NODE,
        ScriptAttrs{ "lua",
            "function f() return require == nil and 1 or 0 end" },
        SourceRange{}, UINT32_MAX
    });
    EXPECT_TRUE(lua.load_inline_scripts(doc).empty());
    EXPECT_NEAR(call_n(lua, "f"), 1.0, 1e-9);
}

TEST(Lua, MathLibraryAvailable) {
    LuaRuntime lua;
    Document doc;
    doc.nodes.push_back(Node{
        NodeType::Script, NO_NODE, NO_NODE, NO_NODE,
        ScriptAttrs{ "lua",
            "function f() return math.sqrt(16) end" },
        SourceRange{}, UINT32_MAX
    });
    EXPECT_TRUE(lua.load_inline_scripts(doc).empty());
    EXPECT_NEAR(call_n(lua, "f"), 4.0, 1e-9);
}

// ─── Module loading (free-form vs module-pattern) ──────────────────

TEST(Lua, ModulePatternBindsReturnedTable) {
    LuaRuntime lua;
    auto errs = lua.load_module(
        "local M = {}\n"
        "function M.add(a, b) return a + b end\n"
        "function M.mul(a, b) return a * b end\n"
        "return M\n",
        "math2", {});
    EXPECT_TRUE(errs.empty());
    EXPECT_NEAR(call_n(lua, "math2.add", {3, 4}), 7.0, 1e-9);
    EXPECT_NEAR(call_n(lua, "math2.mul", {3, 4}), 12.0, 1e-9);
}

TEST(Lua, FreeFormBindsTopLevelGlobals) {
    LuaRuntime lua;
    auto errs = lua.load_module(
        "function shout(x) return x * 100 end\n"
        "function whisper(x) return x / 100 end\n",
        "freeform", {});
    EXPECT_TRUE(errs.empty()) << (errs.empty() ? "" : errs[0].message);
    EXPECT_NEAR(call_n(lua, "freeform.shout",   {2}), 200.0, 1e-9);
    EXPECT_NEAR(call_n(lua, "freeform.whisper", {200}), 2.0, 1e-9);
}

TEST(Lua, ModulePatternLocalsRemainPrivate) {
    LuaRuntime lua;
    auto errs = lua.load_module(
        "local M = {}\n"
        "local function helper(x) return x + 1 end\n"
        "function M.exposed(x) return helper(x) end\n"
        "return M\n",
        "mod", {});
    EXPECT_TRUE(errs.empty());
    EXPECT_NEAR(call_n(lua, "mod.exposed", {5}), 6.0, 1e-9);
    // helper is not exposed.
    std::vector<LuaError> errs2;
    auto v = lua.try_call_number("mod.helper", {5}, {}, errs2);
    EXPECT_FALSE(v.has_value());
}

TEST(Lua, MultipleModulesDontConflict) {
    LuaRuntime lua;
    EXPECT_TRUE(lua.load_module(
        "function compute(x) return x * 2 end\n", "a", {}).empty());
    EXPECT_TRUE(lua.load_module(
        "function compute(x) return x * 3 end\n", "b", {}).empty());
    EXPECT_NEAR(call_n(lua, "a.compute", {5}), 10.0, 1e-9);
    EXPECT_NEAR(call_n(lua, "b.compute", {5}), 15.0, 1e-9);
}

TEST(Lua, EmptyModuleOK) {
    LuaRuntime lua;
    auto errs = lua.load_module("", "empty", {});
    EXPECT_TRUE(errs.empty());
}

// ─── End-to-end: CADML script + airfoil-like profile ──────────────

TEST(Lua, NACAProfileLikeIntegration) {
    LuaRuntime lua;
    auto errs = lua.load_module(
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
)LUA",
        "airfoils", {});
    EXPECT_TRUE(errs.empty()) << (errs.empty() ? "" : errs[0].message);
    auto path = call_s(lua, "airfoils.naca", {100, 0.12, 10});
    EXPECT_NE(path.find("M "), std::string::npos);
    EXPECT_NE(path.find(" L "), std::string::npos);
    EXPECT_NE(path.find("Z"), std::string::npos);
}
