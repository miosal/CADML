// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>

#include <cadml/expression.hpp>
#include <cadml/parser.hpp>
#include <cadml/selector.hpp>
#include <cadml/serializer.hpp>
#include <cadml/sha256.hpp>

#include "assembly_compiler.hpp"
#include "lua_string_eval.hpp"
#include "source_provider.hpp"
#include "unrollers.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace cadml::compile {

namespace fs = std::filesystem;

namespace {

// ─── Source-file hashing (spec §10.4) ────────────────────────────────
//
// SHA-256 of the raw source bytes (UTF-8, post-BOM-stripping but
// pre-LF-normalisation per the parser convention). Output is the
// 64-char lowercase hex form.
std::string content_hash(std::string_view content) {
    // Per spec §10.4: hash is over the bytes AFTER the BOM (so files
    // with and without a BOM that are otherwise identical produce the
    // same hash, which the parser treats as equivalent). Without this
    // a BOM-prefixed save round-trip would invalidate every cached
    // source-map.
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.remove_prefix(3);
    }
    return sha256_hex(content);
}

// Read an entry .cadml file with the same size cap the bundler applies
// to imports. Returns nullopt on missing/short-read; sets *too_large to
// true (when supplied) if the file exists but exceeds kMaxSourceBytes.
std::optional<std::string> read_file_to_string(const fs::path& path,
                                                 bool* too_large = nullptr) {
    if (too_large) *too_large = false;
    std::error_code ec;
    const auto sz = fs::file_size(path, ec);
    if (ec) return std::nullopt;
    if (sz > cadml::kMaxSourceBytes) {
        if (too_large) *too_large = true;
        return std::nullopt;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::string out(sz, '\0');
    f.read(out.data(), static_cast<std::streamsize>(sz));
    if (f.gcount() != static_cast<std::streamsize>(sz)) return std::nullopt;
    return out;
}

// ─── Import-path → key resolution (pure string; no filesystem) ───────
//
// Import resolution produces a normalized, root-relative lookup KEY that
// a SourceProvider (source_provider.hpp) maps to bytes. This is pure
// string math — the provider, not the resolver, decides whether/where a
// key resolves to actual content. Per spec §3.4:
//   * `ctl/...`        → relative to the project root (key as-is)
//   * `./...` `../...` → relative to the importing file's directory
//   * bare path        → relative to the importing file's directory
//
// `importing_dir_key` is the directory part of the importing file's key
// ("" for a file at the root). The result is normalized so that, e.g.,
// `../shared/x` from "sub/a.cadml" and `shared/x` from the root both
// collapse to the same key "shared/x" — which is what makes diamond-
// import detection and the SourceFileId cache work.
std::string resolve_import_key(std::string_view raw_path,
                                std::string_view importing_dir_key) {
    const bool is_ctl =
        raw_path.size() >= 4 && raw_path.compare(0, 4, "ctl/") == 0;
    std::string combined;
    if (is_ctl || importing_dir_key.empty()) {
        combined.assign(raw_path);
    } else {
        combined.reserve(importing_dir_key.size() + 1 + raw_path.size());
        combined.assign(importing_dir_key);
        combined += '/';
        combined.append(raw_path);
    }
    return normalize_key(combined);
}

// Directory part of a normalized key ("" if the key is at the root).
std::string dir_key_of(std::string_view key) {
    const auto slash = key.find_last_of('/');
    if (slash == std::string_view::npos) return {};
    return std::string(key.substr(0, slash));
}

// A normalized key escapes the project root if it climbs out via `..`
// or is root-anchored. `../`-prefixed keys come from `../../..` traversal;
// a leading `/` comes from a root-absolute import like `/etc/passwd`
// (which `fs::path::is_absolute()` does NOT flag on Windows, since it
// lacks a drive letter — so we catch it here for a consistent, honest
// error on every platform). The FilesystemProvider also re-checks
// containment at the filesystem level (symlink-aware); the InMemory
// provider can't escape at all. Belt and braces.
bool key_escapes_root(std::string_view key) {
    return key == ".." || key.rfind("../", 0) == 0 ||
           (!key.empty() && key.front() == '/');
}

// ─── Lua module wrapping ─────────────────────────────────────────────
//
// Imported `.lua` files are synthesised into `<script>` elements at the
// top of the host's body. The script content wraps the user's source
// so that the import alias (e.g. `airfoils`) becomes a global table
// holding the module's exports. This means downstream `<script>` tags
// and CADML `{...}` expressions can call `airfoils.naca(...)` without
// any LuaRuntime-side coordination — the wrapper does the namespacing.
//
// Detection of module-pattern (`return <table>`) vs free-form is done
// the same way as LuaRuntime::load_module: scan backward to the last
// non-blank, non-comment line and check for a leading `return`.

bool lua_ends_with_return(std::string_view src) {
    std::size_t end = src.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(src[end - 1]))) {
        --end;
    }
    if (end == 0) return false;
    while (end > 0) {
        std::size_t line_end = end;
        std::size_t line_start = end;
        while (line_start > 0 && src[line_start - 1] != '\n') --line_start;
        std::string_view line = src.substr(line_start, line_end - line_start);
        std::size_t lo = 0, hi = line.size();
        while (lo < hi && std::isspace(static_cast<unsigned char>(line[lo]))) ++lo;
        while (hi > lo && std::isspace(static_cast<unsigned char>(line[hi - 1]))) --hi;
        std::string_view trimmed = line.substr(lo, hi - lo);
        if (trimmed.size() >= 2 && trimmed.compare(0, 2, "--") == 0) {
            end = (line_start == 0) ? 0 : line_start - 1;
            continue;
        }
        if (trimmed.empty()) {
            end = (line_start == 0) ? 0 : line_start - 1;
            continue;
        }
        return trimmed.size() >= 6 && trimmed.compare(0, 6, "return") == 0 &&
               (trimmed.size() == 6 ||
                std::isspace(static_cast<unsigned char>(trimmed[6])) ||
                trimmed[6] == '{');
    }
    return false;
}

// Identifier-validate the alias before splicing into Lua. Defence in
// depth: the parser already enforces this at the import-statement
// boundary, but we must NOT trust any caller of this function to
// have done so. A malicious alias containing `"` or `\` would
// inject arbitrary Lua code into the wrapper and escape the
// sandbox. Spec §2.8 identifier grammar is [a-z][a-z0-9-]*.
bool is_safe_lua_alias(std::string_view s) {
    if (s.empty()) return false;
    if (!(s[0] >= 'a' && s[0] <= 'z')) return false;
    for (char c : s.substr(1)) {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string wrap_lua_module(std::string_view source, std::string_view alias) {
    if (!is_safe_lua_alias(alias)) {
        // Fail closed: emit a script that throws at load time rather
        // than producing exploitable wrapper source. The bundler
        // already validates aliases upstream; reaching this path
        // would be a bug, but we make the failure visible.
        return "error(\"cadml: refusing to wrap module with unsafe alias\")\n";
    }
    if (lua_ends_with_return(source)) {
        // Module pattern: file body returns a table; bind that table to
        // the alias as a global.
        std::string out;
        out.reserve(source.size() + 128);
        out += "do local _m = (function()\n";
        out += source;
        out += "\nend)(); _G[\"";
        out += alias;
        out += "\"] = _m end\n";
        return out;
    }
    // Free-form: capture top-level globals introduced by the script.
    // We DO NOT remove them from _G at the end because modules
    // routinely have inter-function calls (e.g. compressor.lua's
    // `hub_tx` calls `hub_r`); removing the globals would break
    // those calls when invoked later from outside the module.
    // Trade-off: this leaks the module's symbols into the global
    // namespace, so two modules defining the same function name
    // would collide. Acceptable for typical single-import
    // workflows; revisit with `setfenv`/`_ENV` sandboxing if
    // multi-module collisions become a real issue.
    std::string out;
    out.reserve(source.size() + 256);
    out += "do\n";
    out += "  local __pre = {}\n";
    out += "  for k, _ in pairs(_G) do __pre[k] = true end\n";
    out += source;
    out += "\n  local __m = {}\n";
    out += "  for k, v in pairs(_G) do\n";
    out += "    if not __pre[k] then __m[k] = v end\n";
    out += "  end\n";
    out += "  _G[\"";
    out += alias;
    out += "\"] = __m\n";
    out += "end\n";
    return out;
}

// ─── Recursive import resolver ───────────────────────────────────────

struct ImportContext {
    // `visited` tracks files currently in the import stack — used for
    // cycle detection; entries are erased on unwind. Keyed by the
    // normalized root-relative import key.
    std::unordered_set<std::string> visited;
    // `seen` tracks files ever-imported across the whole traversal —
    // used so diamond imports reuse a single SourceFileId instead of
    // reparsing the file twice. Never erased.
    std::unordered_map<std::string, SourceFileId> seen;
    std::vector<SourceFile>   source_files;    // by id
    std::vector<CompileError> errors;
    std::vector<CompileError> warnings;

    // Register a source file under its normalized relative `key` (which
    // becomes the `<source path="...">` entry verbatim). Idempotent: a
    // key seen before returns its existing id (diamond-import reuse).
    SourceFileId register_file(const std::string& key, std::string_view content) {
        auto it = seen.find(key);
        if (it != seen.end()) return it->second;
        SourceFile sf;
        sf.id   = static_cast<SourceFileId>(source_files.size());
        sf.path = key;
        sf.hash = content_hash(content);
        source_files.push_back(sf);
        seen.emplace(key, sf.id);
        return sf.id;
    }

    // Has this key been imported before (regardless of whether it's
    // still on the stack)?
    bool already_seen(const std::string& key) const {
        return seen.find(key) != seen.end();
    }

    void push_error(CompileError::Category cat, std::string msg, SourceRange src) {
        errors.push_back({ cat, std::move(msg), src });
    }
};

// resolve_imports_into is a template (on the SourceProvider) and recurses
// into itself, so it needs no separate forward declaration — but it calls
// merge_imported_doc, which is therefore defined first, just below.

// Merge the imported document into the host. Imported parts/defs become
// host defs with namespace-prefixed names; nodes are appended; tree
// indices shifted by `offset`. Per spec §6.1, the imported file's
// exported <part>/<assembly> becomes available as the alias element.
void merge_imported_doc(Document& host, Document imported,
                         std::string_view alias) {
    const std::string prefix(alias);

    const auto offset = static_cast<std::uint32_t>(host.nodes.size());

    // Re-link parent / first_child / next_sibling indices.
    for (auto& n : imported.nodes) {
        if (n.parent != NO_NODE)       n.parent       += offset;
        if (n.first_child != NO_NODE)  n.first_child  += offset;
        if (n.next_sibling != NO_NODE) n.next_sibling += offset;
    }

    // Merge def index with prefix. The map's iteration order is
    // hash-defined; copy into a sorted vector so the synthesised
    // host.defs entries land in source order regardless of platform.
    {
        std::vector<std::pair<std::string, std::uint32_t>> sorted(
            imported.defs.begin(), imported.defs.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](auto& a, auto& b) { return a.second < b.second; });
        for (const auto& [name, idx] : sorted) {
            host.defs[prefix + "." + name] = idx + offset;
        }
    }

    // Find the imported export and turn it into a def named `alias`.
    // Spec §2.2 allows >1 top-level export per file; for an aliased
    // import only the first export (in source-file order) is bound to
    // `alias`. Additional exports remain reachable via `alias.<name>`
    // through the merged def table above. Iterating by ascending node
    // index gives a stable "first" across platforms.
    std::vector<std::pair<std::string, std::uint32_t>> sorted_exports(
        imported.exports.begin(), imported.exports.end());
    std::sort(sorted_exports.begin(), sorted_exports.end(),
              [](auto& a, auto& b) { return a.second < b.second; });
    for (const auto& [name, idx] : sorted_exports) {
        const auto host_idx = idx + offset;
        // Convert the export node to a def named after the alias.
        // Imported <part> → <def name="alias">. Imported <assembly>:
        // also represented as <def> for now (assembly compilation
        // rewrites it later).
        if (imported.nodes[idx].type == NodeType::Part) {
            const auto& pa = std::get<PartAttrs>(imported.nodes[idx].attrs);
            DefAttrs da;
            da.name  = prefix;
            da.color = pa.color;   // preserve imported part colour
            imported.nodes[idx].type = NodeType::Def;
            imported.nodes[idx].attrs = da;
        } else if (imported.nodes[idx].type == NodeType::Assembly) {
            DefAttrs da;
            da.name = prefix;
            imported.nodes[idx].type = NodeType::Def;
            imported.nodes[idx].attrs = da;
        }
        host.defs[prefix] = host_idx;
        // Don't add to host.exports — only the entry file's export
        // counts as a host export.
        (void)name;
    }

    // Append nodes.
    for (auto& n : imported.nodes) {
        host.nodes.push_back(std::move(n));
    }

    // Imported file's frontmatter params apply to the imported part
    // (which is now a def). Hoist them as <param> children of that def,
    // preserving declaration order so that derived defaults like
    // `param wh-z = {bk-bot-z + axle-inset}` see their dependencies
    // already bound when the def-eval loop walks param children left
    // to right. Mirrors hoist_entry_params for consistency between the
    // entry-file and imported-file hoists.
    // We don't merge into host.params — those are entry-file params.
    if (auto it = host.defs.find(prefix); it != host.defs.end()) {
        const auto def_idx = it->second;

        std::vector<std::uint32_t> new_param_nodes;
        new_param_nodes.reserve(imported.params.size());
        for (const auto& p : imported.params) {
            ParamAttrs pa;
            pa.name = p.name;
            pa.value_expr = p.value_expr;
            pa.min = p.min;
            pa.max = p.max;

            Node param_node;
            param_node.type = NodeType::Param;
            param_node.parent = def_idx;
            param_node.attrs = pa;
            param_node.source = p.source;

            const auto pn_idx = static_cast<std::uint32_t>(host.nodes.size());
            host.nodes.push_back(std::move(param_node));
            new_param_nodes.push_back(pn_idx);
        }

        // Chain the new param nodes in declaration order, then splice
        // the chain at the FRONT of the def's children list.
        if (!new_param_nodes.empty()) {
            const auto old_first = host.nodes[def_idx].first_child;
            for (std::size_t i = 0; i + 1 < new_param_nodes.size(); ++i) {
                host.nodes[new_param_nodes[i]].next_sibling = new_param_nodes[i + 1];
            }
            host.nodes[new_param_nodes.back()].next_sibling = old_first;
            host.nodes[def_idx].first_child = new_param_nodes.front();
        }
    }
}

// Hard cap on import-chain nesting depth. resolve_imports_into recurses
// once per level of a chain (a0 imports a1 imports a2 …), and each frame
// holds a parsed Document by value, so an attacker-controlled chain of
// distinct files would exhaust the stack — STATUS_STACK_OVERFLOW natively,
// an opaque trap in the WASM build (smallest stack, most exposed surface).
// The `seen`/`visited` sets dedup diamonds and catch cycles but do NOT
// bound a *linear* acyclic chain; that's this cap's job. Real projects
// nest a handful deep, so 64 is generous.
constexpr int kMaxImportDepth = 64;

template <SourceProvider P>
void resolve_imports_into(Document& host, ImportContext& ctx,
                           const P& provider,
                           const std::string& importing_dir_key,
                           int depth = 0) {
    // Snapshot host.imports — we'll process them but not re-resolve
    // imports in newly merged docs (those already had their imports
    // resolved against THEIR base dirs).
    auto imports = host.imports;
    host.imports.clear();   // entry file's import directives are
                             // compiled away (they appear nowhere in
                             // .fcadml frontmatter).

    for (const auto& imp : imports) {
        // SECURITY: refuse absolute import paths outright. They never
        // appear in legitimate authoring and they're the easiest way
        // for a malicious file to point the compiler at arbitrary
        // filesystem locations (/etc/passwd, /proc/*, etc.).
        if (fs::path(imp.path).is_absolute()) {
            ctx.push_error(CompileError::Import,
                "import: absolute paths are not permitted (`" + imp.path +
                "`); use a path relative to the importing file or a"
                " ctl/ catalogue entry", imp.source);
            continue;
        }

        const std::string key = resolve_import_key(imp.path, importing_dir_key);

        // SECURITY: containment — a key that normalizes to something
        // starting with `../` climbed out of the project root. Refuse it
        // (this blocks `../../../etc/passwd`; absolute paths are caught
        // above). The FilesystemProvider re-checks at the filesystem
        // level so symlink redirection can't sneak past either.
        if (key_escapes_root(key)) {
            ctx.push_error(CompileError::Import,
                "import: `" + imp.path + "` resolves outside the project"
                " root; imports must stay inside the project tree",
                imp.source);
            continue;
        }

        if (ctx.visited.count(key)) {
            ctx.push_error(CompileError::Import,
                "circular import: `" + imp.path + "` is already in the"
                " import chain", imp.source);
            continue;
        }

        // Diamond-import short-circuit: if we've already processed this
        // key successfully (not currently in the stack, but seen before
        // in a different sibling chain), skip reparse + re-merge and just
        // reuse the existing SourceFileId. This satisfies the spec's
        // "reuse the existing SourceFileId" requirement and also saves
        // work on transitively-shared Lua modules.
        if (ctx.already_seen(key)) {
            // Diamond imports of pure-data files (helpers, common
            // defs) are silently merged into the host's namespace
            // once. We don't re-emit the script or re-inline defs;
            // the original import did that.
            continue;
        }

        const auto read_result = provider.read(key);
        if (!read_result.contents) {
            if (read_result.too_large) {
                ctx.push_error(CompileError::Import,
                    "imported file `" + key + "` exceeds the " +
                    std::to_string(cadml::kMaxSourceBytes) +
                    "-byte source-size limit", imp.source);
            } else {
                ctx.push_error(CompileError::Import,
                    "cannot find imported file: " + key, imp.source);
            }
            continue;
        }
        const std::string& content = *read_result.contents;
        const auto file_id = ctx.register_file(key, content);

        if (imp.is_lua) {
            // Synthesise a <script> element in the host's body holding
            // the Lua source wrapped to bind the alias as a global.
            // Engine sees a regular <script>; alias becomes available
            // to subsequent expressions via `alias.fn(...)`.
            ScriptAttrs sa;
            sa.lang = "lua";
            sa.source = wrap_lua_module(content, imp.alias);

            Node script_node;
            script_node.type = NodeType::Script;
            script_node.attrs = std::move(sa);
            script_node.parent = NO_NODE;
            script_node.source = imp.source;
            script_node.source.file = file_id;
            const auto script_idx = static_cast<std::uint32_t>(host.nodes.size());
            host.nodes.push_back(std::move(script_node));

            // Splice the script as a top-level sibling at the front of
            // the host so it loads before any geometry that uses it.
            std::uint32_t old_first = NO_NODE;
            for (std::uint32_t i = 0; i < host.nodes.size(); ++i) {
                if (i == script_idx) continue;
                if (host.nodes[i].parent == NO_NODE) { old_first = i; break; }
            }
            host.nodes[script_idx].next_sibling = old_first;
            continue;
        }

        // Recursively parse and resolve.
        ctx.visited.insert(key);

        auto sub = parse(content, file_id);
        for (auto& e : sub.errors) {
            CompileError ce;
            // ParseError carries 5 categories; map each into the
            // CompileError equivalent rather than collapsing
            // Expression / Validation down to Parse (which would
            // hide the parser's deliberate distinction from the
            // user's diagnostic tools).
            switch (e.category) {
                case ParseError::Vocabulary: ce.category = CompileError::Vocabulary; break;
                case ParseError::Schema:     ce.category = CompileError::Schema;     break;
                case ParseError::Validation: ce.category = CompileError::Validation; break;
                case ParseError::Expression: ce.category = CompileError::Schema;     break;
                case ParseError::Parse:      ce.category = CompileError::Parse;      break;
            }
            ce.message = e.message;
            ce.source = e.source;
            ctx.errors.push_back(std::move(ce));
        }
        if (!sub.ok()) {
            ctx.visited.erase(key);
            continue;
        }

        // Bound recursion depth before descending: a deep import chain
        // would otherwise overflow the stack (see kMaxImportDepth). Refuse
        // past the cap with a source-located error instead of crashing —
        // the WASM playground is the most exposed caller of this path.
        if (depth + 1 >= kMaxImportDepth) {
            ctx.push_error(CompileError::Import,
                "import: nesting exceeds the maximum depth of " +
                std::to_string(kMaxImportDepth) + " (at `" + imp.path +
                "`); restructure to reduce chained imports", imp.source);
            ctx.visited.erase(key);
            continue;
        }

        // Recurse into imports of the imported file, relative to ITS
        // directory key.
        resolve_imports_into(sub.document, ctx, provider, dir_key_of(key),
                              depth + 1);

        // Merge into host under the alias.
        merge_imported_doc(host, std::move(sub.document), imp.alias);

        ctx.visited.erase(key);
    }
}

// ─── Param hoisting ──────────────────────────────────────────────────
//
// Frontmatter `param x = N` declarations on the entry file become
// <param> child elements of EVERY top-level <part>/<assembly>/<def>
// in the flat output (per spec §10.2.1).
//
// Three reasons every top-level scope gets the params:
//   - Multi-part entry files (peg-in-hole.cadml: block + peg) need
//     all parts to see the same params.
//   - Top-level <def> instantiated from a <part> evaluates in its
//     OWN scope (caller-scope split for instance-override hygiene),
//     so a doc-level param like `{tube-size}` referenced inside the
//     def's body would otherwise fall through. Hoisting puts the
//     param into the def as a default — instance overrides still
//     win when present.
//   - Existing <param> children of a scope SHADOW the hoisted ones
//     (the engine binds children in document order; hoisted go at
//     the front but the scope-eval routine prefers later same-name
//     bindings). This means a part/def can override a frontmatter
//     param locally without conflict.
void hoist_entry_params(Document& doc) {
    if (doc.params.empty()) return;

    auto hoist_into = [&](std::uint32_t target_idx) {
        // Skip names that the target already declares so the local
        // binding wins. Scope eval goes child-by-child in declaration
        // order; with hoisted params spliced at the FRONT, a
        // same-name local later in the list takes precedence — but
        // we keep this dedup anyway so the .fcadml stays terse.
        std::unordered_set<std::string> existing;
        for (auto& child : doc.children(target_idx)) {
            if (child.dead) continue;
            if (child.type != NodeType::Param) continue;
            existing.insert(std::get<ParamAttrs>(child.attrs).name);
        }

        std::vector<std::uint32_t> new_param_nodes;
        new_param_nodes.reserve(doc.params.size());
        for (const auto& p : doc.params) {
            if (existing.count(p.name)) continue;
            ParamAttrs pa;
            pa.name = p.name;
            pa.value_expr = p.value_expr;
            pa.min = p.min;
            pa.max = p.max;

            Node n;
            n.type = NodeType::Param;
            n.parent = target_idx;
            n.attrs = pa;
            n.source = p.source;

            const auto idx = static_cast<std::uint32_t>(doc.nodes.size());
            doc.nodes.push_back(std::move(n));
            new_param_nodes.push_back(idx);
        }

        // Splice the params at the FRONT of the target's children
        // list, in declaration order (so first param appears first).
        const auto old_first = doc.nodes[target_idx].first_child;
        for (std::size_t i = 0; i + 1 < new_param_nodes.size(); ++i) {
            doc.nodes[new_param_nodes[i]].next_sibling = new_param_nodes[i + 1];
        }
        if (!new_param_nodes.empty()) {
            doc.nodes[new_param_nodes.back()].next_sibling = old_first;
            doc.nodes[target_idx].first_child = new_param_nodes.front();
        }
    };

    // Hoist into every export (top-level <part>/<assembly>) AND every
    // top-level <def> (collected from doc.defs but filtered to those
    // whose parent is the synthetic root, since imports inject
    // qualified entries into doc.defs that point at nested nodes).
    //
    // doc.exports / doc.defs are unordered_maps; iterate by ascending
    // node index so the synthesised <param> children end up at
    // platform-independent positions in doc.nodes.
    auto sorted_indices = [](const std::unordered_map<std::string,
                                                       std::uint32_t>& m) {
        std::vector<std::uint32_t> out;
        out.reserve(m.size());
        for (const auto& [_, v] : m) out.push_back(v);
        std::sort(out.begin(), out.end());
        return out;
    };
    for (const auto export_idx : sorted_indices(doc.exports)) {
        hoist_into(export_idx);
    }
    for (const auto def_idx : sorted_indices(doc.defs)) {
        if (def_idx >= doc.nodes.size()) continue;
        if (doc.nodes[def_idx].dead) continue;
        if (doc.nodes[def_idx].parent != NO_NODE) continue; // top-level only
        hoist_into(def_idx);
    }

    doc.params.clear();   // params now live in the body
}

// ─── Param min/max validation (spec §10.2.2 / pipeline step 10) ──────
//
// For every Instance, look up its def and check that any override whose
// matching def-param declares a min/max evaluates to a number inside
// the declared range. Override values are evaluated in a scope made of
// `entry_params` plus the def's own `<param>` children (the latter shadow
// the former, matching how the engine binds params at instantiation).
//
// Inside an imported sub-assembly, an Instance's `ref_name` is local to
// that sub-assembly's namespace; the host's def index keys it as
// "<containing-def>.<ref_name>". Resolve by walking up to the closest
// Def/Part ancestor and trying the qualified name first.
void validate_param_overrides(const Document& doc,
                                const std::vector<ParamDecl>& entry_params,
                                std::vector<CompileError>& errors) {
    auto closest_def_prefix = [&](std::uint32_t node_idx) -> std::string {
        auto cur = doc.nodes[node_idx].parent;
        while (cur != NO_NODE) {
            const auto& a = doc.nodes[cur];
            if (a.type == NodeType::Def)  return std::get<DefAttrs>(a.attrs).name;
            if (a.type == NodeType::Part) return std::get<PartAttrs>(a.attrs).name;
            cur = a.parent;
        }
        return {};
    };
    auto resolve_def = [&](std::uint32_t node_idx,
                            const std::string& ref_name) -> std::int64_t {
        const auto prefix = closest_def_prefix(node_idx);
        if (!prefix.empty()) {
            const auto it = doc.defs.find(prefix + "." + ref_name);
            if (it != doc.defs.end()) return static_cast<std::int64_t>(it->second);
        }
        const auto it = doc.defs.find(ref_name);
        if (it != doc.defs.end()) return static_cast<std::int64_t>(it->second);
        return -1;
    };

    for (std::uint32_t ni = 0; ni < doc.nodes.size(); ++ni) {
        const auto& n = doc.nodes[ni];
        if (n.dead) continue;
        if (n.type != NodeType::Instance) continue;
        const auto& inst = std::get<InstanceAttrs>(n.attrs);
        if (inst.param_overrides.empty()) continue;

        const auto def_resolved = resolve_def(ni, inst.ref_name);
        if (def_resolved < 0) continue;  // unresolved ref reported elsewhere
        const auto def_idx = static_cast<std::uint32_t>(def_resolved);

        // Collect the def's param decls (min/max + value_expr) in one pass.
        struct DefParam {
            std::string value_expr;
            std::optional<double> min;
            std::optional<double> max;
        };
        std::unordered_map<std::string, DefParam> def_params;
        // Declaration order matters: a param default may reference an
        // earlier param (e.g. `plate-top-z = {leg-h + plate-t}`), so the
        // defaults must be evaluated in source order, not the arbitrary
        // hash order of `def_params`. Keep a parallel ordered list of the
        // param names for the evaluation pass below.
        std::vector<std::string> def_param_order;
        for (auto& child : doc.children(def_idx)) {
            if (child.dead) continue;
            if (child.type != NodeType::Param) continue;
            const auto& pa = std::get<ParamAttrs>(child.attrs);
            if (def_params.emplace(pa.name, DefParam{pa.value_expr, pa.min, pa.max}).second)
                def_param_order.push_back(pa.name);
        }

        // Build the eval scope: entry params, then def's own params (so
        // expressions like `length="bolt_size * 2"` can reference both).
        // Surface the underlying expression error if a default fails
        // (div-by-zero, undefined identifier) — the previous silent
        // discard meant a later instance override referencing the
        // unbound param produced a confusing "undefined identifier"
        // error at the override site instead of the actual root cause.
        ExpressionEvaluator e;
        for (const auto& p : entry_params) {
            std::vector<ExpressionError> errs;
            if (auto v = e.evaluate_number(p.value_expr, p.source, errs)) {
                e.set_param(p.name, *v);
            } else if (!errs.empty()) {
                CompileError ce;
                ce.category = CompileError::Validation;
                ce.message  = "entry param `" + p.name +
                              "` default expression `" + p.value_expr +
                              "` failed: " + errs.front().message;
                ce.source   = p.source;
                errors.push_back(std::move(ce));
            }
        }
        for (const auto& name : def_param_order) {
            const auto& dp = def_params.at(name);
            std::vector<ExpressionError> errs;
            if (auto v = e.evaluate_number(dp.value_expr, n.source, errs)) {
                e.set_param(name, *v);
            } else if (!errs.empty()) {
                CompileError ce;
                ce.category = CompileError::Validation;
                ce.message  = "def param `" + name +
                              "` default expression `" + dp.value_expr +
                              "` failed: " + errs.front().message;
                ce.source   = n.source;
                errors.push_back(std::move(ce));
            }
        }

        for (const auto& [override_name, override_expr] : inst.param_overrides) {
            const auto dp_it = def_params.find(override_name);
            if (dp_it == def_params.end()) {
                // Per spec §6.7: overrides naming a param the def
                // doesn't declare are compile-time errors. Silent
                // swallow used to mask typos like `<my-hole d="3"/>`
                // when the def actually declared `diameter`.
                CompileError ce;
                ce.category = CompileError::Validation;
                ce.message  = "instance `" + inst.ref_name + "` overrides"
                              " param `" + override_name + "` but the"
                              " referenced def declares no such param";
                ce.source   = n.source;
                errors.push_back(std::move(ce));
                continue;
            }
            const auto& dp = dp_it->second;
            if (!dp.min && !dp.max) continue;          // nothing to validate

            std::vector<ExpressionError> errs;
            auto v = e.evaluate_number(override_expr, n.source, errs);
            if (!v) {
                CompileError ce;
                ce.category = CompileError::Validation;
                std::string detail;
                if (!errs.empty()) detail = ": " + errs.front().message;
                ce.message  = "instance `" + inst.ref_name + "`: cannot evaluate"
                              " override `" + override_name + "=" + override_expr +
                              "`" + detail;
                ce.source   = n.source;
                errors.push_back(std::move(ce));
                continue;
            }
            if (dp.min && *v < *dp.min) {
                std::ostringstream oss;
                oss << "instance `" << inst.ref_name << "`: override `"
                    << override_name << "=" << *v << "` is below declared min "
                    << *dp.min;
                CompileError ce;
                ce.category = CompileError::Validation;
                ce.message  = oss.str();
                ce.source   = n.source;
                errors.push_back(std::move(ce));
            }
            if (dp.max && *v > *dp.max) {
                std::ostringstream oss;
                oss << "instance `" << inst.ref_name << "`: override `"
                    << override_name << "=" << *v << "` is above declared max "
                    << *dp.max;
                CompileError ce;
                ce.category = CompileError::Validation;
                ce.message  = oss.str();
                ce.source   = n.source;
                errors.push_back(std::move(ce));
            }
        }
    }
}

// All composition constructs (<for>, <pattern>, <assembly>, <connect>,
// mating instances) are now lowered by the bundler. Any remaining
// instance with at/port outside an assembly context is an error.
//
// Also reject `<extrude>` attributes that the v0.1 engine cannot
// honour: `scale`, `draft`, and `direction`. These were silently
// ignored in earlier builds; rejecting them up front prevents users
// from authoring tapered or downward extrudes that quietly render as
// straight upward ones.
void check_unsupported_constructs(const Document& doc,
                                    std::vector<CompileError>& errors) {
    for (const auto& n : doc.nodes) {
        if (n.dead) continue;
        // Mating instances should only appear inside <assembly>. After
        // assembly compilation, any surviving at/port is an error.
        if (n.type == NodeType::Instance) {
            const auto& inst = std::get<InstanceAttrs>(n.attrs);
            if (!inst.at.empty() || !inst.port.empty()) {
                CompileError e;
                e.category = CompileError::Composition;
                e.message  = std::string(
                    "instance `" + inst.ref_name +
                    "` has at/port outside of <assembly> context");
                e.source = n.source;
                errors.push_back(std::move(e));
            }
        }
        if (n.type == NodeType::Extrude) {
            const auto& a = std::get<ExtrudeAttrs>(n.attrs);
            auto reject = [&](std::string_view attr_name,
                               const std::string& value,
                               std::string_view hint) {
                CompileError e;
                e.category = CompileError::Schema;
                e.message  = std::string("<extrude ") + std::string(attr_name) +
                    "=\"" + value + "\"> is not supported in 0.1; " +
                    std::string(hint);
                e.source = n.source;
                errors.push_back(std::move(e));
            };
            // Accept any value that resolves to the default — `scale="1"`
            // and `scale="1.0"` both mean "no scale", and a literal-text
            // comparison would surprise authors. Expression-form values
            // ({…}) the bundler can't evaluate here are conservatively
            // rejected because the engine would silently ignore them.
            auto literal_equals = [](const std::string& expr, double want) {
                auto v = parse_double_strict(expr);
                return v.has_value() && *v == want;
            };
            auto direction_is_plus_z = [](std::string_view expr) {
                while (!expr.empty() && (expr.front() == ' ' || expr.front() == '\t'))
                    expr.remove_prefix(1);
                while (!expr.empty() && (expr.back() == ' ' || expr.back() == '\t'))
                    expr.remove_suffix(1);
                return expr == "+z" || expr == "z" ||
                       expr == "+Z" || expr == "Z";
            };
            if (!literal_equals(a.scale_expr, 1.0)) {
                reject("scale", a.scale_expr,
                    "remove the attribute or use <loft> between two "
                    "scaled profiles for a tapered extrude.");
            }
            if (!literal_equals(a.draft_expr, 0.0)) {
                reject("draft", a.draft_expr,
                    "remove the attribute or use <loft> between an "
                    "offset profile pair to produce a draft angle.");
            }
            if (!direction_is_plus_z(a.direction_expr)) {
                reject("direction", a.direction_expr,
                    "remove the attribute and wrap the extrude in a "
                    "<group transform=\"rotate(...)\"> to extrude along "
                    "a non-+z axis.");
            }
        }
    }
}

// ─── <def> cycle detection (spec §3 / Block 3 B3.6) ─────────────────
//
// A self-referential def — `<def name="a"><a/></def>` — or a mutual
// chain a→b→a expands forever. The flat evaluator already guards against
// this with a depth-64 recursion limit (it emits a warning and yields an
// empty mesh), so a cycle can never crash the renderer. But catching it
// in the bundler turns a silent empty mesh + runtime warning into a
// clean compile-time error pointing at the offending def — which is what
// the author actually needs.
//
// We build the def→def reference graph (a def D has an edge to def E iff
// D's body contains an instance ref resolving to E) and run a 3-colour
// DFS for a back-edge. Resolution mirrors validate_param_overrides and
// the engine's resolve_def_for_instance (closest named ancestor first,
// then the bare ref). Only def→def edges participate — a use-site
// reference from a <part> body is not part of the graph, so ordinary
// (acyclic) def composition is unaffected.
void check_def_cycles(const Document& doc, std::vector<CompileError>& errors) {
    auto closest_named_prefix = [&](std::uint32_t node_idx) -> std::string {
        auto cur = doc.nodes[node_idx].parent;
        while (cur != NO_NODE) {
            const auto& a = doc.nodes[cur];
            if (a.type == NodeType::Def)  return std::get<DefAttrs>(a.attrs).name;
            if (a.type == NodeType::Part) return std::get<PartAttrs>(a.attrs).name;
            cur = a.parent;
        }
        return {};
    };
    auto resolve_def = [&](std::uint32_t node_idx,
                            const std::string& ref) -> std::int64_t {
        const auto prefix = closest_named_prefix(node_idx);
        if (!prefix.empty()) {
            const auto it = doc.defs.find(prefix + "." + ref);
            if (it != doc.defs.end()) return static_cast<std::int64_t>(it->second);
        }
        const auto it = doc.defs.find(ref);
        if (it != doc.defs.end()) return static_cast<std::int64_t>(it->second);
        return -1;
    };
    auto closest_def_ancestor = [&](std::uint32_t node_idx) -> std::int64_t {
        auto cur = doc.nodes[node_idx].parent;
        while (cur != NO_NODE) {
            if (doc.nodes[cur].type == NodeType::Def)
                return static_cast<std::int64_t>(cur);
            cur = doc.nodes[cur].parent;
        }
        return -1;
    };

    // Adjacency over def node indices. Touch every def so isolated defs
    // are still visited (and so a self-loop on a never-referenced def is
    // detected if its own body references itself).
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> graph;
    for (std::uint32_t i = 0; i < doc.nodes.size(); ++i) {
        if (doc.nodes[i].dead) continue;
        if (doc.nodes[i].type == NodeType::Def) graph[i];
    }
    for (std::uint32_t i = 0; i < doc.nodes.size(); ++i) {
        const auto& n = doc.nodes[i];
        if (n.dead) continue;
        if (n.type != NodeType::Instance) continue;
        const auto src = closest_def_ancestor(i);
        if (src < 0) continue;        // use-site ref, not a def→def edge
        const auto& ia = std::get<InstanceAttrs>(n.attrs);
        const auto tgt = resolve_def(i, ia.ref_name);
        if (tgt < 0) continue;        // unresolved ref reported elsewhere
        graph[static_cast<std::uint32_t>(src)]
            .push_back(static_cast<std::uint32_t>(tgt));
    }

    // 3-colour DFS, iterative with an explicit stack — NOT recursion.
    // SECURITY: a linear chain of un-nested defs (a0→a1→…→aN) makes the
    // DFS go N deep, and N is attacker-controlled (one `<def>` per line
    // in a single file). A recursive lambda overflowed the call stack on
    // such input — ironically crashing inside the very check meant to
    // guard against pathological def graphs. The explicit stack keeps the
    // frontier on the heap, so depth is bounded by memory, not the call
    // stack.
    //
    // Each stack frame is (node, next-edge-index), mirroring a recursion
    // frame: a node stays Gray (on the path) while its edges are being
    // walked, and goes Black once all are exhausted — so a back-edge to a
    // Gray node is a cycle.
    enum Color { White, Gray, Black };
    std::unordered_map<std::uint32_t, Color> color;

    auto report_cycle = [&](std::uint32_t v) {
        const auto& dn = doc.nodes[v];
        const auto name = std::get<DefAttrs>(dn.attrs).name;
        CompileError ce;
        ce.category = CompileError::Composition;
        ce.message  =
            "cyclic <def> reference: `" + name + "` is reachable "
            "from its own body (a self-referential def chain). A "
            "CADML <def> may not reference itself directly or "
            "transitively — the expansion would never terminate.";
        ce.source = dn.source;
        errors.push_back(std::move(ce));
    };

    bool reported = false;
    std::vector<std::pair<std::uint32_t, std::size_t>> stack;  // (node, edge idx)
    // Iterate start nodes in ascending node-index order (source order)
    // so the FIRST cycle reported is the same on every platform.
    // `graph` is an unordered_map; hash-iteration order would otherwise
    // make the error message platform-dependent when a file has more
    // than one independent cycle.
    std::vector<std::uint32_t> start_nodes;
    start_nodes.reserve(graph.size());
    for (const auto& kv : graph) start_nodes.push_back(kv.first);
    std::sort(start_nodes.begin(), start_nodes.end());
    for (const auto start : start_nodes) {
        if (reported) break;
        if (color.count(start)) continue;          // already Black
        color[start] = Gray;
        stack.emplace_back(start, 0);
        while (!stack.empty() && !reported) {
            auto& [u, ei] = stack.back();
            const auto& edges = graph[u];
            if (ei < edges.size()) {
                const auto v = edges[ei++];          // advance this frame's cursor
                const Color cv = color.count(v) ? color[v] : White;
                if (cv == Gray) {                    // back-edge → cycle
                    report_cycle(v);
                    reported = true;
                } else if (cv == White) {
                    color[v] = Gray;
                    stack.emplace_back(v, 0);
                }
                // Black → already fully explored, skip.
            } else {
                color[u] = Black;                    // all edges done
                stack.pop_back();
            }
        }
    }
}

// ─── Selector syntax validation (spec §13 / Block 3 B3.1) ───────────
//
// Selector grammar is static, so its SYNTAX is validated here at compile
// time — a malformed `select` / `open` is a real, source-located error
// rather than something the engine discovers (and could only warn about)
// at render time. The worst-case failure mode was a bad selector
// silently falling back to "match everything"; rejecting it here
// removes that path entirely. Geometric outcomes (e.g. a well-formed selector
// that happens to match zero edges) remain engine-side warnings per
// spec §13.5.
void validate_selectors(const Document& doc, std::vector<CompileError>& errors) {
    auto err = [&](const std::string& msg, const SourceRange& src) {
        CompileError ce;
        ce.category = CompileError::Schema;
        ce.message  = msg;
        ce.source   = src;
        errors.push_back(std::move(ce));
    };

    // True iff `s` is the legacy shell keyword form: a non-empty list of
    // `start` / `end` tokens separated by spaces/commas.
    auto is_shell_keyword_form = [](const std::string& s) {
        std::size_t i = 0;
        bool any = false;
        auto is_sep = [](char c){ return c==','||c==' '||c=='\t'; };
        while (i < s.size()) {
            while (i < s.size() && is_sep(s[i])) ++i;
            const std::size_t ts = i;
            while (i < s.size() && !is_sep(s[i])) ++i;
            if (i > ts) {
                const auto tok = s.substr(ts, i - ts);
                if (tok != "start" && tok != "end") return false;
                any = true;
            }
        }
        return any;
    };

    for (const auto& n : doc.nodes) {
        if (n.dead) continue;
        if (n.type == NodeType::Fillet || n.type == NodeType::Chamfer) {
            const std::string& sel = (n.type == NodeType::Fillet)
                ? std::get<FilletAttrs>(n.attrs).select
                : std::get<ChamferAttrs>(n.attrs).select;
            const char* who = (n.type == NodeType::Fillet) ? "fillet" : "chamfer";
            auto sp = parse_selector(sel);
            if (!sp.ok) {
                err(std::string(who) + " select: " + sp.error, n.source);
            } else if (sp.selector.is_face()) {
                err(std::string(who) + " select expects an edge selector "
                    "(edge:…) or `all`, not a face selector", n.source);
            }
        } else if (n.type == NodeType::Shell) {
            const std::string& open = std::get<ShellAttrs>(n.attrs).open;
            if (open.empty()) continue;
            if (is_shell_keyword_form(open)) continue;   // legacy start/end
            auto sp = parse_selector(open);
            if (!sp.ok) {
                err("shell open: " + sp.error, n.source);
            } else if (sp.selector.is_all()) {
                err("shell open=\"all\" is invalid — it would remove every "
                    "face and leave an empty mesh (spec §12.4)", n.source);
            } else if (sp.selector.is_edge()) {
                err("shell open expects a face selector (face:…) or the "
                    "`start`/`end` keywords, not an edge selector", n.source);
            }
        }
    }
}

}  // namespace

// ─── Public API ──────────────────────────────────────────────────────

namespace {

// Parse the entry source into `result.document`, forwarding parse errors.
// Returns false (with errors populated) if parsing failed.
bool parse_entry(CompileResult& result, std::string_view source) {
    auto parsed = parse(source, /*file_id=*/0);
    for (auto& e : parsed.errors) {
        CompileError ce;
        // Map every ParseError::Category to its CompileError peer
        // (Expression / Validation otherwise silently collapse to
        // Parse, hiding the parser's intent from diagnostic tools).
        switch (e.category) {
            case ParseError::Vocabulary: ce.category = CompileError::Vocabulary; break;
            case ParseError::Schema:     ce.category = CompileError::Schema;     break;
            case ParseError::Validation: ce.category = CompileError::Validation; break;
            case ParseError::Expression: ce.category = CompileError::Schema;     break;
            case ParseError::Parse:      ce.category = CompileError::Parse;      break;
        }
        ce.message = e.message;
        ce.source  = e.source;
        result.errors.push_back(std::move(ce));
    }
    if (!parsed.ok()) return false;
    // Per spec §15.3: a 0.1.x compiler accepts `version 0.1` and
    // patch-level refinements (`0.1.0`, `0.1.1`, …) but rejects
    // any other declaration so a future 0.2 file can't silently parse
    // under 0.1 semantics. (Empty version is already rejected by the
    // parser when the file has any content; that case won't reach
    // here.)
    if (!parsed.document.meta.version.empty()) {
        const auto& v = parsed.document.meta.version;
        const bool ok =
            v == "0.1" ||
            (v.size() >= 4 && v.compare(0, 4, "0.1.") == 0);
        if (!ok) {
            CompileError ce;
            ce.category = CompileError::Schema;
            ce.message  = "unrecognized spec version `" + v +
                "` — this compiler implements 0.1.x only (see spec §15.3)";
            ce.source = {};
            result.errors.push_back(std::move(ce));
            return false;
        }
    }
    result.document = std::move(parsed.document);
    return true;
}

// Seed the entry as source_files[0] — UNLESS parse() already populated
// source_files from a <sources> block (a .fcadml round-trip), in which
// case the original table is preserved so src= back-references keep
// pointing at the user-authored paths. `entry_key` is the path recorded
// for the entry in the <sources> table.
void seed_entry_source(CompileResult& result, std::string_view source,
                        std::string_view entry_key) {
    if (result.document.source_files.empty()) {
        SourceFile entry_sf;
        entry_sf.id   = 0;
        entry_sf.path = std::string(entry_key);
        entry_sf.hash = content_hash(source);
        result.document.source_files = { entry_sf };
    }
}

// The provider-independent lowering pipeline: everything from the
// param-snapshot through serialization. Shared by compile_string
// (filesystem) and compile_in_memory (in-memory) — they differ only in
// HOW imports were resolved before this runs, not in what happens after.
void run_lowering(CompileResult& result, const CompileOptions& opts) {
    // Capture entry-file params before hoisting moves them into the body.
    const auto entry_params = result.document.params;

    // <for> + <pattern> unrolling. Both share one cumulative UnrollBudget
    // so a <for> nested in a <pattern> (or vice versa) charges the same
    // pool — stops two stacked steps=100000 loops blowing past the
    // per-loop caps to 1e10 nodes.
    detail::UnrollBudget unroll_budget;
    detail::unroll_for_elements(result.document, entry_params,
                                 unroll_budget, result.errors);
    detail::unroll_pattern_elements(result.document, entry_params,
                                     unroll_budget, result.errors);

    // Resolve Lua function-call attribute expressions eagerly (the flat
    // engine has no Lua bridge). Bare param refs without `(` are left
    // symbolic for the engine.
    detail::resolve_lua_calls(result.document, entry_params, result.errors);

    // Assemblies — after unrolling so the compiler sees a flat instance list.
    detail::compile_assemblies(result.document, entry_params,
                                 result.errors, result.warnings);

    // Validate param min/max against instance overrides BEFORE hoisting,
    // while entry_params is still the unmoved declaration list.
    validate_param_overrides(result.document, entry_params, result.errors);

    hoist_entry_params(result.document);
    check_unsupported_constructs(result.document, result.errors);

    // Reject self-referential / mutually-recursive defs at compile time
    // (the engine has a runtime depth guard too, but a source-located
    // compile error is the better authoring signal).
    check_def_cycles(result.document, result.errors);

    // Validate modifier selector syntax (spec §13) — a malformed
    // select/open is a real error, not a silent engine fallback.
    validate_selectors(result.document, result.errors);

    if (!result.ok()) return;

    SerializeOptions sopts;
    sopts.include_source_map = opts.include_source_map;
    result.flat_text = serialize(result.document, sopts);
}

}  // namespace

CompileResult compile_string(std::string_view source,
                              const fs::path& base_dir,
                              const CompileOptions& opts) {
    CompileResult result;
    if (!parse_entry(result, source)) return result;
    seed_entry_source(result, source, "<entry>");

    if (!base_dir.empty()) {
        // Filesystem-backed imports rooted at base_dir. The entry itself
        // was supplied as `source` (not read through the provider), and
        // it lives at the root, so its imports resolve relative to "".
        FilesystemProvider provider{ base_dir };
        ImportContext ctx;
        ctx.source_files = result.document.source_files;
        resolve_imports_into(result.document, ctx, provider,
                              /*importing_dir_key=*/"");
        result.document.source_files = std::move(ctx.source_files);
        for (auto& e : ctx.errors)   result.errors.push_back(std::move(e));
        for (auto& w : ctx.warnings) result.warnings.push_back(std::move(w));
    } else if (!result.document.imports.empty()) {
        // Single-file mode: imports declared but nowhere to resolve them.
        CompileError e;
        e.category = CompileError::Import;
        e.message  = "import directives present but no base directory"
                     " supplied — use compile_file, compile_in_memory,"
                     " or pass base_dir";
        result.errors.push_back(std::move(e));
        return result;
    }

    run_lowering(result, opts);
    return result;
}

CompileResult compile_in_memory(const std::vector<InMemoryFile>& files,
                                 std::string_view entry_path,
                                 const CompileOptions& opts) {
    CompileResult result;

    InMemoryProvider provider{ files };
    const std::string entry_key = normalize_key(entry_path);

    const auto entry_src = provider.read(entry_key);
    if (!entry_src.contents) {
        CompileError e;
        e.category = CompileError::Import;
        if (entry_src.too_large) {
            e.message = "entry file `" + std::string(entry_path) +
                "` exceeds the " +
                std::to_string(cadml::kMaxSourceBytes) +
                "-byte source-size limit";
        } else {
            e.message = "entry file `" + std::string(entry_path) +
                "` is not among the provided in-memory files";
        }
        result.errors.push_back(std::move(e));
        return result;
    }

    if (!parse_entry(result, *entry_src.contents)) return result;
    seed_entry_source(result, *entry_src.contents, entry_key);

    ImportContext ctx;
    ctx.source_files = result.document.source_files;
    // Register the entry as file 0 in both maps so a self-import or a
    // diamond back to the entry is detected / reuses id 0.
    ctx.visited.insert(entry_key);
    ctx.seen.emplace(entry_key, static_cast<SourceFileId>(0));
    resolve_imports_into(result.document, ctx, provider,
                          dir_key_of(entry_key));
    result.document.source_files = std::move(ctx.source_files);
    for (auto& e : ctx.errors)   result.errors.push_back(std::move(e));
    for (auto& w : ctx.warnings) result.warnings.push_back(std::move(w));

    run_lowering(result, opts);
    return result;
}

CompileResult compile_file(const fs::path& entry, const CompileOptions& opts) {
    CompileResult result;
    if (!fs::exists(entry)) {
        CompileError e;
        e.category = CompileError::Parse;
        e.message  = "cannot find entry file: " + entry.string();
        result.errors.push_back(std::move(e));
        return result;
    }
    bool too_large = false;
    const auto content_opt = read_file_to_string(entry, &too_large);
    if (!content_opt) {
        CompileError e;
        e.category = CompileError::Import;
        if (too_large) {
            e.message = "entry file `" + entry.string() +
                "` exceeds the " +
                std::to_string(cadml::kMaxSourceBytes) +
                "-byte source-size limit";
        } else {
            e.message = "cannot read entry file: " + entry.string();
        }
        result.errors.push_back(std::move(e));
        return result;
    }
    const auto& content = *content_opt;

    // Resolve the entry path to an absolute form before deriving the
    // import base directory. For a bare relative entry like
    // `cadmlc bolt.cadml`, fs::path::parent_path() returns "" and
    // compile_string then refuses every import with "no base_dir".
    // weakly_canonical anchors the relative entry to the current
    // working directory, so `./bolt.cadml` and `bolt.cadml` resolve
    // the same way an absolute path always did.
    std::error_code ec;
    auto canonical_entry = fs::weakly_canonical(entry, ec);
    if (ec) canonical_entry = fs::absolute(entry, ec);
    auto base_dir = canonical_entry.parent_path();
    if (base_dir.empty()) base_dir = fs::current_path();

    result = compile_string(content, base_dir, opts);

    // Replace the synthetic "<entry>" path with the real filename.
    if (!result.document.source_files.empty()) {
        result.document.source_files[0].path =
            entry.filename().generic_string();
        // Re-emit if source map is enabled (path went into the output).
        if (opts.include_source_map) {
            SerializeOptions sopts;
            sopts.include_source_map = true;
            result.flat_text = serialize(result.document, sopts);
        }
    }
    return result;
}

}  // namespace cadml::compile
