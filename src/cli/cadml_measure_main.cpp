// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>
#include <cadml/engine/flat_analysis.hpp>
#include <cadml/engine/flat_evaluator.hpp>

#include "cli_panic.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

void print_usage(std::ostream& os) {
    os <<
        "cadmlmeasure - element-keyed measurement probes\n"
        "\n"
        "Usage:\n"
        "  cadmlmeasure <entry.cadml> [-p NAME] <probes...>\n"
        "\n"
        "Probes (repeatable):\n"
        "  --bbox ID                axis-aligned bbox of element\n"
        "  --min-distance A:B       min vertex-to-vertex distance\n"
        "  --mean-distance A:B      mean pairwise distance\n"
        "  --max-distance A:B       max vertex-to-vertex distance\n"
        "\n"
        "Options:\n"
        "  -p, --part NAME          run probes against this part\n"
        "                             (default: every part, first match wins)\n"
        "  -h, --help               show this help\n"
        "\n"
        "Tip: run cadmltopo first to find element ids.\n";
}

bool parse_pair(const std::string& spec,
                std::uint32_t& a, std::uint32_t& b) {
    const auto colon = spec.find(':');
    if (colon == std::string::npos) return false;
    try {
        a = static_cast<std::uint32_t>(std::stoul(spec.substr(0, colon)));
        b = static_cast<std::uint32_t>(std::stoul(spec.substr(colon + 1)));
        return true;
    } catch (...) { return false; }
}

}  // namespace

int main(int argc, char** argv) {
    ::cadml::cli::install_panic_handler();
    fs::path entry;
    std::string only_part;
    std::vector<cadml::engine::MeasureProbe> probes;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") { print_usage(std::cout); return 0; }
        if (arg == "-p" || arg == "--part") {
            if (i + 1 >= argc) { std::cerr << "error: --part requires arg\n"; return 2; }
            only_part = argv[++i]; continue;
        }
        if (arg == "--bbox") {
            if (i + 1 >= argc) { std::cerr << "error: --bbox requires ID\n"; return 2; }
            cadml::engine::MeasureProbe p;
            p.kind = cadml::engine::MeasureKind::Bbox;
            try { p.element_a = std::stoul(argv[++i]); }
            catch (...) { std::cerr << "error: --bbox needs an integer ID\n"; return 2; }
            probes.push_back(p);
            continue;
        }
        auto distance_kind = [&](cadml::engine::MeasureKind k, const char* name) -> bool {
            if (i + 1 >= argc) {
                std::cerr << "error: " << name << " requires A:B\n";
                return false;
            }
            cadml::engine::MeasureProbe p;
            p.kind = k;
            if (!parse_pair(argv[++i], p.element_a, p.element_b)) {
                std::cerr << "error: " << name << " expects A:B form\n";
                return false;
            }
            probes.push_back(p);
            return true;
        };
        if (arg == "--min-distance") {
            if (!distance_kind(cadml::engine::MeasureKind::DistanceMin,
                                "--min-distance")) return 2;
            continue;
        }
        if (arg == "--mean-distance") {
            if (!distance_kind(cadml::engine::MeasureKind::DistanceMean,
                                "--mean-distance")) return 2;
            continue;
        }
        if (arg == "--max-distance") {
            if (!distance_kind(cadml::engine::MeasureKind::DistanceMax,
                                "--max-distance")) return 2;
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option `" << arg << "`\n";
            print_usage(std::cerr); return 2;
        }
        if (entry.empty()) { entry = std::string(arg); continue; }
        std::cerr << "error: unexpected positional `" << arg << "`\n"; return 2;
    }

    if (entry.empty()) { print_usage(std::cerr); return 2; }
    if (probes.empty()) {
        std::cerr << "error: no probes specified — pass --bbox / --*-distance.\n"
                      "       run cadmltopo first to find element ids.\n";
        return 2;
    }

    auto cr = cadml::compile::compile_file(entry);
    for (const auto& w : cr.warnings) std::fprintf(stderr, "warning: %s\n", w.message.c_str());
    for (const auto& e : cr.errors)   std::fprintf(stderr, "error: %s\n",   e.message.c_str());
    if (!cr.ok()) return 1;
    auto er = cadml::engine::evaluate_flat(cr.document);
    for (const auto& e : er.errors) std::fprintf(stderr, "error: %s\n", e.message.c_str());
    if (!er.ok()) return 1;
    if (er.parts.empty()) { std::fprintf(stderr, "error: no parts produced\n"); return 1; }

    const std::string u = cr.document.meta.units.empty()
        ? std::string{ "mm" } : cr.document.meta.units;

    int failures = 0;
    for (std::size_t pi = 0; pi < probes.size(); ++pi) {
        const auto& p = probes[pi];

        // Run against every part; first part where the element_a (and
        // element_b for distance probes) is populated wins. Probes
        // target a single element by id; the part it lives in is
        // implicit.
        cadml::engine::MeasureItem hit;
        for (const auto& part : er.parts) {
            if (!only_part.empty() && only_part != part.name) continue;
            const auto r = cadml::engine::flat_measure(part.mesh, { p });
            if (!r.items.empty()) {
                hit = r.items[0];
                if (hit.ok) break;
            }
        }

        const char* label = hit.kind.c_str();
        if (!hit.ok) {
            std::printf("[%zu] %s element=%u : FAILED  (%s)\n",
                        pi, label, p.element_a,
                        hit.error.empty() ? "no triangles for element"
                                            : hit.error.c_str());
            ++failures;
            continue;
        }

        if (hit.kind == "bbox") {
            std::printf("[%zu] bbox      element=%u\n"
                        "      min  %.2f, %.2f, %.2f\n"
                        "      max  %.2f, %.2f, %.2f\n"
                        "      size %.2f x %.2f x %.2f %s\n",
                        pi, hit.element_a,
                        hit.bbox_min[0], hit.bbox_min[1], hit.bbox_min[2],
                        hit.bbox_max[0], hit.bbox_max[1], hit.bbox_max[2],
                        hit.size[0], hit.size[1], hit.size[2], u.c_str());
        } else {
            std::printf("[%zu] %-13s element_a=%u element_b=%u\n"
                        "      distance %.4f %s   over %llu pair(s)\n",
                        pi, label, hit.element_a, hit.element_b,
                        hit.distance, u.c_str(),
                        static_cast<unsigned long long>(hit.pair_count));
        }
    }
    return failures == 0 ? 0 : 1;
}
