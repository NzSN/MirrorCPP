# Packaging smoke (design §7, M5)

This directory holds a tiny *downstream consumer* project used to verify that
the installed MirrorCPP package works through the normal CMake consumption
path:

```cmake
find_package(mirrorcpp CONFIG REQUIRED)
target_link_libraries(app PRIVATE mirrorcpp::mirrorcpp)
```

It is intentionally NOT part of the MirrorCPP build tree (the root
'test/CMakeLists.txt' only adds 'unit' and 'integration').

## Files

- 'CMakeLists.txt' — standalone consumer project; find_package(mirrorcpp
  CONFIG) + link 'mirrorcpp::mirrorcpp'. When the machine has no system
  nlohmann_json, it fetches v3.11.3 (same version the library build uses)
  so the smoke runs hermetically.
- 'main.cpp' — exercises the installed package: umbrella header, version
  macro/function, and a 'register' message encode round-trip through the
  public API.
- 'run_smoke.sh' — the smoke driver: installs the built tree into a scratch
  prefix, configures + builds + runs the consumer against it with
  CMAKE_PREFIX_PATH=<prefix>.

## Running

```sh
ctest --test-dir build -R packaging_smoke --output-on-failure
# or manually:
test/packaging/run_smoke.sh <mirrorcpp-build-dir>
```

The installed package config (mirrorcppConfig.cmake) re-attaches the public
dependencies (nlohmann_json, Boost headers, and OpenSSL when built with
TLS) to the imported 'mirrorcpp::mirrorcpp' target.
