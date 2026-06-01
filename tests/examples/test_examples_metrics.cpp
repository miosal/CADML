// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>
#include <cadml/engine/flat_evaluator.hpp>
#include <cadml/types.hpp>

#include "example_metrics.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;
using cadml::examples::ExampleMetrics;

std::map<std::string, ExampleMetrics> load_manifest() {
    std::map<std::string, ExampleMetrics> out;
#ifdef CADML_METRICS_MANIFEST
    std::ifstream f(CADML_METRICS_MANIFEST);
    std::string line;
    while (std::getline(f, line)) {
        const auto pos = line.find_first_not_of(" \t");
        if (pos == std::string::npos || line[pos] == '#') continue;
        std::istringstream ss(line);
        std::string name;
        ExampleMetrics m;
        if (ss >> name >> m.parts >> m.volume_mm3 >> m.dx >> m.dy >> m.dz) {
            out.emplace(std::move(name), m);
        }
    }
#endif
    return out;
}

class ExampleMetricsTest
    : public testing::TestWithParam<std::pair<std::string, ExampleMetrics>> {};

TEST_P(ExampleMetricsTest, MatchesInvariants) {
    const auto& [name, want] = GetParam();

#ifndef CADML_EXAMPLES_DIR
    GTEST_SKIP() << "CADML_EXAMPLES_DIR not defined";
#else
    const fs::path cadml = fs::path{ CADML_EXAMPLES_DIR } / name / (name + ".cadml");
    ASSERT_TRUE(fs::is_regular_file(cadml)) << "missing example: " << cadml.string();

    auto cr = cadml::compile::compile_file(cadml);
    ASSERT_TRUE(cr.ok()) << "compile failed: "
        << (cr.errors.empty() ? "(no message)" : cr.errors[0].message);
    auto er = cadml::engine::evaluate_flat(cr.document);
    ASSERT_TRUE(er.ok()) << "evaluate failed: "
        << (er.errors.empty() ? "(no message)" : er.errors[0].message);

    const auto u2mm = cadml::units_to_mm_scale(cr.document.meta.units);
    const ExampleMetrics got = cadml::examples::compute_metrics(er, u2mm.value_or(1.0));

    EXPECT_EQ(got.parts, want.parts) << "part count changed for `" << name << "`";

    auto check = [&](const char* what, double g, double w) {
        EXPECT_TRUE(cadml::examples::within_tol(g, w))
            << name << ": " << what << " drifted beyond tolerance.\n"
            << "  expected: " << w << "\n"
            << "  got:      " << g << "\n"
            << "If this engine change was intentional, regenerate:\n"
            << "  cmake --build --preset default --target cadml_regen_example_metrics\n"
            << "  then run the cadml_regen_example_metrics binary.";
    };
    check("volume_mm3", got.volume_mm3, want.volume_mm3);
    check("bbox dx", got.dx, want.dx);
    check("bbox dy", got.dy, want.dy);
    check("bbox dz", got.dz, want.dz);
#endif
}

struct Namer {
    template <class Info>
    std::string operator()(const Info& info) const {
        std::string n = info.param.first;
        for (char& c : n) if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        return n;
    }
};

INSTANTIATE_TEST_SUITE_P(
    Examples,
    ExampleMetricsTest,
    testing::ValuesIn([] {
        std::vector<std::pair<std::string, ExampleMetrics>> v;
        for (auto& [k, m] : load_manifest()) v.emplace_back(k, m);
        return v;
    }()),
    Namer{});

}  // namespace
