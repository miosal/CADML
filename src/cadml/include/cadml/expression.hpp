// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/types.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cadml {

// A diagnostic from the expression evaluator. Wrapped into ParseError /
// runtime errors by callers.
struct ExpressionError {
    std::string message;
    SourceRange source;
};

// Result of evaluating an expression. Type-erased into a small union;
// the engine and compiler interpret based on the attribute type they
// expect.
struct ExpressionValue {
    enum Kind { Number, String, Vector };
    Kind kind = Number;

    double number = 0;
    std::string str;
    Vec3 vector;

    static ExpressionValue from_number(double n) {
        ExpressionValue v; v.kind = Number; v.number = n; return v;
    }
    static ExpressionValue from_string(std::string s) {
        ExpressionValue v; v.kind = String; v.str = std::move(s); return v;
    }
    static ExpressionValue from_vector(Vec3 vec) {
        ExpressionValue v; v.kind = Vector; v.vector = vec; return v;
    }
};

// Callback invoked when the evaluator encounters a function call it
// doesn't recognise as a built-in. The callback may resolve to a Lua
// function via the LuaRuntime, or report failure (return nullopt).
using ExternalFunctionCall = std::function<
    std::optional<ExpressionValue>(
        std::string_view name,
        const std::vector<ExpressionValue>& args)>;

// Stateful expression evaluator. Carries a parameter scope and an
// optional external-function callback.
class ExpressionEvaluator {
public:
    ExpressionEvaluator();
    ~ExpressionEvaluator();

    ExpressionEvaluator(ExpressionEvaluator&&) noexcept;
    ExpressionEvaluator& operator=(ExpressionEvaluator&&) noexcept;
    ExpressionEvaluator(const ExpressionEvaluator&) = delete;
    ExpressionEvaluator& operator=(const ExpressionEvaluator&) = delete;

    // Bind a numeric parameter into scope. Subsequent expressions
    // referencing `name` resolve to `value`. Names follow CADML
    // kebab-case convention.
    void set_param(std::string name, double value);

    // Bulk variant for convenience.
    void set_params(const std::unordered_map<std::string, double>& params);

    // Clear all bound parameters.
    void clear_params();

    // Install the external-function callback. Used to bridge to Lua.
    void set_external_func(ExternalFunctionCall fn);

    // Evaluate an expression, returning a numeric value. Bare literals
    // (e.g. "10", "3.14") are handled. Bracketed expressions ("{a + 1}")
    // are parsed and evaluated.
    //
    // On error, returns nullopt and appends to `errors_out`.
    std::optional<double> evaluate_number(
        std::string_view expr,
        SourceRange source,
        std::vector<ExpressionError>& errors_out);

    // Evaluate as a vec3. Accepts:
    //   - axis aliases ("+z")
    //   - "x y z" literal triples
    //   - "{expr}" returning a vector
    std::optional<Vec3> evaluate_vector(
        std::string_view expr,
        SourceRange source,
        std::vector<ExpressionError>& errors_out);

    // Evaluate as a string. Accepts string literals and `{expr}`
    // returning a string (e.g. Lua-generated SVG path data).
    std::optional<std::string> evaluate_string(
        std::string_view expr,
        SourceRange source,
        std::vector<ExpressionError>& errors_out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cadml
