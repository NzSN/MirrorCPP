# Downstream packaging smoke: this CMake project is NOT part of the
# MirrorCPP build. It is configured standalone by test/packaging/run_smoke.sh
# against an installed mirrorcpp tree (design §7, M5):
#
#   cmake -S test/packaging -B <consumer-build> \
#         -DCMAKE_PREFIX_PATH=<install-prefix>
#
# find_package(mirrorcpp CONFIG) pulls in mirrorcppConfig.cmake, which
# re-attaches the public dependencies (nlohmann_json, Boost headers, and
# OpenSSL when the installed library was built with TLS).
cmake_minimum_required(VERSION 3.24)
project(mirrorcpp_packaging_consumer CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(mirrorcpp CONFIG REQUIRED)

# Hermetic boost: the installed config's fallback creates Boost::headers
# with no include dirs when Boost is not installed system-wide. run_smoke.sh
# stages the build-tree boost headers under <prefix>/include; point the
# imported target at them in that case.
if(TARGET Boost::headers)
  get_target_property(_boost_incs Boost::headers INTERFACE_INCLUDE_DIRECTORIES)
  if(NOT _boost_incs AND EXISTS "${CMAKE_PREFIX_PATH}/include/boost")
    set_target_properties(Boost::headers PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_PREFIX_PATH}/include")
  endif()
endif()

# nlohmann_json must be findable when mirrorcppConfig.cmake runs
# find_dependency(nlohmann_json 3.11); run_smoke.sh installs the build-tree
# copy into the same prefix so it resolves hermetically.

add_executable(mirrorcpp_packaging_consumer main.cpp)
target_link_libraries(mirrorcpp_packaging_consumer PRIVATE mirrorcpp::mirrorcpp)
