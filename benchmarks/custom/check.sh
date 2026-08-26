#!/bin/bash
# Validate every custom/ benchmark on the host: builds each one with the
# native compiler against host_test.c and checks it against its
# bench_expected() value. Run this after touching anything under custom/ --
# the bare-metal build has no console, so this is the only place a
# behavioural change is actually visible.
set -u
cd "$(dirname "$0")/.."

OUT=${TMPDIR:-/tmp}
rc=0

for d in custom/*/; do
    [ -d "$d" ] || continue
    name=$(basename "$d")
    bin="$OUT/${name}_host"
    if ! gcc -Wall -Wextra -O2 -I custom -o "$bin" "$d"*.c custom/host_test.c; then
        echo "BUILD FAIL  $name"
        rc=1
        continue
    fi
    printf '%-12s ' "$name"
    "$bin" || rc=1
done

exit $rc
