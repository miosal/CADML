// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>
#include <cadml/engine/flat_analysis.hpp>

#include "cli_panic.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    ::cadml::cli::install_panic_handler();
    fs::path entry;
    bool csv = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::cout <<
                "cadmlholes - cylindrical-feature inventory\n"
                "Usage: cadmlholes <entry.cadml> [--csv]\n";
            return 0;
        }
        if (arg == "--csv") { csv = true; continue; }
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option `" << arg << "`\n"; return 2;
        }
        if (entry.empty()) { entry = std::string(arg); continue; }
        std::cerr << "error: unexpected positional `" << arg << "`\n"; return 2;
    }
    if (entry.empty()) {
        std::cerr << "Usage: cadmlholes <entry.cadml>\n"; return 2;
    }

    auto cr = cadml::compile::compile_file(entry);
    for (const auto& w : cr.warnings) std::fprintf(stderr, "warning: %s\n", w.message.c_str());
    for (const auto& e : cr.errors)   std::fprintf(stderr, "error: %s\n",   e.message.c_str());
    if (!cr.ok()) return 1;

    auto report = cadml::engine::flat_holes(cr.document);

    // Document units → label suffix. flat_holes returns raw doc-unit
    // diameters / depths; the CSV column headers and the table /
    // catalog labels follow the file's declared units.
    const std::string u = cr.document.meta.units.empty()
        ? std::string{ "mm" } : cr.document.meta.units;

    if (csv) {
        std::printf("part,role,node_id,diameter_%s,depth_%s,axis,error\n",
                    u.c_str(), u.c_str());
        for (const auto& e : report.entries) {
            std::printf("%s,%s,%u,%.3f,%.3f,%s,%s\n",
                        e.part_name.c_str(), e.role.c_str(),
                        e.node_id, e.diameter_mm, e.depth_mm,
                        e.axis.c_str(), e.error_hint.c_str());
        }
        return 0;
    }

    if (report.entries.empty()) {
        std::printf("(no drilled holes detected)\n");
        return 0;
    }

    // Roll up by diameter for a "drill catalog" summary at the end.
    std::map<double, int> by_diameter;
    const std::string dia_hdr   = "dia ("   + u + ")";
    const std::string depth_hdr = "depth (" + u + ")";
    std::printf("%-30s  %-9s  %6s  %10s  %10s  %s\n",
                "part", "role", "node", dia_hdr.c_str(),
                depth_hdr.c_str(), "axis");
    for (const auto& e : report.entries) {
        std::printf("%-30s  %-9s  %6u  %10.3f  %10.3f  %s%s%s\n",
                    e.part_name.c_str(), e.role.c_str(), e.node_id,
                    e.diameter_mm, e.depth_mm, e.axis.c_str(),
                    e.error_hint.empty() ? "" : "  *",
                    e.error_hint.empty() ? "" : e.error_hint.c_str());
        if (e.diameter_mm > 0) ++by_diameter[e.diameter_mm];
    }
    std::printf("\nDrill catalog (rolled up by diameter):\n");
    for (const auto& [d, n] : by_diameter) {
        std::printf("  %6.3f %s   x %d\n", d, u.c_str(), n);
    }
    return 0;
}
