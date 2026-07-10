# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## プロジェクト概要

組み込み向けプログラムにおいて、**実行時に実際に流れてくるデータの数値的な大きさ（レンジ／有効ビット幅）の分布**を実測調査するプロジェクトのための、QEMU arm-softmmu向けTCGプラグイン。プログラム上の型宣言（int32_tかどうか等）ではなく、実行中に実際にALU演算に現れる値の大きさを対象とする。

最終目的は、FPGA上に実装する**ソフトコア**の設計（データパス幅、演算器の規模等）の裏付けを得ること。

- 対象：ALU系命令(ADD/SUB/MUL/DIV/AND/OR/XOR/CMP等)のオペランド値
- 検討：LDR/STR等のアドレス計算由来の値

## 現状のセットアップ状況

`plugin/` 以下にビットワイズ計装プラグイン（bitwidth plugin）のv1実装が完成済み。QEMU本体は `/home/sena/app/qemu`（ソース、ビルド済み `build/`）と `/home/sena/app/qemu-v9`（インストール先prefix、`include/qemu-plugin.h` を含む）にあり、このリポジトリはそれらに対する out-of-tree プラグイン開発用。

- QEMUターゲット：`arm-softmmu`（マシン：`mps2-an385`, Cortex-M3）。バイナリ：`/home/sena/app/qemu-v9/bin/qemu-system-arm`
- クロスコンパイラ：`arm-none-eabi-gcc`（PATH設定済み、14.2.1）
- 開発環境：WSL（Ubuntu）。QEMU 9.0はソースからビルド済み（`--enable-plugins` 有効、capstoneは未リンクのため `qemu_plugin_insn_disas()` は使えない → 命令デコードは自前実装、詳細は `plugin/decode.c` 参照）
- プラグインAPI：`qemu-plugin.h`（`qemu_plugin_register_vcpu_insn_exec_cb` 等）。QEMU 9.0の `qemu_plugin_register_vcpu_insn_exec_cb` は**命令実行前**に発火する（実測で確認済み。書き戻し系ALU命令の結果は次の命令のコールバック時点まで遅延サンプリングする設計、詳細は `plugin/bitwidth.c` 冒頭コメント参照）

### ビルド

```bash
cd plugin && make            # QEMU_SRC のデフォルトは /home/sena/app/qemu-v9
```

→ `plugin/libbitwidth.so` が生成される。

### 実行

```bash
qemu-system-arm -M mps2-an385 -nographic -monitor none -serial none -display none \
  -plugin plugin/libbitwidth.so,out=bitwidth.csv \
  -kernel <対象elf>
```

QEMUを止める際、SIGKILLだとatexitコールバックが走らずCSVが出ないので注意。SIGTERM推奨、またはguest側でシャットダウンを実装。`out=` を省略すると `bitwidth.csv`（カレントディレクトリ）に出力。

### 出力フォーマット

CSV: `class,bitwidth,count`。`class` は `ADD/SUB/RSB/ADC/SBC/AND/ORR/EOR/BIC/MVN/MUL/DIV/LSL/LSR/ASR/ROR/CMP/CMN/TST/TEQ/STR`、`bitwidth` は0〜32の有効ビット幅。

### 一時ファイル

動作確認用のスクラッチファイル（テストELF、デコーダ検証スクリプト等）は `temp/` 以下に置く。

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

## ターゲットプログラム（案）

- Embench-IoTから、状態機械パーサ系・CRC32など軽量なもの数本
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
- [ ] 各ターゲットプログラムのビルド・実行・データ収集
- [ ] 結果の可視化・分析
- [ ] （任意）SMULL/UMULL等64bit乗算命令への対応拡張
- [ ] （任意）STM/PUSH等複数レジスタ転送への対応拡張
