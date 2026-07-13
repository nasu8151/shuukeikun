#!/bin/bash
# Run every successfully-built benchmark ELF under qemu-system-arm with the
# bitwidth plugin attached, each producing its own bitwidth CSV.
set -u
cd "$(dirname "$0")"
QEMU=qemu-system-arm
PLUGIN=../plugin/libbitwidth.so
OUT=results
mkdir -p "$OUT"
RUNTIME=${RUNTIME:-3}

for elf in build/*.elf; do
    name=$(basename "$elf" .elf)
    csv="$OUT/$name.csv"
    rm -f "$csv"
    "$QEMU" -M mps2-an385 -nographic -monitor none -serial none -display none \
        -plugin "$PLUGIN,out=$csv" -kernel "$elf" &
    qpid=$!
    sleep "$RUNTIME"
    kill -TERM "$qpid" 2>/dev/null
    sleep 0.5
    kill -9 "$qpid" 2>/dev/null
    wait "$qpid" 2>/dev/null
    if [ -s "$csv" ]; then
        n=$(($(wc -l < "$csv") - 1))
        echo "OK    $name ($n rows)"
    else
        echo "EMPTY $name"
    fi
done
