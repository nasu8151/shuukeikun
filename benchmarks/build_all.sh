#!/bin/bash
# Try to build every single-source-file Embench-IoT benchmark for bare-metal
# mps2-an385 (Cortex-M3), for the purpose of exercising the bitwidth QEMU
# plugin against real, established codebases. Not intended to reproduce an
# official Embench score (no calibrated scale factors, no accurate timing).
set -u
cd "$(dirname "$0")"
EMB=embench-iot
OUT=build
mkdir -p "$OUT"

SRCS_COMMON="startup.s board_support.c $EMB/support/main.c $EMB/support/beebsc.c"

declare -a OK=()
declare -a FAIL=()

for d in "$EMB"/src/*/; do
    name=$(basename "$d")
    csrc=$(find "$d" -maxdepth 1 -name "*.c" | head -1)
    nfiles=$(find "$d" -maxdepth 1 -name "*.c" | wc -l)
    if [ "$nfiles" -ne 1 ]; then
        echo "SKIP  $name (multi-file: $nfiles .c files)"
        continue
    fi
    elf="$OUT/$name.elf"
    log="$OUT/$name.log"
    if arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -nostartfiles --specs=nosys.specs -O2 \
        -DWARMUP_HEAT=1 -DGLOBAL_SCALE_FACTOR=1 \
        -I "$EMB/support" -I "$d" \
        -T mps2.ld \
        -o "$elf" \
        $SRCS_COMMON "$csrc" -lm > "$log" 2>&1; then
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
