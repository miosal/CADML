// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/lua.hpp>

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

namespace cadml {

namespace {

// CADML kebab → Lua snake.
std::string kebab_to_snake(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (char ch : in) out.push_back(ch == '-' ? '_' : ch);
    return out;
}

// True if `s`, after stripping trailing whitespace + comments, ends with
// a `return <table-or-name>` statement. Heuristic — accepts:
//   `return M`
//   `return {...}`
// Reasonable false-positive tolerance: a comment containing "return" at
// the end of file would trip it. Mitigation: we only inspect the last
// non-comment line.
bool ends_with_return(std::string_view src) {
    // Scan backward, skipping trailing whitespace.
    std::size_t end = src.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(src[end - 1]))) {
        --end;
    }
    if (end == 0) return false;

    // Walk back to the start of the last non-blank line (skip comments
    // -- entire-line `-- ...` only; inline `--` we ignore for simplicity).
    while (end > 0) {
        std::size_t line_end = end;
        std::size_t line_start = end;
        while (line_start > 0 && src[line_start - 1] != '\n') --line_start;
        std::string_view line = src.substr(line_start, line_end - line_start);
        // Trim leading/trailing whitespace from the line.
        std::size_t lstart = 0;
        while (lstart < line.size() &&
               std::isspace(static_cast<unsigned char>(line[lstart]))) ++lstart;
        std::size_t lend = line.size();
        while (lend > lstart &&
               std::isspace(static_cast<unsigned char>(line[lend - 1]))) --lend;
        std::string_view trimmed = line.substr(lstart, lend - lstart);
        // Skip pure-comment lines.
        if (trimmed.size() >= 2 && trimmed.compare(0, 2, "--") == 0) {
            end = (line_start == 0) ? 0 : line_start - 1;
            continue;
        }
        if (trimmed.empty()) {
            end = (line_start == 0) ? 0 : line_start - 1;
            continue;
        }
        // First non-blank, non-comment line from the end.
        return trimmed.size() >= 6 && trimmed.compare(0, 6, "return") == 0 &&
               (trimmed.size() == 6 ||
                std::isspace(static_cast<unsigned char>(trimmed[6])) ||
                trimmed[6] == '{');
    }
    return false;
}

}  // namespace

// ─── Pimpl ────────────────────────────────────────────────────────────

struct LuaRuntime::Impl {
    sol::state lua;
    std::unordered_map<std::string, double> params;

    Impl() { reset_state(); }

    void reset_state() {
        lua = sol::state{};
        lua.open_libraries(
            sol::lib::base,
            sol::lib::math,
            sol::lib::string,
            sol::lib::table);

        // Defence in depth — clobber unsafe globals exposed by `base`.
        lua["dofile"]         = sol::nil;
        lua["loadfile"]       = sol::nil;
        lua["load"]           = sol::nil;
        lua["loadstring"]     = sol::nil;
        lua["require"]        = sol::nil;
        lua["collectgarbage"] = sol::nil;
        lua["rawget"]         = sol::nil;
        lua["rawset"]         = sol::nil;
        lua["rawequal"]       = sol::nil;

        // `string.dump` returns a function's compiled bytecode. It
        // does not by itself escape the sandbox (since `load` /
        // `loadstring` are already nilled out), but it pairs with any
        // future Lua-version regression in the bytecode loader to
        // become a sandbox escape. Cheaper to remove than to defend.
        lua["string"]["dump"] = sol::nil;

        sol::table cadml = lua.create_named_table("cadml");

        cadml.set_function("param",
            [this](const std::string& name) -> double {
                // Try as-given (snake or kebab), then translate either
                // direction so callers can use whichever form they like.
                auto it = params.find(name);
                if (it != params.end()) return it->second;
                // Try kebab form (replace _ with -).
                std::string kebab(name);
                std::replace(kebab.begin(), kebab.end(), '_', '-');
                it = params.find(kebab);
                if (it != params.end()) return it->second;
                // Per spec (lua-embedding.md §3.1): unknown param
                // names must raise a Lua error, never silently return
                // 0. Silent zeros produce subtly-wrong geometry that's
                // very hard to debug.
                throw std::runtime_error(
                    "cadml.param: unknown param `" + name + "`");
            });

        cadml.set_function("path",
            [](sol::table points) -> std::string {
                std::ostringstream ss;
                ss.precision(6);
                bool first = true;
                for (std::size_t i = 1; i <= points.size(); ++i) {
                    sol::table pt = points[i];
                    const double x = pt[1].get<double>();
                    const double y = pt[2].get<double>();
                    if (first) {
                        ss << "M " << x << "," << y;
                        first = false;
                    } else {
                        ss << " " << x << "," << y;
                    }
                }
                std::string result = ss.str();
                // Insert "L " between the M coord and the rest, so the
                // result is a proper SVG path (M ... L ... Z).
                const auto first_space = result.find(' ', 2);
                if (first_space != std::string::npos &&
                    result.size() > first_space + 1) {
                    result.insert(first_space + 1, "L ");
                }
                result += " Z";
                return result;
            });

        // Some bundled Lua builds strip math.rad/math.deg; defensively
        // restore them if missing.
        lua.script(
            "if not math.rad then math.rad = function(d) "
            "return d * math.pi / 180 end end");
        lua.script(
            "if not math.deg then math.deg = function(r) "
            "return r * 180 / math.pi end end");

        // Seed math.random with a fixed value so the bundler stays
        // deterministic across runs (compiler.md §4 / lua-embedding.md
        // §7). The seed value is a public part of the spec — do not
        // change it without bumping the spec version, since it would
        // alter the byte-output of every .cadml that uses
        // math.random().
        lua.script("math.randomseed(0x12345678, 0xCADD1E12)");
    }

    void rebind_params() {
        // Bind every param into the global scope under its snake_case
        // name (one-directional kebab→snake, per spec §8.2). Direct
        // identifier access from Lua reads the snake form; cadml.param()
        // accepts either form.
        for (const auto& [name, value] : params) {
            lua[kebab_to_snake(name)] = value;
        }
    }

    // Resolve a (possibly dotted) identifier to a Lua function.
    // Examples: "naca" (top-level), "airfoils.naca" (module field),
    // "cadml.path" (built-in).
    sol::function resolve(const std::string& name) {
        std::size_t dot = name.find('.');
        if (dot == std::string::npos) {
            sol::object obj = lua[name];
            if (obj.is<sol::function>()) return obj;
            return {};
        }
        sol::object obj = lua[name.substr(0, dot)];
        if (!obj.is<sol::table>()) return {};
        sol::table cur = obj;
        std::size_t pos = dot + 1;
        while (true) {
            std::size_t next_dot = name.find('.', pos);
            std::string seg = (next_dot == std::string::npos)
                ? name.substr(pos)
                : name.substr(pos, next_dot - pos);
            sol::object child = cur[seg];
            if (next_dot == std::string::npos) {
                if (child.is<sol::function>()) return child;
                return {};
            }
            if (!child.is<sol::table>()) return {};
            cur = child;
            pos = next_dot + 1;
        }
    }

    sol::protected_function_result call_function(
        const sol::function& fn, const std::vector<double>& args)
    {
        switch (args.size()) {
            case 0: return fn();
            case 1: return fn(args[0]);
            case 2: return fn(args[0], args[1]);
            case 3: return fn(args[0], args[1], args[2]);
            case 4: return fn(args[0], args[1], args[2], args[3]);
            case 5: return fn(args[0], args[1], args[2], args[3], args[4]);
            case 6: return fn(args[0], args[1], args[2], args[3], args[4], args[5]);
            default: {
                sol::table tab = lua.create_table();
                for (std::size_t i = 0; i < args.size(); ++i) tab[i + 1] = args[i];
                return fn(sol::as_args(tab));
            }
        }
    }
};

LuaRuntime::LuaRuntime() : impl_(std::make_unique<Impl>()) {}
LuaRuntime::~LuaRuntime() = default;

void LuaRuntime::set_params(
    const std::unordered_map<std::string, double>& params)
{
    impl_->params = params;
    impl_->rebind_params();
}

std::vector<LuaError> LuaRuntime::load_inline_scripts(const Document& doc) {
    std::vector<LuaError> errors;
    for (const auto& node : doc.nodes) {
        if (node.type != NodeType::Script) continue;
        const auto& attrs = std::get<ScriptAttrs>(node.attrs);
        if (attrs.lang != "lua") {
            errors.push_back({
                "unsupported script language: " + attrs.lang, node.source });
            continue;
        }
        if (attrs.source.empty()) continue;

        auto result = impl_->lua.safe_script(
            attrs.source, sol::script_pass_on_error);
        if (!result.valid()) {
            sol::error err = result;
            errors.push_back({
                std::string("Lua error: ") + err.what(), node.source });
        }
    }
    return errors;
}

std::vector<LuaError> LuaRuntime::load_module(
    std::string_view source,
    std::string_view alias,
    SourceRange source_range)
{
    std::vector<LuaError> errors;

    // SECURITY: validate the alias against the spec §2.8 identifier
    // grammar before splicing into generated Lua source. The bundler
    // upstream enforces this at the import-statement boundary, but
    // load_module is a public API and we must not trust the caller.
    auto is_safe_alias = [](std::string_view s) {
        if (s.empty()) return false;
        if (!(s[0] >= 'a' && s[0] <= 'z')) return false;
        for (char c : s.substr(1)) {
            const bool ok = (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-';
            if (!ok) return false;
        }
        return true;
    };
    if (!is_safe_alias(alias)) {
        errors.push_back({
            std::string("Lua module load: alias `") + std::string(alias) +
            "` is not a valid kebab-case identifier (spec §2.8); refusing"
            " to splice into Lua wrapper", source_range });
        return errors;
    }

    if (source.empty()) {
        // Empty module — bind alias to empty table (no-op import).
        impl_->lua.create_named_table(std::string(alias));
        return errors;
    }

    if (ends_with_return(source)) {
        // Module pattern: `local M = ... return M`. Wrap to capture the
        // returned table directly into the alias.
        const std::string wrapper =
            "do local _module = (function()\n" + std::string(source) +
            "\nend)(); _G[\"" + std::string(alias) + "\"] = _module end";
        auto result = impl_->lua.safe_script(
            wrapper, sol::script_pass_on_error);
        if (!result.valid()) {
            sol::error err = result;
            errors.push_back({
                std::string("Lua module `") + std::string(alias) + "`: " +
                err.what(), source_range });
        }
    } else {
        // Free-form: capture top-level globals defined by the script
        // into a per-import table. Implementation: snapshot `_G` keys
        // before/after, the difference becomes the alias's fields.
        impl_->lua.script(
            "local __cadml_pre = {}\n"
            "for k, _ in pairs(_G) do __cadml_pre[k] = true end\n"
            "_G[\"__cadml_pre\"] = __cadml_pre");

        auto result = impl_->lua.safe_script(
            std::string(source), sol::script_pass_on_error);
        if (!result.valid()) {
            sol::error err = result;
            errors.push_back({
                std::string("Lua module `") + std::string(alias) + "`: " +
                err.what(), source_range });
            impl_->lua["__cadml_pre"] = sol::nil;
            return errors;
        }

        // Move new globals into the alias table.
        const std::string finalise =
            "local __mod = {}\n"
            "for k, v in pairs(_G) do\n"
            "  if not __cadml_pre[k] and k ~= \"__cadml_pre\" then\n"
            "    __mod[k] = v\n"
            "    _G[k] = nil\n"
            "  end\n"
            "end\n"
            "_G[\"__cadml_pre\"] = nil\n"
            "_G[\"" + std::string(alias) + "\"] = __mod";
        auto fr = impl_->lua.safe_script(
            finalise, sol::script_pass_on_error);
        if (!fr.valid()) {
            sol::error err = fr;
            errors.push_back({
                std::string("Lua module `") + std::string(alias) +
                "` finalisation: " + err.what(), source_range });
        }
    }
    return errors;
}

std::optional<double> LuaRuntime::try_call_number(
    std::string_view name,
    const std::vector<double>& args,
    SourceRange source,
    std::vector<LuaError>& errors_out)
{
    sol::function fn = impl_->resolve(std::string(name));
    if (!fn.valid()) return std::nullopt;

    auto result = impl_->call_function(fn, args);
    if (!result.valid()) {
        sol::error err = result;
        errors_out.push_back({
            std::string("Lua call `") + std::string(name) + "`: " +
            err.what(), source });
        return std::nullopt;
    }
    sol::object ret = result;
    if (ret.is<double>()) return ret.as<double>();
    if (ret.is<bool>())   return ret.as<bool>() ? 1.0 : 0.0;
    return std::nullopt;
}

std::optional<std::string> LuaRuntime::try_call_string(
    std::string_view name,
    const std::vector<double>& args,
    SourceRange source,
    std::vector<LuaError>& errors_out)
{
    sol::function fn = impl_->resolve(std::string(name));
    if (!fn.valid()) return std::nullopt;

    auto result = impl_->call_function(fn, args);
    if (!result.valid()) {
        sol::error err = result;
        errors_out.push_back({
            std::string("Lua call `") + std::string(name) + "`: " +
            err.what(), source });
        return std::nullopt;
    }
    sol::object ret = result;
    if (ret.is<std::string>()) return ret.as<std::string>();
    return std::nullopt;
}

void LuaRuntime::reset() {
    impl_->reset_state();
    impl_->params.clear();
}

}  // namespace cadml
