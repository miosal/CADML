// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/parser.hpp>

#include <pugixml.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace cadml {

namespace {

// ─── Source location ─────────────────────────────────────────────────────

struct LineCol {
    std::uint32_t line = 1;
    std::uint32_t column = 1;
};

// Precomputed line-start offsets for O(log N) line-col lookup. Built once
// per body parse; used by every node's source-range computation.
struct LineIndex {
    std::vector<std::uint32_t> line_starts;  // byte offset of each line start
    std::uint32_t              base_line = 1; // line number for line_starts[0]

    static LineIndex build(std::string_view body, std::uint32_t base) {
        LineIndex idx;
        idx.base_line = base;
        idx.line_starts.reserve(body.size() / 40 + 1);
        idx.line_starts.push_back(0);
        for (std::size_t i = 0; i < body.size(); ++i) {
            if (body[i] == '\n') {
                idx.line_starts.push_back(static_cast<std::uint32_t>(i + 1));
            }
        }
        return idx;
    }

    LineCol locate(std::ptrdiff_t offset) const {
        if (offset < 0) return { base_line, 1 };
        // Binary-search for the largest line-start ≤ offset.
        const auto target = static_cast<std::uint32_t>(offset);
        auto it = std::upper_bound(line_starts.begin(), line_starts.end(), target);
        if (it == line_starts.begin()) return { base_line, 1 };
        --it;
        const auto line_idx = static_cast<std::uint32_t>(it - line_starts.begin());
        return { base_line + line_idx,
                 static_cast<std::uint32_t>(target - *it + 1) };
    }
};

// Construct a SourceRange for a pugi::xml_node. pugixml's `offset_debug`
// returns the byte offset into the wrapped source; subtract the synthetic-
// root prefix length to get the offset into the user's body text.
SourceRange make_range(const pugi::xml_node& n,
                        const LineIndex& idx,
                        std::size_t prefix_len,
                        SourceFileId file_id) {
    SourceRange r;
    r.file = file_id;
    const auto raw_offset = n.offset_debug();
    const auto rel = (raw_offset < (std::ptrdiff_t)prefix_len)
                         ? std::ptrdiff_t{0}
                         : raw_offset - (std::ptrdiff_t)prefix_len;
    const auto lc = idx.locate(rel);
    r.line = lc.line;
    r.column = lc.column;
    return r;
}

// ─── Attribute helpers ───────────────────────────────────────────────────

// Pull an attribute value as std::string (empty if absent).
std::string attr(const pugi::xml_node& n, const char* name) {
    return n.attribute(name).value();
}

// Pull an attribute, applying a default if absent.
std::string attr_or(const pugi::xml_node& n, const char* name,
                     std::string_view default_value) {
    auto v = n.attribute(name);
    if (!v) return std::string(default_value);
    return v.value();
}

// Pull a boolean attribute. Accepts true/false (lowercase).
bool attr_bool(const pugi::xml_node& n, const char* name, bool default_value) {
    auto v = n.attribute(name);
    if (!v) return default_value;
    std::string s = v.value();
    return s == "true" || s == "1";
}

// Set of reserved attribute names on instance elements (Section 6.7).
// `src` is structural metadata (the bundler's source-map back-pointer);
// without it on this list a re-parsed `.fcadml` treats `src="0:5:2"`
// as a user-authored param override and validation rejects every
// instance whose def doesn't declare a `src` param. That broke
// cadml_compile's documented idempotence on flat input (spec §10.7).
bool is_reserved_instance_attr(std::string_view name) {
    return name == "id" || name == "at" || name == "port" || name == "src";
}

// ─── Per-element attribute extraction ────────────────────────────────────

NodeAttrs build_part_attrs(const pugi::xml_node& n) {
    PartAttrs a;
    a.name  = attr(n, "name");
    a.color = attr(n, "color");
    return a;
}

NodeAttrs build_def_attrs(const pugi::xml_node& n) {
    DefAttrs a;
    a.name  = attr(n, "name");
    a.color = attr(n, "color");
    return a;
}

NodeAttrs build_assembly_attrs(const pugi::xml_node& n) {
    AssemblyAttrs a;
    a.name = attr(n, "name");
    return a;
}

NodeAttrs build_connect_attrs(const pugi::xml_node& n) {
    ConnectAttrs a;
    a.a = attr(n, "a");
    a.b = attr(n, "b");
    // `allow-interference="true"` opts the connected pair out of the
    // `cadml_check` interference report. Any value other than "true"
    // (including missing) leaves the default `false`. Other booleans
    // (`yes`, `1`) are intentionally NOT accepted — keep the spec
    // surface narrow.
    a.allow_interference = (attr(n, "allow-interference") == "true");
    return a;
}

NodeAttrs build_port_attrs(const pugi::xml_node& n) {
    PortAttrs a;
    a.name           = attr(n, "name");
    a.position_expr  = attr(n, "position");
    a.normal_expr    = attr(n, "normal");
    a.up_expr        = attr(n, "up");
    return a;
}

NodeAttrs build_group_attrs(const pugi::xml_node& n) {
    GroupAttrs a;
    a.id        = attr(n, "id");
    a.transform = attr(n, "transform");
    a.color     = attr(n, "color");
    return a;
}

NodeAttrs build_script_attrs(const pugi::xml_node& n) {
    ScriptAttrs a;
    a.lang   = attr_or(n, "lang", "lua");
    a.source = n.text().get();
    return a;
}

NodeAttrs build_for_attrs(const pugi::xml_node& n) {
    ForAttrs a;
    a.var         = attr(n, "var");
    a.from_expr   = attr(n, "from");
    a.to_expr     = attr(n, "to");
    a.steps_expr  = attr(n, "steps");
    a.values      = attr(n, "values");
    return a;
}

// Pull SVG-style fill / stroke into the FillStrokeAttrs base. We
// keep the raw string verbatim — colour-name → "#rrggbb" canonicalisation
// happens later in the engine where the central colour parser lives.
template <class A>
void read_fill_stroke(A& a, const pugi::xml_node& n) {
    a.fill   = attr(n, "fill");
    a.stroke = attr(n, "stroke");
}

NodeAttrs build_circle_attrs(const pugi::xml_node& n) {
    CircleAttrs a;
    a.cx_expr       = attr_or(n, "cx", "0");
    a.cy_expr       = attr_or(n, "cy", "0");
    a.r_expr        = attr(n, "r");
    a.segments_expr = attr(n, "segments");
    read_fill_stroke(a, n);
    return a;
}

NodeAttrs build_rect_attrs(const pugi::xml_node& n) {
    RectAttrs a;
    a.x_expr      = attr_or(n, "x", "0");
    a.y_expr      = attr_or(n, "y", "0");
    a.width_expr  = attr(n, "width");
    a.height_expr = attr(n, "height");
    a.rx_expr     = attr_or(n, "rx", "0");
    a.ry_expr     = attr(n, "ry");  // empty defaults to rx
    read_fill_stroke(a, n);
    return a;
}

NodeAttrs build_path_attrs(const pugi::xml_node& n) {
    PathAttrs a;
    a.d = attr(n, "d");
    read_fill_stroke(a, n);
    return a;
}

NodeAttrs build_sketch_attrs(const pugi::xml_node& n) {
    SketchAttrs a;
    a.plane         = attr_or(n, "plane",    "xy");
    a.origin_expr   = attr_or(n, "origin",   "0 0 0");
    a.rotation_expr = attr_or(n, "rotation", "0");
    a.normal_expr   = attr(n, "normal");
    return a;
}

NodeAttrs build_extrude_attrs(const pugi::xml_node& n) {
    ExtrudeAttrs a;
    a.height_expr    = attr(n, "height");
    a.scale_expr     = attr_or(n, "scale", "1");
    a.draft_expr     = attr_or(n, "draft", "0");
    a.symmetric      = attr_bool(n, "symmetric", false);
    a.direction_expr = attr_or(n, "direction", "+z");
    return a;
}

NodeAttrs build_revolve_attrs(const pugi::xml_node& n) {
    RevolveAttrs a;
    a.axis          = attr(n, "axis");
    a.angle_expr    = attr_or(n, "angle", "360");
    a.segments_expr = attr_or(n, "segments", "");
    return a;
}

NodeAttrs build_helix_attrs(const pugi::xml_node& n) {
    HelixAttrs a;
    a.radius_expr = attr(n, "radius");
    a.pitch_expr  = attr(n, "pitch");
    a.turns_expr  = attr(n, "turns");
    a.taper_expr  = attr_or(n, "taper", "0");
    a.direction   = attr_or(n, "direction", "ccw");
    return a;
}

NodeAttrs build_fillet_attrs(const pugi::xml_node& n) {
    FilletAttrs a;
    a.radius_expr = attr(n, "radius");
    a.select      = attr_or(n, "select", "all");
    return a;
}

NodeAttrs build_chamfer_attrs(const pugi::xml_node& n) {
    ChamferAttrs a;
    a.distance_expr = attr(n, "distance");
    a.angle_expr    = attr_or(n, "angle", "45");
    a.select        = attr_or(n, "select", "all");
    return a;
}

NodeAttrs build_shell_attrs(const pugi::xml_node& n) {
    ShellAttrs a;
    a.thickness_expr = attr(n, "thickness");
    a.open           = attr(n, "open");
    return a;
}

NodeAttrs build_cut_attrs(const pugi::xml_node& n) {
    CutAttrs a;
    a.face       = attr(n, "face");
    a.type       = attr(n, "type");
    a.angle_expr = attr(n, "angle");
    a.miter_expr = attr(n, "miter");
    a.bevel_expr = attr(n, "bevel");
    return a;
}

NodeAttrs build_pattern_attrs(const pugi::xml_node& n) {
    PatternAttrs a;
    a.type         = attr(n, "type");
    a.count_expr   = attr(n, "count");
    a.axis         = attr_or(n, "axis", "z");
    a.spacing_expr = attr(n, "spacing");
    a.angle_expr   = attr_or(n, "angle", "360");
    return a;
}

NodeAttrs build_param_attrs(const pugi::xml_node& n) {
    ParamAttrs a;
    a.name       = attr(n, "name");
    a.value_expr = attr(n, "value");
    // min/max are strict numeric literals — bad input is left empty
    // here and surfaces as a Schema error in parser_body's caller
    // (build_node_attrs validates the result).
    auto min_attr = n.attribute("min");
    auto max_attr = n.attribute("max");
    if (min_attr) a.min = parse_double_strict(min_attr.value());
    if (max_attr) a.max = parse_double_strict(max_attr.value());
    return a;
}

NodeAttrs build_sources_attrs(const pugi::xml_node&) { return SourcesAttrs{}; }

NodeAttrs build_source_attrs(const pugi::xml_node& n) {
    SourceAttrs a;
    a.id   = (std::uint32_t)std::strtoul(attr(n, "id").c_str(), nullptr, 10);
    a.path = attr(n, "path");
    a.hash = attr(n, "hash");
    return a;
}

NodeAttrs build_instance_attrs(const pugi::xml_node& n) {
    InstanceAttrs a;
    a.ref_name = n.name();
    a.id       = attr(n, "id");
    a.at       = attr(n, "at");
    a.port     = attr(n, "port");
    for (auto& ar : n.attributes()) {
        const std::string name = ar.name();
        if (is_reserved_instance_attr(name)) continue;
        a.param_overrides.emplace(name, ar.value());
    }
    return a;
}

NodeAttrs build_unknown_attrs(const pugi::xml_node& n) {
    UnknownAttrs a;
    a.raw_tag_name = n.name();
    for (auto& ar : n.attributes()) {
        a.raw_attrs.emplace_back(ar.name(), ar.value());
    }
    return a;
}

// Dispatch element name → NodeType + NodeAttrs.
struct TypedAttrs {
    NodeType  type;
    NodeAttrs attrs;
};

TypedAttrs classify(const pugi::xml_node& n) {
    const std::string_view name = n.name();
    const NodeType bt = node_type_from_builtin_name(name);

    if (bt == NodeType::Unknown) {
        // Identifier-like names are instance references; anything else
        // is an unknown element (parser-level error follows).
        if (!name.empty() &&
            ((name[0] >= 'a' && name[0] <= 'z'))) {
            return { NodeType::Instance, build_instance_attrs(n) };
        }
        return { NodeType::Unknown, build_unknown_attrs(n) };
    }

    switch (bt) {
        case NodeType::Part:       return { bt, build_part_attrs(n) };
        case NodeType::Def:        return { bt, build_def_attrs(n) };
        case NodeType::Assembly:   return { bt, build_assembly_attrs(n) };
        case NodeType::Connect:    return { bt, build_connect_attrs(n) };
        case NodeType::Port:       return { bt, build_port_attrs(n) };
        case NodeType::Group:      return { bt, build_group_attrs(n) };
        case NodeType::Script:     return { bt, build_script_attrs(n) };
        case NodeType::For:        return { bt, build_for_attrs(n) };

        case NodeType::Circle:     return { bt, build_circle_attrs(n) };
        case NodeType::Rect:       return { bt, build_rect_attrs(n) };
        case NodeType::Path:       return { bt, build_path_attrs(n) };
        case NodeType::Sketch:     return { bt, build_sketch_attrs(n) };

        case NodeType::Extrude:    return { bt, build_extrude_attrs(n) };
        case NodeType::Revolve:    return { bt, build_revolve_attrs(n) };
        case NodeType::Sweep:      return { bt, SweepAttrs{} };
        case NodeType::Loft:       return { bt, LoftAttrs{} };
        case NodeType::Helix:      return { bt, build_helix_attrs(n) };

        case NodeType::Union:      return { bt, UnionAttrs{} };
        case NodeType::Difference: return { bt, DifferenceAttrs{} };
        case NodeType::Intersect:  return { bt, IntersectAttrs{} };
        case NodeType::Hull:       return { bt, HullAttrs{} };
        case NodeType::Svg:        return { bt, SvgAttrs{} };

        case NodeType::Fillet:     return { bt, build_fillet_attrs(n) };
        case NodeType::Chamfer:    return { bt, build_chamfer_attrs(n) };
        case NodeType::Shell:      return { bt, build_shell_attrs(n) };
        case NodeType::Cut:        return { bt, build_cut_attrs(n) };
        case NodeType::Pattern:    return { bt, build_pattern_attrs(n) };

        case NodeType::Param:      return { bt, build_param_attrs(n) };
        case NodeType::Sources:    return { bt, build_sources_attrs(n) };
        case NodeType::Source:     return { bt, build_source_attrs(n) };

        case NodeType::Instance:
        case NodeType::Unknown:
            // Should not reach here — handled above.
            break;
    }
    return { NodeType::Unknown, build_unknown_attrs(n) };
}

// ─── Recursive walker ────────────────────────────────────────────────────

struct Walker {
    BodyResult&            result;
    const LineIndex&       line_index;
    std::size_t            prefix_len;
    SourceFileId           file_id;

    // SECURITY: cap XML nesting depth to prevent stack overflow on
    // pathological inputs (e.g. 100k nested `<group>` elements). 256
    // is well above any legitimate authoring depth.
    static constexpr int kMaxDepth = 256;
    int                    depth = 0;

    void push_error(ParseError::Category cat, std::string msg, SourceRange r) {
        ParseError e;
        e.category = cat;
        e.message = std::move(msg);
        e.source = r;
        result.errors.push_back(std::move(e));
    }

    // Walk `xml_node` and append a Node for it (and recurse for children).
    // Returns the index of the newly-pushed node, or NO_NODE if `xml_node`
    // shouldn't produce a node (text, comment, etc.).
    std::uint32_t walk(const pugi::xml_node& xml_node, std::uint32_t parent) {
        if (xml_node.type() != pugi::node_element) return NO_NODE;
        if (depth >= kMaxDepth) {
            const auto range = make_range(xml_node, line_index,
                                            prefix_len, file_id);
            push_error(ParseError::Schema,
                "XML element nesting exceeds " + std::to_string(kMaxDepth) +
                " levels; refusing to recurse further",
                range);
            return NO_NODE;
        }
        struct DepthGuard {
            int& d;
            explicit DepthGuard(int& depth) : d(depth) { ++d; }
            ~DepthGuard() { --d; }
        } guard(depth);

        auto range = make_range(xml_node, line_index, prefix_len, file_id);

        // Round-trip support: if the input already has a src=
        // back-reference (it does on every .fcadml the bundler emits),
        // honour it instead of the XML node's physical position.
        // Without this, recompiling a .fcadml silently rewrites every
        // src= to point at the .fcadml's own lines, losing the trail
        // back to the user-authored source.
        if (auto src_attr = xml_node.attribute("src")) {
            unsigned file = 0, line = 0, col = 0;
            const int parsed = std::sscanf(src_attr.value(), "%u:%u:%u",
                                            &file, &line, &col);
            if (parsed >= 2) {
                range.file   = static_cast<SourceFileId>(file);
                range.line   = static_cast<std::uint32_t>(line);
                range.column = static_cast<std::uint32_t>(parsed >= 3 ? col : 0);
            }
        }

        auto typed = classify(xml_node);

        if (typed.type == NodeType::Unknown) {
            push_error(ParseError::Vocabulary,
                "unknown element <" + std::string(xml_node.name()) + ">",
                range);
        }

        Node node;
        node.type = typed.type;
        node.parent = parent;
        node.attrs = std::move(typed.attrs);
        node.source = range;

        const auto idx = (std::uint32_t)result.nodes.size();
        result.nodes.push_back(std::move(node));

        // <param min=… max=…> values are strict numeric literals.
        // Detect "attribute present but unparseable" here, because
        // build_param_attrs silently drops bad input into nullopt.
        if (typed.type == NodeType::Param) {
            auto reject_bad = [&](const char* aname) {
                auto a = xml_node.attribute(aname);
                if (!a) return;
                if (parse_double_strict(a.value())) return;
                push_error(ParseError::Schema,
                    std::string("<param name=\"") +
                        xml_node.attribute("name").value() + "\" " +
                        aname + "=\"" + a.value() +
                        "\"> is not a valid number",
                    range);
            };
            reject_bad("min");
            reject_bad("max");
        }

        // Track def + export indices.
        if (typed.type == NodeType::Def) {
            const auto& da = std::get<DefAttrs>(result.nodes[idx].attrs);
            if (da.name.empty()) {
                push_error(ParseError::Schema,
                    "<def> requires a `name` attribute", range);
            } else if (node_type_from_builtin_name(da.name) != NodeType::Unknown) {
                push_error(ParseError::Vocabulary,
                    "<def name=\"" + da.name + "\"> collides with a built-in"
                    " element name", range);
            } else if (result.defs.count(da.name)) {
                push_error(ParseError::Vocabulary,
                    "duplicate <def name=\"" + da.name + "\"> in this file", range);
            } else {
                result.defs[da.name] = idx;
            }
        }

        // Top-level <part>/<assembly> exports tracked when parent is the
        // synthetic root (parent index NO_NODE).
        if (parent == NO_NODE &&
            (typed.type == NodeType::Part || typed.type == NodeType::Assembly)) {
            std::string export_name;
            if (typed.type == NodeType::Part) {
                export_name = std::get<PartAttrs>(result.nodes[idx].attrs).name;
            } else {
                export_name = std::get<AssemblyAttrs>(result.nodes[idx].attrs).name;
            }
            // Empty name is OK at parse time; the bundler fills in
            // filename-based default if needed.
            if (!export_name.empty()) {
                if (result.exports.count(export_name)) {
                    push_error(ParseError::Vocabulary,
                        "duplicate top-level export `" + export_name + "`",
                        range);
                } else {
                    result.exports[export_name] = idx;
                }
            } else {
                // Use a sentinel key so the compiler can still find it.
                static const std::string kAnonymous{"<anonymous>"};
                result.exports[kAnonymous] = idx;
            }
        }

        // Recurse for element children.
        std::uint32_t prev_sibling = NO_NODE;
        std::uint32_t first_child = NO_NODE;

        for (auto child = xml_node.first_child(); child;
             child = child.next_sibling()) {
            if (child.type() != pugi::node_element) continue;
            const auto child_idx = walk(child, idx);
            if (child_idx == NO_NODE) continue;
            if (first_child == NO_NODE) first_child = child_idx;
            if (prev_sibling != NO_NODE) {
                result.nodes[prev_sibling].next_sibling = child_idx;
            }
            prev_sibling = child_idx;
        }
        result.nodes[idx].first_child = first_child;
        return idx;
    }
};

// Recursively search the parsed tree for an XML processing instruction,
// XML declaration, or DOCTYPE node. pugixml is configured to MATERIALISE
// these (parse_pi | parse_doctype | parse_declaration) precisely so they
// can be rejected here. Doing it structurally — rather than scanning the
// raw byte buffer for `<?` / `<!DOCTYPE` — catches them at ANY depth and
// ANY offset (closing the old 16 KiB-prefix gap), and never false-
// positives on those character sequences appearing inside CDATA, text,
// or attribute values. Returns a human-readable description of the first
// offending node, or nullptr if the tree is clean.
const char* find_forbidden_xml_node(const pugi::xml_node& root) {
    // SECURITY: iterative (explicit-stack) DFS, NOT recursion. pugixml's
    // own parser is iterative and happily materialises a tree tens of
    // thousands of nodes deep; a recursive scan here would overflow the
    // call stack on such input — and this runs *before* the Walker's
    // depth cap, so that cap can't save us. The work stack lives on the
    // heap, so depth is bounded by memory, not the call stack. (Anything
    // deeper than the Walker's 256-level cap is rejected by the walker
    // anyway; we still scan the whole tree so a forbidden node at any
    // depth is reported with the same message regardless of nesting.)
    std::vector<pugi::xml_node> stack;
    for (auto c = root.first_child(); c; c = c.next_sibling()) {
        stack.push_back(c);
    }
    while (!stack.empty()) {
        const pugi::xml_node n = stack.back();
        stack.pop_back();
        switch (n.type()) {
            case pugi::node_pi:
                return "processing instruction (`<?…?>`)";
            case pugi::node_declaration:
                return "declaration (`<?xml …?>`)";
            case pugi::node_doctype:
                return "DOCTYPE declaration (`<!DOCTYPE …>`)";
            default:
                break;
        }
        for (auto c = n.first_child(); c; c = c.next_sibling()) {
            stack.push_back(c);
        }
    }
    return nullptr;
}

}  // namespace

// ─── Public entry ───────────────────────────────────────────────────────

BodyResult parse_body(std::string_view body, SourceFileId file_id,
                       std::uint32_t body_line_offset) {
    BodyResult out;

    if (body.empty() || std::all_of(body.begin(), body.end(),
                                     [](char c) { return std::isspace(static_cast<unsigned char>(c)); })) {
        // Empty body — caller decides whether that's an error (via the
        // 1:1 file rule check, which lives at the document level).
        return out;
    }

    // Wrap in synthetic root.
    static constexpr std::string_view kPrefix = "<__cadml_root__>";
    static constexpr std::string_view kSuffix = "</__cadml_root__>";
    std::string wrapped;
    wrapped.reserve(kPrefix.size() + body.size() + kSuffix.size());
    wrapped.append(kPrefix);
    wrapped.append(body);
    wrapped.append(kSuffix);

    pugi::xml_document doc;
    // parse_pi | parse_doctype | parse_declaration: keep these nodes in
    // the tree (pugixml strips them silently by default) so the body can
    // be rejected if it contains any — see find_forbidden_xml_node and
    // spec §2.3. This replaces the old first-16-KiB byte-prefix scan,
    // which missed PIs/DOCTYPEs deeper in the body.
    auto load_result = doc.load_buffer(
        wrapped.data(), wrapped.size(),
        pugi::parse_default | pugi::parse_ws_pcdata
            | pugi::parse_pi | pugi::parse_doctype | pugi::parse_declaration);

    const auto line_index = LineIndex::build(body, body_line_offset);

    if (!load_result) {
        ParseError e;
        e.category = ParseError::Parse;
        // The parse_pi / parse_doctype flags make pugixml *attempt* the
        // reserved `<?xml …?>` declaration and `<!DOCTYPE …>` forms and
        // fail at load time (a generic PI like `<?foo?>` parses fine and
        // is caught structurally below instead). Map those two statuses
        // to the CADML-specific spec §2.3 wording rather than surfacing
        // pugixml's generic text.
        if (load_result.status == pugi::status_bad_pi) {
            e.message = "XML processing instructions (`<?…?>`) are not "
                        "permitted in CADML 0.1";
        } else if (load_result.status == pugi::status_bad_doctype) {
            e.message = "XML DOCTYPE declarations are not permitted in "
                        "CADML 0.1";
        } else {
            e.message = std::string("XML parse error: ")
                      + load_result.description();
        }
        const auto raw = (std::ptrdiff_t)load_result.offset;
        const auto rel = (raw < (std::ptrdiff_t)kPrefix.size())
                             ? std::ptrdiff_t{0}
                             : raw - (std::ptrdiff_t)kPrefix.size();
        const auto lc = line_index.locate(rel);
        e.source.file = file_id;
        e.source.line = lc.line;
        e.source.column = lc.column;
        out.errors.push_back(std::move(e));
        return out;
    }

    auto root = doc.child("__cadml_root__");
    if (!root) {
        ParseError e;
        e.category = ParseError::Parse;
        e.message  = "internal: synthetic root not found";
        out.errors.push_back(std::move(e));
        return out;
    }

    // SECURITY (spec §2.3): reject XML processing instructions, the XML
    // declaration, and DOCTYPE anywhere in the body. They were
    // materialised by the parse_* flags above; find the first one and
    // error out before building any nodes.
    if (const char* forbidden = find_forbidden_xml_node(doc)) {
        ParseError e;
        e.category = ParseError::Parse;
        e.message  = std::string("XML ") + forbidden +
                     " is not permitted in CADML 0.1";
        e.source.file = file_id;
        out.errors.push_back(std::move(e));
        return out;
    }

    Walker walker{ out, line_index, kPrefix.size(), file_id };

    // Walk top-level children.
    std::uint32_t prev_sibling = NO_NODE;
    for (auto child = root.first_child(); child; child = child.next_sibling()) {
        if (child.type() != pugi::node_element) continue;
        const auto idx = walker.walk(child, NO_NODE);
        if (idx == NO_NODE) continue;
        if (prev_sibling != NO_NODE) {
            out.nodes[prev_sibling].next_sibling = idx;
        }
        prev_sibling = idx;
    }

    return out;
}

// ─── parse() — frontmatter + body ────────────────────────────────────────

ParseResult parse(std::string_view source, SourceFileId file_id) {
    ParseResult result;

    // SECURITY: cap source-buffer size before invoking the rest of the
    // pipeline. pugixml allocates a parse tree proportional to input
    // size; without a cap, a malicious multi-GiB input exhausts
    // memory. 64 MiB is much larger than any legitimate document.
    constexpr std::size_t kMaxSourceBytes = 64ull * 1024 * 1024;
    if (source.size() > kMaxSourceBytes) {
        ParseError e;
        e.category = ParseError::Parse;
        e.message = "source too large (" + std::to_string(source.size()) +
                    " bytes; limit " + std::to_string(kMaxSourceBytes) + ")";
        result.errors.push_back(std::move(e));
        return result;
    }

    // NOTE: XML processing-instruction / declaration / DOCTYPE rejection
    // (spec §2.3) is now done structurally in parse_body() via
    // find_forbidden_xml_node — it catches them at any depth and offset,
    // unlike the old first-16-KiB byte-prefix scan that lived here.

    auto fm = parse_frontmatter(source, file_id);
    const bool version_explicitly_set = fm.version_explicitly_set;
    const bool fm_has_content =
        !fm.imports.empty() || !fm.params.empty() ||
        !fm.meta.description.empty() || !fm.meta.tags.empty() ||
        !fm.meta.catalogue_version.empty() ||
        !fm.meta.interference_tolerance.empty() ||
        version_explicitly_set ||
        fm.meta.units != "mm";  // any non-default settings

    result.document.meta    = std::move(fm.meta);
    result.document.imports = std::move(fm.imports);
    result.document.params  = std::move(fm.params);
    for (auto& e : fm.errors)   result.errors.push_back(std::move(e));
    for (auto& w : fm.warnings) result.warnings.push_back(std::move(w));

    // Compute body line offset — frontmatter's last consumed line.
    std::uint32_t body_line = 1;
    for (std::size_t i = 0; i < fm.body_offset && i < source.size(); ++i) {
        if (source[i] == '\n') ++body_line;
    }

    bool body_has_content = false;
    if (fm.body_offset < source.size()) {
        const auto body_view = source.substr(fm.body_offset);
        for (char ch : body_view) {
            if (!std::isspace(static_cast<unsigned char>(ch))) {
                body_has_content = true;
                break;
            }
        }
        auto body = parse_body(body_view, file_id, body_line);
        result.document.nodes   = std::move(body.nodes);
        result.document.defs    = std::move(body.defs);
        result.document.exports = std::move(body.exports);
        for (auto& e : body.errors)   result.errors.push_back(std::move(e));
        for (auto& w : body.warnings) result.warnings.push_back(std::move(w));
    }

    // Spec §3.3: `version` is required when the file has any content.
    // Empty / whitespace-only files are exempt (incremental editing).
    if (!version_explicitly_set && (fm_has_content || body_has_content)) {
        ParseError e;
        e.category = ParseError::Parse;
        e.message  = "missing required `version` setting in frontmatter";
        e.source.file = file_id;
        e.source.line = 1;
        e.source.column = 1;
        result.errors.push_back(std::move(e));
    }

    // Round-trip support: if the input has a top-level <sources> block
    // (every .fcadml emitted by the bundler does), extract it into
    // Document.source_files and mark the nodes dead so they don't
    // re-appear in the body on re-emit. Without this, recompiling a
    // .fcadml accumulates a duplicate <sources> block on every cycle.
    for (std::uint32_t i = 0; i < result.document.nodes.size(); ++i) {
        auto& n = result.document.nodes[i];
        if (n.parent != NO_NODE) continue;
        if (n.type != NodeType::Sources) continue;
        if (n.dead) continue;

        for (std::uint32_t c = n.first_child; c != NO_NODE;
             c = result.document.nodes[c].next_sibling) {
            if (result.document.nodes[c].type != NodeType::Source) continue;
            const auto& sa =
                std::get<SourceAttrs>(result.document.nodes[c].attrs);
            SourceFile sf;
            sf.id   = sa.id;
            sf.path = sa.path;
            sf.hash = sa.hash;
            if (sa.id >= result.document.source_files.size()) {
                result.document.source_files.resize(sa.id + 1);
            }
            result.document.source_files[sa.id] = sf;
            result.document.nodes[c].dead = true;
        }
        n.dead = true;
    }

    return result;
}

ParseResult parse_file(const std::filesystem::path& path) {
    ParseResult result;
    auto push_io_error = [&](std::string msg) {
        ParseError e;
        e.category = ParseError::Parse;
        e.message  = std::move(msg);
        result.errors.push_back(std::move(e));
    };

    // SECURITY: refuse non-regular files (pipes, devices, directories) —
    // fseek-based sizing returns -1 on non-seekable streams, which casts
    // to SIZE_MAX and aborts the subsequent string allocation.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        push_io_error("cannot open file (not a regular file): " + path.string());
        return result;
    }

    // SECURITY: cap input size. 64 MiB is comfortably larger than any
    // legitimate .cadml/.fcadml but small enough that a malicious
    // multi-GiB input does not exhaust memory before we even start
    // parsing.
    constexpr std::uintmax_t kMaxFileBytes = 64ull * 1024 * 1024;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        push_io_error("cannot stat file: " + path.string());
        return result;
    }
    if (file_size > kMaxFileBytes) {
        push_io_error("file too large (" + std::to_string(file_size) +
                      " bytes; limit " + std::to_string(kMaxFileBytes) +
                      "): " + path.string());
        return result;
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        push_io_error("cannot open file: " + path.string());
        return result;
    }
    std::string buf;
    buf.resize(static_cast<std::size_t>(file_size));
    if (file_size > 0) {
        f.read(buf.data(), static_cast<std::streamsize>(file_size));
        if (f.bad() || (f.gcount() != static_cast<std::streamsize>(file_size) &&
                        !f.eof())) {
            push_io_error("short read on file: " + path.string());
            return result;
        }
        // Trim to actual bytes read (handles eof + partial last read).
        buf.resize(static_cast<std::size_t>(f.gcount()));
    }

    result = parse(buf, /*file_id=*/0);

    // Register the entry file at source_files[0]. Path is the filename
    // only; the bundler will rewrite to a relative-to-entry-directory path
    // and compute the hash when it has the full project context.
    //
    // Skip when parse() has already populated source_files from a top-
    // level <sources> block (i.e. the input is a .fcadml round-trip).
    // Overwriting in that case would clobber the original source-file
    // table and break the src= back-references emitted into the body.
    if (result.document.source_files.empty()) {
        SourceFile sf;
        sf.id = 0;
        sf.path = path.filename().string();
        result.document.source_files = { sf };
    }
    return result;
}

}  // namespace cadml
