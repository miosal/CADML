// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/engine/flat_evaluator.hpp>

#include <filesystem>
#include <ostream>
#include <string_view>

namespace cadml::engine {

struct ThreeMfOptions {
    // Document units. Maps directly onto the 3MF `<model unit="...">`
    // attribute. Accepted: "mm", "cm", "m", "in", "ft" (matching CADML's
    // frontmatter units). Defaults to "millimeter" (the 3MF standard
    // recommendation and CADML's default). Any other value is rejected
    // with a runtime_error — the spec enumerates these five.
    std::string units = "mm";

    // `<metadata name="Title">` value. Slicers display this in their
    // outline. Empty disables emission of the Title metadata entry.
    std::string title;
};

// Write a 3MF package to `out`. The stream must be binary — 3MF is ZIP
// (binary) data. Throws std::runtime_error on malformed input (index
// out of range, triangles count not a multiple of 3, unsupported units)
// or on internal ZIP write failure.
void write_3mf(const FlatEvalResult& result,
                std::ostream& out,
                const ThreeMfOptions& opts = {});

// Convenience: open `path` in binary mode and write to it. Errors on
// disk are surfaced as runtime_error (including the delayed-failure
// case where ofstream::close() reports a write-time failure).
void write_3mf(const FlatEvalResult& result,
                const std::filesystem::path& path,
                const ThreeMfOptions& opts = {});

}  // namespace cadml::engine
