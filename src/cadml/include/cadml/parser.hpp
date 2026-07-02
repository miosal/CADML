// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/types.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cadml {

// One diagnostic produced during parsing. Categories track spec §14.1.
struct ParseError {
    enum Category {
        Parse,        // malformed XML / frontmatter syntax / I/O / encoding
        Schema,       // valid syntax, invalid attributes/structure
        Vocabulary,   // unknown element, name collision (single-file only)
        Expression,   // expression syntax error (validated lazily)
        Validation,   // param min/max violation against declared default
    };

    Category category = Parse;
    std::string message;
    SourceRange source;
};

// Returned by parse(). On success, `errors` is empty and `document` is
// valid. On failure, `errors` lists every recoverable error encountered;
// `document` may be partial.
struct [[nodiscard]] ParseResult {
    Document document;
    std::vector<ParseError> errors;
    std::vector<ParseError> warnings;

    // True iff `errors` is empty (warnings do not affect ok()).
    [[nodiscard]] bool ok() const { return errors.empty(); }
};

// Parse a CADML source string. The `file_id` is recorded on every
// SourceRange in the resulting AST; callers pass 0 for the entry file
// or the appropriate index for imported files.
[[nodiscard]] ParseResult parse(std::string_view source, SourceFileId file_id = 0);

// Convenience: read file from disk, then parse. Sets up source_files[0]
// with the file's path and SHA-256 hash.
[[nodiscard]] ParseResult parse_file(const std::filesystem::path& path);

// ─── Sub-stage parsers (exposed for testing) ──────────────────────────

// Parses only the frontmatter region of a source string. Returns a
// partial Document containing meta/imports/params, plus the byte offset
// where the body begins (first `<` after stripping leading whitespace,
// or end-of-source if no body).
struct [[nodiscard]] FrontmatterResult {
    DocumentMeta meta;
    std::vector<ImportDecl> imports;
    std::vector<ParamDecl> params;
    std::size_t body_offset = 0;     // byte index where body begins
    std::vector<ParseError> errors;
    std::vector<ParseError> warnings;

    // True iff the user explicitly set `version` (vs. the default initial
    // value in `meta.version`). Used by parse() to enforce spec §3.3's
    // "version is required" rule when the file has any content.
    bool version_explicitly_set = false;

    [[nodiscard]] bool ok() const { return errors.empty(); }
};

[[nodiscard]] FrontmatterResult parse_frontmatter(std::string_view source,
                                    SourceFileId file_id = 0);

// Parses only the body region of a source string, given the frontmatter
// already parsed. The caller is responsible for splitting at body_offset
// before invoking. Returns a Document whose `meta`, `imports`, `params`
// are uninitialised — only `nodes`, `defs`, `exports` are populated.
struct [[nodiscard]] BodyResult {
    std::vector<Node> nodes;
    std::unordered_map<std::string, std::uint32_t> defs;
    std::unordered_map<std::string, std::uint32_t> exports;
    std::vector<ParseError> errors;
    std::vector<ParseError> warnings;

    [[nodiscard]] bool ok() const { return errors.empty(); }
};

// `spec` selects the reserved built-in set the body is validated
// against (§15.2 version pinning) — parse() passes the document's
// declared version; the default is the latest spec, for tooling that
// parses bare bodies without frontmatter context.
[[nodiscard]] BodyResult parse_body(std::string_view body, SourceFileId file_id = 0,
                      std::uint32_t body_line_offset = 1,
                      SpecVersion spec = kSpecLatest);

}  // namespace cadml
