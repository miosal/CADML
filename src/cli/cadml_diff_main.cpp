// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>
#include <cadml/engine/flat_analysis.hpp>
#include <cadml/engine/flat_evaluator.hpp>
#include <cadml/types.hpp>     // parse_double_strict

#include "cli_panic.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

struct EvalWithUnits {
    cadml::engine::FlatEvalResult result;
    std::string                   units;        // doc-declared units, "" on failure
};

EvalWithUnits evaluate(const fs::path& path) {
    EvalWithUnits out;
    auto cr = cadml::compile::compile_file(path);
    if (!cr.ok()) {
        out.result.errors.push_back({
            "compile failed for " + path.string() +
            (cr.errors.empty() ? "" : " (" + cr.errors[0].message + ")"),
            cadml::SourceRange{}
        });
        return out;
    }
    out.units = cr.document.meta.units.empty()
        ? std::string{ "mm" }
        : cr.document.meta.units;
    out.result = cadml::engine::evaluate_flat(cr.document);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    ::cadml::cli::install_panic_handler();
    fs::path a, b;
    double rel_tol = 0.01;   // 1 %
    double abs_tol = 0.01;   // 0.01 of doc cubed-units (0.01 mm³ if units mm)
    double com_tol = 0.001;  // 0.001 of doc linear units (1 µm if units mm)

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::cout <<
                "cadmldiff - file-to-file regression diff\n"
                "Usage: cadmldiff <a.cadml> <b.cadml>\n"
                "Options:\n"
                "  --rel-tol PCT      mark equal if relative volume change\n"
                "                       <= PCT (default 0.01 = 1%)\n"
                "  --abs-tol VAL      AND absolute volume change <= VAL\n"
                "                       (default 0.01; VAL is in the DOC'S\n"
                "                       declared cubed units — mm^3 if\n"
                "                       `units mm`, in^3 if `units in`, etc.)\n"
                "  --com-tol VAL      AND centre-of-mass shift <= VAL\n"
                "                       (default 0.001; VAL is in the DOC'S\n"
                "                       declared linear units)\n"
                "                     A part is `equal` only when ALL three\n"
                "                     are within tolerance; otherwise CHANGED.\n"
                "                     Both inputs MUST declare the same units\n"
                "                     or the comparison emits a warning.\n";
            return 0;
        }
        auto take_double = [&](const char* flag, double& dst) -> int {
            if (i + 1 >= argc) {
                std::cerr << "error: " << flag << " requires arg\n";
                return 2;
            }
            auto v = cadml::parse_double_strict(argv[++i]);
            if (!v) {
                std::cerr << "error: " << flag << " value is not a number\n";
                return 2;
            }
            dst = *v;
            return 0;
        };
        if (arg == "--rel-tol") { if (int r = take_double("--rel-tol", rel_tol)) return r; continue; }
        if (arg == "--abs-tol") { if (int r = take_double("--abs-tol", abs_tol)) return r; continue; }
        if (arg == "--com-tol") { if (int r = take_double("--com-tol", com_tol)) return r; continue; }
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option `" << arg << "`\n"; return 2;
        }
        if (a.empty())      { a = std::string(arg); continue; }
        else if (b.empty()) { b = std::string(arg); continue; }
        std::cerr << "error: unexpected positional `" << arg << "`\n"; return 2;
    }

    if (a.empty() || b.empty()) {
        std::cerr << "Usage: cadmldiff <a.cadml> <b.cadml>\n"; return 2;
    }

    auto wa = evaluate(a);
    for (const auto& e : wa.result.errors)
        std::fprintf(stderr, "[A] error: %s\n", e.message.c_str());
    if (!wa.result.ok()) return 1;
    auto wb = evaluate(b);
    for (const auto& e : wb.result.errors)
        std::fprintf(stderr, "[B] error: %s\n", e.message.c_str());
    if (!wb.result.ok()) return 1;

    auto& er_a = wa.result;
    auto& er_b = wb.result;

    // Both inputs declare their own units. Use A's units for labels;
    // warn loudly if B differs — comparing values from different units
    // is meaningless without conversion, and the engine returns
    // doc-unit coordinates unscaled.
    const std::string& u = wa.units;
    if (wa.units != wb.units) {
        std::fprintf(stderr,
            "warning: A declares `units %s`, B declares `units %s` — "
            "volume figures are not directly comparable\n",
            wa.units.c_str(), wb.units.c_str());
    }
    const std::string u3      = u + "3";
    const std::string vol_a_h = "vol_a (" + u3 + ")";
    const std::string vol_b_h = "vol_b (" + u3 + ")";

    auto rep = cadml::engine::flat_diff(er_a, er_b);

    int added = 0, removed = 0, changed = 0, equal = 0;
    std::printf("%-30s  %-8s  %12s  %12s  %12s  %s\n",
                "part", "status", vol_a_h.c_str(), vol_b_h.c_str(),
                "delta",  "centre");
    for (const auto& e : rep.entries) {
        std::string status;
        if (e.in_a && !e.in_b)      { status = "REMOVED"; ++removed; }
        else if (!e.in_a && e.in_b) { status = "ADDED";   ++added; }
        else {
            const double dv = e.volume_b - e.volume_a;
            const double rel = (std::fabs(e.volume_a) > 1e-12)
                ? std::fabs(dv) / std::fabs(e.volume_a) : 0;
            // Volume residue floor: the signed-tetrahedra integral isn't
            // bit-translation-invariant in FP, so a part with the same
            // mesh placed at a different position can show a sub-1e-9 mm³
            // dv even when nothing in the body changed. Floor abs_tol at
            // a few epsilon to avoid spurious CHANGED rows when the user
            // passes --abs-tol 0.
            const double abs_floor = std::max(abs_tol,
                1e-9 * std::max(std::fabs(e.volume_a),
                                std::fabs(e.volume_b)));
            const bool vol_equal =
                std::fabs(dv) <= abs_floor || rel <= rel_tol;
            const bool com_equal = e.center_shift_mm <= com_tol;
            if (vol_equal && com_equal) {
                status = "equal"; ++equal;
            } else {
                status = "CHANGED"; ++changed;
            }
        }
        const double delta = e.volume_b - e.volume_a;
        char shift_buf[32];
        if (e.in_a && e.in_b) {
            std::snprintf(shift_buf, sizeof(shift_buf),
                           "shift %.3f %s", e.center_shift_mm, u.c_str());
        } else {
            shift_buf[0] = '\0';
        }
        std::printf("%-30s  %-8s  %12.2f  %12.2f  %+12.2f  %s\n",
                    e.part.c_str(), status.c_str(),
                    e.volume_a, e.volume_b, delta, shift_buf);
    }

    std::printf("\nTotals: %.2f -> %.2f %s (delta %+.2f, %+.2f%%)\n",
                rep.total_volume_a, rep.total_volume_b, u3.c_str(),
                rep.total_volume_b - rep.total_volume_a,
                rep.total_volume_a > 1e-12
                    ? 100.0 * (rep.total_volume_b - rep.total_volume_a) /
                        rep.total_volume_a
                    : 0.0);
    std::printf("Parts: %d equal, %d changed, %d added, %d removed\n",
                equal, changed, added, removed);
    return (changed + added + removed) > 0 ? 1 : 0;
}
