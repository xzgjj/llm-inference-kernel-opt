cmake_minimum_required(VERSION 3.20)
project(llmopt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LLMOPT_ENABLE_LLAMA "Link the real llama.cpp backend (Linux/WSL)" OFF)
option(LLMOPT_BUILD_TESTS "Build doctest unit, contract and integration tests" ON)
option(LLMOPT_COVERAGE "Instrument with gcov (GCC/Clang) for line coverage" OFF)
set(LLMOPT_DEPS_DIR "${CMAKE_SOURCE_DIR}/third_party/_deps" CACHE PATH "FetchContent cache")
# CMake ≥4 默认拒绝 min<3.5 的第三方工程；doctest 2.4.11 仍写 3.0，这里统一放宽下限。
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

include(cmake/deps.cmake)

add_library(llmopt_warnings INTERFACE)
target_compile_options(llmopt_warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Werror -Wpedantic -Wshadow>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>)
target_compile_definitions(llmopt_warnings INTERFACE LLMOPT_VERSION="0.1.0")

# 覆盖率统一挂在这个接口库上：构建与链接两侧都要 --coverage，否则只出 .gcno 不出 .gcda。
add_library(llmopt_coverage INTERFACE)
if(LLMOPT_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(WARNING "LLMOPT_COVERAGE=ON 仅支持 GCC/Clang，当前 ${CMAKE_CXX_COMPILER_ID} 已忽略")
    else()
        target_compile_options(llmopt_coverage INTERFACE --coverage -O0 -g)
        target_link_options(llmopt_coverage INTERFACE --coverage)
    endif()
endif()

add_library(llmopt_core STATIC
    src/common.cpp
    src/protocol.cpp
    src/config.cpp
    src/kv.cpp
    src/preempt.cpp
    src/scheduler.cpp
    src/sink.cpp
    src/metrics.cpp
    src/text.cpp
    src/gate.cpp
    src/gguf.cpp
    src/runner_stub.cpp
    src/log.cpp)
target_include_directories(llmopt_core PUBLIC include)
target_link_libraries(llmopt_core PUBLIC nlohmann_json::nlohmann_json llmopt_coverage PRIVATE llmopt_warnings)
target_link_libraries(llmopt_core PRIVATE concurrentqueue spdlog::spdlog)

if(LLMOPT_ENABLE_LLAMA)
    add_llama_backend(llmopt_core)
endif()

add_executable(llmopt_server src/http_server.cpp src/main.cpp)
target_link_libraries(llmopt_server PRIVATE llmopt_core httplib::httplib spdlog::spdlog llmopt_warnings llmopt_coverage)
if(LLMOPT_ENABLE_LLAMA)
    add_llama_backend(llmopt_server)
    target_sources(llmopt_server PRIVATE src/runner_llama.cpp)
endif()
if(WIN32)
    target_link_libraries(llmopt_server PRIVATE ws2_32)
endif()

if(LLMOPT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
