// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>
#include <cadml/engine/flat_check.hpp>
#include <cadml/engine/flat_evaluator.hpp>

#include "cli_panic.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

// Help text destination depends on context: explicit `--help` lands on
// stdout (the user asked for it); usage triggered by an arg error lands
// on stderr (the user did something wrong).
void print_usage(std::ostream& os) {
    os <<
        "cadmlcheck - CADML validation tool\n"
        "\n"
        "Usage:\n"
        "  cadmlcheck <entry.cadml> [options]\n"
        "\n"
        "Options:\n"
        "  -q, --quiet            Print only diagnostics; omit summary\n"
        "  --strict               Exit non-zero on warnings as well\n"
        "  --tolerance <volume>   Override interference-tolerance.\n"
        "                         Format matches the frontmatter directive\n"
        "                         (e.g., 0.01mm3, 1cm3, 1).\n"
        "  -h, --help             Show this help\n";
}

const char* compile_category_name(cadml::compile::CompileError::Category c) {
    using cadml::compile::CompileError;
    switch (c) {
        case CompileError::Parse:       return "parse";
        case CompileError::Schema:      return "schema";
        case CompileError::Vocabulary:  return "vocabulary";
        case CompileError::Import:      return "import";
        case CompileError::Validation:  return "validation";
        case CompileError::Composition: return "composition";
        case CompileError::Lua:         return "lua";
        case CompileError::Internal:    return "internal";
    }
    return "unknown";
}

void report_compile(const cadml::compile::CompileError& e,
                     const char* severity) {
    std::cerr << severity << "[" << compile_category_name(e.category) << "]";
    if (e.source.file != cadml::NO_FILE && e.source.line > 0) {
        std::cerr << " " << e.source.line << ":" << e.source.column;
    }
    std::cerr << " - " << e.message << "\n";
}

void report_eval(const cadml::engine::FlatEvalError& e,
                  const char* severity) {
    std::cerr << severity << "[engine]";
    if (e.source.file != cadml::NO_FILE && e.source.line > 0) {
        std::cerr << " " << e.source.line << ":" << e.source.column;
    }
    std::cerr << " - " << e.message << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    ::cadml::cli::install_panic_handler();
    fs::path entry;
    bool quiet  = false;
    bool strict = false;
    std::optional<std::string> tolerance_override;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(std::cout);
            return 0;
        }
        if (arg == "-q" || arg == "--quiet") {
            quiet = true;
            continue;
        }
        if (arg == "--strict") {
            strict = true;
            continue;
        }
        if (arg == "--tolerance") {
            if (i + 1 >= argc) {
                std::cerr << "error: --tolerance requires an argument\n";
                return 2;
            }
            tolerance_override = std::string(argv[++i]);
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option `" << arg << "`\n";
            print_usage(std::cerr);
            return 2;
        }
        if (entry.empty()) {
            entry = std::string(arg);
            continue;
        }
        std::cerr << "error: unexpected positional argument `" << arg << "`\n";
        return 2;
    }

    if (entry.empty()) {
        print_usage(std::cerr);
        return 2;
    }

    // ── Compile (handles parse, schema, D2, D3) ───────────────────────
    auto compile_result = cadml::compile::compile_file(entry);

    for (const auto& w : compile_result.warnings) report_compile(w, "warning");
    for (const auto& e : compile_result.errors)   report_compile(e, "error");

    if (!compile_result.ok()) {
        if (!quiet) {
            std::printf("\nFAILED - %zu compile error(s), %zu warning(s)\n",
                          compile_result.errors.size(),
                          compile_result.warnings.size());
        }
        return 1;
    }

    // ── Flat evaluation (mesh production) ─────────────────────────────
    auto eval_result = cadml::engine::evaluate_flat(compile_result.document);

    for (const auto& w : eval_result.warnings) report_eval(w, "warning");
    for (const auto& e : eval_result.errors)   report_eval(e, "error");

    if (!eval_result.ok()) {
        if (!quiet) {
            std::printf("\nFAILED - %zu eval error(s), %zu warning(s)\n",
                          eval_result.errors.size(),
                          eval_result.warnings.size());
        }
        return 1;
    }

    // ── D1/D4 — pairwise interference + tolerance + allow-pairs ──────
    cadml::engine::InterferenceOptions iopts;

    // Tolerance: --tolerance overrides the document directive.
    const auto& tolerance_text = tolerance_override
        ? *tolerance_override
        : compile_result.document.meta.interference_tolerance;
    if (auto t = cadml::parse_interference_tolerance(
            tolerance_text, compile_result.document.meta.units)) {
        iopts.tolerance = *t;
    } else {
        std::cerr << "error: invalid interference-tolerance `"
                   << tolerance_text << "`\n";
        return 2;
    }

    iopts.allow_pairs = compile_result.document.meta.allow_interference_pairs;

    auto interference =
        cadml::engine::check_interference(eval_result, iopts);

    for (const auto& e : interference.errors) {
        std::cerr << "error[interference] - " << e.message << "\n";
    }
    // Volume in document units cubed. We echo back `units^3` so the
    // user can see it without mental conversion. ASCII-only output —
    // legacy Windows consoles (cmd.exe with a non-UTF-8 codepage)
    // mangle non-ASCII bytes.
    const std::string vol_unit =
        compile_result.document.meta.units.empty()
            ? std::string("u^3")
            : compile_result.document.meta.units + "^3";
    for (const auto& r : interference.reports) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6g", r.volume);
        std::cerr << "warning[interference] - `" << r.part_a
                   << "` overlaps `" << r.part_b
                   << "` (volume " << buf << " " << vol_unit << ")\n";
    }

    // ── Summary + exit ────────────────────────────────────────────────
    //
    // Counts kept distinct so the summary line doesn't conflate
    // interference reports (the whole point of the tool) with
    // compile/eval warnings (which spec §14.3 categorises as
    // recoverable issues).
    const std::size_t total_warnings =
        compile_result.warnings.size() + eval_result.warnings.size();
    const std::size_t total_errors =
        compile_result.errors.size() +
        eval_result.errors.size() +
        interference.errors.size();
    const std::size_t interference_count = interference.reports.size();
    const bool clean = (total_errors == 0) &&
                        (total_warnings == 0) &&
                        (interference_count == 0);

    if (!quiet) {
        std::printf("\n%s - %zu error(s), %zu warning(s),"
                      " %zu interference report(s), %zu part(s)\n",
                      clean ? "OK" : "ISSUES",
                      total_errors, total_warnings,
                      interference_count,
                      eval_result.parts.size());
    }

    // Hard failure: any error, or any interference report (default
    // --tolerance=0 makes "any overlap" a failure; user opts out via
    // frontmatter `interference-tolerance` or `--tolerance`).
    if (total_errors > 0 || interference_count > 0) return 1;
    // Soft warnings (D2/D3, eval recoverables) only fail under --strict.
    if (strict && total_warnings > 0)               return 1;
    return 0;
}
