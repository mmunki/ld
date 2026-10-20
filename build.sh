#!/usr/bin/env bash
# ============================================================================
# ld -- build script
# ----------------------------------------------------------------------------
# Builds the linker into bin/ld.
#
#   ./build.sh              build bin/ld
#   ./build.sh clean        remove the build artefacts
#   ./build.sh test         build and run the test suite
#   PREFIX=/opt ./build.sh install
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")"

CC="${CC:-cc}"
CFLAGS="${CFLAGS:--O2 -g}"
PREFIX="${PREFIX:-/usr/local}"

build() {
  mkdir -p build bin
  local objects=()
  for src in src/*.c; do
    local obj="build/$(basename "${src%.c}").o"
    # shellcheck disable=SC2086
    $CC $CFLAGS -std=c11 -Wall -Wextra -Iinclude -c "$src" -o "$obj"
    objects+=("$obj")
  done
  # shellcheck disable=SC2086
  $CC $CFLAGS -o bin/ld "${objects[@]}"
  echo "built bin/ld"
}

case "${1:-build}" in
  build)
    build
    ;;
  clean)
    rm -rf build bin
    ;;
  test)
    build
    ./tests/run.sh
    ;;
  install)
    build
    install -Dm755 bin/ld "$PREFIX/bin/ld"
    ln -sf ld "$PREFIX/bin/aarch64-linux-android-ld"
    echo "installed to $PREFIX/bin"
    ;;
  *)
    echo "usage: $0 [build|clean|test|install]" >&2
    exit 1
    ;;
esac
