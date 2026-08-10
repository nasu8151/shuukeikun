# apa

## 概要

[minc](https://github.com/nasu8151/minc)の最適化のために、組み込み向けプログラムにおける実行時に流れるデータの大きさを調べるプロジェクト。

## 使用したベンチマークとその基準

|                                           ドメイン                                            | 扱い |                       理由                        |
| --------------------------------------------------------------------------------------------- | ---- | ------------------------------------------------- |
| FIRフィルタ、FFT/DCTのフル実装（例：JPEGデコード＝`picojpeg`のWinograd IDCT）                 | 除外 | 専用DSPアクセラレータ・画像コーデックHWに載る想定 |
| AES/SHA等のフル実装                                                                           | 除外 | 専用暗号回路に載る想定                            |
| 制御（PID、状態機械、しきい値判定。例：決定木推論＝`xgboost`）                                | 採用 | ソフトコア側の主戦場                              |
| パーサ（JSON、Modbus、CANフレーム解析等）                                                     | 採用 | 専用回路化されない                                |
| RTOS/スケジューラ                                                                             | 採用 | ソフトコア以外に載せようがない                    |
| CRC/チェックサム（軽量なもの。Reed-Solomon等の誤り訂正符号含む、例：QRコード生成＝`qrduino`） | 採用 | 専用IP化されないケースが多い                      |

## 使い方

### 共通

`qemu-system-arm`(バージョン9.0.0推奨)と`arm-none-eabi-gcc`(バージョン14.2推奨)をインストールして、パスを通しておいてください。また、プラグインのビルドに通常の`gcc`を使用します。  

開発時の実行環境

```
$ qemu-system-arm --version
QEMU emulator version 9.0.0 (v9.0.0)
Copyright (c) 2003-2024 Fabrice Bellard and the QEMU Project developers
```

```
$ arm-none-eabi-gcc --version
arm-none-eabi-gcc (Arm GNU Toolchain 14.2.Rel1 (Build arm-14.52)) 14.2.1 20241119
Copyright (C) 2024 Free Software Foundation, Inc.
This is free software; see the source for copying conditions.  There is NO
warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
```

### 検証のみの場合

```shell
chmod +X ./run_analysis.sh # 一応
./run_analysis.sh
```

```run_analysis.sh```を実行すると、自動でcoremarkおよびembench-iotをクローンします。インターネット接続を確認してください。  

結果は`analysis/output`および`analysis/output_target_domain`に格納されます。

### カスタマイズする場合（一部のみ実行、集計等）

先に`./run_analysis.sh`を実行しておくことをお勧めします。

```shell
make # プラグイン本体のビルド、集計のためのベンチマーク実行
python3 ./analysis/analyze.py # 集計プログラム実行
```

### `analyze.py`の使い方

```
usage: analyze.py [-h] [-i RESULT] [-o OUTPUT] [-e EXCLUDE] [-n] [-c]

Analyze bitwidth-plugin CSV output (class,bitwidth,count) from one or more benchmark runs, and produce summary tables + charts answering the project's core question: how many
effective bits do ALU operands / stored values actually need at runtime? Usage: analysis/.venv/bin/python analysis/analyze.py [args.result] [output_dir] Defaults:
args.result=benchmarks/results, output_dir=analysis/output

options:
  -h, --help            show this help message and exit
  -i RESULT, --result RESULT
  -o OUTPUT, --output OUTPUT
  -e EXCLUDE, --exclude EXCLUDE
  -n, --no_by_benchmark
  -c, --exclude_crypto
```

- `-h`
  - ヘルプを表示します。
- `-i RESULT`, `--result RESULT`
  - プラグインの出力した、bit数集計の結果の入ったcsvファイルのあるフォルダを指定します。
- `-o OUTPUT, --output OUTPUT`
  - 集計結果の画像を出力するフォルダを指定します。
- `-e EXCLUDE, --exclude EXCLUDE`
  - 集計結果より除外するベンチマークの集計結果のファイル名を`.csv`なしで指定します。
- `-c, --exclude_crypto`
  - mincのターゲットから外れているベンチマークを除外します。
  - `"nettle-aes", "nettle-sha256", "md5sum", "aha-mont64", "picojpeg"`が除外されます。

## 注意

- このプロジェクトは、CoreMark, Embench IoTのワークロードを使用して測定を行うが、たとえベンチマークスコアが出たとしても、これを公式のスコアとして用いてはならない。
