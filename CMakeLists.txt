# MirrorCPP — C++ client library for the ModelMirrors conformance-checking protocol.
# Design: docs/design.md (§6 dependencies/build, §7 project layout).
# CMake >= 3.24, C++23 (GCC 13+, Clang 17+, MSVC 19.38+).

cmake_minimum_required(VERSION 3.24)

project(mirrorcpp
  VERSION 0.1.0
  DESCRIPTION "C++ client library for the ModelMirrors conformance-checking protocol"
  HOMEPAGE_URL "https://github.com/NzSN/MirrorCPP"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options (design §6)
# ---------------------------------------------------------------------------
option(MIRRORCPP_WITH_TLS       "Build TLS transport + registry connect (requires OpenSSL >= 3.0)" ON)
option(MIRRORCPP_BUILD_TESTS    "Build unit/integration tests (requires Catch2 v3)" ON)
option(MIRRORCPP_BUILD_EXAMPLES "Build example programs" ON)
option(MIRRORCPP_BUILD_CLI      "Build the mirrorcpp-validate CLI" ON)
option(MIRRORCPP_WERROR         "Treat warnings as errors" OFF)
set(MIRRORCPP_SANITIZE "" CACHE STRING
    "Comma-separated sanitizers applied to all targets (e.g. address,undefined); empty disables")

# ---------------------------------------------------------------------------
# Language baseline + warnings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra)
if(MIRRORCPP_WERROR)
  add_compile_options(-Werror)
endif()

if(MIRRORCPP_SANITIZE)
  string(REPLACE "," ";" MIRRORCPP_SANITIZE_LIST "${MIRRORCPP_SANITIZE}")
  foreach(_s IN LISTS MIRRORCPP_SANITIZE_LIST)
    string(STRIP "${_s}" _s)
    if(_s)
      add_compile_options(-fsanitize=${_s} -fno-omit-frame-pointer)
      add_link_options(-fsanitize=${_s})
    endif()
  endforeach()
endif()

include(GNUInstallDirs)
include(CTest)   # provides BUILD_TESTING + enable_testing()

# ---------------------------------------------------------------------------
# Dependencies (design §6)
# ---------------------------------------------------------------------------
include(FetchContent)

# -- nlohmann_json >= 3.11: JSON DOM (header-only). find_package first,
#    FetchContent fallback (no system headers on CI/dev machines).
find_package(nlohmann_json 3.11 QUIET CONFIG)
if(NOT nlohmann_json_FOUND)
  message(STATUS "mirrorcpp: nlohmann_json not found, fetching v3.11.3 into build tree")
  FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# -- OpenSSL >= 3.0: TLS 1.3 transport, SHA-256 pins (optional build feature).
if(MIRRORCPP_WITH_TLS)
  find_package(OpenSSL 3.0 REQUIRED)
endif()

# -- Boost.Multiprecision: header-only cpp_int (the only Boost component used).
#    Prefer a system Boost; otherwise fetch just the needed boostorg header
#    modules (hermetic, no system headers required).
find_package(Boost 1.74 QUIET)
if(Boost_FOUND AND EXISTS "${Boost_INCLUDE_DIR}/boost/multiprecision/cpp_int.hpp")
  message(STATUS "mirrorcpp: using system Boost ${Boost_VERSION} at ${Boost_INCLUDE_DIR}")
  if(NOT TARGET Boost::headers)
    add_library(Boost::headers INTERFACE IMPORTED)
    set_target_properties(Boost::headers PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIR}")
  endif()
else()
  message(STATUS "mirrorcpp: system Boost.Multiprecision not found; fetching boostorg headers (boost-1.85.0)")
  set(MIRRORCPP_BOOST_TAG boost-1.85.0)
  set(_boost_modules
    multiprecision config core assert static_assert throw_exception exception
    type_traits utility integer mpl preprocessor predef rational
    smart_ptr tuple io winapi)
  set(_boost_incdirs "")
  foreach(_bm IN LISTS _boost_modules)
    FetchContent_Declare(boost_${_bm}
      GIT_REPOSITORY https://github.com/boostorg/${_bm}.git
      GIT_TAG        ${MIRRORCPP_BOOST_TAG}
      GIT_SHALLOW    TRUE)
    FetchContent_GetProperties(boost_${_bm})
    if(NOT boost_${_bm}_POPULATED)
      FetchContent_Populate(boost_${_bm})
    endif()
    list(APPEND _boost_incdirs "${boost_${_bm}_SOURCE_DIR}/include")
  endforeach()
  list(REMOVE_DUPLICATES _boost_incdirs)
  add_library(Boost::headers INTERFACE IMPORTED)
  # NOTE: INTERFACE_INCLUDE_DIRECTORIES (not _SYSTEM_...) - the _SYSTEM_ variant
  # does not propagate to consumers of an INTERFACE IMPORTED library, which broke
  # include resolution for mirrorcpp (value.hpp needs boost/multiprecision).
  set_target_properties(Boost::headers PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_boost_incdirs}")
endif()

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------
add_library(mirrorcpp
  src/value.cpp
  src/protocol.cpp
  src/client.cpp
  src/registry.cpp
  src/spec.cpp
  src/transport.cpp
  src/detail/net.cpp
  src/detail/process.cpp
  src/detail/http.cpp)
if(MIRRORCPP_WITH_TLS)
  target_sources(mirrorcpp PRIVATE src/detail/tls.cpp)
endif()
add_library(mirrorcpp::mirrorcpp ALIAS mirrorcpp)

target_include_directories(mirrorcpp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# Dependencies are PUBLIC only for the build tree; the installed package config
# re-attaches them to the imported target (mirrorcppConfig.cmake.in), so the
# exported target set never references third-party targets.
target_link_libraries(mirrorcpp
  PUBLIC $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>
         $<BUILD_INTERFACE:Boost::headers>)
if(MIRRORCPP_WITH_TLS)
  target_link_libraries(mirrorcpp
    PUBLIC $<BUILD_INTERFACE:OpenSSL::SSL>
           $<BUILD_INTERFACE:OpenSSL::Crypto>)
endif()

# Private compile definition so the library knows at preprocess time whether it
# was built with TLS (registry.cpp uses it to guard connect_from_registry's
# dependency on the TLS implementation). Never exported to consumers.
target_compile_definitions(mirrorcpp PRIVATE
  MIRRORCPP_WITH_TLS=$<BOOL:${MIRRORCPP_WITH_TLS}>)

set_target_properties(mirrorcpp PROPERTIES
  VERSION     ${PROJECT_VERSION}
  SOVERSION   ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Install rules + package config (design §6: find_package(mirrorcpp CONFIG))
# ---------------------------------------------------------------------------
install(TARGETS mirrorcpp EXPORT mirrorcppTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/mirrorcpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT mirrorcppTargets
  FILE mirrorcppTargets.cmake
  NAMESPACE mirrorcpp::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mirrorcpp)

include(CMakePackageConfigHelpers)
configure_package_config_file(
  cmake/mirrorcppConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/mirrorcppConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mirrorcpp)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/mirrorcppConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/mirrorcppConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/mirrorcppConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mirrorcpp)

# ---------------------------------------------------------------------------
# Optional components
# ---------------------------------------------------------------------------
if(MIRRORCPP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(MIRRORCPP_BUILD_CLI)
  add_subdirectory(cli)
endif()
if(MIRRORCPP_BUILD_TESTS)
  add_subdirectory(test)
endif()
