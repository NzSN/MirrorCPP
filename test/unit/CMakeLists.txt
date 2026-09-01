add_executable(mirrorcpp_smoke_test smoke_test.cpp)
target_link_libraries(mirrorcpp_smoke_test PRIVATE
  mirrorcpp::mirrorcpp
  Catch2::Catch2WithMain)

# ctest wiring via Catch2 discovery
catch_discover_tests(mirrorcpp_smoke_test)

# Transport unit tests: loopback TCP echo, framing, stdio spawn/exit-code.
# They reach the internal detail/line.hpp framing helper via ${CMAKE_SOURCE_DIR}/src
# and use std::thread for the loopback echo fixture.
add_executable(mirrorcpp_transport_test transport_test.cpp)
target_link_libraries(mirrorcpp_transport_test PRIVATE
  mirrorcpp::mirrorcpp
  Catch2::Catch2WithMain)
find_package(Threads REQUIRED)
target_link_libraries(mirrorcpp_transport_test PRIVATE Threads::Threads)
target_include_directories(mirrorcpp_transport_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

catch_discover_tests(mirrorcpp_transport_test)

# TLS unit tests (design §3.6/§5.3/§9): throwaway PKI via the openssl CLI + a
# loopback C++ TLS test server. Only when the library is built with TLS.
if(MIRRORCPP_WITH_TLS)
  add_executable(mirrorcpp_tls_test tls_test.cpp)
  target_link_libraries(mirrorcpp_tls_test PRIVATE
    mirrorcpp::mirrorcpp
    OpenSSL::SSL
    Catch2::Catch2WithMain)
  find_package(Threads REQUIRED)
  target_link_libraries(mirrorcpp_tls_test PRIVATE Threads::Threads)
  catch_discover_tests(mirrorcpp_tls_test)
endif()

# Registry discovery/connect unit tests (design §5.4/§8): loopback HTTP stub +
# fail-closed matrix; the connect_from_registry cases reuse tls_fixture.hpp.
# Whole binary is TLS-gated (connect_from_registry is a TLS-only API).
if(MIRRORCPP_WITH_TLS)
  add_executable(mirrorcpp_registry_test registry_test.cpp)
  target_link_libraries(mirrorcpp_registry_test PRIVATE
    mirrorcpp::mirrorcpp
    OpenSSL::SSL
    Catch2::Catch2WithMain)
  find_package(Threads REQUIRED)
  target_link_libraries(mirrorcpp_registry_test PRIVATE Threads::Threads)
  # Same TLS-awareness as the library so the fixture + connect tests compile in.
  target_compile_definitions(mirrorcpp_registry_test PRIVATE
    MIRRORCPP_WITH_TLS=$<BOOL:${MIRRORCPP_WITH_TLS}>)
  catch_discover_tests(mirrorcpp_registry_test)
endif()

# Value + ITF codec unit tests (design §8: value matrix, #bigint, #map, sets).
add_executable(mirrorcpp_value_test value_test.cpp)
target_link_libraries(mirrorcpp_value_test PRIVATE
  mirrorcpp::mirrorcpp
  Catch2::Catch2WithMain)
catch_discover_tests(mirrorcpp_value_test)

# Error model unit tests (design §4.3).
add_executable(mirrorcpp_error_test error_test.cpp)
target_link_libraries(mirrorcpp_error_test PRIVATE
  mirrorcpp::mirrorcpp
  Catch2::Catch2WithMain)
catch_discover_tests(mirrorcpp_error_test)

# Protocol codec + diff hints + phase guard unit tests (design §8).
add_executable(mirrorcpp_protocol_test protocol_test.cpp)
target_link_libraries(mirrorcpp_protocol_test PRIVATE
  mirrorcpp::mirrorcpp
  Catch2::Catch2WithMain)
catch_discover_tests(mirrorcpp_protocol_test)

# Spec EXTENDS/INSTANCE closure resolution tests (design §5.5/§8).
# Need the fixture dir (test/fixtures) and the vendored specs dir baked in.
add_executable(mirrorcpp_spec_test spec_test.cpp)
target_link_libraries(mirrorcpp_spec_test PRIVATE
  mirrorcpp::mirrorcpp
  Catch2::Catch2WithMain)
target_compile_definitions(mirrorcpp_spec_test PRIVATE
  MIRRORCPP_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures"
  MIRRORCPP_SPECS_DIR="${CMAKE_SOURCE_DIR}/specs")
catch_discover_tests(mirrorcpp_spec_test)

# Golden wire corpus replay (guide §10 item 1): the vendored
# fixtures/golden/*.jsonl (frozen from Haskell ModelMirrors@3496251) drive
# decode-side (mirror_messages), encode-side (client_messages), value-codec
# (decode_only), registry-parser (consul_payloads) and equality (diff_cases)
# vectors. Always on — the corpus is vendored, no MIRROR_BIN needed.
add_executable(mirrorcpp_golden_corpus_test golden_corpus_test.cpp)
target_link_libraries(mirrorcpp_golden_corpus_test PRIVATE
  mirrorcpp::mirrorcpp
  Catch2::Catch2WithMain)
target_compile_definitions(mirrorcpp_golden_corpus_test PRIVATE
  MIRRORCPP_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures")
catch_discover_tests(mirrorcpp_golden_corpus_test)

