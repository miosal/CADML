// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>
#include <cadml/engine/flat_analysis.hpp>
#include <cadml/engine/flat_evaluator.hpp>

#include "cli_panic.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    ::cadml::cli::install_panic_handler();
    fs::path entry;
    std::string only_part;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::cout <<
                "cadmltopo - per-element topology breakdown\n"
                "Usage: cadmltopo <entry.cadml> [-p NAME]\n";
            return 0;
        }
        if (arg == "-p" || arg == "--part") {
            if (i + 1 >= argc) { std::cerr << "error: --part requires arg\n"; return 2; }
            only_part = argv[++i]; continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option `" << arg << "`\n"; return 2;
        }
        if (entry.empty()) { entry = std::string(arg); continue; }
        std::cerr << "error: unexpected positional `" << arg << "`\n"; return 2;
    }
    if (entry.empty()) {
        std::cerr << "Usage: cadmltopo <entry.cadml> [-p NAME]\n"; return 2;
    }

    auto cr = cadml::compile::compile_file(entry);
    for (const auto& w : cr.warnings) std::fprintf(stderr, "warning: %s\n", w.message.c_str());
    for (const auto& e : cr.errors)   std::fprintf(stderr, "error: %s\n",   e.message.c_str());
    if (!cr.ok()) return 1;
    auto er = cadml::engine::evaluate_flat(cr.document);
    for (const auto& e : er.errors) std::fprintf(stderr, "error: %s\n", e.message.c_str());
    if (!er.ok()) return 1;
    if (er.parts.empty()) { std::fprintf(stderr, "(no parts)\n"); return 0; }

    // Use the document's `units` as the label suffix — flat_topology
    // returns raw doc-unit values so the labels need to follow.
    const std::string u  = cr.document.meta.units.empty()
        ? std::string{ "mm" } : cr.document.meta.units;
    const std::string u2 = u + "^2";
    const std::string u3 = u + "^3";

    bool any = false;
    for (const auto& p : er.parts) {
        if (!only_part.empty() && only_part != p.name) continue;
        any = true;
        auto t = cadml::engine::flat_topology(p.mesh, cr.document);

        std::printf("Part: %s\n", p.name.c_str());
        std::printf("  triangles      %llu\n",
                    static_cast<unsigned long long>(t.triangles));
        std::printf("  vertices       %llu\n",
                    static_cast<unsigned long long>(t.vertices));
        std::printf("  volume         %.2f %s\n", t.volume_mm3, u3.c_str());
        std::printf("  surface area   %.2f %s\n", t.surface_area_mm2, u2.c_str());
        std::printf("  bbox           %.1f x %.1f x %.1f %s\n",
                    t.bbox_max[0] - t.bbox_min[0],
                    t.bbox_max[1] - t.bbox_min[1],
                    t.bbox_max[2] - t.bbox_min[2],
                    u.c_str());

        if (!t.elements.empty()) {
            std::printf("  elements (%zu):\n", t.elements.size());
            // Per-element volume uses signed-tetrahedron integration:
            // a feature contributed inside a <difference> as a cutter
            // shows up with a negative volume (it removed material from
            // the part). Show the sign in the table and annotate so the
            // user doesn't read it as a tool bug.
            const std::string vol_hdr = "volume (" + u3 + ",± = cutter)";
            const std::string surf_hdr = "surface (" + u2 + ")";
            std::printf("    %-6s  %-12s  %-22s  %8s  %24s  %14s\n",
                        "id", "tag", "name", "tris",
                        vol_hdr.c_str(), surf_hdr.c_str());
            for (const auto& e : t.elements) {
                std::printf("    %-6u  %-12s  %-22s  %8llu  %+24.2f  %14.2f\n",
                            e.node_id, e.tag.c_str(),
                            e.name.empty() ? "(unnamed)" : e.name.c_str(),
                            static_cast<unsigned long long>(e.triangles),
                            e.volume_mm3, e.surface_area_mm2);
            }
        }
        std::printf("\n");
    }
    if (!only_part.empty() && !any) {
        std::fprintf(stderr, "error: no part named '%s'\n", only_part.c_str());
        return 1;
    }
    return 0;
}
