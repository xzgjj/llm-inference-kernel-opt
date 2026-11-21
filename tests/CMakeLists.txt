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

# nightly_real_backend.py 由 CI real-backend 作业直接调用（需真模型与 LLAMA 构建），
# 不进 CTest 注册表，避免无模型环境把 nightly 用例误当默认测试。
llmopt_add_doctest(unit_protocol "unit;fast;hermetic" SOURCES unit/protocol_test.cc)
llmopt_add_doctest(unit_sha256 "unit;fast;hermetic" SOURCES unit/sha256_test.cc)
llmopt_add_doctest(unit_config "unit;fast;hermetic" SOURCES unit/config_test.cc)
llmopt_add_doctest(unit_kv "unit;fast;hermetic" SOURCES unit/kv_test.cc)
llmopt_add_doctest(unit_gate "unit;fast;hermetic" SOURCES unit/gate_test.cc)
llmopt_add_doctest(unit_gguf "unit;fast;hermetic" SOURCES unit/gguf_test.cc)
llmopt_add_doctest(unit_stub_runner "unit;fast;hermetic" SOURCES unit/stub_runner_test.cc)
llmopt_add_doctest(unit_text "unit;fast;hermetic" SOURCES unit/text_test.cc)
llmopt_add_doctest(unit_sink "unit;fast;hermetic" SOURCES unit/sink_test.cc)
llmopt_add_doctest(unit_metrics "unit;fast;hermetic" SOURCES unit/metrics_test.cc)
llmopt_add_doctest(unit_service "unit;fast;hermetic" SOURCES unit/service_test.cc)
# DEBUG-04：线程 join 的 RAII 守卫，异常展开路径不得 terminate。
llmopt_add_doctest(unit_thread_guard "unit;fast;hermetic" SOURCES unit/thread_guard_test.cc)
llmopt_add_doctest(unit_rate_limit "unit;fast;hermetic" SOURCES unit/rate_limit_test.cc)
llmopt_add_doctest(unit_auth "unit;fast;hermetic" SOURCES unit/auth_test.cc)
llmopt_add_doctest(unit_resource "unit;fast" SOURCES unit/resource_test.cc)
llmopt_add_doctest(chunk_planner_test "unit;fast;hermetic"
                   SOURCES unit/scheduler/chunk_planner_test.cc)
llmopt_add_doctest(cost_model_test "unit;fast;hermetic"
                   SOURCES unit/scheduler/cost_model_test.cc)
llmopt_add_doctest(contract_scheduler "contract;fast;hermetic" SOURCES contract/scheduler_test.cc)
llmopt_add_doctest(contract_continuous_batch "contract;fast;hermetic"
                   SOURCES contract/continuous_batch_test.cc)
llmopt_add_doctest(contract_batch_policy "contract;fast;hermetic"
                   SOURCES contract/batch_policy_test.cc)
llmopt_add_doctest(preempt_test "contract;fast;hermetic" SOURCES contract/preempt_test.cc)
llmopt_add_doctest(state_test "contract;fast;hermetic" SOURCES contract/state_test.cc)
llmopt_add_doctest(lifecycle_test "contract;fast;hermetic" SOURCES contract/lifecycle_test.cc)
llmopt_add_doctest(admission_test "contract;fast;hermetic" SOURCES contract/admission_test.cc)
llmopt_add_doctest(contract_tenant "contract;fast;hermetic" SOURCES contract/tenant_test.cc)
# DEBUG-01/02：/metrics 只读 Core 快照；租户名额失败不留空计数器、不泄漏准入。
llmopt_add_doctest(contract_metrics_snapshot "contract;fast;hermetic"
                   SOURCES contract/metrics_snapshot_test.cc)
llmopt_add_doctest(completion_watchdog_test "contract;fast;hermetic"
                   SOURCES contract/watchdog_test.cc)
# 阶段 5：C-P5-01…03 预留值对象只证明默认值等价 v0，不接入热路径。
llmopt_add_doctest(unit_p5_reserved "unit;fast;hermetic" SOURCES unit/p5_reserved_test.cc)

# 4-G0-7：只有真实后端构建才有 C 句柄可验；单独编译 runner_llama.cpp 以拿到工厂。
if(LLMOPT_ENABLE_LLAMA)
    llmopt_add_doctest(runner_raii_test "unit;fast;asan"
                       SOURCES unit/runner_raii_test.cc ${CMAKE_SOURCE_DIR}/src/runner_llama.cpp)
    add_llama_backend(runner_raii_test)

    # 真模型路径单独进程级：缺 LLMOPT_TEST_MODEL 时 CTest 记 Skip(77)，不再伪装 Passed。
    add_executable(runner_real_probe integration/runner_real_probe.cc
                   ${CMAKE_SOURCE_DIR}/src/runner_llama.cpp)
    target_link_libraries(runner_real_probe PRIVATE llmopt_core llmopt_warnings)
    add_llama_backend(runner_real_probe)
    if(Python3_Interpreter_FOUND)
        add_test(NAME integration_runner_real
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_runner_real.py
                         $<TARGET_FILE:runner_real_probe>)
        set_tests_properties(integration_runner_real PROPERTIES
                             LABELS "integration;slow;needs-gpu"
                             SKIP_RETURN_CODE 77
                             TIMEOUT 300)
    endif()
endif()

# 阶段 12 S5：崩溃注入 fixture，由 Python 用例断言 dump 内容与退出语义。
add_executable(crash_fixture tools/crash_fixture.cc)
target_link_libraries(crash_fixture PRIVATE llmopt_core llmopt_warnings)

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

    # 6-G2：四层优先级与启动来源打印（CLI > env > file > default）。
    add_test(NAME integration_server_config_sources
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_config_sources.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(integration_server_config_sources PROPERTIES
                         LABELS "integration;fast;hermetic")
    set_tests_properties(integration_server_config_sources PROPERTIES TIMEOUT 120)

    # D6-2：配置 api-key 后 /v1/** 必须 Bearer；探针与 /metrics 开放。
    add_test(NAME auth_test
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_auth.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(auth_test PROPERTIES
                         LABELS "integration;fast;hermetic")
    set_tests_properties(auth_test PROPERTIES TIMEOUT 120)

    # 阶段 11：静态 key → tenant；每 key 限流与 KV 配额独立。
    add_test(NAME tenant_keys_test
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_tenant_keys.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(tenant_keys_test PROPERTIES
                         LABELS "integration;fast;hermetic")
    set_tests_properties(tenant_keys_test PROPERTIES TIMEOUT 120)

    # 阶段 12 S5：SIGSEGV → logs/crash dump（含 signal/pid/version）→ 非零退出，只留 2 份。
    add_test(NAME crash_dump_test
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_crash_dump.py
                     $<TARGET_FILE:crash_fixture>)
    set_tests_properties(crash_dump_test PROPERTIES
                         LABELS "integration;fast;hermetic")
    set_tests_properties(crash_dump_test PROPERTIES TIMEOUT 120)

    # 阶段 12 S4：同 id 多摘要轮换 + Windows 轮询热加载（Linux 走 SIGHUP）。
    add_test(NAME keys_reload_test
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_keys_reload.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(keys_reload_test PROPERTIES
                         LABELS "integration;fast;hermetic")
    set_tests_properties(keys_reload_test PROPERTIES TIMEOUT 180)

    # 阶段 12 S1：非流式断连 → ≤2s 取消 → KV 归零。
    add_test(NAME disconnect_cancel_test
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_disconnect_cancel.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(disconnect_cancel_test PROPERTIES
                         LABELS "integration;fast;hermetic")
    set_tests_properties(disconnect_cancel_test PROPERTIES TIMEOUT 120)

    # 6-G4：令牌桶超额 429 + Retry-After，回填后恢复。
    add_test(NAME backpressure_test
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_rate_limit.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(backpressure_test PROPERTIES
                         LABELS "integration;fast;hermetic")
    set_tests_properties(backpressure_test PROPERTIES TIMEOUT 120)

    # 6-G3：SIGTERM 排空（Windows 跳过，CI Linux 真跑）。
    add_test(NAME service_state_test
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_drain.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(service_state_test PROPERTIES
                         LABELS "integration;fast;hermetic")
    set_tests_properties(service_state_test PROPERTIES TIMEOUT 180)

    # 6-G1/6-G5：三端点兼容面、stop/n/seed、旧端点回归与请求 ID 日志。
    add_test(NAME compat_openai_test
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_compat_openai.py
                     $<TARGET_FILE:llmopt_server> ${CMAKE_BINARY_DIR}/evidence)
    set_tests_properties(compat_openai_test PROPERTIES
                         LABELS "integration;fast;hermetic")
    set_tests_properties(compat_openai_test PROPERTIES TIMEOUT 180)

    add_test(NAME integration_server_admission
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_admission.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(integration_server_admission PROPERTIES
                         LABELS "integration;fast;hermetic")
    set_tests_properties(integration_server_admission PROPERTIES TIMEOUT 180)

    add_test(NAME integration_server_queue_capacity
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_queue_capacity.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(integration_server_queue_capacity PROPERTIES
                         LABELS "integration;fast;hermetic")
    set_tests_properties(integration_server_queue_capacity PROPERTIES TIMEOUT 180)

    # P1-3：static / continuous 黑盒对照，证明"完成即补位"与"整批跑空"可区分。
    add_test(NAME integration_server_batch_policy
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_batch_policy.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(integration_server_batch_policy PROPERTIES
                         LABELS "integration;fast;hermetic")
    set_tests_properties(integration_server_batch_policy PROPERTIES TIMEOUT 180)

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

    add_test(NAME integration_server_control_isolation
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_control_isolation.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(integration_server_control_isolation PROPERTIES
                         LABELS "integration;fast;hermetic")
    set_tests_properties(integration_server_control_isolation PROPERTIES TIMEOUT 120)

    if(LLMOPT_ENABLE_TLS)
        add_test(NAME integration_server_tls
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_tls.py
                         $<TARGET_FILE:llmopt_server>)
        set_tests_properties(integration_server_tls PROPERTIES
                             LABELS "integration;fast;hermetic"
                             SKIP_RETURN_CODE 77)
        set_tests_properties(integration_server_tls PROPERTIES TIMEOUT 120)
    endif()

    add_test(NAME integration_server_tls_config_error
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server_tls_config_error.py
                     $<TARGET_FILE:llmopt_server>)
    set_tests_properties(integration_server_tls_config_error PROPERTIES
                         LABELS "integration;fast;hermetic")
    set_tests_properties(integration_server_tls_config_error PROPERTIES TIMEOUT 120)

    # 真实服务契约：需要外部已运行的真模型服务（LLMOPT_REAL_BASE）；未设置即 SKIP(77)。
    add_test(NAME e2e_real_service_contract
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_real_service_contract.py)
    set_tests_properties(e2e_real_service_contract PROPERTIES
                         LABELS "e2e;slow;needs-gpu"
                         SKIP_RETURN_CODE 77
                         TIMEOUT 300)
endif()

# 并行安全边界：integration 用例都会起真实进程、抢临时端口（free_port 的 bind/close 之间
# 存在 TOCTOU），因此共用一把 RESOURCE_LOCK 串行执行；unit/contract 不带锁，-j 仍并行。
# 只按 LABELS 里的 integration 选取，不能把同目录的 unit/contract 一起锁掉（那会退化成串行）。
get_property(llmopt_dir_tests DIRECTORY PROPERTY TESTS)
set(llmopt_locked_tests "")
foreach(t IN LISTS llmopt_dir_tests)
    get_test_property(${t} LABELS llmopt_test_labels)
    if(llmopt_test_labels MATCHES "integration" AND NOT t MATCHES "^e2e_")
        list(APPEND llmopt_locked_tests "${t}")
    endif()
endforeach()
if(llmopt_locked_tests)
    set_tests_properties(${llmopt_locked_tests} PROPERTIES RESOURCE_LOCK llmopt_integration)
endif()
