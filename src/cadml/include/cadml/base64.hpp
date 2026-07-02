// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cadml {

// Base64-encode raw bytes (standard alphabet, `=` padded).
[[nodiscard]] std::string base64_encode(std::string_view data);

// Decode a base64 string into raw bytes. ASCII whitespace (newlines used
// to wrap long payloads) is skipped; decoding stops at the first `=`
// padding. Returns nullopt on any non-base64, non-whitespace character so
// a corrupt payload surfaces as a diagnostic rather than garbage.
[[nodiscard]] std::optional<std::string> base64_decode(std::string_view in);

}  // namespace cadml
