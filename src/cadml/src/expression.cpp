// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/expression.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace cadml {

namespace {

constexpr double kPi       = 3.14159265358979323846;
constexpr double kTau      = 6.28318530717958647692;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;

enum class TokenType {
    Number, Identifier,
    Plus, Minus, Star, Slash, Percent,
    LParen, RParen, Comma, Dot,
    End
};

struct Token {
    TokenType type;
    double number = 0;
    std::string ident;     // owned (we may join dotted segments)
};

// Thrown across the parser stack when a function returns a string. The
// numeric parser propagates it up so try_eval_string() can catch.
//
// TODO: replace exception-based string return with an EvalValue
// variant (double | string | Vec3 | error). Throwing across the
// recursive-descent stack pays unwind cost on every numeric eval and
// only the explicit `try_eval_string` caller observes the string —
// other call sites silently drop it.
struct StringResult : std::exception {
    std::string value;
    explicit StringResult(std::string v) : value(std::move(v)) {}
    const char* what() const noexcept override { return "string result"; }
};

class Tokenizer {
public:
    explicit Tokenizer(std::string_view s) : src_(s) {}

    Token next() {
        skip_whitespace();
        if (pos_ >= src_.size()) return { TokenType::End };

        const char ch = src_[pos_];
        switch (ch) {
            case '+': ++pos_; return { TokenType::Plus };
            case '-': ++pos_; return { TokenType::Minus };
            case '*': ++pos_; return { TokenType::Star };
            case '/': ++pos_; return { TokenType::Slash };
            case '%': ++pos_; return { TokenType::Percent };
            case '(': ++pos_; return { TokenType::LParen };
            case ')': ++pos_; return { TokenType::RParen };
            case ',': ++pos_; return { TokenType::Comma };
            case '.':
                // A standalone `.` is the dot operator; `.5` starts a
                // number. Distinguish by lookahead.
                if (pos_ + 1 < src_.size() &&
                    std::isdigit(static_cast<unsigned char>(src_[pos_ + 1]))) {
                    return read_number();
                }
                ++pos_; return { TokenType::Dot };
            default: break;
        }

        if (std::isdigit(static_cast<unsigned char>(ch))) return read_number();
        if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
            return read_identifier();
        }

        throw std::runtime_error(
            std::string("unexpected character '") + ch + "' in expression");
    }

private:
    std::string_view src_;
    std::size_t      pos_ = 0;

    void skip_whitespace() {
        while (pos_ < src_.size() &&
               std::isspace(static_cast<unsigned char>(src_[pos_]))) {
            ++pos_;
        }
    }

    Token read_number() {
        const std::size_t start = pos_;
        while (pos_ < src_.size() &&
               (std::isdigit(static_cast<unsigned char>(src_[pos_])) ||
                src_[pos_] == '.')) {
            ++pos_;
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) {
                ++pos_;
            }
            while (pos_ < src_.size() &&
                   std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                ++pos_;
            }
        }
        Token t{ TokenType::Number };
        // Route through cadml::parse_double_strict so the host's
        // LC_NUMERIC (e.g. de_DE comma-decimal) can't silently
        // truncate `0.5` to `0`. The tokenizer's char-class scan
        // above already validated the substring is a well-formed
        // number, so parse_double_strict should always succeed; on
        // a defensive overflow we fall back to 0 (the tokenizer has
        // no error channel, but the upstream evaluator will see the
        // 0 propagate).
        auto v = cadml::parse_double_strict(src_.substr(start, pos_ - start));
        t.number = v.value_or(0.0);
        return t;
    }

    Token read_identifier() {
        const std::size_t start = pos_;
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                ++pos_;
                continue;
            }
            // Hyphen joins identifier components only when followed by
            // alphanumeric — distinguishes "max-thickness" (one identifier)
            // from "a - b" (two identifiers + minus).
            if (c == '-' && pos_ + 1 < src_.size() &&
                std::isalnum(static_cast<unsigned char>(src_[pos_ + 1]))) {
                ++pos_;
                continue;
            }
            break;
        }
        Token t{ TokenType::Identifier };
        t.ident = std::string(src_.substr(start, pos_ - start));
        return t;
    }
};

class Parser {
public:
    Parser(std::string_view src,
           const std::unordered_map<std::string, double>& params,
           const ExternalFunctionCall& ext_func)
        : tok_(src), params_(params), ext_func_(ext_func) { advance(); }

    double parse_expression() {
        DepthGuard g(depth_);
        double result = parse_term();
        while (cur_.type == TokenType::Plus || cur_.type == TokenType::Minus) {
            const bool is_plus = cur_.type == TokenType::Plus;
            advance();
            const double rhs = parse_term();
            result = is_plus ? result + rhs : result - rhs;
        }
        return result;
    }

private:
    Tokenizer tok_;
    Token     cur_{};
    const std::unordered_map<std::string, double>& params_;
    const ExternalFunctionCall& ext_func_;

    // SECURITY: bound the recursive-descent parser's stack consumption.
    // Without this, an input like `{(((…1…)))}` with thousands of
    // open parens reliably SIGSEGVs the evaluator. 256 leaves ample
    // legitimate headroom (typical expressions are < 10 deep).
    static constexpr int kMaxDepth = 256;
    int depth_ = 0;

    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& depth) : d(depth) {
            if (++d > kMaxDepth) {
                throw std::runtime_error(
                    "expression nested too deeply (limit " +
                    std::to_string(kMaxDepth) + ")");
            }
        }
        ~DepthGuard() { --d; }
    };

    void advance() { cur_ = tok_.next(); }

    double parse_term() {
        DepthGuard g(depth_);
        double result = parse_unary();
        while (cur_.type == TokenType::Star ||
               cur_.type == TokenType::Slash ||
               cur_.type == TokenType::Percent) {
            const auto op = cur_.type;
            advance();
            const double rhs = parse_unary();
            switch (op) {
                case TokenType::Star:    result *= rhs; break;
                case TokenType::Slash:
                    if (rhs == 0.0) {
                        throw std::runtime_error(
                            "division by zero in expression "
                            "(IEEE-754 inf would silently propagate; "
                            "see docs/spec/expressions.md §9)");
                    }
                    result /= rhs;
                    break;
                case TokenType::Percent:
                    if (rhs == 0.0) {
                        throw std::runtime_error(
                            "modulo by zero in expression "
                            "(std::fmod(_, 0) is NaN; "
                            "see docs/spec/expressions.md §9)");
                    }
                    result = std::fmod(result, rhs);
                    break;
                default: break;
            }
        }
        return result;
    }

    double parse_unary() {
        DepthGuard g(depth_);
        if (cur_.type == TokenType::Minus) {
            advance();
            return -parse_unary();
        }
        if (cur_.type == TokenType::Plus) {
            advance();
            return parse_unary();
        }
        return parse_atom();
    }

    double parse_atom() {
        if (cur_.type == TokenType::Number) {
            const double v = cur_.number;
            advance();
            return v;
        }

        if (cur_.type == TokenType::Identifier) {
            std::string name = std::move(cur_.ident);
            advance();

            // Dotted path: identifier.identifier(...). For Lua module
            // references like `airfoils.naca(c, t, n)`. Join with `.`.
            while (cur_.type == TokenType::Dot) {
                advance();
                if (cur_.type != TokenType::Identifier) {
                    throw std::runtime_error(
                        "expected identifier after `.` in expression");
                }
                name += '.';
                name += cur_.ident;
                advance();
            }

            if (cur_.type == TokenType::LParen) {
                advance();
                std::vector<double> args;
                if (cur_.type != TokenType::RParen) {
                    args.push_back(parse_expression());
                    while (cur_.type == TokenType::Comma) {
                        advance();
                        args.push_back(parse_expression());
                    }
                }
                expect(TokenType::RParen);
                return call_function(name, args);
            }

            // Plain identifier reference.
            auto it = params_.find(name);
            if (it != params_.end()) return it->second;
            throw std::runtime_error("undefined identifier: " + name);
        }

        if (cur_.type == TokenType::LParen) {
            advance();
            const double v = parse_expression();
            expect(TokenType::RParen);
            return v;
        }

        throw std::runtime_error("unexpected token in expression");
    }

    void expect(TokenType t) {
        if (cur_.type != t) {
            throw std::runtime_error("expected token not found in expression");
        }
        advance();
    }

    double call_function(const std::string& name,
                          const std::vector<double>& args) {
        // Built-in math (degrees for trig).
        if (name == "sin"   && args.size() == 1) return std::sin(args[0] * kDegToRad);
        if (name == "cos"   && args.size() == 1) return std::cos(args[0] * kDegToRad);
        if (name == "tan"   && args.size() == 1) return std::tan(args[0] * kDegToRad);
        if (name == "asin"  && args.size() == 1) return std::asin(args[0]) * kRadToDeg;
        if (name == "acos"  && args.size() == 1) return std::acos(args[0]) * kRadToDeg;
        if (name == "atan2" && args.size() == 2) return std::atan2(args[0], args[1]) * kRadToDeg;
        if (name == "sqrt"  && args.size() == 1) return std::sqrt(args[0]);
        if (name == "abs"   && args.size() == 1) return std::abs(args[0]);
        if (name == "min"   && args.size() == 2) return std::min(args[0], args[1]);
        if (name == "max"   && args.size() == 2) return std::max(args[0], args[1]);
        if (name == "pow"   && args.size() == 2) return std::pow(args[0], args[1]);
        if (name == "floor" && args.size() == 1) return std::floor(args[0]);
        if (name == "ceil"  && args.size() == 1) return std::ceil(args[0]);
        if (name == "round" && args.size() == 1) return std::round(args[0]);

        // External function (Lua bridge). Returns optional ExpressionValue.
        if (ext_func_) {
            std::vector<ExpressionValue> ext_args;
            ext_args.reserve(args.size());
            for (double a : args) {
                ext_args.push_back(ExpressionValue::from_number(a));
            }
            auto result = ext_func_(name, ext_args);
            if (result) {
                if (result->kind == ExpressionValue::Number) return result->number;
                if (result->kind == ExpressionValue::String) {
                    throw StringResult(std::move(result->str));
                }
                if (result->kind == ExpressionValue::Vector) {
                    // Vectors don't implicitly become numbers; reject.
                    throw std::runtime_error(
                        "function `" + name + "` returned a vector in numeric context");
                }
            }
        }

        throw std::runtime_error("unknown function: " + name);
    }
};

// Evaluate a string that may contain `{...}` placeholders mixed with
// literal text. Returns the fully numeric resolution. Throws on error.
double eval_with_placeholders(
    std::string_view expr,
    const std::unordered_map<std::string, double>& params,
    const ExternalFunctionCall& ext_func)
{
    if (expr.empty()) throw std::runtime_error("empty expression");

    while (!expr.empty() && std::isspace(static_cast<unsigned char>(expr.front()))) {
        expr.remove_prefix(1);
    }
    while (!expr.empty() && std::isspace(static_cast<unsigned char>(expr.back()))) {
        expr.remove_suffix(1);
    }
    if (expr.empty()) throw std::runtime_error("empty expression");

    // Mixed-form: literal numbers or text with `{...}` blocks. We resolve
    // each block against the same param context, splice numeric results
    // into the string, then re-parse the result.
    if (expr.find('{') != std::string_view::npos) {
        std::string resolved;
        resolved.reserve(expr.size());
        std::size_t i = 0;
        while (i < expr.size()) {
            if (expr[i] == '{') {
                const std::size_t end = expr.find('}', i);
                if (end == std::string_view::npos) {
                    throw std::runtime_error("unclosed `{` in expression");
                }
                const auto inner = expr.substr(i + 1, end - i - 1);
                Parser p(inner, params, ext_func);
                const double val = p.parse_expression();
                // Locale-pinned formatter — `{0.1 + 0.2}` must always
                // re-emit as `0.3...`, never as `0,3...` even when the
                // host runs a comma-decimal locale.
                resolved += cadml::format_double_canonical(val, 15);
                i = end + 1;
                continue;
            }
            resolved += expr[i++];
        }
        Parser p(resolved, params, ext_func);
        return p.parse_expression();
    }

    Parser p(expr, params, ext_func);
    return p.parse_expression();
}

}  // namespace

// ─── Pimpl ───────────────────────────────────────────────────────────

struct ExpressionEvaluator::Impl {
    std::unordered_map<std::string, double> params;
    ExternalFunctionCall                    external;

    Impl() {
        params["pi"]  = kPi;
        params["tau"] = kTau;
    }
};

ExpressionEvaluator::ExpressionEvaluator()
    : impl_(std::make_unique<Impl>()) {}

ExpressionEvaluator::~ExpressionEvaluator() = default;

ExpressionEvaluator::ExpressionEvaluator(ExpressionEvaluator&&) noexcept = default;
ExpressionEvaluator& ExpressionEvaluator::operator=(ExpressionEvaluator&&) noexcept = default;

void ExpressionEvaluator::set_param(std::string name, double value) {
    impl_->params[std::move(name)] = value;
}

void ExpressionEvaluator::set_params(
    const std::unordered_map<std::string, double>& params)
{
    for (const auto& [k, v] : params) impl_->params[k] = v;
}

void ExpressionEvaluator::clear_params() {
    impl_->params.clear();
    impl_->params["pi"]  = kPi;
    impl_->params["tau"] = kTau;
}

void ExpressionEvaluator::set_external_func(ExternalFunctionCall fn) {
    impl_->external = std::move(fn);
}

std::optional<double> ExpressionEvaluator::evaluate_number(
    std::string_view expr,
    SourceRange source,
    std::vector<ExpressionError>& errors_out)
{
    try {
        return eval_with_placeholders(expr, impl_->params, impl_->external);
    } catch (const StringResult&) {
        errors_out.push_back({ "string result in numeric context", source });
    } catch (const std::exception& e) {
        errors_out.push_back({ e.what(), source });
    }
    return std::nullopt;
}

std::optional<Vec3> ExpressionEvaluator::evaluate_vector(
    std::string_view expr,
    SourceRange source,
    std::vector<ExpressionError>& errors_out)
{
    // Try axis-alias first (parse-time resolution sugar).
    if (auto v = parse_axis_alias(expr)) return *v;

    // Trim.
    while (!expr.empty() && std::isspace(static_cast<unsigned char>(expr.front()))) {
        expr.remove_prefix(1);
    }
    while (!expr.empty() && std::isspace(static_cast<unsigned char>(expr.back()))) {
        expr.remove_suffix(1);
    }
    if (expr.empty()) {
        errors_out.push_back({ "empty vector expression", source });
        return std::nullopt;
    }

    // "x y z" (with each component possibly a `{expr}` or literal).
    // Split on top-level whitespace; for components with embedded braces,
    // skip whitespace inside the braces.
    std::vector<std::string> components;
    {
        std::string cur;
        int depth = 0;
        for (char ch : expr) {
            if (ch == '{') ++depth;
            else if (ch == '}') --depth;
            if (depth == 0 && std::isspace(static_cast<unsigned char>(ch))) {
                if (!cur.empty()) {
                    components.push_back(std::move(cur));
                    cur.clear();
                }
                continue;
            }
            cur.push_back(ch);
        }
        if (!cur.empty()) components.push_back(std::move(cur));
    }

    if (components.size() != 3) {
        errors_out.push_back({
            "vector expression must have 3 components (got " +
            std::to_string(components.size()) + ")", source });
        return std::nullopt;
    }

    Vec3 out;
    double* fields[3] = { &out.x, &out.y, &out.z };
    for (std::size_t i = 0; i < 3; ++i) {
        try {
            *fields[i] = eval_with_placeholders(
                components[i], impl_->params, impl_->external);
        } catch (const std::exception& e) {
            errors_out.push_back({
                std::string("vector component ") + std::to_string(i) + ": " +
                e.what(), source });
            return std::nullopt;
        }
    }
    return out;
}

std::optional<std::string> ExpressionEvaluator::evaluate_string(
    std::string_view expr,
    SourceRange source,
    std::vector<ExpressionError>& errors_out)
{
    // If the expression is a `{...}` form, evaluate; if the result is a
    // string (e.g. cadml.path() output), return it. Otherwise return the
    // numeric result formatted as text.
    //
    // If the expression has no braces, treat the input as a literal
    // string (path data, etc.) — return as-is.
    if (expr.find('{') == std::string_view::npos) {
        return std::string(expr);
    }
    try {
        const double val = eval_with_placeholders(
            expr, impl_->params, impl_->external);
        // Same locale-pinned formatter the inline-placeholder path
        // uses, so `evaluate_string` and `eval_with_placeholders`
        // produce byte-identical numeric output regardless of host
        // LC_NUMERIC.
        return cadml::format_double_canonical(val, 15);
    } catch (const StringResult& sr) {
        return sr.value;
    } catch (const std::exception& e) {
        errors_out.push_back({ e.what(), source });
        return std::nullopt;
    }
}

}  // namespace cadml
