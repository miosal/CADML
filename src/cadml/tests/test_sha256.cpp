// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#include <cadml/sha256.hpp>

#include <gtest/gtest.h>

#include <set>
#include <string>

using namespace cadml;

TEST(SHA256, EmptyString) {
    EXPECT_EQ(sha256_hex(""),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(SHA256, AbcKnownVector) {
    EXPECT_EQ(sha256_hex("abc"),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(SHA256, FIPS180_4_AppendixB2) {
    EXPECT_EQ(sha256_hex(
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(SHA256, OneMillionAs) {
    // FIPS 180-4 Appendix B.3 — "a" repeated 1,000,000 times.
    std::string million_a(1'000'000, 'a');
    EXPECT_EQ(sha256_hex(million_a),
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(SHA256, BlockBoundaryCases) {
    // Sanity: every input length yields exactly 64 hex chars.
    EXPECT_EQ(sha256_hex(std::string(55, 'a')).size(), 64u);
    EXPECT_EQ(sha256_hex(std::string(56, 'a')).size(), 64u);
    EXPECT_EQ(sha256_hex(std::string(64, 'a')).size(), 64u);
    EXPECT_EQ(sha256_hex(std::string(119, 'a')).size(), 64u);
    EXPECT_EQ(sha256_hex(std::string(120, 'a')).size(), 64u);
}

TEST(SHA256, OutputIsAlwaysLowercase) {
    auto h = sha256_hex("test");
    for (char c : h) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "non-lowercase-hex char: " << c;
    }
}

TEST(SHA256, OutputAlways64Chars) {
    EXPECT_EQ(sha256_hex("").size(), 64u);
    EXPECT_EQ(sha256_hex("a").size(), 64u);
    EXPECT_EQ(sha256_hex(std::string(10000, 'x')).size(), 64u);
}

TEST(SHA256, DifferentInputsDifferentHashes) {
    EXPECT_NE(sha256_hex("a"), sha256_hex("b"));
    EXPECT_NE(sha256_hex("hello"), sha256_hex("Hello"));
    EXPECT_NE(sha256_hex("test"), sha256_hex("test "));
}

TEST(SHA256, IdempotentSameInputSameOutput) {
    const std::string input = "the quick brown fox jumps over the lazy dog";
    EXPECT_EQ(sha256_hex(input), sha256_hex(input));
    EXPECT_EQ(sha256_hex(input), sha256_hex(input));
}

TEST(SHA256, DistributionOfSimilarInputs) {
    std::set<std::string> seen;
    for (int i = 0; i < 100; ++i) {
        std::string s = "version 0.1\nparam x = " + std::to_string(i) + "\n<part/>";
        seen.insert(sha256_hex(s));
    }
    EXPECT_EQ(seen.size(), 100u);
}
