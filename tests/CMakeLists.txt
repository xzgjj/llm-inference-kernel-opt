# tests/CMakeLists.txt — 每个用例一个可执行，CTest label 与 docs/testing.md 对齐。
# 约定：unit/contract 用 doctest 单进程运行；integration 允许起子进程，仍标 fast。
include(CTest)

find_package(Python3 COMPONENTS Interpreter QUIET)

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")

# 统一登记：name 即可执行名，labels 供 -L 选择，sources 为用例文件。
# 粒度到 case：doctest_discover_tests 在构建后枚举 --list-test-cases，
# 每个 case 一条 CTest（形如 unit_config.端口、上下文…），可 -R 单跑、可 -j 并行。
function(llmopt_add_doctest name labels)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE llmopt_core doctest::doctest llmopt_warnings llmopt_coverage)
    # TEST_LIST 让 CTest 侧拿到该可执行枚举出的用例名；标签在我们的 include 文件里统一打。
    doctest_discover_tests(${name}
                           TEST_PREFIX "${name}."
                           TEST_LIST "${name}_TESTS"
                           WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    # doctest 的 PROPERTIES 透传会把 "unit;fast;hermetic" 按分号拆成多个属性名，
    # 这里改用目录级 TEST_INCLUDE_FILES：在它之后执行，按用例名逐个打标签。
    set(label_file "${CMAKE_CURRENT_BINARY_DIR}/${name}_labels.cmake")
    file(WRITE "${label_file}"
         "# 由 tests/CMakeLists.txt 生成：给 ${name} 的每个用例打标签\n"
         "foreach(t IN LISTS ${name}_TESTS)\n"
         "  set_tests_properties(\${t} PROPERTIES LABELS \"${labels}\" TIMEOUT 120)\n"
         "endforeach()\n")
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${label_file}")
endfunction()

llmopt_add_doctest(unit_protocol "unit;fast;hermetic" SOURCES unit/protocol_test.cc)
llmopt_add_doctest(unit_config "unit;fast;hermetic" SOURCES unit/config_test.cc)
llmopt_add_doctest(unit_kv "unit;fast;hermetic" SOURCES unit/kv_test.cc)
llmopt_add_doctest(unit_gate "unit;fast;hermetic" SOURCES unit/gate_test.cc)
llmopt_add_doctest(unit_gguf "unit;fast;hermetic" SOURCES unit/gguf_test.cc)
llmopt_add_doctest(unit_stub_runner "unit;fast;hermetic" SOURCES unit/stub_runner_test.cc)
llmopt_add_doctest(unit_text "unit;fast;hermetic" SOURCES unit/text_test.cc)
llmopt_add_doctest(unit_sink "unit;fast;hermetic" SOURCES unit/sink_test.cc)
llmopt_add_doctest(unit_metrics "unit;fast;hermetic" SOURCES unit/metrics_test.cc)
llmopt_add_doctest(unit_resource "unit;fast" SOURCES unit/resource_test.cc)
llmopt_add_doctest(chunk_planner_test "unit;fast;hermetic"
                   SOURCES unit/scheduler/chunk_planner_test.cc)
llmopt_add_doctest(cost_model_test "unit;fast;hermetic"
                   SOURCES unit/scheduler/cost_model_test.cc)
llmopt_add_doctest(contract_scheduler "contract;fast;hermetic" SOURCES contract/scheduler_test.cc)
llmopt_add_doctest(contract_continuous_batch "contract;fast;hermetic"
                   SOURCES contract/continuous_batch_test.cc)
llmopt_add_doctest(preempt_test "contract;fast;hermetic" SOURCES contract/preempt_test.cc)
llmopt_add_doctest(state_test "contract;fast;hermetic" SOURCES contract/state_test.cc)
llmopt_add_doctest(lifecycle_test "contract;fast;hermetic" SOURCES contract/lifecycle_test.cc)
llmopt_add_doctest(admission_test "contract;fast;hermetic" SOURCES contract/admission_test.cc)
# 阶段 5：C-P5-01…03 预留值对象只证明默认值等价 v0，不接入热路径。
llmopt_add_doctest(unit_p5_reserved "unit;fast;hermetic" SOURCES unit/p5_reserved_test.cc)

# 4-G0-7：只有真实后端构建才有 C 句柄可验；单独编译 runner_llama.cpp 以拿到工厂。
if(LLMOPT_ENABLE_LLAMA)
    llmopt_add_doctest(runner_raii_test "unit;fast;asan"
                       SOURCES unit/runner_raii_test.cc ${CMAKE_SOURCE_DIR}/src/runner_llama.cpp)
    add_llama_backend(runner_raii_test)
endif()

if(Python3_Interpreter_FOUND)
    add_test(NAME integration_server_smoke
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_smoke.py
                     $<TARGET_FILE:llmopt_server> ${CMAKE_BINARY_DIR}/evidence)
    set_tests_properties(integration_server_smoke PROPERTIES LABELS "integration;fast;hermetic")
    set_tests_properties(integration_server_smoke PROPERTIES TIMEOUT 120)

    add_test(NAME integration_server_concurrency
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_concurrency.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(integration_server_concurrency PROPERTIES LABELS "integration;fast;hermetic")
    set_tests_properties(integration_server_concurrency PROPERTIES TIMEOUT 120)

    add_test(NAME integration_server_chunked
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_chunked.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(integration_server_chunked PROPERTIES LABELS "integration;fast;hermetic")
    set_tests_properties(integration_server_chunked PROPERTIES TIMEOUT 180)

    add_test(NAME integration_server_metrics
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_metrics.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(integration_server_metrics PROPERTIES LABELS "integration;fast;hermetic")
    set_tests_properties(integration_server_metrics PROPERTIES TIMEOUT 120)

    add_test(NAME integration_server_admission
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_admission.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(integration_server_admission PROPERTIES
                         LABELS "integration;fast;hermetic")
    set_tests_properties(integration_server_admission PROPERTIES TIMEOUT 180)

    # 4-G1：小矩阵端到端；档间重启与 manifest/CSV 契约。
    add_test(NAME integration_server_matrix
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_matrix.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(integration_server_matrix PROPERTIES
                         LABELS "integration;fast;hermetic")
    set_tests_properties(integration_server_matrix PROPERTIES TIMEOUT 240)

    # 4-G6：长短 1:1 混合，分组入表且长请求不被饿死。
    add_test(NAME integration_server_mixed
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_mixed.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(integration_server_mixed PROPERTIES
                         LABELS "integration;fast;hermetic")
    set_tests_properties(integration_server_mixed PROPERTIES TIMEOUT 240)

    # 4-G0-2：慢读 1 帧/s 与快请求并发，输出 backpressure.csv。
    add_test(NAME integration_server_slow_client
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_slow_client.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(integration_server_slow_client PROPERTIES
                         LABELS "integration;fast;hermetic")
    set_tests_properties(integration_server_slow_client PROPERTIES TIMEOUT 120)

    # 真实服务契约：需要外部已运行的真模型服务（LLMOPT_REAL_BASE）；未设置即 SKIP(77)。
    add_test(NAME e2e_real_service_contract
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_real_service_contract.py)
    set_tests_properties(e2e_real_service_contract PROPERTIES
                         LABELS "e2e;slow;needs-gpu"
                         SKIP_RETURN_CODE 77
                         TIMEOUT 300)
endif()
