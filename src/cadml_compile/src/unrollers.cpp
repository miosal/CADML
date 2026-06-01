// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include "unrollers.hpp"

#include "subtree_ops.hpp"

#include <cadml/compile/bundler.hpp>
#include <cadml/expression.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace cadml::compile::detail {

namespace {

constexpr double kPi = 3.14159265358979323846;

void push_error(std::vector<CompileError>& errors_out,
                 CompileError::Category cat,
                 std::string msg, SourceRange src) {
    errors_out.push_back({ cat, std::move(msg), src });
}

// Format a double for substitution into expression text.
// Normalises -0 → 0 so transforms like translate(-0, 0, 0) (which
// arise from `axis * iteration` with iteration == 0) round-trip
// cleanly. Locale-pinned via format_double_canonical so a
// de_DE host doesn't emit translate(0,5, ...) which would then
// fail to re-tokenize (the `,` is the argument separator).
std::string format_number(double v) {
    if (v == 0.0) v = 0.0;   // strips signed zero
    return cadml::format_double_canonical(v, 15);
}

// Build an evaluator pre-loaded with frontmatter param defaults. Used
// to compile-time-resolve <for>/<pattern> bounds and counts.
ExpressionEvaluator make_evaluator(const std::vector<ParamDecl>& params,
                                     std::vector<CompileError>& errors_out,
                                     SourceRange src_for_errors) {
    ExpressionEvaluator e;
    // Two-pass: first bind literal defaults, then evaluate expression
    // defaults that may reference earlier params. Accept the simpler
    // single-pass model — params evaluate in declaration order.
    for (const auto& p : params) {
        std::vector<ExpressionError> exprs;
        auto v = e.evaluate_number(p.value_expr, p.source, exprs);
        if (v) {
            e.set_param(p.name, *v);
        } else {
            // Skip silently — not all params need to be evaluable at
            // unroll time; most loops reference numeric literals or
            // other already-resolved params.
            (void)errors_out;
            (void)src_for_errors;
            for (const auto& ee : exprs) (void)ee;
        }
    }
    return e;
}

// Parse the explicit-values list of a <for>. Returns each token as a
// string; the caller decides whether to treat as numeric.
std::vector<std::string> split_values(std::string_view src) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : src) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!cur.empty()) { out.push_back(std::move(cur)); cur.clear(); }
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

// ─── Top-level head finder ──────────────────────────────────────────
//
// Top-level siblings (parent == NO_NODE) form a chain accessed via
// next_sibling, but there's no explicit head pointer in Document. To
// splice top-level changes, we synthesise a head pointer from the first
// node with parent == NO_NODE.

std::uint32_t find_top_level_head(const Document& doc) {
    for (std::uint32_t i = 0; i < doc.nodes.size(); ++i) {
        if (doc.nodes[i].parent == NO_NODE) return i;
    }
    return NO_NODE;
}

// Find the head pointer for `node_idx`'s sibling chain. If parent !=
// NO_NODE, returns nullptr (caller uses parent.first_child). For
// top-level, returns a pointer to a synthesised head we maintain.
struct SiblingChainHead {
    std::uint32_t* ptr = nullptr;       // null for non-top-level
    std::uint32_t  synthetic_head = NO_NODE;
};

// Track the top-level head ourselves across passes. Since we may unlink
// and re-splice top-level nodes, we need to re-derive the head each
// time we touch the top-level chain.
//
// In practice this works because: after an unroll pass, the only way to
// reach the top-level chain is via the parent==NO_NODE filter. The "head"
// of the chain is the first such node by insertion order; unlink/splice
// keeps the chain consistent.

}  // namespace

// ─── <for> unroller ──────────────────────────────────────────────────

namespace {

void unroll_one_for(Document& doc, std::uint32_t for_idx,
                     const std::vector<ParamDecl>& params,
                     UnrollBudget& budget,
                     std::vector<CompileError>& errors_out) {
    const auto for_attrs = std::get<ForAttrs>(doc.nodes[for_idx].attrs);
    const auto for_source = doc.nodes[for_idx].source;

    // Helper: emit error + mark the <for> dead so the outer find-loop
    // doesn't re-process it. Always called on error-return paths.
    auto fail = [&](std::string msg) {
        push_error(errors_out, CompileError::Composition, std::move(msg),
                    for_source);
        doc.nodes[for_idx].dead = true;
    };

    if (for_attrs.var.empty()) {
        fail("<for> requires a `var` attribute");
        return;
    }

    // Reject loop variables that shadow a frontmatter param. The
    // textual substitution would otherwise rewrite the param's name
    // inside any `{…}` expression in the loop body — silently
    // capturing it. A future binding-based unroller can lift this
    // restriction; for now, force authors to rename one of the two.
    for (const auto& p : params) {
        if (p.name == for_attrs.var) {
            fail("<for var=\"" + for_attrs.var +
                  "\"> shadows the param `" + p.name +
                  "` declared at line " + std::to_string(p.source.line) +
                  ". Rename either the loop variable or the param so the"
                  " substitution into the loop body is unambiguous.");
            return;
        }
    }

    // Determine iteration values.
    std::vector<std::string> values_str;   // textual form for substitution

    if (!for_attrs.values.empty()) {
        // Explicit-values form: raw token list.
        values_str = split_values(for_attrs.values);
        if (values_str.empty()) {
            fail("<for var=\"" + for_attrs.var +
                  "\"> values list is empty");
            return;
        }
    } else {
        // Uniform range form.
        if (for_attrs.from_expr.empty() || for_attrs.to_expr.empty() ||
            for_attrs.steps_expr.empty()) {
            fail("<for var=\"" + for_attrs.var +
                  "\"> requires either `values` or `from`+`to`+`steps`");
            return;
        }
        auto evaluator = make_evaluator(params, errors_out, for_source);
        std::vector<ExpressionError> exprs;
        const auto from_opt  = evaluator.evaluate_number(for_attrs.from_expr,
                                                          for_source, exprs);
        const auto to_opt    = evaluator.evaluate_number(for_attrs.to_expr,
                                                          for_source, exprs);
        const auto steps_opt = evaluator.evaluate_number(for_attrs.steps_expr,
                                                          for_source, exprs);
        if (!from_opt || !to_opt || !steps_opt) {
            std::string detail;
            for (const auto& e : exprs) {
                detail += "; ";
                detail += e.message;
            }
            fail("<for> bounds must resolve at compile time" + detail);
            return;
        }
        // SECURITY: range-check the steps value BEFORE casting to int.
        // static_cast<int>(huge_double) is UB in C++; on most ABIs it
        // wraps past INT_MIN, surviving the `< 1` guard and triggering
        // a SIZE_MAX-equivalent allocation. Also caps the legitimate
        // upper bound to prevent a single `<for steps="1e9"/>` from
        // OOMing the host.
        constexpr int kMaxForSteps = 100000;
        if (!std::isfinite(*steps_opt) ||
            *steps_opt < 1.0 || *steps_opt > static_cast<double>(kMaxForSteps)) {
            fail("<for steps=\"\"> must be a finite integer in [1, " +
                 std::to_string(kMaxForSteps) + "]");
            return;
        }
        const int steps = static_cast<int>(*steps_opt);
        values_str.reserve(steps);
        if (steps == 1) {
            values_str.push_back(format_number(*from_opt));
        } else {
            for (int i = 0; i < steps; ++i) {
                const double t = static_cast<double>(i) /
                                  static_cast<double>(steps - 1);
                const double v = *from_opt + t * (*to_opt - *from_opt);
                values_str.push_back(format_number(v));
            }
        }
    }

    // Collect children of the <for> element.
    std::vector<std::uint32_t> body_children;
    for (std::uint32_t c = doc.nodes[for_idx].first_child;
         c != NO_NODE; c = doc.nodes[c].next_sibling) {
        body_children.push_back(c);
    }

    if (body_children.empty()) {
        // Empty body — just unlink the <for>; nothing to splice in.
        std::uint32_t synthetic_head = find_top_level_head(doc);
        std::uint32_t* head_ptr =
            (doc.nodes[for_idx].parent == NO_NODE) ? &synthetic_head : nullptr;
        unlink_from_parent(doc, for_idx, head_ptr);
        doc.nodes[for_idx].dead = true;
        return;
    }

    // SECURITY: charge the cumulative unroll budget before allocating.
    // Predicted node count = iterations * (1 wrapper + sum-of-body-sizes).
    // We use the *current* body sizes; any inner <for>/<pattern> has
    // already been unrolled by the inside-out outer driver, so the
    // body sizes already reflect their multiplied form.
    {
        std::uint64_t body_size_total = 0;
        for (auto body_idx : body_children) {
            body_size_total += collect_subtree(doc, body_idx).size();
        }
        const std::uint64_t per_iter = 1u /*wrapper*/ + body_size_total;
        const std::uint64_t predicted =
            static_cast<std::uint64_t>(values_str.size()) * per_iter;
        if (!budget.charge(predicted)) {
            fail("<for> cumulative unroll node budget exceeded "
                 "(predicted " + std::to_string(predicted) +
                 " new nodes; budget " + std::to_string(budget.limit) +
                 "). Nest fewer or smaller loops, or split the file.");
            return;
        }
    }

    // Build the iteration chain. For each iteration, wrap the body
    // children in a <group iteration="N">.
    //
    // Steps:
    //   1. Find the <for>'s parent + the <for>'s next sibling.
    //   2. Unlink <for> from its parent.
    //   3. For each iteration value:
    //        a. Create a wrapper Group node (parent = <for>'s parent).
    //        b. Clone each body child under the wrapper.
    //        c. Apply variable substitution to the wrapper subtree.
    //   4. Splice the chain of wrapper nodes into the parent's children
    //      list, in place of the original <for>.

    const auto parent_idx       = doc.nodes[for_idx].parent;
    const auto following_idx    = doc.nodes[for_idx].next_sibling;
    std::uint32_t synthetic_head = find_top_level_head(doc);
    std::uint32_t* head_ptr =
        (parent_idx == NO_NODE) ? &synthetic_head : nullptr;
    const auto anchor_idx = unlink_from_parent(doc, for_idx, head_ptr);

    std::uint32_t chain_first = NO_NODE;
    std::uint32_t chain_last  = NO_NODE;

    for (std::size_t iter = 0; iter < values_str.size(); ++iter) {
        // Create wrapper Group.
        Node wrapper;
        wrapper.type   = NodeType::Group;
        wrapper.parent = parent_idx;
        wrapper.attrs  = GroupAttrs{};
        wrapper.source = for_source;
        wrapper.iteration = static_cast<std::uint32_t>(iter);
        const auto wrapper_idx = static_cast<std::uint32_t>(doc.nodes.size());
        doc.nodes.push_back(wrapper);

        // Clone each body child into the wrapper.
        std::uint32_t prev_child = NO_NODE;
        std::uint32_t first_child = NO_NODE;
        for (auto body_idx : body_children) {
            const auto cloned = clone_subtree(doc, body_idx, wrapper_idx);
            if (first_child == NO_NODE) first_child = cloned;
            if (prev_child != NO_NODE) {
                doc.nodes[prev_child].next_sibling = cloned;
            }
            prev_child = cloned;
        }
        doc.nodes[wrapper_idx].first_child = first_child;

        // Substitute the loop variable.
        substitute_var_in_subtree(doc, wrapper_idx,
                                    for_attrs.var, values_str[iter]);

        // Append wrapper to the iteration chain.
        if (chain_first == NO_NODE) chain_first = wrapper_idx;
        if (chain_last  != NO_NODE) {
            doc.nodes[chain_last].next_sibling = wrapper_idx;
        }
        chain_last = wrapper_idx;
    }

    splice_after(doc, parent_idx, head_ptr,
                  anchor_idx, chain_first, chain_last, following_idx);

    // Mark the original <for> as dead so the outer loop's next pass
    // doesn't find it again, and the serializer skips it.
    doc.nodes[for_idx].dead = true;
}

}  // namespace

void unroll_for_elements(Document& doc,
                          const std::vector<ParamDecl>& params,
                          UnrollBudget& budget,
                          std::vector<CompileError>& errors_out) {
    // Multi-pass to handle nested <for>: process inside-out.
    // Snapshot indices before each pass since we mutate doc.nodes.
    while (true) {
        if (budget.tripped) break;
        std::uint32_t target = NO_NODE;
        // Find a <for> whose subtree contains no other <for>.
        for (std::uint32_t i = 0; i < doc.nodes.size(); ++i) {
            if (doc.nodes[i].dead) continue;
            if (doc.nodes[i].type != NodeType::For) continue;
            // Check if this <for>'s subtree contains another live <for>.
            bool has_nested = false;
            for (auto idx : collect_subtree(doc, i)) {
                if (idx != i && !doc.nodes[idx].dead &&
                    doc.nodes[idx].type == NodeType::For) {
                    has_nested = true; break;
                }
            }
            if (!has_nested) { target = i; break; }
        }
        if (target == NO_NODE) break;
        unroll_one_for(doc, target, params, budget, errors_out);
    }
}

// ─── <pattern> unroller ──────────────────────────────────────────────

namespace {

// Build the transform string for a single pattern iteration.
std::string compute_pattern_transform(
    const PatternAttrs& pa,
    int iteration_index,
    int total_count,
    ExpressionEvaluator& evaluator,
    SourceRange src,
    std::vector<CompileError>& errors_out)
{
    std::vector<ExpressionError> exprs;
    if (pa.type == "linear") {
        const auto spacing_opt =
            evaluator.evaluate_number(pa.spacing_expr, src, exprs);
        if (!spacing_opt) {
            push_error(errors_out, CompileError::Composition,
                "<pattern type=\"linear\"> spacing must resolve at"
                " compile time", src);
            return {};
        }
        // Resolve axis to a vec3.
        auto axis_vec = parse_axis_alias(pa.axis);
        if (!axis_vec) {
            // Try as expression (rare; usually axis is literal alias).
            auto v = evaluator.evaluate_vector(pa.axis, src, exprs);
            if (!v) {
                push_error(errors_out, CompileError::Composition,
                    "<pattern> axis must be a known axis alias"
                    " (+x/-z/etc.) or vec3 expression", src);
                return {};
            }
            axis_vec = *v;
        }
        const double offset = (*spacing_opt) * iteration_index;
        const auto x = format_number(axis_vec->x * offset);
        const auto y = format_number(axis_vec->y * offset);
        const auto z = format_number(axis_vec->z * offset);
        return "translate(" + x + ", " + y + ", " + z + ")";
    } else if (pa.type == "circular") {
        const auto angle_opt =
            evaluator.evaluate_number(pa.angle_expr, src, exprs);
        if (!angle_opt) {
            push_error(errors_out, CompileError::Composition,
                "<pattern type=\"circular\"> angle must resolve at"
                " compile time", src);
            return {};
        }
        if (total_count < 1) return {};
        const double step = (*angle_opt) / static_cast<double>(total_count);
        const double angle_deg = step * iteration_index;
        auto axis_vec = parse_axis_alias(pa.axis);
        if (!axis_vec) {
            auto v = evaluator.evaluate_vector(pa.axis, src, exprs);
            if (!v) {
                push_error(errors_out, CompileError::Composition,
                    "<pattern> axis must be a known axis alias"
                    " (+x/-z/etc.) or vec3 expression", src);
                return {};
            }
            axis_vec = *v;
        }
        const auto a  = format_number(angle_deg);
        const auto ax = format_number(axis_vec->x);
        const auto ay = format_number(axis_vec->y);
        const auto az = format_number(axis_vec->z);
        return "rotate(" + a + ", " + ax + ", " + ay + ", " + az + ")";
    } else {
        push_error(errors_out, CompileError::Composition,
            "<pattern type=\"" + pa.type + "\"> — type must be"
            " \"linear\" or \"circular\"", src);
        return {};
    }
}

void unroll_one_pattern(Document& doc, std::uint32_t pat_idx,
                          const std::vector<ParamDecl>& params,
                          UnrollBudget& budget,
                          std::vector<CompileError>& errors_out) {
    const auto pat_attrs = std::get<PatternAttrs>(doc.nodes[pat_idx].attrs);
    const auto pat_source = doc.nodes[pat_idx].source;

    auto fail = [&](std::string msg) {
        push_error(errors_out, CompileError::Composition, std::move(msg),
                    pat_source);
        doc.nodes[pat_idx].dead = true;
    };

    auto evaluator = make_evaluator(params, errors_out, pat_source);
    std::vector<ExpressionError> exprs;
    const auto count_opt =
        evaluator.evaluate_number(pat_attrs.count_expr, pat_source, exprs);
    if (!count_opt) {
        fail("<pattern> count must resolve at compile time");
        return;
    }
    // SECURITY: range-check before cast — see <for steps> above for
    // the rationale. UB on signed-overflow casts otherwise.
    constexpr int kMaxPatternCount = 10000;
    if (!std::isfinite(*count_opt) ||
        *count_opt < 1.0 || *count_opt > static_cast<double>(kMaxPatternCount)) {
        fail("<pattern count=\"\"> must be a finite integer in [1, " +
             std::to_string(kMaxPatternCount) + "]");
        return;
    }
    const int count = static_cast<int>(*count_opt);

    // Collect children.
    std::vector<std::uint32_t> body_children;
    for (std::uint32_t c = doc.nodes[pat_idx].first_child;
         c != NO_NODE; c = doc.nodes[c].next_sibling) {
        body_children.push_back(c);
    }

    // SECURITY: charge cumulative unroll budget BEFORE mutating the
    // tree. Mirrors unroll_one_for. Predicted = count * (1 wrapper +
    // total body subtree sizes). Body subtree sizes already reflect
    // any inner <for>/<pattern> unrolled by the inside-out driver.
    if (!body_children.empty()) {
        std::uint64_t body_size_total = 0;
        for (auto body_idx : body_children) {
            body_size_total += collect_subtree(doc, body_idx).size();
        }
        const std::uint64_t per_iter = 1u /*wrapper*/ + body_size_total;
        const std::uint64_t predicted =
            static_cast<std::uint64_t>(count) * per_iter;
        if (!budget.charge(predicted)) {
            fail("<pattern> cumulative unroll node budget exceeded "
                 "(predicted " + std::to_string(predicted) +
                 " new nodes; budget " + std::to_string(budget.limit) +
                 "). Nest fewer or smaller patterns, or split the file.");
            return;
        }
    }

    const auto parent_idx    = doc.nodes[pat_idx].parent;
    const auto following_idx = doc.nodes[pat_idx].next_sibling;
    std::uint32_t synthetic_head = find_top_level_head(doc);
    std::uint32_t* head_ptr =
        (parent_idx == NO_NODE) ? &synthetic_head : nullptr;
    const auto anchor_idx = unlink_from_parent(doc, pat_idx, head_ptr);

    if (body_children.empty()) {
        // Nothing to repeat — but still need to mark the <pattern> as
        // dead so the outer find-loop doesn't re-process it.
        doc.nodes[pat_idx].dead = true;
        return;
    }

    std::uint32_t chain_first = NO_NODE;
    std::uint32_t chain_last  = NO_NODE;

    for (int iter = 0; iter < count; ++iter) {
        const auto transform_str = compute_pattern_transform(
            pat_attrs, iter, count, evaluator, pat_source, errors_out);
        if (transform_str.empty()) continue;

        Node wrapper;
        wrapper.type   = NodeType::Group;
        wrapper.parent = parent_idx;
        GroupAttrs ga;
        ga.transform = transform_str;
        wrapper.attrs  = ga;
        wrapper.source = pat_source;
        wrapper.iteration = static_cast<std::uint32_t>(iter);
        const auto wrapper_idx = static_cast<std::uint32_t>(doc.nodes.size());
        doc.nodes.push_back(wrapper);

        std::uint32_t prev_child = NO_NODE;
        std::uint32_t first_child = NO_NODE;
        for (auto body_idx : body_children) {
            const auto cloned = clone_subtree(doc, body_idx, wrapper_idx);
            if (first_child == NO_NODE) first_child = cloned;
            if (prev_child != NO_NODE) {
                doc.nodes[prev_child].next_sibling = cloned;
            }
            prev_child = cloned;
        }
        doc.nodes[wrapper_idx].first_child = first_child;

        if (chain_first == NO_NODE) chain_first = wrapper_idx;
        if (chain_last  != NO_NODE) {
            doc.nodes[chain_last].next_sibling = wrapper_idx;
        }
        chain_last = wrapper_idx;
    }

    splice_after(doc, parent_idx, head_ptr,
                  anchor_idx, chain_first, chain_last, following_idx);

    // Mark the original <pattern> as dead (same rationale as
    // unroll_one_for above).
    doc.nodes[pat_idx].dead = true;
}

}  // namespace

void unroll_pattern_elements(Document& doc,
                              const std::vector<ParamDecl>& params,
                              UnrollBudget& budget,
                              std::vector<CompileError>& errors_out) {
    while (true) {
        if (budget.tripped) break;
        std::uint32_t target = NO_NODE;
        for (std::uint32_t i = 0; i < doc.nodes.size(); ++i) {
            if (doc.nodes[i].dead) continue;
            if (doc.nodes[i].type != NodeType::Pattern) continue;
            // Process inside-out: skip patterns containing other live patterns.
            bool has_nested = false;
            for (auto idx : collect_subtree(doc, i)) {
                if (idx != i && !doc.nodes[idx].dead &&
                    doc.nodes[idx].type == NodeType::Pattern) {
                    has_nested = true; break;
                }
            }
            if (!has_nested) { target = i; break; }
        }
        if (target == NO_NODE) break;
        unroll_one_pattern(doc, target, params, budget, errors_out);
    }
}

}  // namespace cadml::compile::detail
