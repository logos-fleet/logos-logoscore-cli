# ── Unit tests (no external runtime needed) ──────────────────────────────────

# Testable library: config, daemon_state, output, commands (with mock client)
set(TEST_LIB_SOURCES
    ${CMAKE_SOURCE_DIR}/src/config.cpp
    ${CMAKE_SOURCE_DIR}/src/paths.cpp
    ${CMAKE_SOURCE_DIR}/src/yaml_json.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/daemon_state.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/access_policy_arg.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/port_allocator.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/token_store.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/log_sink.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/package_bootstrap.cpp
    # The proxied-call envelope. Pure (no Qt, no logos-protocol), which is why
    # it can link here at all — and why the ok / METHOD_FAILED / METHOD_NOT_FOUND
    # decision is unit-tested rather than only reachable from a live daemon.
    ${CMAKE_SOURCE_DIR}/src/core_service/call_envelope.cpp
    ${CMAKE_SOURCE_DIR}/src/client/client_state.cpp
    ${CMAKE_SOURCE_DIR}/src/client/output.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/status_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/load_module_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/unload_module_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/reload_module_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/list_modules_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/module_info_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/call_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/arg_coerce.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/watch_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/stats_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/stop_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/issue_token_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/revoke_token_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/list_tokens_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/package_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/catalog_command.cpp
    ${CMAKE_SOURCE_DIR}/src/client/commands/config_command.cpp
)

add_library(logosctl_testlib STATIC ${TEST_LIB_SOURCES})
target_include_directories(logosctl_testlib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${LOGOS_LIBLOGOS_ROOT}/include
)
target_link_libraries(logosctl_testlib PUBLIC
    CLI11::CLI11
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    fmt::fmt
    yaml-cpp::yaml-cpp
    spdlog::spdlog
)

# Unit tests
add_executable(unit_tests
    test_config.cpp
    test_log_sink.cpp
    test_access_policy_arg.cpp
    test_daemon_state.cpp
    test_paths.cpp
    test_output.cpp
    test_commands.cpp
    test_mode_detection.cpp
    test_token_store.cpp
    test_port_allocator.cpp
    test_process_util.cpp
    test_local_endpoint.cpp
    test_package_bootstrap.cpp
    test_call_envelope.cpp
    test_arg_coerce.cpp
    test_rpc_deadlines.cpp
)

target_include_directories(unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(unit_tests PRIVATE
    logosctl_testlib
    GTest::gtest_main
    Qt${QT_VERSION_MAJOR}::Core
)

gtest_discover_tests(unit_tests)

# ── CLI integration tests (require compiled logosctl binary) ─────────────────

add_executable(cli_tests
    test_cli.cpp
)

target_link_libraries(cli_tests PRIVATE
    GTest::gtest_main
    Qt${QT_VERSION_MAJOR}::Core
)

target_include_directories(cli_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

gtest_discover_tests(cli_tests)

# The same suite against logoscore. Both binaries ship, so both are tested;
# this target goes away with the tool.
add_executable(cli_tests_logoscore
    test_cli_logoscore.cpp
)

target_link_libraries(cli_tests_logoscore PRIVATE
    GTest::gtest_main
    Qt${QT_VERSION_MAJOR}::Core
)

target_include_directories(cli_tests_logoscore PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

gtest_discover_tests(cli_tests_logoscore)

# ── Daemon-backed integration tests (own target, own CTest entry) ────────────
#
# Kept separate from cli_tests and registered as a SINGLE CTest test
# (not gtest_discover_tests): the suite shares one daemon across all
# LoadedModuleTest cases via SetUpTestSuite(). Per-case discovery would
# make CTest spawn one process per test, defeating that sharing and
# starting a fresh daemon for every case. The Nix check runs the binary
# directly (also one process), so sharing holds there too.

add_executable(integration_tests
    test_integration.cpp
)

target_link_libraries(integration_tests PRIVATE
    GTest::gtest_main
    Qt${QT_VERSION_MAJOR}::Core
)

target_include_directories(integration_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Same again for logoscore. Deleted with the tool.
add_executable(integration_tests_logoscore
    test_integration_logoscore.cpp
)

target_link_libraries(integration_tests_logoscore PRIVATE
    GTest::gtest_main
    Qt${QT_VERSION_MAJOR}::Core
)

target_include_directories(integration_tests_logoscore PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME integration_tests COMMAND integration_tests)
