// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include "subtree_ops.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace cadml::compile::detail {

namespace {

// CADML identifiers are kebab-case (a-z, 0-9, '-'), but `{expr}`
// blocks routinely contain Lua function calls and variable refs —
// and Lua identifiers use underscores. Treat `_` as part of an
// identifier so loop-var substitution doesn't carve a name like
// `blade_t` into `blade` + `_` + `t` and clobber the trailing
// `t` with the loop variable's value.
bool is_id_start(char ch) {
    return (ch >= 'a' && ch <= 'z') || ch == '_';
}
bool is_id_cont(char ch)  {
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_';
}

// Substitute `s` field by walking the variant alternative.
template <typename A>
void substitute_in_attrs_impl(A& a, std::string_view var, std::string_view value);

// Convenience wrapper: dispatch on variant.
void substitute_in_attrs(NodeAttrs& attrs,
                          std::string_view var,
                          std::string_view value) {
    std::visit([&](auto& a) { substitute_in_attrs_impl(a, var, value); },
                attrs);
}

}  // namespace

// ─── collect_subtree ─────────────────────────────────────────────────

std::vector<std::uint32_t> collect_subtree(const Document& doc,
                                            std::uint32_t root_idx) {
    std::vector<std::uint32_t> out;
    if (root_idx == NO_NODE) return out;
    // Iterative DFS to avoid stack overflow on deep trees.
    std::vector<std::uint32_t> stack;
    stack.push_back(root_idx);
    while (!stack.empty()) {
        const auto idx = stack.back(); stack.pop_back();
        if (idx == NO_NODE) continue;
        out.push_back(idx);
        // Push children in reverse so DFS visits in declaration order.
        std::vector<std::uint32_t> children;
        for (std::uint32_t c = doc.nodes[idx].first_child;
             c != NO_NODE; c = doc.nodes[c].next_sibling) {
            children.push_back(c);
        }
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            stack.push_back(*it);
        }
    }
    return out;
}

// ─── clone_subtree ───────────────────────────────────────────────────

std::uint32_t clone_subtree(Document& doc, std::uint32_t root_idx,
                             std::uint32_t new_parent_idx,
                             std::uint32_t iteration) {
    if (root_idx == NO_NODE) return NO_NODE;

    const auto old_indices = collect_subtree(doc, root_idx);

    // Build mapping old → new.
    std::unordered_map<std::uint32_t, std::uint32_t> mapping;
    mapping.reserve(old_indices.size());
    const auto base = static_cast<std::uint32_t>(doc.nodes.size());
    for (std::size_t i = 0; i < old_indices.size(); ++i) {
        mapping[old_indices[i]] = base + static_cast<std::uint32_t>(i);
    }

    // Append clones. Important: doc.nodes may grow inside the loop, but
    // we never read newly-pushed nodes, only existing ones via old_indices.
    for (auto old_idx : old_indices) {
        // Reserve once we know the final size to avoid invalidating the
        // reference during back-and-forth.
        Node copy = doc.nodes[old_idx];
        doc.nodes.push_back(copy);
    }

    // Re-link.
    for (auto old_idx : old_indices) {
        const auto new_idx = mapping[old_idx];
        Node& n = doc.nodes[new_idx];
        if (n.parent != NO_NODE && mapping.count(n.parent)) {
            n.parent = mapping[n.parent];
        } else {
            // Outermost node — reparent to the splice target.
            n.parent = new_parent_idx;
        }
        if (n.first_child != NO_NODE && mapping.count(n.first_child)) {
            n.first_child = mapping[n.first_child];
        } else {
            n.first_child = NO_NODE;
        }
        if (n.next_sibling != NO_NODE && mapping.count(n.next_sibling)) {
            n.next_sibling = mapping[n.next_sibling];
        } else {
            n.next_sibling = NO_NODE;
        }
    }

    const auto new_root = mapping[root_idx];
    if (iteration != UINT32_MAX) doc.nodes[new_root].iteration = iteration;
    return new_root;
}

// ─── substitute_var_in_string ────────────────────────────────────────
//
// Substitution semantics:
//   * `{var}` (the whole brace block is exactly the variable, trimmed)
//     → `value` with braces stripped. Lets string values flow into
//     literal text contexts like `id="bolt-{c}"` → `id="bolt-nw"`.
//   * `{var + ...}` (variable is part of a larger expression) →
//     `{value + ...}` — braces preserved, identifier replaced inline.
//   * `var` outside braces → unchanged (CADML expressions live inside
//     braces; bare identifiers in attribute values are literal text).

namespace {

bool is_ws(char ch) {
    return std::isspace(static_cast<unsigned char>(ch));
}

// Substitute identifier matches inside one brace block's content.
std::string substitute_inside_braces(std::string_view inner,
                                       std::string_view var,
                                       std::string_view value) {
    std::string out;
    out.reserve(inner.size());
    std::size_t i = 0;
    while (i < inner.size()) {
        if (is_id_start(inner[i])) {
            std::size_t end = i;
            while (end < inner.size() && is_id_cont(inner[end])) ++end;
            std::string_view tok(inner.data() + i, end - i);
            if (tok == var) out.append(value);
            else            out.append(tok);
            i = end;
        } else {
            out.push_back(inner[i]);
            ++i;
        }
    }
    return out;
}

}  // namespace

std::string substitute_var_in_string(std::string_view s,
                                      std::string_view var,
                                      std::string_view value) {
    if (var.empty() || s.empty()) return std::string(s);

    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '{') {
            const auto end = s.find('}', i);
            if (end == std::string_view::npos) {
                // Unclosed brace — leave the rest unchanged.
                out.append(s.substr(i));
                break;
            }
            const auto inner = s.substr(i + 1, end - i - 1);
            // Trim whitespace and check whole-block match.
            std::size_t lo = 0, hi = inner.size();
            while (lo < hi && is_ws(inner[lo])) ++lo;
            while (hi > lo && is_ws(inner[hi - 1])) --hi;
            const auto trimmed = inner.substr(lo, hi - lo);
            if (trimmed == var) {
                // Replace the whole `{var}` (braces and all) with value.
                out.append(value);
            } else {
                // Identifier-aware substitution inside the brace.
                out.push_back('{');
                out.append(substitute_inside_braces(inner, var, value));
                out.push_back('}');
            }
            i = end + 1;
        } else {
            out.push_back(s[i]);
            ++i;
        }
    }
    return out;
}

// ─── substitute_var_in_subtree ───────────────────────────────────────

namespace {

#define SUBST_FIELD(a, field) \
    a.field = substitute_var_in_string(a.field, var, value)

template <typename A>
void substitute_in_attrs_impl(A& a, std::string_view var, std::string_view value) {
    if constexpr (std::is_same_v<A, PartAttrs>) {
        SUBST_FIELD(a, name);
        SUBST_FIELD(a, color);
    } else if constexpr (std::is_same_v<A, DefAttrs>) {
        SUBST_FIELD(a, name);
    } else if constexpr (std::is_same_v<A, AssemblyAttrs>) {
        SUBST_FIELD(a, name);
    } else if constexpr (std::is_same_v<A, ConnectAttrs>) {
        SUBST_FIELD(a, a);
        SUBST_FIELD(a, b);
    } else if constexpr (std::is_same_v<A, PortAttrs>) {
        SUBST_FIELD(a, name);
        SUBST_FIELD(a, position_expr);
        SUBST_FIELD(a, normal_expr);
        SUBST_FIELD(a, up_expr);
    } else if constexpr (std::is_same_v<A, GroupAttrs>) {
        SUBST_FIELD(a, id);
        SUBST_FIELD(a, transform);
        SUBST_FIELD(a, color);
    } else if constexpr (std::is_same_v<A, ScriptAttrs>) {
        // Script body is opaque Lua code; do not substitute.
        (void)var; (void)value;
    } else if constexpr (std::is_same_v<A, ForAttrs>) {
        SUBST_FIELD(a, var);
        SUBST_FIELD(a, from_expr);
        SUBST_FIELD(a, to_expr);
        SUBST_FIELD(a, steps_expr);
        SUBST_FIELD(a, values);
    } else if constexpr (std::is_same_v<A, CircleAttrs>) {
        SUBST_FIELD(a, cx_expr);
        SUBST_FIELD(a, cy_expr);
        SUBST_FIELD(a, r_expr);
    } else if constexpr (std::is_same_v<A, RectAttrs>) {
        SUBST_FIELD(a, x_expr);
        SUBST_FIELD(a, y_expr);
        SUBST_FIELD(a, width_expr);
        SUBST_FIELD(a, height_expr);
        SUBST_FIELD(a, rx_expr);
        SUBST_FIELD(a, ry_expr);
    } else if constexpr (std::is_same_v<A, PathAttrs>) {
        SUBST_FIELD(a, d);
    } else if constexpr (std::is_same_v<A, SketchAttrs>) {
        SUBST_FIELD(a, plane);
        SUBST_FIELD(a, origin_expr);
        SUBST_FIELD(a, rotation_expr);
        SUBST_FIELD(a, normal_expr);
    } else if constexpr (std::is_same_v<A, ExtrudeAttrs>) {
        SUBST_FIELD(a, height_expr);
        SUBST_FIELD(a, scale_expr);
        SUBST_FIELD(a, draft_expr);
        SUBST_FIELD(a, direction_expr);
    } else if constexpr (std::is_same_v<A, RevolveAttrs>) {
        SUBST_FIELD(a, axis);
        SUBST_FIELD(a, angle_expr);
        SUBST_FIELD(a, segments_expr);
    } else if constexpr (std::is_same_v<A, HelixAttrs>) {
        SUBST_FIELD(a, radius_expr);
        SUBST_FIELD(a, pitch_expr);
        SUBST_FIELD(a, turns_expr);
        SUBST_FIELD(a, taper_expr);
        SUBST_FIELD(a, direction);
    } else if constexpr (std::is_same_v<A, FilletAttrs>) {
        SUBST_FIELD(a, radius_expr);
        SUBST_FIELD(a, select);
    } else if constexpr (std::is_same_v<A, ChamferAttrs>) {
        SUBST_FIELD(a, distance_expr);
        SUBST_FIELD(a, angle_expr);
        SUBST_FIELD(a, select);
    } else if constexpr (std::is_same_v<A, ShellAttrs>) {
        SUBST_FIELD(a, thickness_expr);
        SUBST_FIELD(a, open);
    } else if constexpr (std::is_same_v<A, CutAttrs>) {
        SUBST_FIELD(a, face);
        SUBST_FIELD(a, type);
        SUBST_FIELD(a, angle_expr);
        SUBST_FIELD(a, miter_expr);
        SUBST_FIELD(a, bevel_expr);
    } else if constexpr (std::is_same_v<A, PatternAttrs>) {
        SUBST_FIELD(a, type);
        SUBST_FIELD(a, count_expr);
        SUBST_FIELD(a, axis);
        SUBST_FIELD(a, spacing_expr);
        SUBST_FIELD(a, angle_expr);
    } else if constexpr (std::is_same_v<A, ParamAttrs>) {
        SUBST_FIELD(a, name);
        SUBST_FIELD(a, value_expr);
    } else if constexpr (std::is_same_v<A, SourceAttrs>) {
        SUBST_FIELD(a, path);
        SUBST_FIELD(a, hash);
    } else if constexpr (std::is_same_v<A, InstanceAttrs>) {
        SUBST_FIELD(a, ref_name);
        SUBST_FIELD(a, id);
        SUBST_FIELD(a, at);
        SUBST_FIELD(a, port);
        for (auto& [k, v] : a.param_overrides) {
            v = substitute_var_in_string(v, var, value);
        }
    } else if constexpr (std::is_same_v<A, UnknownAttrs>) {
        for (auto& [k, v] : a.raw_attrs) {
            v = substitute_var_in_string(v, var, value);
        }
    }
    // SweepAttrs / LoftAttrs / UnionAttrs / DifferenceAttrs /
    // IntersectAttrs / HullAttrs / SvgAttrs / SourcesAttrs have no
    // string fields.
    (void)var; (void)value;
}

#undef SUBST_FIELD

}  // namespace

void substitute_var_in_subtree(Document& doc, std::uint32_t root_idx,
                                std::string_view var,
                                std::string_view value) {
    if (root_idx == NO_NODE) return;
    const auto indices = collect_subtree(doc, root_idx);
    for (auto idx : indices) {
        substitute_in_attrs(doc.nodes[idx].attrs, var, value);
    }
}

// ─── unlink_from_parent ──────────────────────────────────────────────

std::uint32_t unlink_from_parent(Document& doc,
                                  std::uint32_t node_idx,
                                  std::uint32_t* top_level_head) {
    if (node_idx == NO_NODE) return NO_NODE;
    const auto parent = doc.nodes[node_idx].parent;

    // Find previous sibling.
    std::uint32_t prev = NO_NODE;
    std::uint32_t cur  = NO_NODE;
    if (parent == NO_NODE) {
        if (!top_level_head) return NO_NODE;  // can't fix up
        cur = *top_level_head;
    } else {
        cur = doc.nodes[parent].first_child;
    }
    while (cur != NO_NODE && cur != node_idx) {
        prev = cur;
        cur = doc.nodes[cur].next_sibling;
    }
    if (cur != node_idx) return NO_NODE;  // not in chain

    const auto next = doc.nodes[node_idx].next_sibling;
    if (prev == NO_NODE) {
        // Unlinking the head.
        if (parent == NO_NODE) {
            *top_level_head = next;
        } else {
            doc.nodes[parent].first_child = next;
        }
    } else {
        doc.nodes[prev].next_sibling = next;
    }
    doc.nodes[node_idx].next_sibling = NO_NODE;
    return prev;
}

// ─── splice_after ────────────────────────────────────────────────────

void splice_after(Document& doc,
                   std::uint32_t parent_idx,
                   std::uint32_t* top_level_head,
                   std::uint32_t anchor_idx,
                   std::uint32_t chain_first,
                   std::uint32_t chain_last,
                   std::uint32_t following_idx) {
    if (chain_first == NO_NODE) {
        // No insert; reattach following directly.
        if (anchor_idx == NO_NODE) {
            if (parent_idx == NO_NODE) {
                if (top_level_head) *top_level_head = following_idx;
            } else {
                doc.nodes[parent_idx].first_child = following_idx;
            }
        } else {
            doc.nodes[anchor_idx].next_sibling = following_idx;
        }
        return;
    }

    // Connect chain_last to following.
    doc.nodes[chain_last].next_sibling = following_idx;

    // Connect anchor (or parent's first_child) to chain_first.
    if (anchor_idx == NO_NODE) {
        if (parent_idx == NO_NODE) {
            if (top_level_head) *top_level_head = chain_first;
        } else {
            doc.nodes[parent_idx].first_child = chain_first;
        }
    } else {
        doc.nodes[anchor_idx].next_sibling = chain_first;
    }
}

}  // namespace cadml::compile::detail
