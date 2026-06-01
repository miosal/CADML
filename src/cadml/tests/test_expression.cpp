// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/expression.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace cadml;

namespace {

constexpr double kEps = 1e-9;

double eval_n(ExpressionEvaluator& e, std::string_view expr) {
    std::vector<ExpressionError> errs;
    auto v = e.evaluate_number(expr, {}, errs);
    EXPECT_TRUE(v.has_value()) << (errs.empty() ? "" : errs[0].message);
    return v.value_or(0);
}

bool eval_n_fails(ExpressionEvaluator& e, std::string_view expr) {
    std::vector<ExpressionError> errs;
    auto v = e.evaluate_number(expr, {}, errs);
    return !v.has_value();
}

}  // namespace

// ─── Numeric basics ──────────────────────────────────────────────────

TEST(Expression, BareInteger) {
    ExpressionEvaluator e;
    EXPECT_NEAR(eval_n(e, "10"), 10.0, kEps);
}

TEST(Expression, BareDecimal) {
    ExpressionEvaluator e;
    EXPECT_NEAR(eval_n(e, "3.14"), 3.14, kEps);
}

TEST(Expression, NegativeLiteral) {
    ExpressionEvaluator e;
    EXPECT_NEAR(eval_n(e, "-5"), -5.0, kEps);
}

TEST(Expression, ScientificNotation) {
    ExpressionEvaluator e;
    EXPECT_NEAR(eval_n(e, "1e3"), 1000.0, kEps);
    EXPECT_NEAR(eval_n(e, "2.5e-2"), 0.025, kEps);
}

TEST(Expression, AdditionAndSubtraction) {
    ExpressionEvaluator e;
    EXPECT_NEAR(eval_n(e, "{1 + 2}"), 3.0, kEps);
    EXPECT_NEAR(eval_n(e, "{10 - 4 - 3}"), 3.0, kEps);
}

TEST(Expression, MultiplicationAndDivision) {
    ExpressionEvaluator e;
    EXPECT_NEAR(eval_n(e, "{6 * 7}"), 42.0, kEps);
    EXPECT_NEAR(eval_n(e, "{20 / 4 / 5}"), 1.0, kEps);
}

TEST(Expression, Modulo) {
    ExpressionEvaluator e;
    EXPECT_NEAR(eval_n(e, "{10 % 3}"), 1.0, kEps);
}

TEST(Expression, OperatorPrecedence) {
    ExpressionEvaluator e;
    EXPECT_NEAR(eval_n(e, "{2 + 3 * 4}"), 14.0, kEps);
    EXPECT_NEAR(eval_n(e, "{(2 + 3) * 4}"), 20.0, kEps);
}

TEST(Expression, UnaryMinus) {
    ExpressionEvaluator e;
    EXPECT_NEAR(eval_n(e, "{-5}"), -5.0, kEps);
    EXPECT_NEAR(eval_n(e, "{-(2 + 3)}"), -5.0, kEps);
    EXPECT_NEAR(eval_n(e, "{--5}"), 5.0, kEps);
}

// ─── Mixed-form expressions (literal + braces) ──────────────────────

TEST(Expression, LiteralWithLeadingMinusAndBraces) {
    ExpressionEvaluator e;
    e.set_param("w", 10);
    EXPECT_NEAR(eval_n(e, "-{w/2}"), -5.0, kEps);
}

TEST(Expression, MultipleBraceBlocks) {
    ExpressionEvaluator e;
    e.set_param("a", 3);
    e.set_param("b", 4);
    EXPECT_NEAR(eval_n(e, "{a} + {b}"), 7.0, kEps);
}

TEST(Expression, LiteralAndBracesMixed) {
    ExpressionEvaluator e;
    e.set_param("x", 2);
    EXPECT_NEAR(eval_n(e, "10 + {x*2} - 1"), 13.0, kEps);
}

// ─── Parameter binding ──────────────────────────────────────────────

TEST(Expression, ParamReference) {
    ExpressionEvaluator e;
    e.set_param("chord", 100);
    EXPECT_NEAR(eval_n(e, "{chord}"), 100.0, kEps);
}

TEST(Expression, KebabCaseParam) {
    ExpressionEvaluator e;
    e.set_param("max-thickness", 0.12);
    EXPECT_NEAR(eval_n(e, "{max-thickness}"), 0.12, kEps);
}

TEST(Expression, ParamArithmetic) {
    ExpressionEvaluator e;
    e.set_param("d", 10);
    EXPECT_NEAR(eval_n(e, "{d / 2}"), 5.0, kEps);
}

TEST(Expression, BulkParamSet) {
    ExpressionEvaluator e;
    e.set_params({{"a", 3}, {"b", 4}});
    EXPECT_NEAR(eval_n(e, "{a*a + b*b}"), 25.0, kEps);
}

TEST(Expression, BuiltinPi) {
    ExpressionEvaluator e;
    EXPECT_NEAR(eval_n(e, "{pi}"), 3.14159265358979323846, 1e-12);
}

TEST(Expression, BuiltinTau) {
    ExpressionEvaluator e;
    EXPECT_NEAR(eval_n(e, "{tau}"), 6.28318530717958647692, 1e-12);
}

TEST(Expression, UndefinedParamErrors) {
    ExpressionEvaluator e;
    EXPECT_TRUE(eval_n_fails(e, "{undefined}"));
}

// ─── Built-in math functions ────────────────────────────────────────

TEST(Expression, TrigInDegrees) {
    ExpressionEvaluator e;
    EXPECT_NEAR(eval_n(e, "{sin(90)}"), 1.0, kEps);
    EXPECT_NEAR(eval_n(e, "{cos(0)}"),  1.0, kEps);
    EXPECT_NEAR(eval_n(e, "{cos(180)}"), -1.0, kEps);
}

TEST(Expression, InverseTrigReturnsDegrees) {
    ExpressionEvaluator e;
    EXPECT_NEAR(eval_n(e, "{asin(1)}"), 90.0, kEps);
    EXPECT_NEAR(eval_n(e, "{atan2(1, 1)}"), 45.0, kEps);
}

TEST(Expression, SqrtAndPow) {
    ExpressionEvaluator e;
    EXPECT_NEAR(eval_n(e, "{sqrt(16)}"), 4.0, kEps);
    EXPECT_NEAR(eval_n(e, "{pow(2, 10)}"), 1024.0, kEps);
}

TEST(Expression, MinMaxAbs) {
    ExpressionEvaluator e;
    EXPECT_NEAR(eval_n(e, "{min(5, 3)}"), 3.0, kEps);
    EXPECT_NEAR(eval_n(e, "{max(5, 3)}"), 5.0, kEps);
    EXPECT_NEAR(eval_n(e, "{abs(-7)}"),   7.0, kEps);
}

TEST(Expression, FloorCeilRound) {
    ExpressionEvaluator e;
    EXPECT_NEAR(eval_n(e, "{floor(2.7)}"), 2.0, kEps);
    EXPECT_NEAR(eval_n(e, "{ceil(2.1)}"),  3.0, kEps);
    EXPECT_NEAR(eval_n(e, "{round(2.5)}"), 3.0, kEps);
}

TEST(Expression, UnknownFunctionErrors) {
    ExpressionEvaluator e;
    EXPECT_TRUE(eval_n_fails(e, "{nope(1, 2)}"));
}

// ─── Dotted-path identifiers ────────────────────────────────────────

TEST(Expression, DottedFunctionViaExternal) {
    ExpressionEvaluator e;
    e.set_external_func([](std::string_view name,
                            const std::vector<ExpressionValue>& args) {
        if (name == "airfoils.naca") {
            EXPECT_EQ(args.size(), 3u);
            return std::optional{ExpressionValue::from_number(42.0)};
        }
        return std::optional<ExpressionValue>{};
    });
    EXPECT_NEAR(eval_n(e, "{airfoils.naca(5, 0.12, 40)}"), 42.0, kEps);
}

TEST(Expression, DottedThreeLevels) {
    ExpressionEvaluator e;
    e.set_external_func([](std::string_view name,
                            const std::vector<ExpressionValue>&) {
        if (name == "a.b.c") return std::optional{ExpressionValue::from_number(7.0)};
        return std::optional<ExpressionValue>{};
    });
    EXPECT_NEAR(eval_n(e, "{a.b.c()}"), 7.0, kEps);
}

// ─── Vector expressions ─────────────────────────────────────────────

TEST(Expression, VectorAxisAlias) {
    ExpressionEvaluator e;
    std::vector<ExpressionError> errs;
    auto v = e.evaluate_vector("+z", {}, errs);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->z, 1.0);
}

TEST(Expression, VectorLiteralTriple) {
    ExpressionEvaluator e;
    std::vector<ExpressionError> errs;
    auto v = e.evaluate_vector("1 2 3", {}, errs);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->x, 1.0);
    EXPECT_EQ(v->y, 2.0);
    EXPECT_EQ(v->z, 3.0);
}

TEST(Expression, VectorWithExpressionComponent) {
    ExpressionEvaluator e;
    e.set_param("h", 10);
    std::vector<ExpressionError> errs;
    auto v = e.evaluate_vector("0 0 {h}", {}, errs);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->z, 10.0);
}

TEST(Expression, VectorWrongComponentCountErrors) {
    ExpressionEvaluator e;
    std::vector<ExpressionError> errs;
    auto v = e.evaluate_vector("1 2", {}, errs);
    EXPECT_FALSE(v.has_value());
    EXPECT_EQ(errs.size(), 1u);
}

// ─── String expressions ────────────────────────────────────────────

TEST(Expression, LiteralStringPassthrough) {
    ExpressionEvaluator e;
    std::vector<ExpressionError> errs;
    auto v = e.evaluate_string("M 0,0 L 10,0 Z", {}, errs);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "M 0,0 L 10,0 Z");
}

TEST(Expression, StringFromExternalFunc) {
    ExpressionEvaluator e;
    e.set_external_func([](std::string_view name,
                            const std::vector<ExpressionValue>&) {
        if (name == "naca") {
            return std::optional{ExpressionValue::from_string("M 0,0 L 1,1 Z")};
        }
        return std::optional<ExpressionValue>{};
    });
    std::vector<ExpressionError> errs;
    auto v = e.evaluate_string("{naca()}", {}, errs);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "M 0,0 L 1,1 Z");
}

TEST(Expression, NumberFormattedAsString) {
    ExpressionEvaluator e;
    std::vector<ExpressionError> errs;
    auto v = e.evaluate_string("{2 + 3}", {}, errs);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "5");
}

// ─── Edge cases ─────────────────────────────────────────────────────

TEST(Expression, EmptyExpressionFails) {
    ExpressionEvaluator e;
    EXPECT_TRUE(eval_n_fails(e, ""));
    EXPECT_TRUE(eval_n_fails(e, "   "));
}

TEST(Expression, UnclosedBraceFails) {
    ExpressionEvaluator e;
    EXPECT_TRUE(eval_n_fails(e, "{1 + 2"));
}

TEST(Expression, WhitespaceInsideBracesIgnored) {
    ExpressionEvaluator e;
    e.set_param("a", 5);
    EXPECT_NEAR(eval_n(e, "{a*2}"),    10.0, kEps);
    EXPECT_NEAR(eval_n(e, "{a * 2}"),  10.0, kEps);
    EXPECT_NEAR(eval_n(e, "{ a * 2 }"), 10.0, kEps);
}

TEST(Expression, ClearParamsRetainsBuiltins) {
    ExpressionEvaluator e;
    e.set_param("custom", 1);
    e.clear_params();
    EXPECT_TRUE(eval_n_fails(e, "{custom}"));
    EXPECT_NEAR(eval_n(e, "{pi}"), 3.14159265358979, 1e-12);
}
