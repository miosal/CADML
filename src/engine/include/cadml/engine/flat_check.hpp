// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/engine/flat_evaluator.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cadml::engine {

// One pair of parts whose meshes interfere (intersect with volume
// above the configured tolerance). Reports carry only part names
// today; a future revision can add `SourceRange` fields when a UI
// consumer needs click-to-source navigation on the report.
struct InterferenceReport {
    std::string part_a;
    std::string part_b;
    double      volume = 0.0;     // measured intersection volume
};

struct [[nodiscard]] InterferenceResult {
    std::vector<InterferenceReport>   reports;
    std::vector<FlatEvalError>        errors;     // measurement failures
    [[nodiscard]] bool ok() const { return errors.empty(); }
    bool clean() const { return ok() && reports.empty(); }
};

struct InterferenceOptions {
    // Volume tolerance, in cubic units of the document. Pairs with
    // measured intersection volume ≤ tolerance are considered clean.
    // Default 0 — any non-trivial overlap is flagged.
    double tolerance = 0.0;

    // Pairs of part names whose mutual overlap is acceptable
    // (suppressed from reports). Symmetric: `{"a","b"}` suppresses
    // both (a,b) and (b,a). Populated by callers that read
    // `Document::meta.allow_interference_pairs` (set by the bundler
    // from `<connect ... allow-interference="true"/>`).
    std::vector<std::pair<std::string, std::string>> allow_pairs;
};

// Walk every (i, j) pair of `result.parts` and measure the
// intersection volume. Volumes greater than `opts.tolerance` are
// recorded as InterferenceReport entries; measurement failures land
// in errors so the caller can distinguish "no interference" from
// "couldn't tell".
//
// O(n²) where n = result.parts.size(). For small assemblies (<100
// parts) this is fine; tools that scale further can pre-cull with
// bounding-box checks before the full Manifold intersect — left as
// a future optimization.
InterferenceResult check_interference(const FlatEvalResult& result,
                                        const InterferenceOptions& opts = {});

}  // namespace cadml::engine
