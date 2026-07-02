// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/base64.hpp>

#include <gtest/gtest.h>

#include <string>

using cadml::base64_decode;
using cadml::base64_encode;

// RFC 4648 §10 test vectors pin both directions of the alphabet,
// padding, and remainder handling.
TEST(Base64, Rfc4648Vectors) {
    const struct { const char* raw; const char* encoded; } vec[] = {
        { "",       ""         },
        { "f",      "Zg=="     },
        { "fo",     "Zm8="     },
        { "foo",    "Zm9v"     },
        { "foob",   "Zm9vYg==" },
        { "fooba",  "Zm9vYmE=" },
        { "foobar", "Zm9vYmFy" },
    };
    for (const auto& v : vec) {
        EXPECT_EQ(base64_encode(v.raw), v.encoded);
        const auto d = base64_decode(v.encoded);
        ASSERT_TRUE(d.has_value()) << v.encoded;
        EXPECT_EQ(*d, v.raw);
    }
}

TEST(Base64, BinaryRoundTrip) {
    // All 256 byte values, at every length mod 3 — embedded NULs and
    // high bytes must survive (STL blobs are arbitrary binary).
    std::string raw;
    for (int i = 0; i < 256; ++i) raw.push_back(static_cast<char>(i));
    for (std::size_t len : { raw.size(), raw.size() - 1, raw.size() - 2 }) {
        const std::string s = raw.substr(0, len);
        const auto d = base64_decode(base64_encode(s));
        ASSERT_TRUE(d.has_value());
        EXPECT_EQ(*d, s);
    }
}

TEST(Base64, DecodeSkipsWhitespace) {
    // Long payloads are wrapped with newlines; whitespace is not data.
    EXPECT_EQ(base64_decode("Zm9v\nYmFy"), "foobar");
    EXPECT_EQ(base64_decode(" Z m 9 v \t\r\n"), "foo");
}

TEST(Base64, DecodeRejectsInvalidCharacters) {
    EXPECT_FALSE(base64_decode("Zm9v!").has_value());
    EXPECT_FALSE(base64_decode("Zm-v").has_value());   // url-safe alphabet
    EXPECT_FALSE(base64_decode("!!!not-base64!!!").has_value());
}

TEST(Base64, DecodeStopsAtPadding) {
    // Nothing after the first `=` is read.
    EXPECT_EQ(base64_decode("Zg==garbage-after-padding"), "f");
}
