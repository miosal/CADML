// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include "assembly_compiler.hpp"

#include "subtree_ops.hpp"

#include <cadml/compile/bundler.hpp>
#include <cadml/expression.hpp>

#include <algorithm>
#include <cstdint>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace cadml::compile::detail {

namespace {

void push_error(std::vector<CompileError>& errors_out,
                 CompileError::Category cat,
                 std::string msg, SourceRange src) {
    errors_out.push_back({ cat, std::move(msg), src });
}

// ─── Find the def for an instance ────────────────────────────────────

std::int64_t find_def_idx(const Document& doc, const std::string& name) {
    auto it = doc.defs.find(name);
    if (it == doc.defs.end()) return -1;
    return static_cast<std::int64_t>(it->second);
}

// ─── Collect ports from a def, evaluating expressions ───────────────

struct ResolvedPort {
    std::string name;
    Vec3        position;
    Vec3        normal;
    Vec3        up;
    SourceRange source;
};

// Evaluate `<port>` children of the def, producing numeric positions.
// `extra_params` are instance-specific overrides layered on top of
// entry-file params + def-level <param> children (which were hoisted by
// import resolution).
std::vector<ResolvedPort> collect_ports(
    const Document& doc, std::uint32_t def_idx,
    const std::vector<ParamDecl>& entry_params,
    const std::unordered_map<std::string, std::string>& extra_params,
    std::vector<CompileError>& errors_out)
{
    std::vector<ResolvedPort> out;

    // Build the eval scope: entry params first, then the def's own
    // <param> children, then instance overrides.
    ExpressionEvaluator e;
    auto eval_into_scope = [&](const std::string& name,
                                const std::string& expr) {
        std::vector<ExpressionError> errs;
        auto v = e.evaluate_number(expr, {}, errs);
        if (v) e.set_param(name, *v);
    };

    for (const auto& p : entry_params) {
        eval_into_scope(p.name, p.value_expr);
    }
    // Def's <param> children (from hoisted imports).
    for (auto& child : doc.children(def_idx)) {
        if (child.dead) continue;
        if (child.type != NodeType::Param) continue;
        const auto& pa = std::get<ParamAttrs>(child.attrs);
        eval_into_scope(pa.name, pa.value_expr);
    }
    // Instance-specific overrides last.
    for (const auto& [name, expr] : extra_params) {
        eval_into_scope(name, expr);
    }

    for (auto& child : doc.children(def_idx)) {
        if (child.dead) continue;
        if (child.type != NodeType::Port) continue;
        const auto& pa = std::get<PortAttrs>(child.attrs);

        ResolvedPort rp;
        rp.name = pa.name;
        rp.source = child.source;

        std::vector<ExpressionError> errs;
        auto pos = e.evaluate_vector(pa.position_expr, child.source, errs);
        if (!pos) {
            push_error(errors_out, CompileError::Composition,
                "port `" + pa.name + "`: invalid `position` (" +
                pa.position_expr + ")", child.source);
            continue;
        }
        rp.position = *pos;

        auto nrm = e.evaluate_vector(pa.normal_expr, child.source, errs);
        if (!nrm) {
            push_error(errors_out, CompileError::Composition,
                "port `" + pa.name + "`: invalid `normal` (" +
                pa.normal_expr + ")", child.source);
            continue;
        }
        rp.normal = *nrm;

        if (pa.up_expr.empty()) {
            // Per spec / implementation-notes.md §11: a <port> without
            // `up` leaves one rotational DOF unconstrained, which is
            // never what the user wants in an assembly context. Be
            // loud about it rather than silently picking a default
            // that drifts mate orientations.
            push_error(errors_out, CompileError::Composition,
                "port `" + pa.name + "` is missing required `up` "
                "attribute. A port needs position + normal + up to "
                "fully constrain mating orientation. (Pick `up` as "
                "any unit vector orthogonal to `normal`.)",
                child.source);
            continue;
        } else {
            auto up = e.evaluate_vector(pa.up_expr, child.source, errs);
            if (!up) {
                push_error(errors_out, CompileError::Composition,
                    "port `" + pa.name + "`: invalid `up` (" +
                    pa.up_expr + ")", child.source);
                continue;
            }
            rp.up = *up;
        }

        out.push_back(rp);
    }
    return out;
}

const ResolvedPort* find_port(const std::vector<ResolvedPort>& ports,
                                const std::string& name) {
    for (const auto& p : ports) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

// ─── Dotted port-path resolution (sub-assembly traversal) ────────────
//
// `at="a.b.c"` on a child of an outer assembly means: starting from the
// parent instance's def, walk through nested Instance children matching
// the leading segments by id (or auto-id == ref_name), then look up the
// trailing segment as a port name on the deepest def. Today we only
// descend through BARE sub-instances (no own at/port mating), so each
// step contributes identity to the placement and the leaf port's
// def-local position is the answer. Mating sub-instances would require
// pre-solving their placements (and an accumulated Mat4 transform);
// that's deferred.
std::optional<ResolvedPort> resolve_dotted_port(
    const Document& doc,
    std::int64_t def_root_idx,
    const std::vector<std::string>& segments,
    const std::vector<ParamDecl>& entry_params,
    SourceRange src,
    std::vector<CompileError>& errors_out)
{
    if (def_root_idx < 0 || segments.empty()) return std::nullopt;

    // Locate sub-instance child of `current_def` whose id (or auto-id
    // derived from ref_name when id is absent) equals `segment_id`.
    auto find_subinst = [&](std::uint32_t current_def, const std::string& seg_id)
        -> std::int64_t {
        for (auto& child : doc.children(current_def)) {
            if (child.dead) continue;
            if (child.type != NodeType::Instance) continue;
            const auto& ia = std::get<InstanceAttrs>(child.attrs);
            const std::string id = ia.id.empty() ? ia.ref_name : ia.id;
            if (id == seg_id) {
                return static_cast<std::int64_t>(&child - &doc.nodes[0]);
            }
        }
        return -1;
    };

    auto current_def = static_cast<std::uint32_t>(def_root_idx);
    // Track the deepest sub-instance encountered: its param_overrides are
    // what bind into the leaf def's port-expression scope, NOT the outer
    // caller's overrides (which apply to a different def).
    const std::unordered_map<std::string, std::string>* leaf_overrides = nullptr;

    // Inside an imported def, sub-instances' tag names (`ref_name`) are
    // local to that def's namespace. The host's def index keys them as
    // "<containing-def>.<local-name>". Resolve by trying the qualified
    // name first, then the bare name (for entry-file local defs).
    auto qualified_def_lookup = [&](std::uint32_t containing_def,
                                      const std::string& ref_name) -> std::int64_t {
        const auto& cn = doc.nodes[containing_def];
        std::string prefix;
        if (cn.type == NodeType::Def) {
            prefix = std::get<DefAttrs>(cn.attrs).name;
        } else if (cn.type == NodeType::Part) {
            prefix = std::get<PartAttrs>(cn.attrs).name;
        }
        if (!prefix.empty()) {
            const auto qualified = prefix + "." + ref_name;
            const auto idx = find_def_idx(doc, qualified);
            if (idx >= 0) return idx;
        }
        return find_def_idx(doc, ref_name);
    };

    // Walk all but the last segment, descending one Instance per step.
    for (std::size_t i = 0; i + 1 < segments.size(); ++i) {
        const auto sub_node_idx = find_subinst(current_def, segments[i]);
        if (sub_node_idx < 0) {
            push_error(errors_out, CompileError::Composition,
                "dotted port path: cannot find sub-instance `" + segments[i] +
                "` inside def", src);
            return std::nullopt;
        }
        const auto& sub_node = doc.nodes[sub_node_idx];
        const auto& sub_ia = std::get<InstanceAttrs>(sub_node.attrs);

        // Bare sub-instances only — mating sub-instances would require
        // pre-solving their placement, which the bundler doesn't do.
        if (!sub_ia.at.empty() || !sub_ia.port.empty()) {
            push_error(errors_out, CompileError::Composition,
                "dotted port path: sub-instance `" + segments[i] +
                "` has at/port mating; nested mating-driven traversal is"
                " not yet supported", src);
            return std::nullopt;
        }

        const auto next_def_idx = qualified_def_lookup(current_def, sub_ia.ref_name);
        if (next_def_idx < 0) {
            push_error(errors_out, CompileError::Composition,
                "dotted port path: sub-instance `" + segments[i] +
                "` references unknown def `" + sub_ia.ref_name + "`", src);
            return std::nullopt;
        }
        leaf_overrides = &sub_ia.param_overrides;
        current_def = static_cast<std::uint32_t>(next_def_idx);
    }

    // Terminal segment is a port name on the deepest def.
    static const std::unordered_map<std::string, std::string> kEmpty;
    auto leaf_ports = collect_ports(doc, current_def, entry_params,
                                      leaf_overrides ? *leaf_overrides : kEmpty,
                                      errors_out);
    const auto* leaf = find_port(leaf_ports, segments.back());
    if (!leaf) {
        push_error(errors_out, CompileError::Composition,
            "dotted port path: port `" + segments.back() +
            "` not found on terminal def", src);
        return std::nullopt;
    }

    ResolvedPort out;
    out.name     = leaf->name;
    out.position = leaf->position;
    out.normal   = leaf->normal;
    out.up       = leaf->up;
    out.source   = leaf->source;
    return out;
}

std::vector<std::string> split_dots(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '.') {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

// ─── Instance bookkeeping ────────────────────────────────────────────

struct InstanceInfo {
    std::uint32_t            node_idx;
    std::string              id;            // explicit or auto-generated
    std::string              ref;           // alias name
    std::string              at;            // parent port (if mating)
    std::string              port;          // own port (if mating)
    std::uint32_t            parent_inst;   // index into instances vector;
                                             // UINT32_MAX if top-level
    std::unordered_map<std::string, std::string> param_overrides;
    SourceRange              source;
};

// Recursively collect instances within an assembly subtree, building
// parent links between mating instances and assigning auto-ids.
void collect_instances(const Document& doc, std::uint32_t parent_xml,
                        std::uint32_t parent_inst,
                        std::vector<InstanceInfo>& out,
                        std::unordered_map<std::string, int>& id_counters)
{
    for (auto& child : doc.children(parent_xml)) {
        if (child.dead) continue;
        if (child.type != NodeType::Instance) continue;

        const auto& ia = std::get<InstanceAttrs>(child.attrs);
        const auto child_idx = static_cast<std::uint32_t>(&child - &doc.nodes[0]);

        InstanceInfo info;
        info.node_idx = child_idx;
        info.ref = ia.ref_name;
        info.at = ia.at;
        info.port = ia.port;
        info.parent_inst = parent_inst;
        info.param_overrides = ia.param_overrides;
        info.source = child.source;

        // ID: explicit, or counter-suffixed alias name (stable per file).
        if (!ia.id.empty()) {
            info.id = ia.id;
        } else {
            const int n = id_counters[ia.ref_name]++;
            info.id = (n == 0) ? ia.ref_name
                                : ia.ref_name + "#" + std::to_string(n + 1);
        }

        const auto inst_idx = static_cast<std::uint32_t>(out.size());
        out.push_back(std::move(info));

        // Recurse into the instance's body for nested mating instances.
        collect_instances(doc, child_idx, inst_idx, out, id_counters);
    }
}

// ─── Connect representation ──────────────────────────────────────────

struct Connect {
    std::uint32_t inst_a;
    std::string   port_a;
    std::uint32_t inst_b;
    std::string   port_b;
    SourceRange   source;
    bool          allow_interference = false;
};

}  // namespace

// ─── Per-assembly compilation ────────────────────────────────────────

namespace {

void compile_one_assembly(Document& doc, std::uint32_t asm_idx,
                            const std::vector<ParamDecl>& entry_params,
                            std::vector<CompileError>& errors_out,
                            std::vector<CompileError>& warnings_out)
{
    const auto asm_source = doc.nodes[asm_idx].source;
    const auto asm_attrs = std::get<AssemblyAttrs>(doc.nodes[asm_idx].attrs);
    const std::string asm_name =
        asm_attrs.name.empty() ? "<anonymous-assembly>" : asm_attrs.name;

    auto fail = [&](std::string msg, SourceRange src = {}) {
        if (!src.valid()) src = asm_source;
        push_error(errors_out, CompileError::Composition, std::move(msg), src);
    };

    // 1. Collect all instances with auto-id assignment.
    std::vector<InstanceInfo> instances;
    std::unordered_map<std::string, int> id_counters;
    collect_instances(doc, asm_idx, UINT32_MAX, instances, id_counters);

    if (instances.empty()) {
        // Empty assembly — emit empty <part> and mark the assembly dead.
        Node part_node;
        part_node.type = NodeType::Part;
        PartAttrs pa; pa.name = asm_name;
        part_node.attrs = pa;
        part_node.parent = doc.nodes[asm_idx].parent;
        part_node.source = asm_source;
        const auto part_idx = static_cast<std::uint32_t>(doc.nodes.size());
        doc.nodes.push_back(part_node);
        std::uint32_t synthetic_head = NO_NODE;
        for (std::uint32_t i = 0; i < doc.nodes.size(); ++i) {
            if (doc.nodes[i].parent == NO_NODE && !doc.nodes[i].dead) {
                synthetic_head = i; break;
            }
        }
        std::uint32_t* hp =
            (doc.nodes[asm_idx].parent == NO_NODE) ? &synthetic_head : nullptr;
        const auto anchor = unlink_from_parent(doc, asm_idx, hp);
        const auto following = doc.nodes[asm_idx].next_sibling;
        splice_after(doc, doc.nodes[asm_idx].parent, hp, anchor,
                      part_idx, part_idx, following);
        doc.nodes[asm_idx].dead = true;
        return;
    }

    // Build id → instance index map (for connect resolution).
    std::unordered_map<std::string, std::uint32_t> id_map;
    for (std::uint32_t i = 0; i < instances.size(); ++i) {
        id_map[instances[i].id] = i;
    }

    // 2. Lower nested at/port mates into Connect entries.
    std::vector<Connect> connects;
    for (std::uint32_t i = 0; i < instances.size(); ++i) {
        const auto& inst = instances[i];
        if (inst.at.empty() && inst.port.empty()) continue;
        if (inst.at.empty() || inst.port.empty()) {
            fail("instance `" + inst.id +
                  "` requires both `at` and `port` (or neither) for mating",
                  inst.source);
            continue;
        }
        if (inst.parent_inst == UINT32_MAX) {
            fail("mating instance `" + inst.id +
                  "` (with at/port) cannot appear at the top level of"
                  " <assembly>; it must be nested under a parent instance",
                  inst.source);
            continue;
        }
        Connect c;
        c.inst_a = i;
        c.port_a = inst.port;
        c.inst_b = inst.parent_inst;
        // The `at` may be a dotted path for sub-assembly reach (e.g.
        // "plate-bottom.top"). Dotted resolution happens after step 4
        // once each instance's def-local ports are computed.
        c.port_b = inst.at;
        c.source = inst.source;
        connects.push_back(c);
    }

    // 3. Collect explicit <connect> elements (direct children of the asm).
    for (auto& child : doc.children(asm_idx)) {
        if (child.dead) continue;
        if (child.type != NodeType::Connect) continue;
        const auto& ca = std::get<ConnectAttrs>(child.attrs);
        const auto split = [](const std::string& s)
            -> std::pair<std::string, std::string> {
            const auto dot = s.find('.');
            if (dot == std::string::npos) return { s, {} };
            return { s.substr(0, dot), s.substr(dot + 1) };
        };
        const auto [id_a, port_a] = split(ca.a);
        const auto [id_b, port_b] = split(ca.b);
        if (!id_map.count(id_a)) {
            fail("<connect>: unknown instance id `" + id_a + "`", child.source);
            continue;
        }
        if (!id_map.count(id_b)) {
            fail("<connect>: unknown instance id `" + id_b + "`", child.source);
            continue;
        }
        if (id_a == id_b) {
            // Self-mate is nonsense in the rigid port-mate model — every
            // instance is rigidly defined, so mating to itself either
            // collapses to identity (vacuous) or over-constrains the
            // single body (impossible). Reject explicitly so it doesn't
            // slip past D3's `cur < other` dedup as a silent no-op.
            fail("<connect>: cannot mate instance `" + id_a +
                  "` to itself", child.source);
            continue;
        }
        Connect c;
        c.inst_a = id_map[id_a]; c.port_a = port_a;
        c.inst_b = id_map[id_b]; c.port_b = port_b;
        c.source = child.source;
        c.allow_interference = ca.allow_interference;
        connects.push_back(c);

        // Record exempt part pairs in doc.meta so
        // `cadml_check` can suppress reports between them. The pair
        // is keyed by the connected instances' def names (== part
        // names in the flat output for imported parts). De-dup by
        // ordered pair to avoid the same connect aliasing twice.
        if (ca.allow_interference) {
            const auto& def_a = instances[id_map[id_a]].ref;
            const auto& def_b = instances[id_map[id_b]].ref;
            auto& pairs = doc.meta.allow_interference_pairs;
            const std::pair<std::string, std::string> p{def_a, def_b};
            if (std::find(pairs.begin(), pairs.end(), p) == pairs.end() &&
                std::find(pairs.begin(), pairs.end(),
                           std::pair{def_b, def_a}) == pairs.end()) {
                pairs.push_back(p);
            }
        }
    }

    // 4. For each instance, validate its def exists and pre-compute its
    // resolved ports.
    std::vector<std::vector<ResolvedPort>> instance_ports(instances.size());
    for (std::uint32_t i = 0; i < instances.size(); ++i) {
        const auto& inst = instances[i];
        const auto def_idx = find_def_idx(doc, inst.ref);
        if (def_idx < 0) {
            fail("instance `" + inst.id + "` references unknown def `" +
                  inst.ref + "`", inst.source);
            continue;
        }
        instance_ports[i] = collect_ports(
            doc, static_cast<std::uint32_t>(def_idx),
            entry_params, inst.param_overrides, errors_out);
    }

    // 4b. Resolve dotted port paths (sub-assembly traversal).
    // For every connect side whose name contains a dot, resolve the
    // path against that instance's def and append the result to
    // instance_ports[that_inst] under the dotted name. find_port then
    // works for both direct and dotted names without further changes.
    auto resolve_side = [&](std::uint32_t inst_i, const std::string& port_name) {
        if (port_name.find('.') == std::string::npos) return;
        if (find_port(instance_ports[inst_i], port_name)) return; // already resolved
        const auto def_idx = find_def_idx(doc, instances[inst_i].ref);
        if (def_idx < 0) return; // already reported in step 4
        auto resolved = resolve_dotted_port(
            doc, def_idx, split_dots(port_name),
            entry_params, instances[inst_i].source, errors_out);
        if (resolved) {
            resolved->name = port_name;
            instance_ports[inst_i].push_back(*resolved);
        }
    };
    for (const auto& c : connects) {
        resolve_side(c.inst_a, c.port_a);
        resolve_side(c.inst_b, c.port_b);
    }

    // 5. Solve the connect graph. Anchor first instance at identity;
    // BFS outward placing each connected instance.
    std::vector<Mat4> transforms(instances.size(), Mat4::identity());
    std::vector<bool> placed(instances.size(), false);

    // Build adjacency: for each instance, list connects that touch it.
    std::vector<std::vector<std::uint32_t>> adj(instances.size());
    for (std::uint32_t k = 0; k < connects.size(); ++k) {
        adj[connects[k].inst_a].push_back(k);
        adj[connects[k].inst_b].push_back(k);
    }

    // Compute the transform that placing `target_inst` via connect `c`
    // (anchored on the already-placed `source_inst`) would produce.
    // Returns nullopt and pushes an error on missing-port failures.
    // No state is modified — caller decides whether to apply the
    // transform (forward edge) or just compare it (back edge / D3).
    auto compute_placement_via_connect =
        [&](std::uint32_t target_inst,
             std::uint32_t source_inst,
             const Connect& c) -> std::optional<Mat4> {
        const std::string& target_port_name =
            (c.inst_a == target_inst) ? c.port_a : c.port_b;
        const std::string& source_port_name =
            (c.inst_a == target_inst) ? c.port_b : c.port_a;
        const auto* tp = find_port(instance_ports[target_inst], target_port_name);
        const auto* sp = find_port(instance_ports[source_inst], source_port_name);
        if (!tp) {
            fail("instance `" + instances[target_inst].id + "`: port `" +
                  target_port_name + "` not found on def `" +
                  instances[target_inst].ref + "`", c.source);
            return std::nullopt;
        }
        if (!sp) {
            fail("instance `" + instances[source_inst].id + "`: port `" +
                  source_port_name + "` not found on def `" +
                  instances[source_inst].ref + "`", c.source);
            return std::nullopt;
        }
        return compute_port_alignment(
            tp->position, tp->normal, tp->up,
            sp->position, sp->normal, sp->up,
            transforms[source_inst]);
    };

    auto place_via_connect = [&](std::uint32_t target_inst,
                                   std::uint32_t source_inst,
                                   const Connect& c) -> bool {
        auto t = compute_placement_via_connect(target_inst, source_inst, c);
        if (!t) return false;
        transforms[target_inst] = *t;
        placed[target_inst] = true;
        return true;
    };

    // True iff back-edge connect `c` is consistent with the BFS
    // placements transforms[c.inst_a] and transforms[c.inst_b] —
    // i.e., the two ports COINCIDE in world space (the port-mate
    // constraint itself: positions equal, normals anti-parallel,
    // up-vectors aligned).
    //
    // Why not "compute the would-be placement and compare to the
    // BFS one"? The natural-feeling check
    //   compute_port_alignment(a, b, T_b) == T_a   ?
    // would only work if `compute_port_alignment` were symmetric in
    // (a, b), but it isn't: when T_b carries a non-trivial rotation
    // (i.e. b was itself placed via a port mate, so chained mates),
    // the function's chosen rotation for `a` differs depending on
    // direction even when the cycle IS consistent. Comparing world-
    // space port frames directly sidesteps that asymmetry — it
    // checks the constraint the user actually authored, regardless
    // of which side BFS happened to use as the source.
    auto connect_consistent = [&](const Connect& c) -> std::optional<bool> {
        const auto* pa = find_port(instance_ports[c.inst_a], c.port_a);
        const auto* pb = find_port(instance_ports[c.inst_b], c.port_b);
        if (!pa) {
            fail("instance `" + instances[c.inst_a].id + "`: port `" +
                  c.port_a + "` not found on def `" +
                  instances[c.inst_a].ref + "`", c.source);
            return std::nullopt;
        }
        if (!pb) {
            fail("instance `" + instances[c.inst_b].id + "`: port `" +
                  c.port_b + "` not found on def `" +
                  instances[c.inst_b].ref + "`", c.source);
            return std::nullopt;
        }
        auto vec_close = [](Vec3 u, Vec3 v) {
            constexpr double kTol = 1e-3;
            return std::abs(u.x - v.x) <= kTol &&
                    std::abs(u.y - v.y) <= kTol &&
                    std::abs(u.z - v.z) <= kTol;
        };
        // Position coincidence.
        const Vec3 pos_a_w =
            transforms[c.inst_a].transform_point(pa->position);
        const Vec3 pos_b_w =
            transforms[c.inst_b].transform_point(pb->position);
        if (!vec_close(pos_a_w, pos_b_w)) return false;
        // Normal anti-parallel (the port-mate flip).
        const Vec3 nA_w = transforms[c.inst_a]
            .transform_direction(pa->normal.normalized());
        const Vec3 nB_w = transforms[c.inst_b]
            .transform_direction(pb->normal.normalized());
        if (!vec_close(nA_w + nB_w, Vec3{0, 0, 0})) return false;
        // Up-vectors aligned (after Gram-Schmidt against each normal,
        // matching what compute_port_alignment does internally).
        Vec3 ua = pa->up - pa->normal * (pa->up.dot(pa->normal));
        ua = ua.normalized();
        Vec3 ub = pb->up - pb->normal * (pb->up.dot(pb->normal));
        ub = ub.normalized();
        const Vec3 ua_w = transforms[c.inst_a].transform_direction(ua);
        const Vec3 ub_w = transforms[c.inst_b].transform_direction(ub);
        if (!vec_close(ua_w, ub_w)) return false;
        return true;
    };

    // BFS from each connected component's seed (instance 0 first).
    // `component_id[i]` records which component each instance landed
    // in — used below for disconnected-instance reporting.
    //
    // **Edge case**: if a connect's port resolution fails inside
    // `place_via_connect`, that target stays unplaced and is later
    // picked up as a fresh seed — getting counted as a spurious extra
    // component. In practice `place_via_connect` failure already
    // pushes a hard error onto `errors_out`, so the compile fails and
    // the spurious-disconnect warning is moot.
    // Track connect indices that close a cycle (over-constrained-mate
    // back edges). BFS doesn't use them for placement; later we test
    // whether each back edge is *consistent* with the BFS placement
    // and warn if not.
    std::vector<std::uint32_t> back_edges;

    std::vector<std::uint32_t> component_id(instances.size(), UINT32_MAX);
    std::uint32_t next_component = 0;
    for (std::uint32_t seed = 0; seed < instances.size(); ++seed) {
        if (placed[seed]) continue;
        // If the seed has no incoming connects to a placed instance,
        // use it as a fresh anchor at identity.
        placed[seed] = true;
        transforms[seed] = Mat4::identity();
        component_id[seed] = next_component;
        std::queue<std::uint32_t> q;
        q.push(seed);
        while (!q.empty()) {
            const auto cur = q.front(); q.pop();
            for (auto k : adj[cur]) {
                const auto& c = connects[k];
                const auto other = (c.inst_a == cur) ? c.inst_b : c.inst_a;
                if (placed[other]) {
                    // Back edge — both endpoints already placed by an
                    // earlier BFS path. Record for D3 cycle-consistency
                    // analysis below. Skip set-insertion duplicates by
                    // only recording from the lower-indexed side.
                    //
                    // Cross-component guard: if `cur` and `other` are in
                    // different BFS components, this connect is the one
                    // whose forward placement failed (the only way a
                    // would-be neighbor ends up as a separate seed). It
                    // already pushed an error from `place_via_connect`;
                    // re-running the lookup in the D3 loop below would
                    // double-report the same missing-port error.
                    if (cur < other &&
                        component_id[cur] == component_id[other]) {
                        back_edges.push_back(k);
                    }
                    continue;
                }
                if (place_via_connect(other, cur, c)) {
                    component_id[other] = next_component;
                    q.push(other);
                }
            }
        }
        ++next_component;
    }

    // Disconnected-instance detection.
    //
    // A multi-component assembly means the user authored multiple
    // sub-trees that aren't bound by any <connect> or at/port mate.
    // The bundler still places them (each component anchored at
    // identity), but they pile up at the origin and visually overlap.
    // This is almost always unintentional — flag it as a warning so
    // cadml_check can surface it without failing the build.
    //
    // Single-instance assemblies and assemblies that resolve to one
    // component are clean. Component count > 1 → warn, listing the
    // instance IDs in each component beyond the first.
    if (next_component > 1) {
        // Build per-component instance-id lists (skip the seed
        // component, which is "the main assembly").
        std::vector<std::vector<std::string>> by_component(next_component);
        for (std::uint32_t i = 0; i < instances.size(); ++i) {
            if (component_id[i] == UINT32_MAX) continue;
            by_component[component_id[i]].push_back(instances[i].id);
        }
        std::string msg = "assembly `" + asm_name +
            "` has " + std::to_string(next_component) +
            " disconnected sub-trees (no <connect> or at/port mating"
            " links them; each component anchored at the origin and"
            " will visually overlap):";
        for (std::uint32_t c = 0; c < next_component; ++c) {
            msg += "\n  component ";
            msg += std::to_string(c);
            msg += ": ";
            for (std::size_t k = 0; k < by_component[c].size(); ++k) {
                if (k) msg += ", ";
                msg += "`" + by_component[c][k] + "`";
            }
        }
        CompileError w;
        w.category = CompileError::Composition;
        w.message  = std::move(msg);
        w.source   = asm_source;
        warnings_out.push_back(std::move(w));
    }

    // Over-constrained mate detection.
    //
    // Each back edge in the connect graph implies a placement for
    // its target via the source, in addition to whatever the BFS
    // already assigned. If those placements DISAGREE, the user has
    // an over-constrained design — multiple connects forcing
    // inconsistent transforms. ("Under-constrained" in the rigid
    // port-mate model is the same as disconnected, which the
    // disconnected-instance check already catches; the meaningful
    // inverse check is detecting redundant constraints that conflict.)
    //
    // Consistent cycles (the closing edge agrees with the BFS path)
    // are a feature — they document the structure without breaking
    // it. We only warn when the cycle inconsistently closes.
    for (const auto k : back_edges) {
        const auto& c = connects[k];
        auto consistent = connect_consistent(c);
        if (!consistent) continue;       // port-not-found already errored
        if (*consistent) continue;       // cycle closes consistently

        CompileError w;
        w.category = CompileError::Composition;
        w.message  = "assembly `" + asm_name +
            "`: <connect a=`" + instances[c.inst_a].id +
            "." + c.port_a + "` b=`" + instances[c.inst_b].id +
            "." + c.port_b + "`> closes a cycle whose closing transform"
            " disagrees with the rest of the connect graph (over-"
            "constrained design — at least one mate must be relaxed"
            " or the geometry adjusted)";
        w.source   = c.source;
        warnings_out.push_back(std::move(w));
    }

    // 6. Build the output <part>.
    Node part_node;
    part_node.type = NodeType::Part;
    PartAttrs pa; pa.name = asm_name;
    part_node.attrs = pa;
    part_node.parent = doc.nodes[asm_idx].parent;
    part_node.source = asm_source;
    const auto part_idx = static_cast<std::uint32_t>(doc.nodes.size());
    doc.nodes.push_back(part_node);

    std::uint32_t prev_child = NO_NODE;
    std::uint32_t first_child = NO_NODE;

    for (std::uint32_t i = 0; i < instances.size(); ++i) {
        const auto& inst = instances[i];

        // Outer <group id="..." transform="...">.
        Node group_node;
        group_node.type = NodeType::Group;
        GroupAttrs ga;
        ga.id = inst.id;
        ga.transform = mat4_to_transform_string(transforms[i]);
        group_node.attrs = ga;
        group_node.parent = part_idx;
        group_node.source = inst.source;
        const auto group_idx = static_cast<std::uint32_t>(doc.nodes.size());
        doc.nodes.push_back(group_node);

        // Bare instance reference inside the group.
        Node inst_node;
        inst_node.type = NodeType::Instance;
        InstanceAttrs ia;
        ia.ref_name = inst.ref;
        ia.param_overrides = inst.param_overrides;
        inst_node.attrs = ia;
        inst_node.parent = group_idx;
        inst_node.source = inst.source;
        const auto inst_idx = static_cast<std::uint32_t>(doc.nodes.size());
        doc.nodes.push_back(inst_node);
        doc.nodes[group_idx].first_child = inst_idx;

        if (first_child == NO_NODE) first_child = group_idx;
        if (prev_child != NO_NODE) {
            doc.nodes[prev_child].next_sibling = group_idx;
        }
        prev_child = group_idx;
    }
    doc.nodes[part_idx].first_child = first_child;

    // 7. Splice the part in place of the assembly.
    const auto parent_idx = doc.nodes[asm_idx].parent;
    const auto following_idx = doc.nodes[asm_idx].next_sibling;
    std::uint32_t synthetic_head = NO_NODE;
    for (std::uint32_t j = 0; j < doc.nodes.size(); ++j) {
        if (doc.nodes[j].parent == NO_NODE && !doc.nodes[j].dead) {
            synthetic_head = j; break;
        }
    }
    std::uint32_t* hp = (parent_idx == NO_NODE) ? &synthetic_head : nullptr;
    const auto anchor = unlink_from_parent(doc, asm_idx, hp);
    splice_after(doc, parent_idx, hp, anchor, part_idx, part_idx, following_idx);

    // Update doc.exports if the assembly was an export.
    if (asm_attrs.name.empty()) {
        // anonymous assembly — replace <anonymous> entry if present
        if (auto it = doc.exports.find("<anonymous>"); it != doc.exports.end()
            && it->second == asm_idx) {
            doc.exports["<anonymous>"] = part_idx;
        }
    } else {
        if (auto it = doc.exports.find(asm_attrs.name); it != doc.exports.end()
            && it->second == asm_idx) {
            doc.exports[asm_attrs.name] = part_idx;
        }
    }

    // Mark the entire original assembly subtree dead — its nodes
    // (instances with at/port, original <connect>s, etc.) have been
    // replaced by the synthesised <part>'s structure.
    for (auto idx : collect_subtree(doc, asm_idx)) {
        doc.nodes[idx].dead = true;
    }
}

}  // namespace

void compile_assemblies(Document& doc,
                         const std::vector<ParamDecl>& entry_params,
                         std::vector<CompileError>& errors_out,
                         std::vector<CompileError>& warnings_out) {
    // Multi-pass to handle nested <assembly> (rare in practice but
    // possible). Process inside-out.
    //
    // **Scope**: only NodeType::Assembly nodes are checked.
    // Imported `.cadml` files whose top-level was an `<assembly>` have
    // been renamed to `NodeType::Def` by `merge_imported_doc` and
    // slip through this loop — a disconnect problem in the imported
    // file is only flagged when the user compiles that file as the
    // entry. Deep-graph checking lands when there's a concrete user
    // need.
    while (true) {
        std::uint32_t target = NO_NODE;
        for (std::uint32_t i = 0; i < doc.nodes.size(); ++i) {
            if (doc.nodes[i].dead) continue;
            if (doc.nodes[i].type != NodeType::Assembly) continue;
            bool has_nested = false;
            for (auto idx : collect_subtree(doc, i)) {
                if (idx != i && !doc.nodes[idx].dead &&
                    doc.nodes[idx].type == NodeType::Assembly) {
                    has_nested = true; break;
                }
            }
            if (!has_nested) { target = i; break; }
        }
        if (target == NO_NODE) break;
        compile_one_assembly(doc, target, entry_params,
                              errors_out, warnings_out);
    }
}

}  // namespace cadml::compile::detail
