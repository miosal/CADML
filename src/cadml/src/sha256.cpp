// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/sha256.hpp>

#include <picosha2.h>

#include <string>
#include <string_view>

namespace cadml {

std::string sha256_hex(std::string_view data) {
    std::string hex_str;
    picosha2::hash256_hex_string(
        data.begin(), data.end(), hex_str);
    return hex_str;
}

}  // namespace cadml
