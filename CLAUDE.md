# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## プロジェクト概要

組み込み向けプログラムにおいて、**実行時に実際に流れてくるデータの数値的な大きさ（レンジ／有効ビット幅）の分布**を実測調査するプロジェクトのための、QEMU arm-softmmu向けTCGプラグイン。プログラム上の型宣言（int32_tかどうか等）ではなく、実行中に実際にALU演算に現れる値の大きさを対象とする。

最終目的は、FPGA上に実装する**ソフトコア**の設計（データパス幅、演算器の規模等）の裏付けを得ること。

- 対象：ALU系命令(ADD/SUB/MUL/DIV/AND/OR/XOR/CMP等)のオペランド値
- 検討：LDR/STR等のアドレス計算由来の値

## 現状のセットアップ状況

パイプラインは3段構成で、いずれも実装済み：

1. `plugin/` — ビットワイズ計装プラグイン（bitwidth plugin）v1実装
2. `benchmarks/` — 対象プログラム（Embench-IoT + CoreMark）のビルド・QEMU実行・CSV収集
3. `analysis/` — 収集したCSVの集計・可視化（`analyze.py`）

リポジトリ直下の `Makefile` は `plugin` と `benchmarks` をまとめてビルドする（`make` / `make clean`）。QEMU本体は `/home/sena/app/qemu`（ソース、ビルド済み `build/`）と `/home/sena/app/qemu-v9`（インストール先prefix、`include/qemu-plugin.h` を含む）にあり、このリポジトリはそれらに対する out-of-tree プラグイン開発用。

- QEMUターゲット：`arm-softmmu`（マシン：`mps2-an385`, Cortex-M3）。バイナリ：`/home/sena/app/qemu-v9/bin/qemu-system-arm`
- クロスコンパイラ：`arm-none-eabi-gcc`（PATH設定済み、14.2.1）
- 開発環境：WSL（Ubuntu）。QEMU 9.0はソースからビルド済み（`--enable-plugins` 有効、capstoneは未リンクのため `qemu_plugin_insn_disas()` は使えない → 命令デコードは自前実装、詳細は `plugin/decode.c` 参照）
- プラグインAPI：`qemu-plugin.h`（`qemu_plugin_register_vcpu_insn_exec_cb` 等）。QEMU 9.0の `qemu_plugin_register_vcpu_insn_exec_cb` は**命令実行前**に発火する（実測で確認済み。書き戻し系ALU命令の結果は次の命令のコールバック時点まで遅延サンプリングする設計、詳細は `plugin/bitwidth.c` 冒頭コメント参照）

### ビルド

```bash
make                          # リポジトリ直下: plugin + benchmarks を一括ビルド
cd plugin && make             # プラグインのみ。QEMU_SRC は get_qemu.sh が
                               # `dirname $(dirname $(which qemu-system-arm))` で自動検出（PATH上のqemu-system-armから逆算）。
                               # 手動指定する場合は make QEMU_SRC=/path/to/qemu
```

→ `plugin/libbitwidth.so` が生成される。

### 実行（プラグイン単体）

```bash
qemu-system-arm -M mps2-an385 -nographic -monitor none -serial none -display none \
  -plugin plugin/libbitwidth.so,out=bitwidth.csv \
  -kernel <対象elf>
```

QEMUを止める際、SIGKILLだとatexitコールバックが走らずCSVが出ないので注意。SIGTERM推奨、またはguest側でシャットダウンを実装。`out=` を省略すると `bitwidth.csv`（カレントディレクトリ）に出力。

### ベンチマークのビルド・実行（`benchmarks/`）

```bash
cd benchmarks
./build_all.sh   # または `make`（= benchmarks/Makefile 経由で build_all.sh を呼ぶ）
                  # Embench-IoT (benchmarks/embench-iot/src/*/) のうち単一.cファイル構成の
                  # ものだけを arm-none-eabi-gcc -O2 でmps2-an385向けにビルドし、build/*.elf に出力。
                  # 複数ファイル構成のベンチマークはSKIPされる（ビルド不可ではなくpolicyでの除外）。
./run_all.sh      # build/*.elf それぞれをqemu-system-arm+bitwidth pluginで実行し、
                  # results/<name>.csv に出力。1本あたり RUNTIME 秒（デフォルト3秒）走らせてSIGTERM。
```

CoreMark（`benchmarks/coremark/` + 移植レイヤ `benchmarks/coremark_port/`）は上記スクリプトの対象外で、`results/coremark.csv` は別途手動ビルド・実行して置いたもの。`board_support.c`／`startup.s`／`mps2.ld` はEmbench/CoreMark共通の最小ベアメタル起動コード（タイミング精度は考慮しない、プラグインを走らせるためだけの実装）。

### 分析（`analysis/`）

```bash
analysis/.venv/bin/python analysis/analyze.py                      # 全ベンチマーク対象（benchmarks/results/ → analysis/output/）
analysis/.venv/bin/python analysis/analyze.py -c                   # crypto系フル実装(nettle-aes/nettle-sha256/md5sum/aha-mont64)を除外
                                                                     # → 「専用回路に載る」ターゲットドメイン外を落とした集計。output_target_domain/ 等、
                                                                     # -o で出力先を明示的に分けて使う
```

集計表（class毎のp50/p90/p99/max）を標準出力に、累積分布・class別内訳・ベンチマーク別内訳のPNGを出力ディレクトリに書く。`-i` で入力CSVディレクトリ、`-e` で個別ベンチマークの除外を指定可能。

### 出力フォーマット

CSV: `class,bitwidth,count`。`class` は `ADD/SUB/RSB/ADC/SBC/AND/ORR/EOR/BIC/MVN/MUL/DIV/LSL/LSR/ASR/ROR/CMP/CMN/TST/TEQ/STR`、`bitwidth` は0〜32の有効ビット幅。

### 一時ファイル・サンプルコード

- `temp/` — 動作確認用のスクラッチファイル（テストELF、デコーダ検証スクリプト等）
- `examples/` — 汎用の最小ベアメタルスタートアップ例（`start.s`/`vectors.s`/`link.ld`）とサニティチェック用コード（`sancheck/`）。`benchmarks/` 用の起動コードとは別物

## ターゲットシステムの想定

- 実装先：FPGA上のソフトコア
- 構成イメージ：dsPICのPIC24コアに近い。DSP的な重い処理（信号処理・暗号のフル実装）は専用ハードウェアアクセラレータが担当し、ソフトコアは以下を担当する：
  - アクセラレータの制御レジスタ設定
  - DMA記述子の準備・管理
  - 割り込みハンドラでの結果取り出し・軽い後処理
  - 制御ループ、パース処理、RTOS/スケジューリングなど、専用回路化するほどではない全般処理
- 現時点の検証環境：**Cortex-M**を暫定ターゲットとする（ISA差の影響は限定的と判断。詳細は下記「アーキテクチャ差についての判断」参照）

## 調査対象から除外したもの・した理由

|                                          ドメイン                                           |        扱い        |                                   理由                                    |
| ------------------------------------------------------------------------------------------- | ------------------ | ------------------------------------------------------------------------- |
| FIRフィルタ、FFTのフル実装                                                                  | 除外               | 専用DSPアクセラレータに載る想定                                           |
| AES/SHA等のフル実装                                                                         | 除外               | 専用暗号回路に載る想定                                                    |
| 制御（PID、状態機械、しきい値判定）                                                         | 採用               | ソフトコア側の主戦場                                                      |
| パーサ（JSON、Modbus、CANフレーム解析等）                                                   | 採用               | 専用回路化されない                                                        |
| RTOS/スケジューラ                                                                           | 採用               | ソフトコア以外に載せようがない                                            |
| CRC/チェックサム（軽量なもの）                                                              | 採用               | 専用IP化されないケースが多い                                              |
| **アクセラレータ随伴処理**（レジスタ設定、DMA記述子操作、割り込みハンドラでの結果取り出し） | 採用（要追加検討） | dsPIC的構成の核心部分。適切な既存ベンチマークが見当たらないため自作が必要 |

## アーキテクチャ差についての判断

- アプリケーションのアルゴリズム由来の値分布（PIDゲインの桁数、JSON内の数値の大きさ等）はCPUアーキテクチャにほぼ依存しない → 今回知りたい本体はここ
- 一方、アドレス計算やレジスタ幅・命令セットの都合で生まれる中間値はISA依存（Cortex-MのThumb2即値表現、RISC-Vのオフセット計算など）
- 対応方針：集計時に**ALU系演算命令（ADD/SUB/MUL/DIV/AND/OR/XOR/CMP等）のオペランドのみ**を対象とし、LDR/STR等のアドレス計算由来の値は除外するフィルタリングを行う。これにより将来RISC-Vや自作ISAへ移植した際の結果とも比較しやすくする

## 調査手法：選定の経緯

検討した4手法：
1. コンパイラ計装（LLVM/GCCパス）＋ RTT/SWOログ
2. CoreSight（ETM＋DWT）によるハードウェアトレース
3. **ISAシミュレータ（QEMU/Renode等）でのフック ← 採用**

**選定理由**：全命令・全オペランドを網羅的に観測でき、実装コストと確度のバランスが良いため、QEMUのTCGプラグイン機構を用いる方針とした。

## 環境構築（進行中）

### 対象ボード・ツールチェーン
- QEMU `arm-softmmu`（`mps2-an385` = Cortex-M3）
- クロスコンパイラ：`arm-none-eabi-gcc`(パスは通っている)
- 開発環境：WSL（Ubuntu）上でQEMUをソースビルド

### プラグイン機構
- QEMU本体のソース変更・専用コンパイラパス不要
- `qemu-plugin.h` の API（`qemu_plugin_register_vcpu_insn_exec_cb` 等）を使い、命令実行ごとにコールバックを挿入
- サンプルは `contrib/plugins/` にあり、まずここで動作確認する

### 計装ロジックの設計方針（v1実装済み、`plugin/decode.c` / `plugin/bitwidth.c`）

命令デコードはQEMU自身のdecodetree定義（`target/arm/tcg/t16.decode`, `t32.decode`）のビットフィールドをそのまま参照して自前実装（capstone不使用）。900命令超の自作テストコーパス（-O0〜-Os）を`arm-none-eabi-objdump`の実際の逆アセンブル結果と突き合わせて分類ロジックを検証済み。

- **測定対象は「結果」**：ALU書き戻し系命令（ADD/SUB/RSB/ADC/SBC/AND/ORR/EOR/BIC/MVN/MUL/DIV/LSL/LSR/ASR/ROR）はデスティネーションレジスタに書き戻された値、STR系（STR/STRB/STRH、単一レジスタのみ）はメモリに書かれるデータ値（アドレスは対象外）を測定。入力オペランドは測定しない（ユーザ判断）
- **CMP/CMN/TST/TEQ**：書き戻し先レジスタが無いため、プラグイン側で内部的に破棄される演算結果を再計算（例：CMPなら`Rn - オペランド2`）して測定
- **除外**：MOV/MOVW/MOVT（定数生成、ALU演算ではない）、SMULL/UMULL/SMLAL/UMLAL（64bit結果、v1対象外）、STM/PUSH/STRD等の複数レジスタ転送、SP/PCが関与するADD/SUB（アドレス計算とみなす）
- **有効ビット幅**：ALU系は符号付き32bit整数として`32 - clz(abs(x))`（0は0bit）。STR系はアクセスサイズでマスクした後、符号なしのビットパターンとして`32 - clz(x)`
- **重要な実装上の注意**：`qemu_plugin_register_vcpu_insn_exec_cb`のコールバックは命令実行**前**に発火する（QEMU 9.0で実測確認、ヘッダのドキュメントだけでは分からない）。書き戻し系の結果は当該命令の次の命令のコールバック時点（＝前命令完了後）まで遅延サンプリングして対応。TB内で書き戻し命令が最後の命令の場合はサンプリングされない（稀、許容する測定誤差として記録）
- 実行終了時（`qemu_plugin_register_atexit_cb`）に `class,bitwidth,count` のCSVをダンプ → Pythonで可視化

## ターゲットプログラム

### 実装済み（`benchmarks/`、データ収集済み）

- Embench-IoT（単一.cファイル構成の全ベンチマーク、`benchmarks/build_all.sh` が自動選定）：`aha-mont64` `crc32` `depthconv` `edn` `huffbench` `matmult-int` `md5sum` `nettle-aes` `nettle-sha256` `nsichneu` `sglib-combined` `slre` `statemate` `tarfind` `ud` `wikisort`
- CoreMark（`benchmarks/coremark/` + `benchmarks/coremark_port/`、手動ビルド）

分析時（`analyze.py -c`）は `nettle-aes`/`nettle-sha256`/`md5sum`/`aha-mont64` を専用回路領域として除外し、ターゲットドメイン内の分布のみを見る。

### 未着手（案のまま）

- cJSON等によるメッセージパース
- FreeRTOSデモ（タスクスケジューラ＋簡易ドライバ層、DMA記述子操作を含むもの）
- 自作の簡易PIDループ＋しきい値判定コード
- 自作の「アクセラレータ随伴処理」模擬コード（レジスタ設定、DMA記述子、リングバッファ管理）※既存ベンチマークに適切なものがないため新規作成が必要

## 現在地・次のアクション

- [x] 調査手法の選定（QEMU TCGプラグイン）
- [x] ターゲットドメインの絞り込み
- [x] QEMUのプラグイン対応ビルド
- [x] `contrib/plugins/` のサンプルプラグインで動作確認
- [x] mps2-an385向けの最小限テストプログラム（起動確認用）の用意（`temp/vectors.s` + 適当な `_start`）
- [x] arm-none-eabi-gcc環境の確認・セットアップ
- [x] ALU命令判定・ビット幅集計プラグインの設計・実装（`plugin/`、v1完成、自作サニティテストで数値レベルの一致を確認済み）
- [ ] 「アクセラレータ随伴処理」模擬コードの自作
- [x] 各ターゲットプログラムのビルド・実行・データ収集
- [x] 結果の可視化・分析
- [ ] （任意）SMULL/UMULL等64bit乗算命令への対応拡張
- [ ] （任意）STM/PUSH等複数レジスタ転送への対応拡張
