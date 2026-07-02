// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>
#include <cadml/engine/flat_3mf.hpp>
#include <cadml/engine/flat_evaluator.hpp>

#include "cli_panic.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

void print_usage(std::ostream& os) {
    os <<
        "cadml3mf - CADML 3MF exporter\n"
        "\n"
        "Usage:\n"
        "  cadml3mf <entry.cadml> -o <out.3mf> [options]\n"
        "\n"
        "Options:\n"
        "  -o <path>         3MF output path (required)\n"
        "  --title <text>    Title metadata (default: entry filename stem)\n"
        "  --units <unit>    Override units (mm/cm/m/in/ft);\n"
        "                    default uses the document's `units` setting\n"
        "  -h, --help        Show this help\n";
}

}  // namespace

int main(int argc, char** argv) {
    ::cadml::cli::install_panic_handler();
    fs::path entry;
    fs::path output;
    std::string title;
    std::string units_override;
    bool title_set = false;

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
        if (arg == "--title") {
            if (i + 1 >= argc) {
                std::cerr << "error: --title requires an argument\n";
                return 2;
            }
            title = argv[++i];
            title_set = true;
            continue;
        }
        if (arg == "--units") {
            if (i + 1 >= argc) {
                std::cerr << "error: --units requires an argument\n";
                return 2;
            }
            units_override = argv[++i];
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

    if (er.parts.empty()) {
        std::fprintf(stderr,
            "error: no parts produced; nothing to export\n");
        return 1;
    }

    cadml::engine::ThreeMfOptions opts;
    // Authored units win unless overridden by --units. The DocumentMeta
    // default is "mm" (matching ThreeMfOptions default), so passing the
    // value through always lands on a writer-recognised string.
    opts.units = units_override.empty()
        ? cr.document.meta.units
        : units_override;
    opts.title = title_set ? title : entry.stem().string();

    try {
        cadml::engine::write_3mf(er, output, opts);
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
