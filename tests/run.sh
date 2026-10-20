#!/usr/bin/env bash
# ============================================================================
# ld -- test suite
# ----------------------------------------------------------------------------
# The tests need an AArch64 cross compiler and qemu-user.  When they are not
# available the tests are skipped instead of failing.
#
#   ./tests/run.sh              run every test
#   ./tests/run.sh static       run the tests whose name matches "static"
# ============================================================================
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
LD="$ROOT/bin/ld"
WORK="$(mktemp -d)"
FILTER="${1:-}"

PASS=0
FAIL=0
SKIP=0

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

say()  { printf '%s\n' "$*"; }
ok()   { PASS=$((PASS + 1)); printf '  \033[32mPASS\033[0m  %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  \033[31mFAIL\033[0m  %s\n' "$1"; }
skip() { SKIP=$((SKIP + 1)); printf '  \033[33mSKIP\033[0m  %s (%s)\n' "$1" "$2"; }

# --------------------------------------------------------------- toolchain
CROSS="${CROSS:-aarch64-linux-gnu-}"
CC="${CROSS}gcc"
AR="${CROSS}ar"
READELF="${CROSS}readelf"
QEMU="${QEMU:-qemu-aarch64}"

if [ ! -x "$LD" ]; then
  say "ld: the linker has not been built, run 'make' first"
  exit 1
fi

HAVE_CROSS=0
if command -v "$CC" >/dev/null 2>&1; then
  HAVE_CROSS=1
fi
HAVE_QEMU=0
if command -v "$QEMU" >/dev/null 2>&1; then
  HAVE_QEMU=1
fi

SYSROOT=""
if [ -d /usr/aarch64-linux-gnu/lib ]; then
  SYSROOT="/usr/aarch64-linux-gnu"
fi

want() {
  case "$FILTER" in
    "") return 0 ;;
    *) case "$1" in *"$FILTER"*) return 0 ;; *) return 1 ;; esac ;;
  esac
}

# ------------------------------------------------------------------ helpers
compile() {
  # compile <source> <object> [extra flags...]
  local src="$1" obj="$2"; shift 2
  "$CC" -c "$@" -o "$obj" "$ROOT/tests/src/$src"
}

check_output() {
  # check_output <name> <expected> <command...>
  local name="$1" expected="$2"; shift 2
  local output
  output="$("$@" 2>&1)"
  if [ "$output" = "$expected" ]; then
    ok "$name"
  else
    bad "$name"
    say "      expected: $expected"
    say "      got:      $output"
  fi
}

# ============================================================================
# 1. a static executable without any C library
# ============================================================================
if want "static"; then
  if [ "$HAVE_CROSS" != 1 ]; then
    skip "static executable" "$CC not found"
  else
    compile freestanding.c "$WORK/freestanding.o" -nostdlib -ffreestanding \
      -fno-pie -O2
    if ! "$LD" -static -e _start -o "$WORK/freestanding" \
         "$WORK/freestanding.o" 2>"$WORK/err"; then
      bad "static executable (link)"
      cat "$WORK/err"
    elif [ "$HAVE_QEMU" != 1 ]; then
      skip "static executable" "$QEMU not found"
    else
      check_output "static executable" \
        "ld: hello from a statically linked program" \
        "$QEMU" "$WORK/freestanding"
    fi
  fi
fi

# ============================================================================
# 2. a position independent executable that uses the C library
# ============================================================================
if want "pie"; then
  if [ "$HAVE_CROSS" != 1 ] || [ -z "$SYSROOT" ]; then
    skip "position independent executable" "no aarch64 sysroot"
  else
    compile plain.c "$WORK/plain.o" -fPIE -O2
    "$LD" -pie -o "$WORK/plain" "$WORK/plain.o" \
      "$SYSROOT/lib/crt1.o" "$SYSROOT/lib/crti.o" "$SYSROOT/lib/crtn.o" \
      -dynamic-linker /lib/ld-linux-aarch64.so.1 "$SYSROOT/lib/libc.so.6" \
      2>"$WORK/err"
    if [ $? != 0 ]; then
      bad "position independent executable (link)"
      cat "$WORK/err"
    elif [ "$HAVE_QEMU" != 1 ]; then
      skip "position independent executable" "$QEMU not found"
    else
      check_output "position independent executable" \
        "ld: hello from a position independent executable" \
        "$QEMU" -L "$SYSROOT" "$WORK/plain"
    fi
  fi
fi

# ============================================================================
# 3. a shared library that is loaded at run time
# ============================================================================
if want "shared"; then
  if [ "$HAVE_CROSS" != 1 ] || [ -z "$SYSROOT" ]; then
    skip "shared library" "no aarch64 sysroot"
  else
    compile greet.c "$WORK/greet.o" -fPIC -O2
    if ! "$LD" -shared -soname libgreet.so -o "$WORK/libgreet.so" \
         "$WORK/greet.o" 2>"$WORK/err"; then
      bad "shared library (link)"
      cat "$WORK/err"
    else
      compile use_greet.c "$WORK/use_greet.o" -fPIE -O2
      "$LD" -pie -o "$WORK/use_greet" "$WORK/use_greet.o" \
        "$SYSROOT/lib/crt1.o" "$SYSROOT/lib/crti.o" "$SYSROOT/lib/crtn.o" \
        -L"$WORK" -lgreet -dynamic-linker /lib/ld-linux-aarch64.so.1 \
        "$SYSROOT/lib/libc.so.6" 2>"$WORK/err"
      if [ $? != 0 ]; then
        bad "shared library (link the program)"
        cat "$WORK/err"
      elif [ "$HAVE_QEMU" != 1 ]; then
        skip "shared library" "$QEMU not found"
      else
        check_output "shared library" \
          "ld: hello from the shared library: 42" \
          env LD_LIBRARY_PATH="$WORK" "$QEMU" -L "$SYSROOT" "$WORK/use_greet"
      fi
    fi
  fi
fi

# ============================================================================
# 4. a static archive
# ============================================================================
if want "archive"; then
  if [ "$HAVE_CROSS" != 1 ] || [ -z "$SYSROOT" ]; then
    skip "static archive" "no aarch64 sysroot"
  else
    compile libmath.c "$WORK/libmath.o" -fPIE -O2
    "$AR" rcs "$WORK/libmath.a" "$WORK/libmath.o"
    compile archive_user.c "$WORK/archive_user.o" -fPIE -O2
    "$LD" -pie -o "$WORK/archive_user" "$WORK/archive_user.o" \
      "$WORK/libmath.a" "$SYSROOT/lib/crt1.o" "$SYSROOT/lib/crti.o" \
      "$SYSROOT/lib/crtn.o" -dynamic-linker /lib/ld-linux-aarch64.so.1 \
      "$SYSROOT/lib/libc.so.6" 2>"$WORK/err"
    if [ $? != 0 ]; then
      bad "static archive (link)"
      cat "$WORK/err"
    elif [ "$HAVE_QEMU" != 1 ]; then
      skip "static archive" "$QEMU not found"
    else
      check_output "static archive" \
        "ld: the archive answered 42" \
        "$QEMU" -L "$SYSROOT" "$WORK/archive_user"
    fi
  fi
fi

# ============================================================================
# 5. a global constructor
# ============================================================================
if want "constructor"; then
  GCCDIR="$(dirname "$(command -v "$CC" 2>/dev/null || echo /nonexistent)")"
  CRT=""
  for dir in /usr/lib/gcc-cross/aarch64-linux-gnu/*; do
    [ -f "$dir/crtbeginS.o" ] && CRT="$dir"
  done
  if [ "$HAVE_CROSS" != 1 ] || [ -z "$SYSROOT" ] || [ -z "$CRT" ]; then
    skip "global constructor" "no aarch64 sysroot or crt files"
  else
    compile constructor.c "$WORK/constructor.o" -fPIE -O2
    "$LD" -pie -o "$WORK/constructor" "$WORK/constructor.o" \
      "$CRT/crtbeginS.o" "$CRT/crtendS.o" "$SYSROOT/lib/Scrt1.o" \
      "$SYSROOT/lib/crti.o" "$SYSROOT/lib/crtn.o" \
      -dynamic-linker /lib/ld-linux-aarch64.so.1 "$SYSROOT/lib/libc.so.6" \
      2>"$WORK/err"
    if [ $? != 0 ]; then
      bad "global constructor (link)"
      cat "$WORK/err"
    elif [ "$HAVE_QEMU" != 1 ]; then
      skip "global constructor" "$QEMU not found"
    else
      check_output "global constructor" \
        "ld: the constructor ran
ld: main ran" \
        "$QEMU" -L "$SYSROOT" "$WORK/constructor"
    fi
  fi
fi

# ============================================================================
# 6. the structure of the produced image
# ============================================================================
if want "layout"; then
  if [ "$HAVE_CROSS" != 1 ] || [ -z "$SYSROOT" ]; then
    skip "image layout" "no aarch64 sysroot"
  else
    compile plain.c "$WORK/plain.o" -fPIE -O2
    "$LD" -pie -o "$WORK/plain" "$WORK/plain.o" \
      "$SYSROOT/lib/crt1.o" "$SYSROOT/lib/crti.o" "$SYSROOT/lib/crtn.o" \
      -dynamic-linker /lib/ld-linux-aarch64.so.1 "$SYSROOT/lib/libc.so.6" \
      2>/dev/null

    problems=""
    # the image must be a position independent executable
    "$READELF" -h "$WORK/plain" | grep -q "DYN" || problems="$problems type"
    # it must have a program interpreter
    "$READELF" -l "$WORK/plain" | grep -q "INTERP" || problems="$problems interp"
    # and a dynamic section with the expected entries
    "$READELF" -d "$WORK/plain" | grep -q "NEEDED" || problems="$problems needed"
    "$READELF" -d "$WORK/plain" | grep -q "GNU_HASH" || problems="$problems gnu-hash"
    # the symbol table has to start with the null symbol
    "$READELF" --dyn-syms "$WORK/plain" | grep -q "^ *0: " || \
      problems="$problems dynsym"
    if [ -z "$problems" ]; then
      ok "image layout"
    else
      bad "image layout ($problems )"
    fi
  fi
fi

# ============================================================================
say ""
say "passed: $PASS  failed: $FAIL  skipped: $SKIP"
[ "$FAIL" = 0 ]
