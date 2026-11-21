// sha256_test.cc — NIST FIPS 180-4 标准向量；任何实现偏差都必须在这里先红。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "llmopt/sha256.hpp"

#include "doctest/doctest.h"

#include <cctype>

TEST_CASE("sha256 空串与 abc 标准向量") {
    CHECK_EQ(llmopt::sha256_hex(""),
             "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_EQ(llmopt::sha256_hex("abc"),
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("sha256 跨块边界向量") {
    CHECK_EQ(llmopt::sha256_hex(
                     "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("sha256 输出长度与大小写") {
    const std::string digest = llmopt::sha256_hex("llmopt");
    CHECK_EQ(digest.size(), 64U);
    for (char ch : digest) {
        CHECK(std::isxdigit(static_cast<unsigned char>(ch)));
        CHECK(ch == std::tolower(static_cast<unsigned char>(ch)));
    }
}
