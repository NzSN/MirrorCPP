# Tests (design §8). Unit tests require no mirror; integration tests are driven
# by $MIRROR_BIN and are added as sources land.
#
# Catch2 v3 — find_package first, FetchContent fallback.
find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
  message(STATUS "mirrorcpp: Catch2 v3 not found, fetching v3.5.0 into build tree")
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.0
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(Catch2)
endif()
include(Catch)

add_subdirectory(unit)
add_subdirectory(integration)

# Packaging smoke (design §7, M5): install into a scratch prefix and
# configure+build+run a tiny downstream consumer via find_package(mirrorcpp
# CONFIG). Slow (two extra CMake configure/build cycles); run explicitly
# with: ctest -R packaging_smoke. Skipped in sanitizer builds: linking a
# sanitizer-instrumented static lib into a non-sanitized consumer is
# expected to fail (undefined __asan/__ubsan symbols), so the smoke is only
# meaningful against an unsanitized install.
if(NOT MIRRORCPP_SANITIZE)
  add_test(NAME packaging_smoke
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/packaging/run_smoke.sh ${CMAKE_BINARY_DIR})
  set_tests_properties(packaging_smoke PROPERTIES
    TIMEOUT 600
    RUN_SERIAL TRUE)
endif()
