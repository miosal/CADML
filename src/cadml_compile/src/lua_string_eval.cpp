// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include "lua_string_eval.hpp"

#include <cadml/compile/bundler.hpp>
#include <cadml/expression.hpp>
#include <cadml/lua.hpp>

#include <unordered_map>
#include <variant>

namespace cadml::compile::detail {

namespace {

// Initialize a caller-provided LuaRuntime with frontmatter params
// and all inline <script> content from the document. (Synthesised
// wrappers from imported .lua modules are <script> elements too,
// so a single load_inline_scripts pass picks them up.) Returns by
// reference because LuaRuntime is move-deleted.
void init_lua_runtime(
    LuaRuntime& rt,
    const Document& doc,
    const std::unordered_map<std::string, double>& params,
    std::vector<CompileError>& errors_out)
{
    rt.set_params(params);
    auto load_errs = rt.load_inline_scripts(doc);
    for (auto& le : load_errs) {
        CompileError ce;
        ce.category = CompileError::Lua;
        ce.message  = le.message;
        ce.source   = le.source;
        errors_out.push_back(std::move(ce));
    }
}

// Convert ExpressionValue args to plain doubles for the Lua bridge.
// Lua functions in our model take numeric inputs; string and vector
// arguments aren't supported through the bridge today (they would
// need additional plumbing in LuaRuntime::try_call_*).
std::vector<double> to_doubles(const std::vector<ExpressionValue>& args) {
    std::vector<double> out;
    out.reserve(args.size());
    for (const auto& a : args) {
        if (a.kind == ExpressionValue::Number) out.push_back(a.number);
        else                                    out.push_back(0.0);
    }
    return out;
}

// Build an ExpressionEvaluator with the LuaRuntime hooked into its
// external-function bridge. The closure tries try_call_string first
// (returns immediately if the name resolves to a string-returning
// function) then try_call_number (for number-returning functions).
ExpressionEvaluator make_evaluator(
    const std::unordered_map<std::string, double>& params,
    LuaRuntime& rt)
{
    ExpressionEvaluator e;
    for (const auto& [name, val] : params) e.set_param(name, val);
    e.set_external_func([&rt](std::string_view name,
                                const std::vector<ExpressionValue>& args)
                                -> std::optional<ExpressionValue> {
        const auto numeric_args = to_doubles(args);
        std::vector<LuaError> ignored;  // diagnostics here would be
                                         // confusing — caller can't
                                         // attribute them to a specific
                                         // CADML node anyway.
        if (auto s = rt.try_call_string(name, numeric_args, {}, ignored)) {
            return ExpressionValue::from_string(*s);
        }
        ignored.clear();
        if (auto v = rt.try_call_number(name, numeric_args, {}, ignored)) {
            return ExpressionValue::from_number(*v);
        }
        return std::nullopt;
    });
    return e;
}

// Substitute every `{...}` block inside `s` whose inner expression
// contains `(` (i.e. a function call) and is evaluable. Param
// references like `{hub-h}` are left untouched — those are
// part-level params resolved at engine eval_part time.
//
// On evaluation success the block (including the braces) is
// replaced by the returned string value; the surrounding text is
// left as-is. On failure the block is preserved verbatim so the
// engine can have another go.
void resolve_blocks_in_string(std::string& s,
                                ExpressionEvaluator& eval,
                                SourceRange src)
{
    if (s.find('{') == std::string::npos) return;

    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '{') {
            out.push_back(s[i++]);
            continue;
        }
        const std::size_t end = s.find('}', i);
        if (end == std::string::npos) {
            // Unclosed brace; leave the rest as-is.
            out.append(s.substr(i));
            break;
        }
        const std::string_view block(s.data() + i, end - i + 1);
        // Heuristic: only resolve blocks that LOOK like function
        // calls. Bare param refs like `{hub-h}` stay for the engine.
        if (block.find('(') == std::string_view::npos) {
            out.append(block);
            i = end + 1;
            continue;
        }
        std::vector<ExpressionError> ee;
        const auto resolved = eval.evaluate_string(block, src, ee);
        if (resolved && ee.empty()) {
            out.append(*resolved);
        } else {
            // Leave verbatim — engine will warn appropriately if
            // the call truly doesn't resolve.
            out.append(block);
        }
        i = end + 1;
    }
    s = std::move(out);
}

}  // namespace

void resolve_lua_calls(Document& doc,
                        const std::vector<ParamDecl>& entry_params,
                        std::vector<CompileError>& errors_out)
{
    // Collect entry params as a numeric scope. Parameters can
    // reference earlier ones, so iterate in declaration order.
    std::unordered_map<std::string, double> params_map;
    {
        ExpressionEvaluator e_temp;
        for (const auto& p : entry_params) {
            std::vector<ExpressionError> exprs;
            if (auto v = e_temp.evaluate_number(p.value_expr, p.source, exprs)) {
                e_temp.set_param(p.name, *v);
                params_map[p.name] = *v;
            }
        }
    }

    LuaRuntime rt;
    init_lua_runtime(rt, doc, params_map, errors_out);
    auto eval = make_evaluator(params_map, rt);

    // Walk every node and resolve the relevant string-typed attrs.
    // Conservative scope: only attrs known to commonly carry Lua
    // function calls in real-world examples (path d, sketch frame,
    // group transform, primitive numeric attrs that may compute
    // through Lua). Param refs without `(` are skipped by the
    // resolver heuristic above.
    for (auto& n : doc.nodes) {
        if (n.dead) continue;
        const SourceRange src = n.source;
        std::visit([&](auto& a) {
            using A = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<A, PathAttrs>) {
                resolve_blocks_in_string(a.d, eval, src);
            } else if constexpr (std::is_same_v<A, SketchAttrs>) {
                resolve_blocks_in_string(a.origin_expr,   eval, src);
                resolve_blocks_in_string(a.rotation_expr, eval, src);
                resolve_blocks_in_string(a.normal_expr,   eval, src);
            } else if constexpr (std::is_same_v<A, GroupAttrs>) {
                resolve_blocks_in_string(a.transform, eval, src);
            } else if constexpr (std::is_same_v<A, ExtrudeAttrs>) {
                resolve_blocks_in_string(a.height_expr,    eval, src);
                resolve_blocks_in_string(a.scale_expr,     eval, src);
                resolve_blocks_in_string(a.draft_expr,     eval, src);
                resolve_blocks_in_string(a.direction_expr, eval, src);
            } else if constexpr (std::is_same_v<A, RevolveAttrs>) {
                resolve_blocks_in_string(a.angle_expr,    eval, src);
                resolve_blocks_in_string(a.segments_expr, eval, src);
            } else if constexpr (std::is_same_v<A, HelixAttrs>) {
                resolve_blocks_in_string(a.radius_expr, eval, src);
                resolve_blocks_in_string(a.pitch_expr,  eval, src);
                resolve_blocks_in_string(a.turns_expr,  eval, src);
                resolve_blocks_in_string(a.taper_expr,  eval, src);
            } else if constexpr (std::is_same_v<A, CircleAttrs>) {
                resolve_blocks_in_string(a.cx_expr,       eval, src);
                resolve_blocks_in_string(a.cy_expr,       eval, src);
                resolve_blocks_in_string(a.r_expr,        eval, src);
                resolve_blocks_in_string(a.segments_expr, eval, src);
            } else if constexpr (std::is_same_v<A, RectAttrs>) {
                resolve_blocks_in_string(a.x_expr,      eval, src);
                resolve_blocks_in_string(a.y_expr,      eval, src);
                resolve_blocks_in_string(a.width_expr,  eval, src);
                resolve_blocks_in_string(a.height_expr, eval, src);
                resolve_blocks_in_string(a.rx_expr,     eval, src);
                resolve_blocks_in_string(a.ry_expr,     eval, src);
            } else if constexpr (std::is_same_v<A, FilletAttrs>) {
                resolve_blocks_in_string(a.radius_expr, eval, src);
            } else if constexpr (std::is_same_v<A, ChamferAttrs>) {
                resolve_blocks_in_string(a.distance_expr, eval, src);
                resolve_blocks_in_string(a.angle_expr,    eval, src);
            } else if constexpr (std::is_same_v<A, ShellAttrs>) {
                resolve_blocks_in_string(a.thickness_expr, eval, src);
            } else if constexpr (std::is_same_v<A, CutAttrs>) {
                resolve_blocks_in_string(a.angle_expr, eval, src);
                resolve_blocks_in_string(a.miter_expr, eval, src);
                resolve_blocks_in_string(a.bevel_expr, eval, src);
            } else if constexpr (std::is_same_v<A, ParamAttrs>) {
                resolve_blocks_in_string(a.value_expr, eval, src);
            } else if constexpr (std::is_same_v<A, PortAttrs>) {
                resolve_blocks_in_string(a.position_expr, eval, src);
                resolve_blocks_in_string(a.normal_expr,   eval, src);
                resolve_blocks_in_string(a.up_expr,       eval, src);
            } else if constexpr (std::is_same_v<A, PatternAttrs>) {
                resolve_blocks_in_string(a.spacing_expr, eval, src);
                resolve_blocks_in_string(a.angle_expr,   eval, src);
            }
            // Other Attrs variants (UnionAttrs, DifferenceAttrs,
            // IntersectAttrs, HullAttrs, SvgAttrs, SourcesAttrs,
            // SourceAttrs, AssemblyAttrs, ConnectAttrs, ScriptAttrs,
            // DefAttrs, PartAttrs, ForAttrs, LoftAttrs, SweepAttrs,
            // InstanceAttrs, UnknownAttrs) carry no fields that
            // the engine evaluates as expressions. Skipping is safe.
        }, n.attrs);
    }
}

}  // namespace cadml::compile::detail
