// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/types.hpp>

#include <vector>

namespace cadml::compile {

struct CompileError;

namespace detail {

void compile_assemblies(Document& doc,
                         const std::vector<ParamDecl>& entry_params,
                         std::vector<CompileError>& errors_out,
                         std::vector<CompileError>& warnings_out);

}  // namespace detail
}  // namespace cadml::compile
