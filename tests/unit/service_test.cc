// service_test.cc — C-14 状态机：合法迁移表、幂等拒绝与 accepting 语义。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/service.hpp"

using namespace llmopt;

TEST_CASE("Starting 只能前进，不能回退") {
    ServiceController service;
    CHECK(service.state() == ServiceState::Starting);
    CHECK_FALSE(service.accepting());
    REQUIRE(service.transit(ServiceState::Ready));
    CHECK(service.state() == ServiceState::Ready);
    CHECK(service.accepting());
    CHECK_FALSE(service.transit(ServiceState::Starting));
    CHECK_FALSE(service.transit(ServiceState::Ready)); // 同态迁移拒绝
}

TEST_CASE("Ready → Draining → Stopped 是唯一收尾路径") {
    ServiceController service;
    REQUIRE(service.transit(ServiceState::Ready));
    REQUIRE(service.transit(ServiceState::Draining));
    CHECK_FALSE(service.accepting());
    CHECK_FALSE(service.transit(ServiceState::Ready));
    REQUIRE(service.transit(ServiceState::Stopped));
    CHECK_FALSE(service.transit(ServiceState::Draining));
    CHECK_FALSE(service.transit(ServiceState::Ready));
    CHECK_FALSE(service.transit(ServiceState::Starting));
}

TEST_CASE("启动失败可从 Starting 直接 Stopped，状态名可打印") {
    ServiceController service;
    REQUIRE(service.transit(ServiceState::Stopped));
    CHECK(std::string(service_state_name(service.state())) == "stopped");
    CHECK(std::string(service_state_name(ServiceState::Ready)) == "ready");
    CHECK(std::string(service_state_name(ServiceState::Draining)) == "draining");
}
