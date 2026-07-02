// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/base64.hpp>

#include <cctype>
#include <cstdint>

namespace cadml {

std::string base64_encode(std::string_view data) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const std::uint32_t v =
            (static_cast<std::uint8_t>(data[i])     << 16) |
            (static_cast<std::uint8_t>(data[i + 1]) <<  8) |
             static_cast<std::uint8_t>(data[i + 2]);
        out.push_back(alphabet[(v >> 18) & 0x3F]);
        out.push_back(alphabet[(v >> 12) & 0x3F]);
        out.push_back(alphabet[(v >>  6) & 0x3F]);
        out.push_back(alphabet[ v        & 0x3F]);
    }
    if (const std::size_t rem = data.size() - i; rem) {
        std::uint32_t v = static_cast<std::uint8_t>(data[i]) << 16;
        if (rem == 2) v |= static_cast<std::uint8_t>(data[i + 1]) << 8;
        out.push_back(alphabet[(v >> 18) & 0x3F]);
        out.push_back(alphabet[(v >> 12) & 0x3F]);
        out.push_back(rem == 2 ? alphabet[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

std::optional<std::string> base64_decode(std::string_view in) {
    auto sextet = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    std::string out;
    out.reserve(in.size() / 4 * 3);
    std::uint32_t buf = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;            // padding — nothing more to read
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        const int d = sextet(c);
        if (d < 0) return std::nullopt; // invalid character
        buf = (buf << 6) | static_cast<std::uint32_t>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

}  // namespace cadml
