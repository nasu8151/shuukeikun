#!/bin/bash
# Build every benchmark for bare-metal mps2-an385 (Cortex-M3), for the
# purpose of exercising the bitwidth QEMU plugin: the Embench-IoT suite
# (single- or multi-source-file) plus our own benchmarks under custom/.
# Not intended to reproduce an official Embench score (no calibrated scale
# factors, no accurate timing).
set -u
cd "$(dirname "$0")"
EMB=embench-iot
OUT=build
mkdir -p "$OUT"

declare -a OK=()
declare -a FAIL=()

# Embench-IoT. Skipped entirely if the checkout is absent (run_analysis.sh
# clones it); the custom/ benchmarks below have no such dependency.
SRCS_COMMON="startup.s board_support.c $EMB/support/main.c $EMB/support/beebsc.c"

for d in "$EMB"/src/*/; do
    [ -d "$d" ] || continue
    name=$(basename "$d")
    csrcs=$(find "$d" -maxdepth 1 -name "*.c")
    elf="$OUT/$name.elf"
    log="$OUT/$name.log"
    if arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -nostartfiles --specs=nosys.specs -O2 \
        -DWARMUP_HEAT=1 -DGLOBAL_SCALE_FACTOR=1 \
        -I "$EMB/support" -I "$d" \
        -T mps2.ld \
        -o "$elf" \
        $SRCS_COMMON $csrcs -lm > "$log" 2>&1; then
        echo "OK    $name"
        OK+=("$name")
    else
        echo "FAIL  $name"
        FAIL+=("$name")
    fi
done

# CoreMark. Its sources sit outside the Embench src/ layout and need the
# port layer in coremark_port/, so it gets its own invocation. It used to
# be built by hand, which is how results/coremark.csv once went stale
# against a plugin fix -- keep it in the automated path.
if [ -d coremark ]; then
    name=coremark
    elf="$OUT/$name.elf"
    log="$OUT/$name.log"
    if arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -nostartfiles --specs=nosys.specs -O2 \
        -DITERATIONS=8 -DPERFORMANCE_RUN=1 -DMAIN_HAS_NOARGC=1 '-DFLAGS_STR="-O2"' \
        -I coremark -I coremark_port \
        -T mps2.ld \
        -o "$elf" \
        startup.s coremark_port/core_portme.c coremark_port/ee_printf_stub.c \
        coremark/core_main.c coremark/core_list_join.c coremark/core_matrix.c \
        coremark/core_state.c coremark/core_util.c > "$log" 2>&1; then
        echo "OK    $name"
        OK+=("$name")
    else
        echo "FAIL  $name"
        FAIL+=("$name")
    fi
fi

# Our own benchmarks: the parts of the target domain no off-the-shelf suite
# covers (fixed-point control, accelerator-attendant driver work). Sources
# live in this repo, so these build in a clean checkout. Validate their
# behaviour with custom/check.sh -- the bare-metal build has no console.
CUSTOM_COMMON="startup.s custom/harness.c"

for d in custom/*/; do
    [ -d "$d" ] || continue
    name=$(basename "$d")
    csrcs=$(find "$d" -maxdepth 1 -name "*.c")
    elf="$OUT/$name.elf"
    log="$OUT/$name.log"
    if arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -nostartfiles --specs=nosys.specs -O2 \
        -I custom -I "$d" \
        -T mps2.ld \
        -o "$elf" \
        $CUSTOM_COMMON $csrcs > "$log" 2>&1; then
        echo "OK    $name"
        OK+=("$name")
    else
        echo "FAIL  $name"
        FAIL+=("$name")
    fi
done

echo
echo "=== summary ==="
echo "OK: ${OK[*]}"
echo "FAIL: ${FAIL[*]}"
