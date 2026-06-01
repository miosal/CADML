// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <string>
#include <string_view>

namespace cadml {

// Returns the SHA-256 of `data`, encoded as a 64-character lowercase
// hex string. Deterministic and stable across runs and platforms.
[[nodiscard]] std::string sha256_hex(std::string_view data);

}  // namespace cadml
