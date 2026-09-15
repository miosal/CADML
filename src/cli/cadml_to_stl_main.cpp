// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>
#include <cadml/engine/flat_evaluator.hpp>
#include <cadml/engine/flat_stl.hpp>

#include "cli_panic.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

void print_usage(std::ostream& os) {
    os <<
        "cadmlstl - CADML binary STL exporter\n"
        "\n"
        "Usage:\n"
        "  cadmlstl <entry.cadml> -o <out.stl> [options]\n"
        "\n"
        "Options:\n"
        "  -o <path>          STL output path (required)\n"
        "  --header <text>    80-byte STL header tag (default \"CADML\")\n"
        "  -p, --part NAME    Export only the named top-level part\n"
        "                     (repeatable; default: every part, merged)\n"
        "  -h, --help         Show this help\n";
}

}  // namespace

int main(int argc, char** argv) {
    ::cadml::cli::install_panic_handler();
    fs::path entry;
    fs::path output;
    std::string header = "CADML";
    std::vector<std::string> only_parts;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(std::cout);
            return 0;
        }
        if (arg == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "error: -o requires an argument\n";
                return 2;
            }
            output = argv[++i];
            continue;
        }
        if (arg == "-p" || arg == "--part") {
            if (i + 1 >= argc) {
                std::cerr << "error: --part requires an argument\n";
                return 2;
            }
            only_parts.emplace_back(argv[++i]);
            continue;
        }
        if (arg == "--header") {
            if (i + 1 >= argc) {
                std::cerr << "error: --header requires an argument\n";
                return 2;
            }
            header = argv[++i];
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

    if (entry.empty() || output.empty()) {
        print_usage(std::cerr);
        return 2;
    }

    auto cr = cadml::compile::compile_file(entry);
    for (const auto& w : cr.warnings)
        std::fprintf(stderr, "warning: %s\n", w.message.c_str());
    for (const auto& e : cr.errors)
        std::fprintf(stderr, "error: %s\n", e.message.c_str());
    if (!cr.ok()) return 1;

    auto er = cadml::engine::evaluate_flat(cr.document);
    for (const auto& w : er.warnings)
        std::fprintf(stderr, "warning: %s\n", w.message.c_str());
    for (const auto& e : er.errors)
        std::fprintf(stderr, "error: %s\n", e.message.c_str());
    if (!er.ok()) return 1;

    if (!only_parts.empty()) {
        // Keep only the requested parts, in document order. An unknown
        // name is an error (a typo would otherwise silently export nothing).
        std::vector<cadml::engine::FlatEvalResult::Part> kept;
        for (auto& p : er.parts) {
            for (const auto& want : only_parts) {
                if (p.name == want) { kept.push_back(std::move(p)); break; }
            }
        }
        for (const auto& want : only_parts) {
            bool found = false;
            for (const auto& p : kept) if (p.name == want) found = true;
            if (!found) {
                std::fprintf(stderr, "error: no part named `%s`\n", want.c_str());
                return 1;
            }
        }
        er.parts = std::move(kept);
    }

    if (er.parts.empty()) {
        std::fprintf(stderr,
            "error: no parts produced; nothing to export\n");
        return 1;
    }

    try {
        cadml::engine::write_stl_binary(er, output, header);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 3;
    }

    std::size_t total_tris = 0;
    for (const auto& p : er.parts) total_tris += p.mesh.triangle_count();
    std::printf("wrote %s - %zu triangles from %zu part(s)\n",
                  output.string().c_str(), total_tris, er.parts.size());
    return 0;
}
