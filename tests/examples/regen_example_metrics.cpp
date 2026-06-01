// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org


#include <cadml/compile/bundler.hpp>
#include <cadml/engine/flat_evaluator.hpp>
#include <cadml/types.hpp>

#include "example_metrics.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kHeader =
    "# Geometric-invariant manifest for the example corpus, read by the\n"
    "# cadml_examples tolerance test (test_examples_metrics.cpp).\n"
    "#\n"
    "# Unlike a byte/hash golden, these invariants survive compiler,\n"
    "# optimization-level, and platform differences in Manifold's mesh\n"
    "# output — only a real geometry change moves them. Regenerate with\n"
    "# cadml_regen_example_metrics when a change to the engine is intended.\n"
    "#\n"
    "# Format: <example-name> <parts> <volume_mm3> <dx> <dy> <dz>\n"
    "\n";

std::vector<std::pair<std::string, fs::path>> collect(const fs::path& root) {
    std::vector<std::pair<std::string, fs::path>> v;
    for (auto& dir : fs::directory_iterator(root)) {
        if (!dir.is_directory()) continue;
        const auto name = dir.path().filename().string();
        const auto entry = dir.path() / (name + ".cadml");
        if (fs::is_regular_file(entry)) v.emplace_back(name, entry);
    }
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return v;
}

}  // namespace

int main(int argc, char** argv) {
#ifndef CADML_EXAMPLES_DIR
    std::cerr << "CADML_EXAMPLES_DIR not defined\n";
    return 1;
#else
    bool to_stdout = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--stdout") to_stdout = true;
        else { std::cerr << "unknown flag: " << a << "\n"; return 2; }
    }

    const fs::path root{ CADML_EXAMPLES_DIR };
    if (!fs::is_directory(root)) {
        std::cerr << "examples dir not found: " << root.string() << "\n";
        return 1;
    }

    std::ostringstream out;
    out << kHeader;
    int failures = 0;
    for (const auto& [name, path] : collect(root)) {
        auto cr = cadml::compile::compile_file(path);
        if (!cr.ok()) {
            std::cerr << name << ": compile failed\n"; ++failures; continue;
        }
        auto er = cadml::engine::evaluate_flat(cr.document);
        if (!er.ok()) {
            std::cerr << name << ": evaluate failed\n"; ++failures; continue;
        }
        // Convert raw doc-unit volume into mm³ so manifest values
        // stay consistent even if a future example declares `units in`.
        const auto u2mm = cadml::units_to_mm_scale(cr.document.meta.units);
        const auto m = cadml::examples::compute_metrics(er, u2mm.value_or(1.0));
        char line[256];
        std::snprintf(line, sizeof(line), "%s %zu %.6f %.4f %.4f %.4f\n",
                      name.c_str(), m.parts, m.volume_mm3, m.dx, m.dy, m.dz);
        out << line;
    }

    if (to_stdout) {
        std::cout << out.str();
        return failures == 0 ? 0 : 1;
    }
#ifndef CADML_METRICS_MANIFEST_DIR
    std::cerr << "CADML_METRICS_MANIFEST_DIR not defined\n";
    return 1;
#else
    const fs::path dir{ CADML_METRICS_MANIFEST_DIR };
    std::ofstream f(dir / "example-metrics.txt", std::ios::binary);
    if (!f) { std::cerr << "cannot write example-metrics.txt\n"; return 1; }
    f << out.str();
    std::cout << "wrote example-metrics.txt to " << dir.string() << "\n";
    return failures == 0 ? 0 : 1;
#endif
#endif
}
