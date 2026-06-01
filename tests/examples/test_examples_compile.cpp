// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/compile/bundler.hpp>
#include <cadml/engine/flat_evaluator.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Walks the configured examples dir, returning all .cadml entry files
// (skips .lua helpers, READMEs, .stl outputs, etc.). Sorted for
// deterministic test ordering.
std::vector<fs::path> discover_examples() {
    std::vector<fs::path> out;
#ifndef CADML_EXAMPLES_DIR
    return out;
#else
    const fs::path root{ CADML_EXAMPLES_DIR };
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    for (auto& dir : fs::directory_iterator(root, ec)) {
        if (!dir.is_directory()) continue;
        // Convention: the example dir's entry file is named after the
        // directory (`hex-bolt/hex-bolt.cadml`). Multi-file examples
        // like caster-wheel have multiple .cadml files in the dir
        // but only the top-level one matches this convention.
        const auto name = dir.path().filename().string();
        const auto entry = dir.path() / (name + ".cadml");
        if (fs::is_regular_file(entry, ec)) {
            out.push_back(entry);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
#endif
}

class ExampleCompiles : public testing::TestWithParam<fs::path> {};

TEST_P(ExampleCompiles, CompileAndEvaluate) {
    const auto path = GetParam();

    // Compile.
    auto compile_result = cadml::compile::compile_file(path);
    ASSERT_TRUE(compile_result.ok())
        << "example " << path.string() << " failed to compile: "
        << (compile_result.errors.empty()
                ? std::string("(no message)")
                : compile_result.errors[0].message);

    // Evaluate the flat document.
    auto eval_result = cadml::engine::evaluate_flat(compile_result.document);
    ASSERT_TRUE(eval_result.ok())
        << "example " << path.string() << " evaluated with errors: "
        << (eval_result.errors.empty()
                ? std::string("(no message)")
                : eval_result.errors[0].message);

    // At least one part with a non-empty mesh.
    ASSERT_FALSE(eval_result.parts.empty())
        << "example " << path.string() << " evaluated to zero parts";
    std::size_t total_triangles = 0;
    for (const auto& p : eval_result.parts) {
        total_triangles += p.mesh.triangle_count();
    }
    EXPECT_GT(total_triangles, 0u)
        << "example " << path.string() << " evaluated to zero triangles";
}

// Friendly test names: derive from the .cadml file stem.
struct StemNamer {
    template <class ParamInfo>
    std::string operator()(const ParamInfo& info) const {
        auto name = info.param.stem().string();
        // GoogleTest only accepts [A-Za-z0-9_] in test names.
        for (char& c : name) if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        return name;
    }
};

INSTANTIATE_TEST_SUITE_P(
    ExamplesCorpus,
    ExampleCompiles,
    testing::ValuesIn(discover_examples()),
    StemNamer{});

}  // namespace
