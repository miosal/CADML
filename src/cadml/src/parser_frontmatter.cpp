// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/parser.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_set>

namespace cadml {

namespace {

// ─── Source navigation ───────────────────────────────────────────────────

struct Cursor {
    std::string_view src;
    std::size_t      pos  = 0;     // byte offset
    std::uint32_t    line = 1;     // 1-based
    std::uint32_t    col  = 1;     // 1-based
};

void advance(Cursor& c, std::size_t n) {
    for (std::size_t i = 0; i < n && c.pos < c.src.size(); ++i) {
        if (c.src[c.pos] == '\n') {
            ++c.line;
            c.col = 1;
        } else {
            ++c.col;
        }
        ++c.pos;
    }
}

// True if `c` is at end of source.
bool at_end(const Cursor& c) {
    return c.pos >= c.src.size();
}

// Strip a UTF-8 BOM if present at the start.
void strip_bom(Cursor& c) {
    if (c.src.size() >= 3 &&
        static_cast<unsigned char>(c.src[0]) == 0xEF &&
        static_cast<unsigned char>(c.src[1]) == 0xBB &&
        static_cast<unsigned char>(c.src[2]) == 0xBF) {
        // Skip three BOM bytes without changing line/col.
        c.pos += 3;
    }
}

// ─── Line extraction ─────────────────────────────────────────────────────

// Capture the next logical line starting at `c.pos`. Returns the line
// content (without the trailing LF) and advances `c` past the LF.
// If the line begins with a body-starting `<`, returns empty content
// and leaves `c` pointing at the `<`.
std::string_view next_line(Cursor& c, SourceRange& range) {
    range.line = c.line;
    range.column = c.col;

    const std::size_t line_start = c.pos;
    while (c.pos < c.src.size() && c.src[c.pos] != '\n') {
        ++c.pos;
        ++c.col;
    }
    const std::size_t line_end = c.pos;

    // Consume the LF if present.
    if (c.pos < c.src.size() && c.src[c.pos] == '\n') {
        ++c.pos;
        ++c.line;
        c.col = 1;
    }

    range.length = static_cast<std::uint32_t>(line_end - line_start);

    auto v = c.src.substr(line_start, line_end - line_start);

    // Trim a trailing CR (CRLF normalisation).
    if (!v.empty() && v.back() == '\r') {
        v.remove_suffix(1);
    }

    return v;
}

// ─── String utilities ────────────────────────────────────────────────────

// Trim leading whitespace; return remainder.
std::string_view ltrim(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

// Trim trailing whitespace.
std::string_view rtrim(std::string_view s) {
    std::size_t i = s.size();
    while (i > 0 && std::isspace(static_cast<unsigned char>(s[i - 1]))) --i;
    return s.substr(0, i);
}

std::string_view trim(std::string_view s) {
    return rtrim(ltrim(s));
}

// Strip an in-line comment (`# ...`) from `line`. Doesn't honour `#`
// inside quoted strings (the only place `#` could legitimately appear
// in frontmatter values is inside a quoted `description`).
std::string_view strip_comment(std::string_view line) {
    bool in_quote = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '"') {
            in_quote = !in_quote;
        } else if (ch == '#' && !in_quote) {
            return line.substr(0, i);
        }
    }
    return line;
}

// Split the first whitespace-delimited token off `line` and return it.
// `rest` is updated to point at the remainder (with leading whitespace
// trimmed).
std::string_view take_token(std::string_view line, std::string_view& rest) {
    std::size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    const std::size_t start = i;
    while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    const std::string_view tok = line.substr(start, i - start);
    rest = ltrim(line.substr(i));
    return tok;
}

// Parse a quoted string. Expects the input to start with '"'. Returns
// the unquoted content and updates `rest` to point past the closing
// quote. Reports a parse error on unterminated quote.
std::optional<std::string> parse_quoted(std::string_view in,
                                         std::string_view& rest) {
    if (in.empty() || in[0] != '"') return std::nullopt;
    std::string out;
    std::size_t i = 1;
    while (i < in.size()) {
        const char ch = in[i];
        if (ch == '"') {
            rest = ltrim(in.substr(i + 1));
            return out;
        }
        if (ch == '\\' && i + 1 < in.size()) {
            const char nx = in[i + 1];
            if (nx == '"' || nx == '\\') {
                out.push_back(nx);
                i += 2;
                continue;
            }
        }
        out.push_back(ch);
        ++i;
    }
    // Unterminated.
    return std::nullopt;
}

// Match: identifier following spec §2.8 — [a-z][a-z0-9-]*.
bool is_identifier_start(char ch) {
    return ch >= 'a' && ch <= 'z';
}

bool is_identifier_continue(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
}

// Try to take an identifier from the start of `in`. Returns nullopt if
// the input doesn't begin with a valid identifier.
std::optional<std::string_view> take_identifier(std::string_view in,
                                                  std::string_view& rest) {
    if (in.empty() || !is_identifier_start(in[0])) return std::nullopt;
    std::size_t i = 1;
    while (i < in.size() && is_identifier_continue(in[i])) ++i;
    rest = in.substr(i);
    return in.substr(0, i);
}

// Parse a numeric literal (decimal or scientific notation). Updates
// `rest` past the consumed number. Scans for the longest numeric
// prefix manually and routes it through parse_double_strict so the
// host's LC_NUMERIC locale can't truncate `60.0` to `60`.
std::optional<double> take_number(std::string_view in,
                                    std::string_view& rest) {
    const auto trimmed = ltrim(in);
    if (trimmed.empty()) return std::nullopt;

    std::size_t end = 0;
    if (end < trimmed.size() &&
        (trimmed[end] == '+' || trimmed[end] == '-')) ++end;
    bool saw_digit = false;
    while (end < trimmed.size() &&
           std::isdigit(static_cast<unsigned char>(trimmed[end]))) {
        ++end; saw_digit = true;
    }
    if (end < trimmed.size() && trimmed[end] == '.') {
        ++end;
        while (end < trimmed.size() &&
               std::isdigit(static_cast<unsigned char>(trimmed[end]))) {
            ++end; saw_digit = true;
        }
    }
    if (end < trimmed.size() &&
        (trimmed[end] == 'e' || trimmed[end] == 'E')) {
        ++end;
        if (end < trimmed.size() &&
            (trimmed[end] == '+' || trimmed[end] == '-')) ++end;
        while (end < trimmed.size() &&
               std::isdigit(static_cast<unsigned char>(trimmed[end]))) {
            ++end;
        }
    }
    if (!saw_digit) return std::nullopt;
    auto v = parse_double_strict(trimmed.substr(0, end));
    if (!v) return std::nullopt;
    rest = trimmed.substr(end);
    return v;
}

// ─── Settings recognition ────────────────────────────────────────────────

// True iff `tok` is a known setting keyword.
bool is_setting_keyword(std::string_view tok) {
    return tok == "version" ||
           tok == "units" ||
           tok == "description" ||
           tok == "tags" ||
           tok == "catalogue-version" ||
           tok == "interference-tolerance";
}

// Apply a setting to DocumentMeta. Returns nullopt on success, or an
// error message string on failure. The caller tracks duplicate detection
// via a separate `seen_settings` set; this function does not detect
// duplicates itself (the default-initialised value of `meta.version`
// makes that check ambiguous here).
std::optional<std::string> apply_setting(DocumentMeta& meta,
                                          std::string_view key,
                                          std::string_view value) {
    if (key == "version") {
        const auto v = std::string(trim(value));
        if (v.empty()) return std::string("version requires a value");
        // Normalise to three-component form ("0.1" → "0.1.0").
        std::string normalised = v;
        std::size_t dots = std::count(normalised.begin(), normalised.end(), '.');
        while (dots < 2) {
            normalised += ".0";
            ++dots;
        }
        meta.version = normalised;
        return std::nullopt;
    }
    if (key == "units") {
        const auto v = std::string(trim(value));
        if (v != "mm" && v != "cm" && v != "m" && v != "in" && v != "ft") {
            return std::string("units must be one of mm/cm/m/in/ft (got '" + v + "')");
        }
        meta.units = v;
        return std::nullopt;
    }
    if (key == "description") {
        // Value is a quoted string.
        std::string_view rest;
        auto s = parse_quoted(ltrim(value), rest);
        if (!s) return std::string("description requires a quoted string");
        if (!trim(rest).empty()) {
            return std::string("unexpected text after description");
        }
        meta.description = std::move(*s);
        return std::nullopt;
    }
    if (key == "tags") {
        std::string_view rest;
        auto s = parse_quoted(ltrim(value), rest);
        if (!s) return std::string("tags requires a quoted string");
        if (!trim(rest).empty()) {
            return std::string("unexpected text after tags");
        }
        meta.tags = std::move(*s);
        return std::nullopt;
    }
    if (key == "catalogue-version") {
        meta.catalogue_version = std::string(trim(value));
        return std::nullopt;
    }
    if (key == "interference-tolerance") {
        const auto raw = std::string(trim(value));
        // Validate the form NOW so malformed input fails compile
        // rather than slipping through to a downstream tool. We
        // validate against `mm` doc-units — that's always valid
        // and catches every form-error category (bad number, bad
        // suffix, missing cube marker, unknown unit). The actual
        // conversion at use-time still uses the doc's real units.
        if (!parse_interference_tolerance(raw, "mm").has_value()) {
            return std::string(
                "interference-tolerance must be a non-negative number"
                " optionally followed by a volume unit (mm^3, cm^3,"
                " m^3, in^3, ft^3); got `" + raw + "`");
        }
        meta.interference_tolerance = raw;
        return std::nullopt;
    }
    return std::string("internal error: unrecognised setting keyword");
}

// ─── Directive recognition ───────────────────────────────────────────────

// Try parsing an `import "..." [as <alias>]` line. Returns nullopt if
// the line isn't an import directive (caller should try other shapes).
// Otherwise produces an ImportDecl or returns an error message.
struct ImportParse {
    std::optional<ImportDecl> decl;
    std::optional<std::string> error;
    bool was_import = false;
};

ImportParse parse_import_line(std::string_view line) {
    ImportParse out;
    std::string_view rest;
    auto first = take_token(line, rest);
    if (first != "import") return out;

    out.was_import = true;

    // Path must be quoted.
    auto path = parse_quoted(rest, rest);
    if (!path) {
        out.error = std::string("import: expected quoted path after `import`");
        return out;
    }

    ImportDecl decl;
    decl.path = std::move(*path);

    // Optional `as <alias>`.
    rest = ltrim(rest);
    if (!rest.empty()) {
        auto as_tok = take_token(rest, rest);
        if (as_tok != "as") {
            out.error = std::string("import: expected `as <alias>` after path");
            return out;
        }
        auto alias_id = take_identifier(rest, rest);
        if (!alias_id) {
            out.error = std::string("import: alias must be kebab-case identifier");
            return out;
        }
        decl.alias = std::string(*alias_id);
        if (!trim(rest).empty()) {
            out.error = std::string("import: unexpected text after alias");
            return out;
        }
    } else {
        // Default alias: filename (last path component) without extension.
        // Validate against the identifier grammar (spec §2.8) — this is
        // SECURITY-RELEVANT. The alias is concatenated verbatim into a
        // generated Lua wrapper (wrap_lua_module in bundler.cpp); a
        // filename like `lib"]=nil _G.io=require('io') --.lua` would
        // bypass the Lua sandbox if not validated here.
        const auto& p = decl.path;
        std::size_t slash = p.find_last_of('/');
        const std::string base = (slash == std::string::npos) ? p : p.substr(slash + 1);
        std::size_t dot = base.find_last_of('.');
        std::string candidate = (dot == std::string::npos) ? base : base.substr(0, dot);
        std::string_view leftover;
        auto valid = take_identifier(candidate, leftover);
        if (!valid || !leftover.empty()) {
            out.error = std::string(
                "import: filename-derived alias `" + candidate +
                "` is not a valid kebab-case identifier (spec §2.8). "
                "Use `as <alias>` to supply a valid one.");
            return out;
        }
        decl.alias = std::string(*valid);
    }

    // Catalogue prefix and Lua dispatch are inferred from the path.
    decl.is_catalogue =
        decl.path.size() >= 4 && decl.path.compare(0, 4, "ctl/") == 0;

    const auto& p = decl.path;
    decl.is_lua = p.size() >= 4 &&
                  p.compare(p.size() - 4, 4, ".lua") == 0;

    // Spec §4.4: import alias cannot collide with a built-in element name.
    // (Both the explicitly-given `as <alias>` and the filename-derived
    // default are subject to this rule.) Checked against the 0.1 baseline
    // set here — the file's `version` may not have been read yet
    // (frontmatter ordering is a style rule, not enforced), so names
    // reserved only by newer spec versions are re-checked in
    // parse_frontmatter once all lines are in.
    if (node_type_from_builtin_name(decl.alias, kSpecV01) != NodeType::Unknown) {
        out.error = std::string(
            "import: alias `" + decl.alias +
            "` collides with a built-in element name. Use `as <other>` to"
            " disambiguate.");
        return out;
    }

    out.decl = std::move(decl);
    return out;
}

// ─── Param parsing ───────────────────────────────────────────────────────

// Try parsing a `param <name> = <expr> [(min=<n>, max=<n>)]` line.
struct ParamParse {
    std::optional<ParamDecl> decl;
    std::optional<std::string> error;
    bool was_param = false;
};

ParamParse parse_param_line(std::string_view line) {
    ParamParse out;
    std::string_view rest;
    auto first = take_token(line, rest);
    if (first != "param") return out;

    out.was_param = true;

    auto name_id = take_identifier(rest, rest);
    if (!name_id) {
        out.error = std::string("param: name must be kebab-case identifier");
        return out;
    }

    rest = ltrim(rest);
    if (rest.empty() || rest[0] != '=') {
        out.error = std::string("param: expected `=` after name");
        return out;
    }
    rest = ltrim(rest.substr(1));

    if (rest.empty()) {
        out.error = std::string("param: expected value after `=`");
        return out;
    }

    // Find the start of an optional constraints block: ` (min=..., max=...)`
    // appearing at the END of the line (not inside the value expression
    // itself, which may contain parens for grouping).
    //
    // Heuristic: look for a top-level `(` that's followed eventually by
    // identifier=number pairs and a `)`. We split on the LAST `(` that
    // begins a balanced trailing parenthesised group, then check whether
    // its inner content contains `=`.
    //
    // Known edge case: a value expression ending with a parenthesised
    // call whose argument list contains `=` would be misclassified as
    // constraints. Standard Lua syntax doesn't admit `func(a=1)` keyword
    // args, so this is unlikely in practice. If it ever bites, the
    // workaround is to add explicit empty constraints `(min=-1e30,
    // max=1e30)` or restructure the expression with helper params.
    std::optional<std::string_view> constraint_text;
    std::string_view value_text = rest;

    if (!rest.empty() && rest.back() == ')') {
        // Walk backward to find the matching '('.
        int depth = 0;
        std::size_t lparen = std::string_view::npos;
        for (std::size_t i = rest.size(); i-- > 0; ) {
            const char ch = rest[i];
            if (ch == ')') ++depth;
            else if (ch == '(') {
                --depth;
                if (depth == 0) { lparen = i; break; }
            }
        }
        if (lparen != std::string_view::npos && lparen > 0) {
            // Verify the inside looks like constraints (contains `=` and an
            // identifier we recognise: min/max).
            const auto inner = trim(rest.substr(lparen + 1, rest.size() - lparen - 2));
            if (inner.find('=') != std::string_view::npos) {
                value_text = rtrim(rest.substr(0, lparen));
                constraint_text = inner;
            }
        }
    }

    ParamDecl decl;
    decl.name = std::string(*name_id);
    decl.value_expr = std::string(trim(value_text));

    if (decl.value_expr.empty()) {
        out.error = std::string("param: empty value expression");
        return out;
    }

    if (constraint_text) {
        // Parse comma-separated `key=number` pairs.
        std::string_view ctext = *constraint_text;
        while (!trim(ctext).empty()) {
            std::string_view rest2;
            auto key_id = take_identifier(ltrim(ctext), rest2);
            if (!key_id) {
                out.error = std::string("param: invalid constraint key");
                return out;
            }
            rest2 = ltrim(rest2);
            if (rest2.empty() || rest2[0] != '=') {
                out.error = std::string("param: expected `=` after constraint key");
                return out;
            }
            rest2 = ltrim(rest2.substr(1));
            std::string_view rest3;
            auto val = take_number(rest2, rest3);
            if (!val) {
                out.error = std::string("param: constraint value must be a number");
                return out;
            }
            if (*key_id == "min") decl.min = *val;
            else if (*key_id == "max") decl.max = *val;
            else {
                out.error =
                    std::string("param: unknown constraint `" +
                                std::string(*key_id) + "`");
                return out;
            }
            // Skip an optional trailing comma.
            rest3 = ltrim(rest3);
            if (!rest3.empty() && rest3[0] == ',') rest3.remove_prefix(1);
            ctext = rest3;
        }
    }

    out.decl = std::move(decl);
    return out;
}

// ─── Section state machine ──────────────────────────────────────────────

enum class Section { Settings, Imports, Params };

bool can_appear_in(Section s, std::string_view first_token, bool is_param_kw) {
    if (is_param_kw) return s == Section::Params;
    if (first_token == "import") return s == Section::Imports;
    if (is_setting_keyword(first_token)) return s == Section::Settings;
    return false;
}

}  // namespace

// ─── Public entry point ─────────────────────────────────────────────────

FrontmatterResult parse_frontmatter(std::string_view source,
                                     SourceFileId file_id) {
    FrontmatterResult out;

    Cursor c;
    c.src = source;
    strip_bom(c);

    Section section = Section::Settings;
    std::unordered_set<std::string> seen_settings;

    auto push_error = [&](std::string msg, SourceRange r) {
        ParseError e;
        e.category = ParseError::Parse;
        e.message = std::move(msg);
        e.source = r;
        e.source.file = file_id;
        out.errors.push_back(std::move(e));
    };
    auto push_warning = [&](std::string msg, SourceRange r) {
        ParseError w;
        w.category = ParseError::Parse;
        w.message = std::move(msg);
        w.source = r;
        w.source.file = file_id;
        out.warnings.push_back(std::move(w));
    };

    // Aliases are validated against the 0.1 baseline set as each import
    // line parses; a name reserved only by a NEWER spec version needs the
    // file's final `version` (frontmatter ordering is not enforced), so
    // those are checked here, once, at every exit from the parse loop.
    auto check_versioned_aliases = [&]() {
        const auto spec = spec_version_from_string(out.meta.version);
        for (const auto& decl : out.imports) {
            const auto since = builtin_since(decl.alias);
            if (since && kSpecV01 < *since && *since <= spec) {
                push_error(
                    "import: alias `" + decl.alias + "` collides with a"
                    " built-in element name (reserved since spec " +
                    std::to_string(since->major) + "." +
                    std::to_string(since->minor) + "). Use `as <other>`"
                    " to disambiguate.",
                    decl.source);
            }
        }
    };

    while (!at_end(c)) {
        // Peek: if the next non-whitespace character is `<`, frontmatter
        // is over and the body starts here.
        Cursor probe = c;
        while (!at_end(probe) &&
               std::isspace(static_cast<unsigned char>(probe.src[probe.pos])) &&
               probe.src[probe.pos] != '\n') {
            ++probe.pos;
            ++probe.col;
        }
        if (!at_end(probe) && probe.src[probe.pos] == '<') {
            out.body_offset = probe.pos;
            check_versioned_aliases();
            return out;
        }

        SourceRange line_range;
        line_range.file = file_id;
        auto raw_line = next_line(c, line_range);

        const auto stripped = strip_comment(raw_line);
        const auto trimmed  = trim(stripped);

        if (trimmed.empty()) continue;

        // Classify the statement.
        std::string_view rest;
        const auto first = take_token(trimmed, rest);

        const bool is_param_kw = (first == "param");

        if (!can_appear_in(section, first, is_param_kw)) {
            // If the keyword belongs to a section we've already passed,
            // that's an ordering issue. Otherwise it's an unknown
            // statement.
            const bool is_known =
                first == "param" || first == "import" || is_setting_keyword(first);
            if (is_known) {
                // Try advancing the section monotonically.
                if (first == "import" && section == Section::Settings) {
                    section = Section::Imports;
                } else if (is_param_kw &&
                           (section == Section::Settings ||
                            section == Section::Imports)) {
                    section = Section::Params;
                } else {
                    // Order is a style rule (settings → imports →
                    // params), not a semantic constraint — accept the
                    // statement and emit a warning so authoring tools
                    // still see the lint, but the build doesn't fail
                    // on a misplaced `units` after an import.
                    push_warning(
                        "frontmatter ordering: `" + std::string(first) +
                        "` appears out of the conventional order "
                        "(settings → imports → params); accepted, but "
                        "consider rearranging",
                        line_range);
                }
            } else {
                push_error(
                    "unrecognised frontmatter statement starting with `" +
                    std::string(first) + "`",
                    line_range);
                continue;
            }
        }

        // Dispatch by statement shape.
        if (is_param_kw) {
            auto pr = parse_param_line(trimmed);
            if (pr.error) {
                push_error(*pr.error, line_range);
                continue;
            }
            if (pr.decl) {
                pr.decl->source = line_range;
                out.params.push_back(std::move(*pr.decl));
            }
        } else if (first == "import") {
            auto ir = parse_import_line(trimmed);
            if (ir.error) {
                push_error(*ir.error, line_range);
                continue;
            }
            if (ir.decl) {
                ir.decl->source = line_range;
                out.imports.push_back(std::move(*ir.decl));
            }
        } else if (is_setting_keyword(first)) {
            const auto key = std::string(first);
            if (seen_settings.count(key)) {
                push_error(
                    "duplicate `" + key + "` setting (each setting may"
                    " appear at most once)", line_range);
                continue;
            }
            seen_settings.insert(key);
            if (key == "version") out.version_explicitly_set = true;
            const auto err = apply_setting(out.meta, first, rest);
            if (err) push_error(*err, line_range);
        } else {
            push_error(
                "unrecognised frontmatter statement starting with `" +
                std::string(first) + "`",
                line_range);
        }
    }

    out.body_offset = c.pos;
    check_versioned_aliases();
    return out;
}

}  // namespace cadml
