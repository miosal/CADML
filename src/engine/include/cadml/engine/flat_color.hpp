// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <cstdint>
#include <string_view>

namespace cadml::engine {

std::uint32_t parse_color_rgba(std::string_view s);

}  // namespace cadml::engine
