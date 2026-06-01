// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/engine/flat_evaluator.hpp>

#include <filesystem>
#include <ostream>
#include <string_view>

namespace cadml::engine {

// Write all parts in `result` as a binary STL stream. `header` is the
// 80-byte tag stamped at the start of the file (truncated/zero-padded).
// Throws std::runtime_error on malformed input (index out of range,
// triangle index list not a multiple of 3) — the engine should produce
// well-formed meshes, but this is the boundary so we validate.
void write_stl_binary(const FlatEvalResult& result,
                       std::ostream& out,
                       std::string_view header = "CADML");

// Convenience: open `path` in binary mode and write to it.
void write_stl_binary(const FlatEvalResult& result,
                       const std::filesystem::path& path,
                       std::string_view header = "CADML");

}  // namespace cadml::engine
