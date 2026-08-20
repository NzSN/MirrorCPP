#!/usr/bin/env bash
# Packaging smoke (design §7, M5): install the just-built mirrorcpp into a
# scratch prefix, then configure/build/run a tiny downstream consumer that
# uses find_package(mirrorcpp CONFIG). Run by ctest as "packaging_smoke".
#
# Usage: run_smoke.sh <mirrorcpp-build-dir> [scratch-root]
#   scratch-root defaults to <build-dir>/scratch; the consumer is built under
#   <scratch-root>/consumer-build.
set -euo pipefail

BUILD_DIR=$(cd "$1" && pwd)
SCRATCH_ROOT="${2:-$BUILD_DIR/scratch}"
mkdir -p "$SCRATCH_ROOT"
PREFIX="$SCRATCH_ROOT/prefix"
CONSUMER_SRC="$(cd "$(dirname "$0")/.." && pwd)/packaging"
CONSUMER_BUILD="$SCRATCH_ROOT/consumer-build"
NLOHMANN_SRC="$BUILD_DIR/_deps/nlohmann_json-src"

echo "== packaging smoke: installing mirrorcpp into $PREFIX =="
cmake --install "$BUILD_DIR" --prefix "$PREFIX" >/dev/null

# The installed mirrorcppConfig.cmake calls find_dependency(nlohmann_json 3.11)
# (and Boost/OpenSSL when enabled). On machines without a system copy, install
# the build-tree's fetched nlohmann_json into the same prefix so the consumer
# resolves hermetically.
if [ -d "$NLOHMANN_SRC" ]; then
  echo "== packaging smoke: installing build-tree nlohmann_json into $PREFIX =="
  cmake -S "$NLOHMANN_SRC" -B "$SCRATCH_ROOT/nlohmann-build" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DJSON_BuildTests=OFF >/dev/null
  cmake --install "$SCRATCH_ROOT/nlohmann-build" >/dev/null
fi

# value.hpp includes <boost/multiprecision/cpp_int.hpp> (header-only). Stage
# every fetched boost module's include/ tree into the prefix so the consumer
# compiles hermetically when Boost is not installed system-wide. The installed
# mirrorcppConfig.cmake maps this to Boost::headers via its fallback target.
if ls -d "$BUILD_DIR"/_deps/boost_*-src/include >/dev/null 2>&1; then
  echo "== packaging smoke: staging build-tree boost headers into $PREFIX =="
  for _inc in "$BUILD_DIR"/_deps/boost_*-src/include; do
    cp -R "$_inc"/. "$PREFIX/include/"
  done
fi

echo "== packaging smoke: configuring downstream consumer =="
rm -rf "$CONSUMER_BUILD"
cmake -S "$CONSUMER_SRC" -B "$CONSUMER_BUILD" \
  -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DCMAKE_BUILD_TYPE=Release >/dev/null

echo "== packaging smoke: building downstream consumer =="
cmake --build "$CONSUMER_BUILD" -j"$(nproc)" >/dev/null

echo "== packaging smoke: running downstream consumer =="
"$CONSUMER_BUILD/mirrorcpp_packaging_consumer"
echo "== packaging smoke: OK =="
