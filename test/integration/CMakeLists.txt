# Integration tests (design §8).
#
# These are driven by $MIRROR_BIN over stdio/TCP/mTLS+registry and skip when the
# env var is unset. The fake-mirror tests (design §8) need NO real mirror: a
# scripted python3 peer (test/integration/fake_mirror*) speaks the protocol over
# stdio and is spawned via spawn_mirror (run_client_with_traces).

# ---------------------------------------------------------------------------
# Fake-mirror harness (design §8): python3 scripts spoken to over stdio.
# spawn_mirror runs the script directly (shebang), so the scenario lives in the
# executable name; each wrapper imports the shared fake_mirror.py module.
# ---------------------------------------------------------------------------
set(MIRRORCPP_INTEGRATION_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# The wrappers are executed via their shebang (spawn_mirror passes no args), so
# they need the executable bit.
set(_fake_scripts
  fake_mirror_happy
  fake_mirror_happy_wrap
  fake_mirror_mismatch
  fake_mirror_protoerr
  fake_mirror_unexpected
  fake_mirror_invalid
  fake_mirror_register_happy
  fake_mirror_register_inline
  fake_mirror_validate_valid
  fake_mirror_validate_invalid
  fake_mirror_validate_bound101
  fake_mirror_gen_traces
  fake_mirror_explore_session
  fake_mirror_explore_bad_command
  fake_mirror_explore_stepping
  fake_mirror_trace
  fake_mirror_tcp.py
  fake_mirror_async.py)
foreach(_s IN LISTS _fake_scripts)
  file(CHMOD ${CMAKE_CURRENT_SOURCE_DIR}/${_s}
       PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                   GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endforeach()

# Client stepping-loop integration tests against the fake mirror.
add_executable(mirrorcpp_client_integration_test client_integration_test.cpp)
target_link_libraries(mirrorcpp_client_integration_test PRIVATE
  mirrorcpp::mirrorcpp
  Catch2::Catch2WithMain)
target_include_directories(mirrorcpp_client_integration_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mirrorcpp_client_integration_test PRIVATE
  MIRRORCPP_TEST_INTEGRATION_DIR="${MIRRORCPP_INTEGRATION_DIR}")
catch_discover_tests(mirrorcpp_client_integration_test)

# One-shot flow integration tests (register, register_validate, register_trace_gen).
add_executable(mirrorcpp_one_shot_flows_test one_shot_flows_test.cpp)
target_link_libraries(mirrorcpp_one_shot_flows_test PRIVATE
  mirrorcpp::mirrorcpp
  Catch2::Catch2WithMain)
target_include_directories(mirrorcpp_one_shot_flows_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mirrorcpp_one_shot_flows_test PRIVATE
  MIRRORCPP_TEST_INTEGRATION_DIR="${MIRRORCPP_INTEGRATION_DIR}")
catch_discover_tests(mirrorcpp_one_shot_flows_test)

# Explore-session + register_explore integration tests.
add_executable(mirrorcpp_explore_session_test explore_session_test.cpp)
target_link_libraries(mirrorcpp_explore_session_test PRIVATE
  mirrorcpp::mirrorcpp
  Catch2::Catch2WithMain)
target_include_directories(mirrorcpp_explore_session_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mirrorcpp_explore_session_test PRIVATE
  MIRRORCPP_TEST_INTEGRATION_DIR="${MIRRORCPP_INTEGRATION_DIR}")
catch_discover_tests(mirrorcpp_explore_session_test)

# Async job interface integration tests (guide §6): the async API against the
# scripted TCP fake mirror (fake_mirror_async.py) — submit/query/await/cancel,
# long-poll + idempotent terminals (C18), queue full (C22), unknown ids (C21),
# infraError outcomes, cross-connection job control (C17), stdio preflight.
add_executable(mirrorcpp_async_jobs_test async_jobs_test.cpp)
target_link_libraries(mirrorcpp_async_jobs_test PRIVATE
  mirrorcpp::mirrorcpp
  Catch2::Catch2WithMain)
target_include_directories(mirrorcpp_async_jobs_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mirrorcpp_async_jobs_test PRIVATE
  MIRRORCPP_TEST_INTEGRATION_DIR="${MIRRORCPP_INTEGRATION_DIR}")
catch_discover_tests(mirrorcpp_async_jobs_test)

# ---------------------------------------------------------------------------
# MBT fixture protocol tests (design §8 "Conformance north star"): drive the
# client flows through the trace-driven fake mirror using the vendored
# specs/traces/*.itf.json fixtures (all_steps_done, step_mismatch,
# register_error, explore_session, explore_cmd, fault_close) and assert the
# client emits the exact message-code sequences the protocol model produces.
add_executable(mirrorcpp_mbt_test mbt_test.cpp)
target_link_libraries(mirrorcpp_mbt_test PRIVATE
  mirrorcpp::mirrorcpp
  Catch2::Catch2WithMain)
target_include_directories(mirrorcpp_mbt_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mirrorcpp_mbt_test PRIVATE
  MIRRORCPP_TEST_INTEGRATION_DIR="${MIRRORCPP_INTEGRATION_DIR}"
  MIRRORCPP_SPECS_DIR="${CMAKE_SOURCE_DIR}/specs")
catch_discover_tests(mirrorcpp_mbt_test)

# CLI (mirrorcpp-validate) integration tests (design §5.7): drive the built
# binary against the python fake-mirror over TCP and check the exit-code matrix.
# ---------------------------------------------------------------------------
if(MIRRORCPP_BUILD_CLI)
  add_executable(mirrorcpp_cli_validate_test cli_validate_test.cpp)
  target_link_libraries(mirrorcpp_cli_validate_test PRIVATE
    Catch2::Catch2WithMain)
  target_include_directories(mirrorcpp_cli_validate_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(mirrorcpp_cli_validate_test PRIVATE
    MIRRORCPP_TEST_INTEGRATION_DIR="${MIRRORCPP_INTEGRATION_DIR}"
    MIRRORCPP_CLI_BIN="$<TARGET_FILE:mirrorcpp-validate>"
    MIRRORCPP_WITH_TLS=$<BOOL:${MIRRORCPP_WITH_TLS}>)
  catch_discover_tests(mirrorcpp_cli_validate_test)
endif()

# ---------------------------------------------------------------------------
# Real-mirror test: replays Counter over stdio, TCP, and a real ephemeral-PKI
# mTLS server, then exercises the broader HourClock/async matrix. Standalone
# program: skips with exit code 77 when $MIRROR_BIN is unset.
# ---------------------------------------------------------------------------
add_executable(mirrorcpp_real_mirror_test real_mirror_test.cpp)
target_link_libraries(mirrorcpp_real_mirror_test PRIVATE
  mirrorcpp::mirrorcpp)
target_include_directories(mirrorcpp_real_mirror_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/model-interface/counter/generated)
target_compile_definitions(mirrorcpp_real_mirror_test PRIVATE
  MIRRORCPP_TEST_INTEGRATION_DIR="${MIRRORCPP_INTEGRATION_DIR}")
add_test(NAME real_mirror_hourclock COMMAND mirrorcpp_real_mirror_test)
set_tests_properties(real_mirror_hourclock PROPERTIES
  SKIP_RETURN_CODE 77)
