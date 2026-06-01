// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace cadml::compile {

struct CompileError;  // fwd

namespace detail {

// Cumulative node-creation budget shared across all <for> and
// <pattern> unrolls in a single compile pass.
//
// The per-loop step/count caps (kMaxForSteps = 100000,
// kMaxPatternCount = 10000) prevent a single loop from going wild, but
// nesting multiplies past them: two stacked <for steps="100000"> nest
// to 1e10 cloned nodes — enough to OOM-kill the process from a single
// uploaded file.
//
// `UnrollBudget` accumulates a *predicted* node count across every
// clone the unrollers are about to perform. When the prediction would
// push the running total past `limit`, the unroller in question
// records a Composition error and bails before the allocation happens.
struct UnrollBudget {
    static constexpr std::uint64_t kDefaultLimit = 1'000'000;

    std::uint64_t limit = kDefaultLimit;
    std::uint64_t used  = 0;
    bool          tripped = false;   // set true on the first overflow

    // Try to charge `n` nodes against the budget. Returns true if the
    // charge fit; false (and sets `tripped`) if it would overflow.
    // Once `tripped`, further charges always return false.
    bool charge(std::uint64_t n) {
        if (tripped) return false;
        if (n > limit - used) { tripped = true; return false; }
        used += n;
        return true;
    }
};

// Run the <for> unroller across all <for> elements in `doc`. Errors
// (e.g., non-resolvable bounds) are appended to `errors_out`.
//
// `params` is the entry-file frontmatter param snapshot; loop bounds
// can reference these.
//
// `budget` is shared with `unroll_pattern_elements` — call both with
// the same `UnrollBudget` so nested for-inside-pattern (and vice
// versa) charges accumulate.
void unroll_for_elements(Document& doc,
                          const std::vector<ParamDecl>& params,
                          UnrollBudget& budget,
                          std::vector<CompileError>& errors_out);

// Run the <pattern> unroller across all <pattern> elements in `doc`.
void unroll_pattern_elements(Document& doc,
                              const std::vector<ParamDecl>& params,
                              UnrollBudget& budget,
                              std::vector<CompileError>& errors_out);

}  // namespace detail
}  // namespace cadml::compile
