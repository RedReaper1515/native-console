#!/bin/sh
# Build the DS CPU cores and their tests with AddressSanitizer and
# UndefinedBehaviorSanitizer, under every gcc and clang present, and run all
# three test programs. Any sanitizer report or warning is fatal. Meant for a
# Linux container; from Windows:
#
#   docker run --rm \
#     -v "<repo>:/src:ro" \
#     -v "<SingleStepTests ARM7TDMI v1 dir>:/sst7:ro" \
#     registry.gitlab.steamos.cloud/steamrt/sniper/sdk sh /src/tools/nds/sanitize_cpu.sh
#
# Arguments: [repo, default /src] [ARM7 vector dir, default /sst7].
set -eu

SRC="${1:-/src}"
SST7="${2:-/sst7}"
ARM9_VECTORS="$SRC/roms/nds/armv5-vectors"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CORE="$SRC/runtime-nds/src/nds_cpu.c $SRC/runtime-nds/src/nds_cpu9.c $SRC/runtime-nds/src/nds_cpu7.c \
$SRC/runtime-nds/src/nds_cp15.c $SRC/runtime-nds/src/nds_mul_carry.c"
INCLUDES="-I$SRC/runtime-nds/include -I$SRC/runtime-nds/src"
COMMON="-O1 -g -fno-sanitize-recover=all -fno-omit-frame-pointer"
WARNINGS="-Wall -Wextra -Werror"
# The Steam Runtime's gcc-14 links its sanitizer runtimes statically.
LIBS="-lm -ldl -lpthread -lrt"

status=0
ran=0

# Run a test program; print its last line, or the end of its output if it failed.
check() {
    label="$1"
    shift
    if "$@" >"$OUT/log" 2>&1; then
        echo "   $label: $(tail -n 1 "$OUT/log")"
    else
        echo "   $label: FAILED"
        tail -n 30 "$OUT/log"
        status=1
    fi
}

for CC in gcc-14 gcc clang; do
    command -v "$CC" >/dev/null 2>&1 || continue
    ran=$((ran + 1))
    echo "== $CC: $($CC --version | head -n 1)"

    # AddressSanitizer with UBSan where the compiler's runtime works here,
    # UBSan alone where it does not.
    kinds="address,undefined"
    # shellcheck disable=SC2086
    if ! { $CC -std=c11 $WARNINGS $COMMON -fsanitize=$kinds $INCLUDES $CORE "$SRC/tests/nds/test_nds_cpu.c" \
            -o "$OUT/test_nds_cpu" $LIBS && "$OUT/test_nds_cpu" >/dev/null 2>&1; }; then
        kinds="undefined"
    fi
    SANITIZE="$COMMON -fsanitize=$kinds"
    # shellcheck disable=SC2086
    $CC -std=c11 $WARNINGS $SANITIZE $INCLUDES $CORE "$SRC/tests/nds/test_nds_cpu.c" -o "$OUT/test_nds_cpu" $LIBS
    # shellcheck disable=SC2086
    $CC -std=c11 $WARNINGS $SANITIZE $INCLUDES $CORE "$SRC/tests/nds/sst_arm.c" -o "$OUT/sst_nds_arm" $LIBS
    # The optimised build must be warning-free and agree as well.
    # shellcheck disable=SC2086
    $CC -std=c11 $WARNINGS -O2 $INCLUDES $CORE "$SRC/tests/nds/sst_arm.c" -o "$OUT/sst_nds_arm_o2" $LIBS
    # shellcheck disable=SC2086
    $CC -std=c11 $WARNINGS -O2 $INCLUDES $CORE "$SRC/tests/nds/test_nds_cpu.c" -o "$OUT/test_nds_cpu_o2" $LIBS

    check "directed, $kinds" "$OUT/test_nds_cpu"
    check "ARM9 vectors, $kinds" "$OUT/sst_nds_arm" --cpu 9 "$ARM9_VECTORS" --report 2
    check "ARM7 vectors, $kinds" "$OUT/sst_nds_arm" --cpu 7 "$SST7" --report 2
    check "directed, -O2" "$OUT/test_nds_cpu_o2"
    check "ARM9 vectors, -O2" "$OUT/sst_nds_arm_o2" --cpu 9 "$ARM9_VECTORS" --report 2
    check "ARM7 vectors, -O2" "$OUT/sst_nds_arm_o2" --cpu 7 "$SST7" --report 2
done

if [ "$ran" = 0 ]; then
    echo "no gcc or clang found (fail closed)"
    exit 1
fi
if [ "$status" != 0 ]; then
    echo "FAILED"
    exit 1
fi
echo "all runs clean ($ran compilers)"
