#!/bin/bash
# Fetch benchmark sources if missing, build everything, run the bitwidth
# plugin over all benchmarks, then produce both analysis outputs.
set -euo pipefail
cd "$(dirname "$0")"

cd ./benchmarks

if [ ! -d coremark ]; then
    echo "no coremark detected. downloading..."
    git clone -b main "https://github.com/eembc/coremark.git"
fi
if [ ! -d embench-iot ]; then
    echo "no embench detected. downloading..."
    git clone -b master "https://github.com/embench/embench-iot.git"
fi

cd ../
make
echo "start benchmark."
./benchmarks/run_all.sh
echo "completed benchmark."
echo "start analysis."
analysis/.venv/bin/python ./analysis/analyze.py
echo "start targetted domain analyze."
analysis/.venv/bin/python ./analysis/analyze.py -c -o analysis/output_target_domain
echo "completed analysis."
echo "done!"
