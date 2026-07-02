// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/serializer.hpp>

#include <algorithm>
#include <cstdio>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cadml {

namespace {

// ─── XML attribute escaping ──────────────────────────────────────────

std::string xml_escape_attr(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            default:   out.push_back(ch);
        }
    }
    return out;
}

void emit_attr(std::ostream& os, std::string_view name, std::string_view value) {
    if (value.empty()) return;
    os << ' ' << name << "=\"" << xml_escape_attr(value) << '"';
}

void emit_attr_if(std::ostream& os, std::string_view name,
                  std::string_view value, std::string_view default_value) {
    if (value == default_value || value.empty()) return;
    emit_attr(os, name, value);
}

void emit_bool_attr_if(std::ostream& os, std::string_view name, bool value) {
    if (!value) return;
    os << ' ' << name << "=\"true\"";
}

void emit_optional_double_attr(std::ostream& os, std::string_view name,
                                const std::optional<double>& value) {
    if (!value) return;
    os << ' ' << name << "=\""
       << ::cadml::format_double_canonical(*value, 15) << '"';
}

void indent_to(std::ostream& os, int depth, const std::string& unit) {
    for (int i = 0; i < depth; ++i) os << unit;
}

// ─── Frontmatter emission ────────────────────────────────────────────

// SECURITY: escape a quoted-string frontmatter value so injection of
// `\n<part name="x"/>` (or similar) cannot survive parse → serialise
// → reparse. Frontmatter strings are line-oriented; embedded newlines
// or unescaped `"` would either terminate the directive early or
// inject body XML.
std::string frontmatter_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default:   out += c; break;
        }
    }
    return out;
}

// Verify that a frontmatter token (key, alias, name) matches the
// kebab-case identifier grammar (spec §2.8) before serialising. A
// rogue token containing `\n` or `"` would inject directives.
bool is_safe_frontmatter_token(std::string_view s) {
    if (s.empty()) return false;
    if (!(s[0] >= 'a' && s[0] <= 'z')) return false;
    for (char c : s.substr(1)) {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

void emit_frontmatter(std::ostream& os, const Document& doc, bool flat_form) {
    // Settings (always).
    os << "version " << frontmatter_escape(doc.meta.version) << "\n";
    if (doc.meta.units != "mm")
        os << "units " << frontmatter_escape(doc.meta.units) << "\n";
    if (!doc.meta.description.empty()) {
        os << "description \"" << frontmatter_escape(doc.meta.description) << "\"\n";
    }
    if (!doc.meta.tags.empty()) {
        os << "tags \"" << frontmatter_escape(doc.meta.tags) << "\"\n";
    }
    if (!doc.meta.catalogue_version.empty()) {
        os << "catalogue-version "
           << frontmatter_escape(doc.meta.catalogue_version) << "\n";
    }
    if (!doc.meta.interference_tolerance.empty()) {
        os << "interference-tolerance "
           << frontmatter_escape(doc.meta.interference_tolerance) << "\n";
    }

    if (flat_form) {
        // Flat output: imports + params have been compiled away.
        return;
    }

    if (!doc.imports.empty()) {
        os << "\n";
        for (const auto& imp : doc.imports) {
            os << "import \"" << frontmatter_escape(imp.path) << "\"";
            // Emit alias only when it differs from the default-derived
            // (filename-without-extension) form.
            std::string default_alias;
            const auto& p = imp.path;
            std::size_t slash = p.find_last_of('/');
            const std::string base = (slash == std::string::npos)
                ? p : p.substr(slash + 1);
            std::size_t dot = base.find_last_of('.');
            default_alias = (dot == std::string::npos)
                ? base : base.substr(0, dot);
            if (imp.alias != default_alias) {
                // SECURITY: hard-fail rather than emit an alias that
                // doesn't match identifier grammar — re-parse would
                // accept the malformed line and downstream Lua-wrapper
                // splicing has the same injection risk.
                if (!is_safe_frontmatter_token(imp.alias)) {
                    os << " as INVALID_ALIAS_OMITTED";
                } else {
                    os << " as " << imp.alias;
                }
            }
            os << "\n";
        }
    }

    if (!doc.params.empty()) {
        os << "\n";
        for (const auto& p : doc.params) {
            // Param names go through identifier validation; value_expr
            // is a {…}-style expression and gets newline-escaped to
            // keep one-statement-per-line invariant on round-trip.
            const std::string safe_name = is_safe_frontmatter_token(p.name)
                ? p.name : "INVALID_PARAM_NAME";
            os << "param " << safe_name << " = " << frontmatter_escape(p.value_expr);
            if (p.min || p.max) {
                os << " (";
                bool first = true;
                if (p.min) {
                    os << "min=" << ::cadml::format_double_canonical(*p.min, 15);
                    first = false;
                }
                if (p.max) {
                    if (!first) os << ", ";
                    os << "max=" << ::cadml::format_double_canonical(*p.max, 15);
                }
                os << ")";
            }
            os << "\n";
        }
    }
}

// ─── Source map emission (flat form) ─────────────────────────────────

void emit_sources_table(std::ostream& os, const Document& doc,
                         int depth, const std::string& unit) {
    if (doc.source_files.empty()) return;
    indent_to(os, depth, unit);
    os << "<sources>\n";
    for (const auto& sf : doc.source_files) {
        indent_to(os, depth + 1, unit);
        os << "<source id=\"" << sf.id
           << "\" path=\"" << xml_escape_attr(sf.path) << "\"";
        if (!sf.hash.empty()) {
            os << " hash=\"" << sf.hash << "\"";
        }
        os << "/>\n";
    }
    indent_to(os, depth, unit);
    os << "</sources>\n";
}

// ─── Per-element attribute emitters ──────────────────────────────────

void emit_src_attr(std::ostream& os, const SourceRange& src, bool include) {
    if (!include) return;
    if (src.file == NO_FILE) return;
    os << " src=\"" << src.file << ':' << src.line << ':' << src.column << '"';
}

void emit_node(std::ostream& os, const Document& doc, std::uint32_t idx,
               int depth, const SerializeOptions& opts);

void emit_children_or_close(std::ostream& os, const Document& doc,
                             const Node& n, std::uint32_t idx,
                             std::string_view tag,
                             int depth, const SerializeOptions& opts) {
    if (n.first_child == NO_NODE) {
        os << "/>\n";
        return;
    }
    os << ">\n";
    for (const auto& child : doc.children(idx)) {
        const auto child_idx = static_cast<std::uint32_t>(&child - &doc.nodes[0]);
        emit_node(os, doc, child_idx, depth + 1, opts);
    }
    indent_to(os, depth, opts.indent);
    os << "</" << tag << ">\n";
}

void emit_attrs(std::ostream& os, const Node& n) {
    std::visit([&](const auto& a) {
        using A = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<A, PartAttrs>) {
            emit_attr(os, "name",  a.name);
            emit_attr(os, "color", a.color);
        } else if constexpr (std::is_same_v<A, DefAttrs>) {
            emit_attr(os, "name",  a.name);
            emit_attr(os, "color", a.color);
        } else if constexpr (std::is_same_v<A, AssemblyAttrs>) {
            emit_attr(os, "name", a.name);
        } else if constexpr (std::is_same_v<A, ConnectAttrs>) {
            emit_attr(os, "a", a.a);
            emit_attr(os, "b", a.b);
            if (a.allow_interference) {
                emit_attr(os, "allow-interference", "true");
            }
        } else if constexpr (std::is_same_v<A, PortAttrs>) {
            emit_attr(os, "name",     a.name);
            emit_attr(os, "position", a.position_expr);
            emit_attr(os, "normal",   a.normal_expr);
            emit_attr(os, "up",       a.up_expr);
        } else if constexpr (std::is_same_v<A, GroupAttrs>) {
            emit_attr(os, "id",        a.id);
            emit_attr(os, "transform", a.transform);
            emit_attr(os, "color",     a.color);
        } else if constexpr (std::is_same_v<A, ScriptAttrs>) {
            emit_attr_if(os, "lang", a.lang, "lua");
        } else if constexpr (std::is_same_v<A, ForAttrs>) {
            emit_attr(os, "var",    a.var);
            emit_attr(os, "from",   a.from_expr);
            emit_attr(os, "to",     a.to_expr);
            emit_attr(os, "steps",  a.steps_expr);
            emit_attr(os, "values", a.values);
        } else if constexpr (std::is_same_v<A, CircleAttrs>) {
            emit_attr_if(os, "cx", a.cx_expr, "0");
            emit_attr_if(os, "cy", a.cy_expr, "0");
            emit_attr(os, "r",  a.r_expr);
            emit_attr(os, "segments", a.segments_expr);
            emit_attr(os, "fill",   a.fill);
            emit_attr(os, "stroke", a.stroke);
        } else if constexpr (std::is_same_v<A, RectAttrs>) {
            emit_attr_if(os, "x",      a.x_expr, "0");
            emit_attr_if(os, "y",      a.y_expr, "0");
            emit_attr(os, "width",  a.width_expr);
            emit_attr(os, "height", a.height_expr);
            emit_attr_if(os, "rx", a.rx_expr, "0");
            emit_attr(os, "ry",     a.ry_expr);
            emit_attr(os, "fill",   a.fill);
            emit_attr(os, "stroke", a.stroke);
        } else if constexpr (std::is_same_v<A, PathAttrs>) {
            emit_attr(os, "d", a.d);
            emit_attr(os, "fill",   a.fill);
            emit_attr(os, "stroke", a.stroke);
        } else if constexpr (std::is_same_v<A, SketchAttrs>) {
            emit_attr_if(os, "plane",    a.plane,         "xy");
            emit_attr_if(os, "origin",   a.origin_expr,   "0 0 0");
            emit_attr_if(os, "rotation", a.rotation_expr, "0");
            emit_attr(os, "normal",   a.normal_expr);
        } else if constexpr (std::is_same_v<A, ExtrudeAttrs>) {
            emit_attr(os, "height",       a.height_expr);
            emit_attr_if(os, "scale",     a.scale_expr,     "1");
            emit_attr_if(os, "draft",     a.draft_expr,     "0");
            emit_bool_attr_if(os, "symmetric", a.symmetric);
            emit_attr_if(os, "direction", a.direction_expr, "+z");
        } else if constexpr (std::is_same_v<A, RevolveAttrs>) {
            emit_attr(os, "axis",        a.axis);
            emit_attr_if(os, "angle",    a.angle_expr,    "360");
            emit_attr_if(os, "segments", a.segments_expr, "");
        } else if constexpr (std::is_same_v<A, HelixAttrs>) {
            emit_attr(os, "radius",     a.radius_expr);
            emit_attr(os, "pitch",      a.pitch_expr);
            emit_attr(os, "turns",      a.turns_expr);
            emit_attr_if(os, "taper",   a.taper_expr, "0");
            emit_attr_if(os, "direction", a.direction, "ccw");
        } else if constexpr (std::is_same_v<A, StlAttrs>) {
            emit_attr(os, "src",  a.src);
            emit_attr(os, "data", a.data);
            // Only meaningful alongside `data`; omit the default so an
            // authoring `<stl src=...>` round-trips without noise.
            if (!a.data.empty()) emit_attr_if(os, "encoding", a.encoding, "base64");
        } else if constexpr (std::is_same_v<A, FilletAttrs>) {
            emit_attr(os, "radius",     a.radius_expr);
            emit_attr_if(os, "select",  a.select, "all");
        } else if constexpr (std::is_same_v<A, ChamferAttrs>) {
            emit_attr(os, "distance",   a.distance_expr);
            emit_attr_if(os, "angle",   a.angle_expr, "45");
            emit_attr_if(os, "select",  a.select, "all");
        } else if constexpr (std::is_same_v<A, ShellAttrs>) {
            emit_attr(os, "thickness", a.thickness_expr);
            emit_attr(os, "open",      a.open);
        } else if constexpr (std::is_same_v<A, CutAttrs>) {
            emit_attr(os, "face",  a.face);
            emit_attr(os, "type",  a.type);
            emit_attr(os, "angle", a.angle_expr);
            emit_attr(os, "miter", a.miter_expr);
            emit_attr(os, "bevel", a.bevel_expr);
        } else if constexpr (std::is_same_v<A, PatternAttrs>) {
            emit_attr(os, "type",        a.type);
            emit_attr(os, "count",       a.count_expr);
            emit_attr_if(os, "axis",     a.axis, "z");
            emit_attr(os, "spacing",     a.spacing_expr);
            emit_attr_if(os, "angle",    a.angle_expr, "360");
        } else if constexpr (std::is_same_v<A, ParamAttrs>) {
            emit_attr(os, "name",  a.name);
            emit_attr(os, "value", a.value_expr);
            emit_optional_double_attr(os, "min", a.min);
            emit_optional_double_attr(os, "max", a.max);
        } else if constexpr (std::is_same_v<A, SourceAttrs>) {
            os << " id=\"" << a.id << '"';
            emit_attr(os, "path", a.path);
            emit_attr(os, "hash", a.hash);
        } else if constexpr (std::is_same_v<A, InstanceAttrs>) {
            // Reserved structural attrs first.
            emit_attr(os, "id",   a.id);
            emit_attr(os, "at",   a.at);
            emit_attr(os, "port", a.port);
            // Param overrides (emitted in alphabetical order for stability).
            std::vector<std::pair<std::string, std::string>> sorted(
                a.param_overrides.begin(), a.param_overrides.end());
            std::sort(sorted.begin(), sorted.end());
            for (const auto& [k, v] : sorted) emit_attr(os, k, v);
        } else if constexpr (std::is_same_v<A, UnknownAttrs>) {
            for (const auto& [k, v] : a.raw_attrs) emit_attr(os, k, v);
        }
        // SweepAttrs / LoftAttrs / UnionAttrs / DifferenceAttrs /
        // IntersectAttrs / HullAttrs / SvgAttrs / SourcesAttrs have
        // no attributes.
        (void)a;
    }, n.attrs);
}

std::string_view tag_for(const Node& n) {
    if (n.type == NodeType::Instance) {
        return std::get<InstanceAttrs>(n.attrs).ref_name;
    }
    if (n.type == NodeType::Unknown) {
        return std::get<UnknownAttrs>(n.attrs).raw_tag_name;
    }
    return builtin_name_from_node_type(n.type);
}

void emit_node(std::ostream& os, const Document& doc, std::uint32_t idx,
               int depth, const SerializeOptions& opts) {
    const auto& n = doc.nodes[idx];
    const auto tag = tag_for(n);

    indent_to(os, depth, opts.indent);
    os << '<' << tag;
    emit_attrs(os, n);
    emit_src_attr(os, n.source, opts.include_source_map);

    // Iteration tag from <for>/<pattern> unrolling — useful for tools.
    if (n.iteration != UINT32_MAX) {
        os << " iteration=\"" << n.iteration << "\"";
    }

    // <script> needs special-case body (CDATA-wrapped if it contains <).
    if (n.type == NodeType::Script) {
        const auto& sa = std::get<ScriptAttrs>(n.attrs);
        if (sa.source.empty()) {
            os << "/>\n";
            return;
        }
        // SECURITY: split `]]>` occurrences across two CDATA sections
        // per the standard XML pattern. An unsplit `]]>` in the source
        // would terminate the CDATA early, producing malformed XML
        // that a re-parse would either reject or — worse — silently
        // accept with the trailing content as element body.
        std::string_view src(sa.source);
        os << "><![CDATA[";
        std::size_t i = 0;
        while (i < src.size()) {
            const auto next = src.find("]]>", i);
            if (next == std::string_view::npos) {
                os << src.substr(i);
                break;
            }
            os << src.substr(i, next - i) << "]]]]><![CDATA[>";
            i = next + 3;
        }
        os << "]]></" << tag << ">\n";
        return;
    }

    emit_children_or_close(os, doc, n, idx, tag, depth, opts);
}

}  // namespace

std::string serialize(const Document& doc, const SerializeOptions& opts) {
    std::ostringstream out;
    emit_frontmatter(out, doc, opts.include_source_map);

    // Body — top-level siblings (parent == NO_NODE).
    bool any_body = false;
    if (opts.include_source_map && !doc.source_files.empty()) {
        out << "\n";
        emit_sources_table(out, doc, /*depth=*/0, opts.indent);
        any_body = true;
    }

    for (std::uint32_t i = 0; i < doc.nodes.size(); ++i) {
        if (doc.nodes[i].parent != NO_NODE) continue;
        if (doc.nodes[i].dead) continue;
        if (!any_body) out << "\n";
        any_body = true;
        emit_node(out, doc, i, /*depth=*/0, opts);
    }

    return out.str();
}

}  // namespace cadml
