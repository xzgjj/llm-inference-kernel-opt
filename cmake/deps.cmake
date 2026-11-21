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
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.5.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(httplib nlohmann_json spdlog doctest)

# 真实 llama.cpp 后端：复用 WSL 侧已构建的共享库，不在此处重编依赖树。
# 后端 revision 必须可核对：默认只告警（本机 WSL 目录未必是 tag），CI 设严格模式即失败。
set(LLMOPT_LLAMA_EXPECTED_REVISION "b12557" CACHE STRING "期望的 llama.cpp tag/describe 前缀")
option(LLMOPT_LLAMA_REVISION_STRICT "revision 不一致直接 FATAL_ERROR" OFF)
function(add_llama_backend target)
    set(LLAMA_ROOT "$ENV{LLMOPT_LLAMA_ROOT}" CACHE PATH "llama.cpp source root")
    if(NOT LLAMA_ROOT)
        set(LLAMA_ROOT "/root/llm-src/llama.cpp")
    endif()
    # 版本出口：把实际 describe 写进编译定义，--version 与 /metrics 共用同一字符串。
    set(LLAMA_REVISION "unknown")
    if(EXISTS "${LLAMA_ROOT}/.git")
        execute_process(COMMAND git -C "${LLAMA_ROOT}" describe --tags --always --dirty
                        OUTPUT_VARIABLE LLAMA_DESC OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET RESULT_VARIABLE LLAMA_GIT_RC)
        if(LLAMA_GIT_RC EQUAL 0)
            set(LLAMA_REVISION "${LLAMA_DESC}")
        endif()
    endif()
    if(LLAMA_REVISION STREQUAL "unknown")
        message(WARNING "无法读取 llama.cpp revision（${LLAMA_ROOT} 无 .git），构建将标注 unknown")
    elseif(NOT LLAMA_REVISION MATCHES "${LLMOPT_LLAMA_EXPECTED_REVISION}")
        if(LLMOPT_LLAMA_REVISION_STRICT)
            message(FATAL_ERROR "llama.cpp revision=${LLAMA_REVISION} 与期望 "
                                "${LLMOPT_LLAMA_EXPECTED_REVISION} 不一致（严格模式）")
        else()
            message(WARNING "llama.cpp revision=${LLAMA_REVISION} 与文档期望 "
                            "${LLMOPT_LLAMA_EXPECTED_REVISION} 不一致，请同步文档或设严格模式")
        endif()
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
    target_compile_definitions(${target} PUBLIC LLMOPT_ENABLE_LLAMA=1
                                         LLMOPT_LLAMA_REVISION="${LLAMA_REVISION}")
    set(LLAMA_BIN_DIR "${LLAMA_ROOT}/build-cuda/bin")
    set_target_properties(${target} PROPERTIES BUILD_RPATH "${LLAMA_BIN_DIR}")
endfunction()

