# tests/CMakeLists.txt — 每个用例一个可执行，CTest label 与 docs/testing.md 对齐。
# 约定：unit/contract 用 doctest 单进程运行；integration 允许起子进程，仍标 fast。
include(CTest)

find_package(Python3 COMPONENTS Interpreter QUIET)

# 统一登记：name 即 CTest 名，labels 供 -L 选择，sources 为用例文件。
function(llmopt_add_doctest name labels)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE llmopt_core doctest::doctest llmopt_warnings)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "${labels}")
endfunction()

llmopt_add_doctest(unit_protocol "unit;fast;hermetic" SOURCES unit/protocol_test.cc)
llmopt_add_doctest(unit_config "unit;fast;hermetic" SOURCES unit/config_test.cc)
llmopt_add_doctest(unit_kv "unit;fast;hermetic" SOURCES unit/kv_test.cc)
llmopt_add_doctest(unit_gate "unit;fast;hermetic" SOURCES unit/gate_test.cc)
llmopt_add_doctest(unit_gguf "unit;fast;hermetic" SOURCES unit/gguf_test.cc)
llmopt_add_doctest(unit_stub_runner "unit;fast;hermetic" SOURCES unit/stub_runner_test.cc)
llmopt_add_doctest(contract_scheduler "contract;fast;hermetic" SOURCES contract/scheduler_test.cc)

if(Python3_Interpreter_FOUND)
    add_test(NAME integration_server_smoke
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_smoke.py
                     $<TARGET_FILE:llmopt_server> ${CMAKE_BINARY_DIR}/evidence)
    set_tests_properties(integration_server_smoke PROPERTIES LABELS "integration;fast;hermetic")
    set_tests_properties(integration_server_smoke PROPERTIES TIMEOUT 120)
endif()
