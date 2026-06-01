// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>

#include "cli_panic.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

void print_usage() {
    std::cerr <<
        "cadmlc — CADML 0.1 bundler\n"
        "\n"
        "Usage:\n"
        "  cadmlc <entry.cadml> [-o <output.fcadml>] [--strip-sources]\n"
        "\n"
        "Options:\n"
        "  -o <path>          Write output to <path> (default: stdout)\n"
        "  --strip-sources    Omit <sources> table and src= attributes\n"
        "  -h, --help         Show this help\n";
}

const char* category_name(cadml::compile::CompileError::Category c) {
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

void report_diagnostic(const cadml::compile::CompileError& e,
                        const std::string& severity) {
    std::cerr << severity << "[" << category_name(e.category) << "]";
    if (e.source.file != cadml::NO_FILE && e.source.line > 0) {
        std::cerr << " " << e.source.line << ":" << e.source.column;
    }
    std::cerr << " — " << e.message << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    ::cadml::cli::install_panic_handler();
    fs::path entry;
    fs::path output;
    cadml::compile::CompileOptions opts;
    bool wrote_to_stdout = true;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        }
        if (arg == "--strip-sources") {
            opts.include_source_map = false;
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
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option `" << arg << "`\n";
            print_usage();
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
        print_usage();
        return 2;
    }

    auto result = cadml::compile::compile_file(entry, opts);

    for (const auto& w : result.warnings) report_diagnostic(w, "warning");
    for (const auto& e : result.errors)   report_diagnostic(e, "error");

    if (!result.ok()) return 1;

    if (wrote_to_stdout) {
        std::cout << result.flat_text;
    } else {
        std::ofstream f(output, std::ios::binary);
        if (!f) {
            std::cerr << "error: cannot write to " << output << "\n";
            return 3;
        }
        f << result.flat_text;
    }
    return 0;
}
