// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>
#include <cadml/engine/flat_analysis.hpp>
#include <cadml/engine/flat_evaluator.hpp>
#include <cadml/types.hpp>     // parse_double_strict

#include "cli_panic.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

void print_usage(std::ostream& os) {
    os <<
        "cadmlmass - CADML 0.1 mass + inertia properties\n"
        "\n"
        "Usage:\n"
        "  cadmlmass <entry.cadml> [options]\n"
        "\n"
        "Options:\n"
        "  -p, --part NAME    Only report this part\n"
        "  --material NAME    Catalog density: steel/aluminum/brass/\n"
        "                       copper/titanium/lead/abs/pla/petg/nylon/\n"
        "                       wood-pine/wood-oak/water\n"
        "  --density VALUE    Density in g/cm^3 (overrides --material)\n"
        "  -t, --tensor       Print full inertia tensor\n"
        "  -h, --help         Show this help\n";
}

// Densities in g/cm^3. Values from common engineering references —
// these are nominal, not precision. Multiply by 1000 to get kg/m^3.
const std::map<std::string, double> kMaterials = {
    { "steel",     7.85 },
    { "aluminum",  2.70 },   { "aluminium", 2.70 },
    { "brass",     8.50 },
    { "copper",    8.96 },
    { "titanium",  4.51 },
    { "lead",     11.34 },
    { "abs",       1.05 },
    { "pla",       1.24 },
    { "petg",      1.27 },
    { "nylon",     1.14 },
    { "wood-pine", 0.50 },
    { "wood-oak",  0.75 },
    { "water",     1.00 },
};

}  // namespace

int main(int argc, char** argv) {
    ::cadml::cli::install_panic_handler();
    fs::path entry;
    std::string only_part;
    std::string material;
    double density_override = 0;     // g/cm^3
    bool full_tensor = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(std::cout); return 0;
        }
        if (arg == "-p" || arg == "--part") {
            if (i + 1 >= argc) { std::cerr << "error: --part requires arg\n"; return 2; }
            only_part = argv[++i]; continue;
        }
        if (arg == "--material") {
            if (i + 1 >= argc) { std::cerr << "error: --material requires arg\n"; return 2; }
            material = argv[++i]; continue;
        }
        if (arg == "--density") {
            if (i + 1 >= argc) { std::cerr << "error: --density requires arg\n"; return 2; }
            auto v = cadml::parse_double_strict(argv[++i]);
            if (!v) { std::cerr << "error: --density must be numeric\n"; return 2; }
            density_override = *v;
            continue;
        }
        if (arg == "-t" || arg == "--tensor") { full_tensor = true; continue; }
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option `" << arg << "`\n";
            print_usage(std::cerr); return 2;
        }
        if (entry.empty()) { entry = std::string(arg); continue; }
        std::cerr << "error: unexpected positional `" << arg << "`\n"; return 2;
    }

    if (entry.empty()) { print_usage(std::cerr); return 2; }

    // Resolve density (g/cm^3 -> kg/m^3).
    double density_kg_m3 = 0;
    if (density_override > 0) {
        density_kg_m3 = density_override * 1000.0;
    } else if (!material.empty()) {
        auto it = kMaterials.find(material);
        if (it == kMaterials.end()) {
            std::fprintf(stderr, "error: unknown material '%s'. Try one of:\n",
                          material.c_str());
            for (const auto& kv : kMaterials)
                std::fprintf(stderr, "  %s (%.2f g/cm^3)\n",
                              kv.first.c_str(), kv.second);
            return 2;
        }
        density_kg_m3 = it->second * 1000.0;
        std::printf("[material: %s, density %.2f g/cm^3]\n\n",
                    material.c_str(), it->second);
    }

    auto cr = cadml::compile::compile_file(entry);
    for (const auto& w : cr.warnings) std::fprintf(stderr, "warning: %s\n", w.message.c_str());
    for (const auto& e : cr.errors)   std::fprintf(stderr, "error: %s\n",   e.message.c_str());
    if (!cr.ok()) return 1;

    auto er = cadml::engine::evaluate_flat(cr.document);
    for (const auto& w : er.warnings) std::fprintf(stderr, "warning: %s\n", w.message.c_str());
    for (const auto& e : er.errors)   std::fprintf(stderr, "error: %s\n",   e.message.c_str());
    if (!er.ok()) return 1;
    if (er.parts.empty()) {
        std::fprintf(stderr, "error: no parts produced\n"); return 1;
    }

    bool printed_any = false;
    // Resolve the document's units → mm scale once and hand it to the
    // mass-property library so volume / area / inertia all land in the
    // physical units their `_mm3` / `_mm2` / kg·mm² field names
    // advertise — regardless of whether the file declares `units mm`,
    // `units in`, `units m`, etc.
    const std::string doc_units = cr.document.meta.units.empty()
        ? std::string{ "mm" }
        : cr.document.meta.units;
    const auto unit_scale_opt = cadml::units_to_mm_scale(doc_units);
    if (!unit_scale_opt) {
        std::fprintf(stderr,
            "error: unknown `units %s` in frontmatter; cannot convert "
            "volume to mm for density calculations\n", doc_units.c_str());
        return 1;
    }
    const double unit_to_mm = *unit_scale_opt;

    double total_mass_kg = 0;
    double total_volume_mm3 = 0;
    for (const auto& p : er.parts) {
        if (!only_part.empty() && only_part != p.name) continue;
        printed_any = true;

        auto mp = cadml::engine::flat_mass_properties(
            p.mesh, density_kg_m3, unit_to_mm);
        total_mass_kg += mp.mass_kg;
        total_volume_mm3 += mp.volume_mm3;

        std::printf("Part: %s\n", p.name.c_str());
        std::printf("  triangles      %llu  (%llu degenerate)\n",
                    static_cast<unsigned long long>(p.mesh.triangle_count()),
                    static_cast<unsigned long long>(mp.degenerate_triangles));
        std::printf("  watertight     %s\n",
                    mp.is_watertight ? "yes (heuristic)" : "no");
        std::printf("  volume         %.2f mm^3  (%.3f cm^3)  [document units: %s]\n",
                    mp.volume_mm3, mp.volume_mm3 * 1e-3, doc_units.c_str());
        std::printf("  surface area   %.2f mm^2\n", mp.surface_area_mm2);
        std::printf("  centre of mass %.3f, %.3f, %.3f mm\n",
                    mp.center_of_mass[0], mp.center_of_mass[1],
                    mp.center_of_mass[2]);
        if (mp.mass_kg > 0) {
            std::printf("  mass           %.3f g  (%.4f kg)\n",
                        mp.mass_kg * 1000.0, mp.mass_kg);
            std::printf("  weight         %.3f N  @9.81 m/s^2\n",
                        mp.mass_kg * 9.81);
        }
        // Inertia tensors, both about the local origin and (shifted)
        // about the centre of mass. Units depend on whether the
        // caller supplied a density:
        //   density supplied → kg·mm²
        //   no density       → mm⁵ (pure geometric moment integral)
        // Most engineering use wants the COM tensor; the origin tensor
        // is surfaced too so consumers that need it (joint frames,
        // mounting fixtures) don't have to redo the parallel-axis
        // shift themselves.
        auto extract = [](const std::array<double, 9>& I) {
            struct { double xx, yy, zz, xy, xz, yz; } r;
            r.xx = I[0]; r.yy = I[4]; r.zz = I[8];
            r.xy = I[1]; r.xz = I[2]; r.yz = I[5];
            return r;
        };
        const auto Io = extract(mp.inertia_origin);
        const auto Ic = extract(mp.inertia_com);
        const char* unit = (mp.density_kg_per_m3 > 0)
            ? "kg*mm^2" : "mm^5 (geometric, no density applied)";
        if (full_tensor) {
            std::printf("  inertia about local origin [%s]:\n", unit);
            std::printf("    [ %12.3e %12.3e %12.3e ]\n", Io.xx, Io.xy, Io.xz);
            std::printf("    [ %12.3e %12.3e %12.3e ]\n", Io.xy, Io.yy, Io.yz);
            std::printf("    [ %12.3e %12.3e %12.3e ]\n", Io.xz, Io.yz, Io.zz);
            std::printf("  inertia about COM [%s]:\n", unit);
            std::printf("    [ %12.3e %12.3e %12.3e ]\n", Ic.xx, Ic.xy, Ic.xz);
            std::printf("    [ %12.3e %12.3e %12.3e ]\n", Ic.xy, Ic.yy, Ic.yz);
            std::printf("    [ %12.3e %12.3e %12.3e ]\n", Ic.xz, Ic.yz, Ic.zz);
        } else {
            auto off_max = [](const auto& I) {
                return std::max({ std::fabs(I.xy), std::fabs(I.xz),
                                  std::fabs(I.yz) });
            };
            std::printf("  inertia about local origin (diag)"
                        " Ixx=%.3e Iyy=%.3e Izz=%.3e [%s]\n",
                        Io.xx, Io.yy, Io.zz, unit);
            std::printf("    max off-diagonal magnitude: %.3e\n", off_max(Io));
            std::printf("  inertia about COM             (diag)"
                        " Ixx=%.3e Iyy=%.3e Izz=%.3e [%s]\n",
                        Ic.xx, Ic.yy, Ic.zz, unit);
            std::printf("    max off-diagonal magnitude: %.3e\n", off_max(Ic));
        }
        std::printf("\n");
    }

    if (!only_part.empty() && !printed_any) {
        std::fprintf(stderr, "error: no part named '%s'\n", only_part.c_str());
        return 1;
    }
    if (er.parts.size() > 1 && only_part.empty()) {
        std::printf("Total volume %.2f mm^3 (%.3f cm^3)",
                    total_volume_mm3, total_volume_mm3 * 1e-3);
        if (total_mass_kg > 0) {
            std::printf(", total mass %.3f g (%.4f kg)",
                        total_mass_kg * 1000.0, total_mass_kg);
        }
        std::printf("\n");
    }
    return 0;
}
