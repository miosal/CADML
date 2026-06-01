// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>
#include <cadml/engine/flat_check.hpp>
#include <cadml/engine/flat_evaluator.hpp>

#include "cli_panic.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

void print_usage(std::ostream& os) {
    os <<
        "cadmlbuild - CADML 0.1 build wrapper (compile + check)\n"
        "\n"
        "Usage:\n"
        "  cadmlbuild <entry.cadml> [-o <out.fcadml>] [options]\n"
        "\n"
        "Options:\n"
        "  -o <path>           Output .fcadml path (default: stdout)\n"
        "  --strip-sources     Omit <sources> table and src= attrs\n"
        "  --strict            Treat warnings as failure\n"
        "  --tolerance <vol>   Override interference-tolerance\n"
        "                      (e.g., 0.01mm3, 1cm3, 1)\n"
        "  -q, --quiet         Suppress summary line\n"
        "  -h, --help          Show this help\n";
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
    fs::path output;
    bool wrote_to_stdout = true;
    bool quiet  = false;
    bool strict = false;
    std::optional<std::string> tolerance_override;
    cadml::compile::CompileOptions copts;

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
        if (arg == "--strip-sources") {
            copts.include_source_map = false;
            continue;
        }
        if (arg == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "error: -o requires an argument\n";
                return 2;
            }
            output = argv[++i];
            wrote_to_stdout = false;
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

    // ── Compile ───────────────────────────────────────────────────────
    auto cr = cadml::compile::compile_file(entry, copts);
    for (const auto& w : cr.warnings) report_compile(w, "warning");
    for (const auto& e : cr.errors)   report_compile(e, "error");
    if (!cr.ok()) {
        if (!quiet) {
            std::printf("\nFAILED - %zu compile error(s), %zu warning(s)\n",
                          cr.errors.size(), cr.warnings.size());
        }
        return 1;
    }

    // ── Flat evaluation ───────────────────────────────────────────────
    auto er = cadml::engine::evaluate_flat(cr.document);
    for (const auto& w : er.warnings) report_eval(w, "warning");
    for (const auto& e : er.errors)   report_eval(e, "error");
    if (!er.ok()) {
        if (!quiet) {
            std::printf("\nFAILED - %zu eval error(s), %zu warning(s)\n",
                          er.errors.size(), er.warnings.size());
        }
        return 1;
    }

    // ── Interference check (D1 + D4) ──────────────────────────────────
    cadml::engine::InterferenceOptions iopts;
    const auto& tolerance_text = tolerance_override
        ? *tolerance_override
        : cr.document.meta.interference_tolerance;
    if (auto t = cadml::parse_interference_tolerance(
            tolerance_text, cr.document.meta.units)) {
        iopts.tolerance = *t;
    } else {
        std::cerr << "error: invalid interference-tolerance `"
                   << tolerance_text << "`\n";
        return 2;
    }
    iopts.allow_pairs = cr.document.meta.allow_interference_pairs;

    auto ir = cadml::engine::check_interference(er, iopts);
    for (const auto& e : ir.errors) {
        std::cerr << "error[interference] - " << e.message << "\n";
    }
    const std::string vol_unit =
        cr.document.meta.units.empty()
            ? std::string("u^3")
            : cr.document.meta.units + "^3";
    for (const auto& r : ir.reports) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6g", r.volume);
        std::cerr << "warning[interference] - `" << r.part_a
                   << "` overlaps `" << r.part_b
                   << "` (volume " << buf << " " << vol_unit << ")\n";
    }

    // ── Decide whether to write fcadml ────────────────────────────────
    //
    // Safe-build policy: emit output ONLY when every check is clean.
    // A compile success that conceals interference would mislead a
    // downstream consumer; better to fail loudly and produce nothing.
    const std::size_t total_warnings =
        cr.warnings.size() + er.warnings.size();
    const std::size_t total_errors =
        cr.errors.size() + er.errors.size() + ir.errors.size();
    const std::size_t interference_count = ir.reports.size();
    const bool clean = (total_errors == 0) &&
                        (interference_count == 0) &&
                        (!strict || total_warnings == 0);

    if (!clean) {
        if (!quiet) {
            std::printf("\nISSUES - %zu error(s), %zu warning(s),"
                          " %zu interference report(s) -"
                          " no .fcadml written\n",
                          total_errors, total_warnings,
                          interference_count);
        }
        return 1;
    }

    // ── Write .fcadml ─────────────────────────────────────────────────
    if (wrote_to_stdout) {
        std::cout << cr.flat_text;
        std::cout.flush();
        if (!std::cout) {
            std::cerr << "error: write to stdout failed\n";
            return 3;
        }
    } else {
        std::ofstream f(output, std::ios::binary);
        if (!f) {
            std::cerr << "error: cannot write to "
                       << output.string() << "\n";
            return 3;
        }
        f << cr.flat_text;
        f.close();
        if (!f) {
            std::cerr << "error: write to " << output.string()
                       << " failed (disk full or permission?)\n";
            return 3;
        }
    }

    if (!quiet) {
        if (wrote_to_stdout) {
            std::printf("\nOK - %zu part(s) (.fcadml on stdout)\n",
                          er.parts.size());
        } else {
            std::printf("\nOK - %zu part(s), wrote %s\n",
                          er.parts.size(), output.string().c_str());
        }
    }
    return 0;
}
