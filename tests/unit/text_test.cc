// text_test.cc — 流式 UTF-8 切分契约：半个汉字不得进入 JSON。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/text.hpp"

#include <string>

using namespace llmopt; // NOLINT：用例短名与契约同名

TEST_CASE("ASCII 全部完整") {
    CHECK(utf8_complete_prefix("abc") == 3);
    CHECK(utf8_complete_prefix("") == 0);
}

TEST_CASE("三字节汉字按字节到达时只在完整后放行") {
    const std::string chinese = "\xE4\xB8\xAD"; // 中
    CHECK(utf8_complete_prefix(chinese.substr(0, 1)) == 0);
    CHECK(utf8_complete_prefix(chinese.substr(0, 2)) == 0);
    CHECK(utf8_complete_prefix(chinese) == 3);
    CHECK(utf8_complete_prefix("A" + chinese) == 4);
    CHECK(utf8_complete_prefix("A" + chinese.substr(0, 2)) == 1);
}

TEST_CASE("非法续字节和孤立首字节在前缀处截断") {
    CHECK(utf8_complete_prefix(std::string("\x80")) == 0);
    CHECK(utf8_complete_prefix(std::string("\xE4\x41")) == 0);
    CHECK(utf8_complete_prefix(std::string("AB\xE4\x41")) == 2);
}

TEST_CASE("四字节 emoji 需要四个字节才放行") {
    const std::string emoji = "\xF0\x9F\x98\x80";
    CHECK(utf8_complete_prefix(emoji.substr(0, 3)) == 0);
    CHECK(utf8_complete_prefix(emoji) == 4);
}
