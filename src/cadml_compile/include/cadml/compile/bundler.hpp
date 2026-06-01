// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/types.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cadml::compile {

// One diagnostic from the bundler. Categories track spec §14.1 plus a
// few bundler-specific ones.
struct CompileError {
    enum Category {
        Parse,           // forwarded from libcadml parser
        Schema,          // forwarded from libcadml parser
        Vocabulary,      // forwarded from libcadml parser
        Import,          // missing file, cycle, dispatch failure
        Validation,      // param min/max violation
        Composition,     // assembly / for / pattern / cut not yet supported
        Lua,             // script load / call failure
        Internal,        // bundler bug indicator
    };

    Category    category = Parse;
    std::string message;
    SourceRange source;
};

struct CompileOptions {
    // If true, emit `src=` attributes and the `<sources>` table (default).
    // `cadml_compile --strip-sources` sets this false for production
    // payloads where source mapping is not needed (spec §10.8).
    bool include_source_map = true;
};

struct [[nodiscard]] CompileResult {
    // Flat CADML text — empty on failure.
    std::string flat_text;

    // The compiled Document (post-lowering). Useful for tools that want
    // structured access without re-parsing the flat text.
    Document document;

    std::vector<CompileError> errors;
    std::vector<CompileError> warnings;

    [[nodiscard]] bool ok() const { return errors.empty(); }
};

// Read entry file from disk, resolve all imports relative to the file's
// directory, and emit `.fcadml` text.
[[nodiscard]] CompileResult compile_file(const std::filesystem::path& entry,
                            const CompileOptions& opts = {});

// In-memory variant. `base_dir` is used to resolve relative import paths
// (and `ctl/...` catalogue paths) against the real filesystem. Pass an
// empty path to disable import resolution (single-file only).
[[nodiscard]] CompileResult compile_string(std::string_view source,
                              const std::filesystem::path& base_dir = {},
                              const CompileOptions& opts = {});

// One source file in a fully in-memory project — the unit passed to
// `compile_in_memory`. `path` is a forward-slash relative path (the key
// imports resolve against, e.g. "shared/bolt.cadml"); `contents` is the
// raw file text. Distinct from `cadml::SourceFile` (the id/path/hash
// source-map entry that lands in `.fcadml`).
struct InMemoryFile {
    std::string path;
    std::string contents;
};

// Fully in-memory compile: no filesystem access at all. `files` is the
// complete project (entry + every importable .cadml/.lua), keyed by
// relative path; `entry_path` selects the entry within `files`. Imports
// resolve by lookup in `files` — an import not present yields the same
// "cannot find imported file" error as a missing file on disk. Path
// traversal is structurally impossible (you can only reach keys in the
// map). This is the entry point a WebAssembly / sandboxed host uses:
// marshal the project in, get `.fcadml` (or errors) out, no syscalls.
[[nodiscard]] CompileResult compile_in_memory(
    const std::vector<InMemoryFile>& files,
    std::string_view entry_path,
    const CompileOptions& opts = {});

}  // namespace cadml::compile
