// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cadml/types.hpp>

#include <string>
#include <vector>

namespace cadml::compile {

struct CompileError;  // fwd

namespace detail {

void resolve_lua_calls(Document& doc,
                        const std::vector<ParamDecl>& entry_params,
                        std::vector<CompileError>& errors_out);

}  // namespace detail
}  // namespace cadml::compile
