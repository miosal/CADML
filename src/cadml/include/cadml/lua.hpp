// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/types.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cadml {

// Diagnostic from Lua loading or execution.
struct LuaError {
    std::string message;
    SourceRange source;        // points at the originating CADML node
                                // (script element or import directive)
};

// Stateful Lua runtime. Holds one Lua state per Document. Sandboxed.
class LuaRuntime {
public:
    LuaRuntime();
    ~LuaRuntime();

    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    // Bind CADML params into Lua scope. Names are auto-translated from
    // kebab-case to snake_case at the boundary.
    void set_params(const std::unordered_map<std::string, double>& params);

    // Load all inline <script lang="lua"> elements from `doc` into the
    // shared scope. Top-level functions in inline scripts are visible
    // by their bare name in expression scope.
    std::vector<LuaError> load_inline_scripts(const Document& doc);

    // Load a Lua module file under the given alias. The file's source
    // text is supplied directly (the import resolver already read it).
    // Determines free-form vs module-pattern by checking for a
    // terminating `return <table>` statement.
    //
    // After successful load, the alias is bound in the shared scope as
    // a table containing the module's exports.
    std::vector<LuaError> load_module(std::string_view source,
                                       std::string_view alias,
                                       SourceRange source_range);

    // Try to call a Lua function by name. The function may be a top-
    // level function from an inline script, a module function reached
    // via dotted path ("airfoils.naca"), or a built-in (`cadml.path`,
    // `cadml.param`).
    //
    // Returns nullopt if the name doesn't resolve OR the call errored.
    // Errors are appended to `errors_out`.
    std::optional<double> try_call_number(
        std::string_view name,
        const std::vector<double>& args,
        SourceRange source,
        std::vector<LuaError>& errors_out);

    // Variant returning a string (e.g. cadml.path() result).
    std::optional<std::string> try_call_string(
        std::string_view name,
        const std::vector<double>& args,
        SourceRange source,
        std::vector<LuaError>& errors_out);

    // Reset the runtime to its initial sandboxed state. Used between
    // independent compilations.
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cadml
