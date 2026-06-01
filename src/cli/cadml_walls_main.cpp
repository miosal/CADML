// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>
#include <cadml/engine/flat_analysis.hpp>
#include <cadml/engine/flat_evaluator.hpp>
#include <cadml/types.hpp>

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
    int max_samples = 0;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::cout <<
                "cadmlwalls - per-part wall-thickness analysis\n"
                "Usage: cadmlwalls <entry.cadml> [-p NAME] [--max-samples N]\n";
            return 0;
        }
        if (arg == "-p" || arg == "--part") {
            if (i + 1 >= argc) { std::cerr << "error: --part requires arg\n"; return 2; }
            only_part = argv[++i]; continue;
        }
        if (arg == "--max-samples") {
            if (i + 1 >= argc) { std::cerr << "error: --max-samples requires arg\n"; return 2; }
            try { max_samples = std::stoi(argv[++i]); }
            catch (...) { std::cerr << "error: --max-samples needs an int\n"; return 2; }
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option `" << arg << "`\n"; return 2;
        }
        if (entry.empty()) { entry = std::string(arg); continue; }
        std::cerr << "error: unexpected positional `" << arg << "`\n"; return 2;
    }
    if (entry.empty()) {
        std::cerr << "Usage: cadmlwalls <entry.cadml>\n"; return 2;
    }

    auto cr = cadml::compile::compile_file(entry);
    for (const auto& w : cr.warnings) std::fprintf(stderr, "warning: %s\n", w.message.c_str());
    for (const auto& e : cr.errors)   std::fprintf(stderr, "error: %s\n",   e.message.c_str());
    if (!cr.ok()) return 1;
    auto er = cadml::engine::evaluate_flat(cr.document);
    for (const auto& e : er.errors) std::fprintf(stderr, "error: %s\n", e.message.c_str());
    if (!er.ok()) return 1;
    if (er.parts.empty()) { std::fprintf(stderr, "error: no parts\n"); return 1; }

    cadml::engine::WallThicknessOptions opts;
    opts.max_samples = static_cast<std::uint32_t>(std::max(0, max_samples));

    const std::string u = cr.document.meta.units.empty()
        ? std::string{ "mm" } : cr.document.meta.units;
    // The threshold field is compared against raw DOC-UNIT mesh coords
    // (see flat_analysis.hpp). Rescale so the *intent* — "0.1 µm of
    // coincident-triangle tolerance" — stays unit-invariant.
    if (auto s = cadml::units_to_mm_scale(u); s && *s > 0.0) {
        opts.min_thickness_mm = 1e-4 / *s;
    }
    const std::string lab_min   = "min ("   + u + ")";
    const std::string lab_p1    = "p1 ("    + u + ")";
    const std::string lab_p10   = "p10 ("   + u + ")";

    // Caveat: per the WallThicknessOptions header, samples at sharp
    // convex corners traverse the body diagonal and inflate the
    // percentile statistics upward. Surface that here so a user
    // reading a 17.3 mm wall thickness on a 10 mm cube doesn't
    // assume a thicker wall than they actually have.
    std::printf("# Caveat: vertex-normal ray sampling inflates the\n");
    std::printf("# reported thickness at sharp convex corners (a\n");
    std::printf("# cube's corner samples report ~sqrt(3) * side rather\n");
    std::printf("# than the true minimum). Treat min / p1 / p10 as\n");
    std::printf("# advisory for parts with sharp external corners.\n");
    std::printf("%-30s  %8s  %10s  %10s  %10s  %10s  %10s  %s\n",
                "part", "samples", lab_min.c_str(), lab_p1.c_str(),
                lab_p10.c_str(), "median", "mean", "no-hit %");
    bool any = false;
    for (const auto& p : er.parts) {
        if (!only_part.empty() && only_part != p.name) continue;
        any = true;
        auto r = cadml::engine::flat_wall_thickness(p.mesh, opts);
        const double escape_pct = (r.samples > 0)
            ? 100.0 * static_cast<double>(r.samples_with_no_hit)
                    / static_cast<double>(r.samples)
            : 0.0;
        std::printf("%-30s  %8llu  %10.3f  %10.3f  %10.3f  %10.3f  %10.3f  %6.1f%%\n",
                    p.name.c_str(),
                    static_cast<unsigned long long>(r.samples),
                    r.min_mm, r.p1_mm, r.p10_mm, r.median_mm,
                    r.mean_mm, escape_pct);
    }
    if (!only_part.empty() && !any) {
        std::fprintf(stderr, "error: no part named '%s'\n", only_part.c_str());
        return 1;
    }
    return 0;
}
