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
                "cadmlbounds - bounding shapes (AABB, OBB, sphere, cylinder)\n"
                "Usage: cadmlbounds <entry.cadml> [-p NAME]\n";
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
        std::cerr << "Usage: cadmlbounds <entry.cadml> [-p NAME]\n"; return 2;
    }

    auto cr = cadml::compile::compile_file(entry);
    for (const auto& w : cr.warnings) std::fprintf(stderr, "warning: %s\n", w.message.c_str());
    for (const auto& e : cr.errors)   std::fprintf(stderr, "error: %s\n",   e.message.c_str());
    if (!cr.ok()) return 1;
    auto er = cadml::engine::evaluate_flat(cr.document);
    for (const auto& e : er.errors) std::fprintf(stderr, "error: %s\n", e.message.c_str());
    if (!er.ok()) return 1;
    if (er.parts.empty()) { std::fprintf(stderr, "error: no parts produced\n"); return 1; }

    // Use the document's `units` as the label suffix. flat_bounds
    // returns raw doc-unit values, so a file declared `units in`
    // gets inches printed as `in` / `in^2` / `in^3` — no surprise
    // 25.4× scaling for the user.
    const std::string u  = cr.document.meta.units.empty()
        ? std::string{ "mm" } : cr.document.meta.units;
    const std::string u2 = u + "^2";
    const std::string u3 = u + "^3";

    bool any = false;
    for (const auto& p : er.parts) {
        if (!only_part.empty() && only_part != p.name) continue;
        any = true;
        auto b = cadml::engine::flat_bounds(p.mesh);

        const double aabb_w = b.aabb_max[0] - b.aabb_min[0];
        const double aabb_h = b.aabb_max[1] - b.aabb_min[1];
        const double aabb_d = b.aabb_max[2] - b.aabb_min[2];
        const double aabb_v = aabb_w * aabb_h * aabb_d;

        const double sphere_v = (4.0 / 3.0) * 3.14159265358979323846 *
            b.sphere_radius * b.sphere_radius * b.sphere_radius;

        std::printf("Part: %s\n", p.name.c_str());
        std::printf("  AABB     min %.2f, %.2f, %.2f %s\n",
                    b.aabb_min[0], b.aabb_min[1], b.aabb_min[2], u.c_str());
        std::printf("           max %.2f, %.2f, %.2f %s\n",
                    b.aabb_max[0], b.aabb_max[1], b.aabb_max[2], u.c_str());
        std::printf("           size %.2f x %.2f x %.2f %s  (volume %.0f %s)\n",
                    aabb_w, aabb_h, aabb_d, u.c_str(), aabb_v, u3.c_str());
        std::printf("  principal_axes_box (PCA, NOT min-OBB)\n");
        std::printf("           centre %.2f, %.2f, %.2f %s\n",
                    b.principal_box_center[0], b.principal_box_center[1],
                    b.principal_box_center[2], u.c_str());
        std::printf("           full extents %.2f x %.2f x %.2f %s  "
                    "(volume %.0f %s)\n",
                    b.principal_box_extents[0] * 2, b.principal_box_extents[1] * 2,
                    b.principal_box_extents[2] * 2, u.c_str(),
                    b.principal_box_volume_mm3, u3.c_str());
        std::printf("           axis 1: %+.3f, %+.3f, %+.3f\n",
                    b.principal_box_axes[0], b.principal_box_axes[1], b.principal_box_axes[2]);
        std::printf("           axis 2: %+.3f, %+.3f, %+.3f\n",
                    b.principal_box_axes[3], b.principal_box_axes[4], b.principal_box_axes[5]);
        std::printf("           axis 3: %+.3f, %+.3f, %+.3f\n",
                    b.principal_box_axes[6], b.principal_box_axes[7], b.principal_box_axes[8]);
        std::printf("  sphere   centre %.2f, %.2f, %.2f %s   "
                    "radius %.2f %s  (volume %.0f %s)\n",
                    b.sphere_center[0], b.sphere_center[1], b.sphere_center[2], u.c_str(),
                    b.sphere_radius, u.c_str(), sphere_v, u3.c_str());
        const char* axes[] = { "X", "Y", "Z" };
        for (int k = 0; k < 3; ++k) {
            const auto& ap = b.cyl_axis_point[k];
            std::printf("  cyl%s    radius %.2f %s   height %.2f %s   "
                        "volume %.0f %s\n"
                        "          axis through %.2f, %.2f, %.2f %s "
                        "(AABB midline)\n",
                        axes[k], b.cyl_radius[k], u.c_str(),
                        b.cyl_height[k], u.c_str(),
                        b.cyl_volume[k], u3.c_str(),
                        ap[0], ap[1], ap[2], u.c_str());
        }
        std::printf("\n");
    }
    if (!only_part.empty() && !any) {
        std::fprintf(stderr, "error: no part named '%s'\n", only_part.c_str());
        return 1;
    }
    return 0;
}
