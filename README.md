# apa

## 概要

[minc](https://github.com/nasu8151/minc)の最適化のために、組み込み向けプログラムにおける実行時に流れるデータの大きさを調べるプロジェクト。

## 使い方

まず、`benchmarks/`に[coremark](https://github.com/eembc/coremark)と[embench](https://github.com/embench/embench-iot)をクローンしてください。追跡はされません。

### 手順

```shell
make # do only first
./benchmarks/run_all.sh
cd analysis
python3 -m venv .venv # do only first
python3 ./analyze.py
```

結果はoutputに格納されます。きっと。
