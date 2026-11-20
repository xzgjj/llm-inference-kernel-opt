# 依赖获取：版本与 SPEC-1 §1.3 冻结表一致；只拉取到 third_party/_deps，不入库。
include(FetchContent)
set(FETCHCONTENT_BASE_DIR "${LLMOPT_DEPS_DIR}")
set(FETCHCONTENT_QUIET OFF)
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)

FetchContent_Declare(httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(concurrentqueue
    URL https://github.com/cameron314/concurrentqueue/archive/refs/tags/v1.0.3.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(httplib nlohmann_json spdlog concurrentqueue doctest)

# concurrentqueue 自带 INTERFACE 目标，补 SYSTEM 标记避免第三方头触发 -Werror。
if(TARGET concurrentqueue)
    target_include_directories(concurrentqueue SYSTEM INTERFACE
        "${concurrentqueue_SOURCE_DIR}")
endif()

# doctest 2.4.11 经 <ciso646> 做 MSVC 兼容；libstdc++ 15 起该头在 C++20 下抛 #warning，
# clang 会把它升成 -Werror 硬错（gcc 不报）。第三方头一律按 SYSTEM 处理，不参与本项目告警面。
if(TARGET doctest)
    set_property(TARGET doctest APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
        "${doctest_SOURCE_DIR}")
endif()

# 真实 llama.cpp 后端：复用 WSL 侧已构建的共享库，不在此处重编依赖树。
function(add_llama_backend target)
    set(LLAMA_ROOT "$ENV{LLMOPT_LLAMA_ROOT}" CACHE PATH "llama.cpp source root")
    if(NOT LLAMA_ROOT)
        set(LLAMA_ROOT "/root/llm-src/llama.cpp")
    endif()
    find_path(LLAMA_INCLUDE_DIR llama.h PATHS "${LLAMA_ROOT}/include" NO_DEFAULT_PATH)
    find_path(GGML_INCLUDE_DIR ggml.h PATHS "${LLAMA_ROOT}/ggml/include" NO_DEFAULT_PATH)
    find_library(LLAMA_LIBRARY NAMES llama PATHS "${LLAMA_ROOT}/build-cuda/bin"
        "${LLAMA_ROOT}/build/bin" NO_DEFAULT_PATH)
    find_library(GGML_LIBRARY NAMES ggml PATHS "${LLAMA_ROOT}/build-cuda/bin"
        "${LLAMA_ROOT}/build/bin" NO_DEFAULT_PATH)
    if(NOT LLAMA_INCLUDE_DIR OR NOT LLAMA_LIBRARY OR NOT GGML_LIBRARY)
        message(FATAL_ERROR "LLMOPT_ENABLE_LLAMA=ON 但未找到 llama.cpp 头/库，检查 LLMOPT_LLAMA_ROOT")
    endif()
    target_include_directories(${target} SYSTEM PRIVATE "${LLAMA_INCLUDE_DIR}" "${GGML_INCLUDE_DIR}")
    target_link_libraries(${target} PRIVATE "${LLAMA_LIBRARY}" "${GGML_LIBRARY}")
    target_compile_definitions(${target} PUBLIC LLMOPT_ENABLE_LLAMA=1)
    set(LLAMA_BIN_DIR "${LLAMA_ROOT}/build-cuda/bin")
    set_target_properties(${target} PROPERTIES BUILD_RPATH "${LLAMA_BIN_DIR}")
endfunction()

