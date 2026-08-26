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

**gitに入っているのは30ファイルのみ**（`git ls-files` で確認できる）。ベンチマーク本体（`benchmarks/embench-iot/`, `benchmarks/coremark/`）、ビルド成果物（`benchmarks/build/`, `benchmarks/results/`, `*.o`/`*.so`/`*.elf`）、`temp/`、`.venv`、`analysis/output*/` はすべてgitignore対象。つまりクリーンなクローンには**ベンチマークソースも収集済みCSVも存在しない**（`run_analysis.sh` が clone から再生成する。下記参照）。追跡されているのはプラグイン本体・移植レイヤ・ビルドスクリプト・`analyze.py` だけ。

- QEMUターゲット：`arm-softmmu`（マシン：`mps2-an385`, Cortex-M3）。バイナリ：`/home/sena/app/qemu-v9/bin/qemu-system-arm`
- クロスコンパイラ：`arm-none-eabi-gcc`（PATH設定済み、14.2.1）
- 開発環境：WSL（Ubuntu）。QEMU 9.0はソースからビルド済み（`--enable-plugins` 有効、capstoneは未リンクのため `qemu_plugin_insn_disas()` は使えない → 命令デコードは自前実装、詳細は `plugin/decode.c` 参照）
- プラグインAPI：`qemu-plugin.h`（`qemu_plugin_register_vcpu_insn_exec_cb` 等）。QEMU 9.0の `qemu_plugin_register_vcpu_insn_exec_cb` は**命令実行前**に発火する（実測で確認済み。書き戻し系ALU命令の結果は次の命令のコールバック時点まで遅延サンプリングする設計、詳細は `plugin/bitwidth.c` 冒頭コメント参照）

### フルパイプライン一発実行（`run_analysis.sh`）

```bash
./run_analysis.sh   # ①embench-iot/coremarkが無ければgit clone ②make（plugin+benchmarks）
                     # ③benchmarks/run_all.sh ④analyze.py（全体）⑤analyze.py -c -o analysis/output_target_domain
```

クリーン環境からの再現はこれ1本。ネットワーク接続が要る（GitHubから2リポジトリをclone）。venvパス（`run_analysis.sh`・`analyze.py` docstring とも `.venv/bin/python` で統一済み）とCoreMarkの自動ビルド（`build_all.sh` が3系統ともビルド、下記参照）は解決済み。

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
                  # Embench-IoT (benchmarks/embench-iot/src/*/) の全ベンチマーク（単一/複数.cファイル
                  # 構成いずれも）を arm-none-eabi-gcc -O2 でmps2-an385向けにビルドし、build/*.elf に出力。
                  # 複数ファイル構成のディレクトリは配下の*.cを全てまとめてリンクする。
./run_all.sh      # build/*.elf それぞれをqemu-system-arm+bitwidth pluginで実行し、
                  # results/<name>.csv に出力。1本あたり RUNTIME 秒（デフォルト3秒）走らせてSIGTERM。
```

`build_all.sh` は失敗しても止まらず（`set -u` のみ、`-e` なし）末尾にOK/FAILのサマリを出す。ビルドログは `build/<name>.log`。ビルド対象は3系統：Embench-IoT（`embench-iot/src/*/`）、CoreMark（`coremark/` + 移植レイヤ `coremark_port/`）、自作ベンチ（`custom/*/`）。Embenchのcloneが無ければその系統だけスキップされ、残りはビルドされる。`board_support.c`／`startup.s`／`mps2.ld` は共通の最小ベアメタル起動コード（タイミング精度は考慮しない、プラグインを走らせるためだけの実装）。

`run_all.sh` は `RUNTIME` 秒後にSIGTERMで打ち切るが、**実際には全ベンチが3秒より早く仕事を終えて `hang:` の無限ループでアイドルしている**（アイドル中はALU結果もストアも発生しないのでヒストグラムに寄与しない）。つまり収集されるカウントは「固定ワークロード1回分」であって「3秒間に流れた命令数」ではない。`RUNTIME` は打ち切り保険であって計測窓ではない。

この性質は `analyze.py` の集計方法と直結している：`analyze.py` はベンチ間でカウントを**単純加算**する（ベンチごとの正規化はしない）ので、ワークロードの大きさがそのまま合成分布への寄与の重みになる。現行コーパスの総サンプル数は約50万〜290万（中央値 約1.1M）に収まっている。新しいベンチを足すときはこのレンジに合わせること。

> CoreMark／Embench-IoTのワークロードを使ってはいるが、これは公式スコア測定ではない（スケールファクタ未較正・タイミング非考慮）。出た数字をベンチマークスコアとして扱わないこと（READMEにも明記）。

### 分析（`analysis/`）

```bash
.venv/bin/python analysis/analyze.py                               # 全ベンチマーク対象（benchmarks/results/ → analysis/output/）
.venv/bin/python analysis/analyze.py -c -o analysis/output_target_domain
                                                                     # crypto系フル実装(nettle-aes/nettle-sha256/md5sum/aha-mont64)と
                                                                     # picojpeg(JPEGフル実装)を除外 → 「専用回路に載る」ターゲットドメイン外を
                                                                     # 落とした集計。-o で出力先を明示的に分ける
```

集計表（class毎のp50/p90/p99/max）を標準出力に、累積分布・class別内訳・ベンチマーク別内訳のPNGを出力ディレクトリに書く。`-i` で入力CSVディレクトリ、`-e` で個別ベンチマークの除外、`-n` でベンチマーク別内訳の出力を抑制。除外リスト `CRYPTO_EXCLUDE` は `analyze.py` 内のハードコード定数。

venvは未コミット（`**/.venv/` gitignore）なので新規環境では作成が必要。**リポジトリ直下**に作る（READMEの案内と一致させる）：

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt   # matplotlib==3.11.0 のみ（numpyは依存で入る）
```

`analyze.py` の依存は matplotlib（+ 依存の numpy）だけ。pandasは使っていない。プロットは `matplotlib.use("Agg")` 固定（ヘッドレス前提）で、配色はdatavizスキルのリファレンスパレットに合わせたハードコード定数（`BLUE`/`RED`/`GRID` 等、ファイル冒頭）。

### 出力フォーマット

CSV: `class,bitwidth,count`。`class` は `ADD/SUB/RSB/ADC/SBC/AND/ORR/EOR/BIC/MVN/MUL/DIV/LSL/LSR/ASR/ROR/CMP/CMN/TST/TEQ/STR`、`bitwidth` は0〜32の有効ビット幅。

### 一時ファイル・サンプルコード

- `temp/` — 動作確認用のスクラッチファイル（gitignore対象、テストELF、デコーダ検証スクリプト等）
- **重要**検証時等の一時ファイルは`temp/`に置くこと（ハーネス側のscratchpadではなくこちら）
- `examples/` — gitに入っている唯一のテスト資産。`examples/sancheck/sanity.c` はプラグインのサニティテストで、各行に期待ビット幅がコメントで書いてある（`100000 - 1` → 17bit、`255 & 1` → 1bit、`(unsigned char)100000` のSTR → 8bit 等）。`examples/start.s` / `vectors.s` / `link.ld` は最小のmps2-an385起動コード。プラグインの計測値を人手で突き合わせたいときはこれを使う

### デコーダの検証

`plugin/decode.c` の命令分類ロジックを `arm-none-eabi-objdump` の実際の逆アセンブル結果と突き合わせて検証するスクリプト一式。ホスト（x86-64）上でデコーダをそのままコンパイルして走らせる（デコード対象はARM命令バイト列をデータとして読むだけなので、テスト自体はクロスコンパイル不要）：

```bash
cd temp
gcc -o test_decode test_decode.c ../plugin/decode.c   # decode.c/decode.h をホスト向けにビルド
python3 validate.py corpus1_O0.o                        # 期待クラス(mnemonic→class表)とdecode_insn()の出力を突き合わせ
python3 validate.py corpus2_O2.o                        # corpus{1,2}_{O0,O1,O2,Os}.o の組み合わせで確認
```

不一致（mismatch）と未知ニーモニックを標準出力にリストする。分類ロジックを変更したら必ずここを再実行して回帰確認する。

**注意：この検証ハーネス一式（`temp/test_decode.c`, `temp/validate.py`, `temp/corpus{1,2}.c` とそのビルド済み `.o`）は `temp/` がgitignoreされているためコミットされていない。** 開発機のワークツリーには存在するが、クリーンなクローンには無い。`plugin/decode.c` を触る作業でこれらが見当たらない場合は、コーパス（ALU命令を一通り含むCのソースを `-O0/-O1/-O2/-Os` でコンパイルした `.o`）と突き合わせスクリプトを作り直す必要がある。回帰テストとして残す価値があるので、いずれ `temp/` の外へ移すのが望ましい。

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
| FIRフィルタ、FFT/DCTのフル実装（例：JPEGデコード＝`picojpeg`のWinograd IDCT）                | 除外               | 専用DSPアクセラレータ・画像コーデックHWに載る想定                        |
| AES/SHA等のフル実装                                                                         | 除外               | 専用暗号回路に載る想定                                                    |
| 制御（PID、状態機械、しきい値判定。例：決定木推論＝`xgboost`）                              | 採用               | ソフトコア側の主戦場                                                      |
| パーサ（JSON、Modbus、CANフレーム解析等）                                                   | 採用               | 専用回路化されない                                                        |
| RTOS/スケジューラ                                                                           | 採用               | ソフトコア以外に載せようがない                                            |
| CRC/チェックサム（軽量なもの。Reed-Solomon等の誤り訂正符号含む、例：QRコード生成＝`qrduino`） | 採用               | 専用IP化されないケースが多い                                              |
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

**2ファイルの責務分離が重要**：`decode.c`/`decode.h` はQEMUに一切依存しない純粋な命令デコーダ（`decode_insn(hw1, hw2, size, out)` が Thumb 命令1本を `DecodedInsn{cls, is_writeback, reg_a, reg_b, imm_b, store_size}` に落とすだけ）。だからこそホスト上で単体コンパイルしてobjdumpと突き合わせる検証ができる（上記「デコーダの検証」）。`bitwidth.c` 側はQEMUプラグインAPIとの接着＋レジスタ読み出し＋ヒストグラム集計＋CSV出力に専念する。**デコーダを拡張するときはQEMUのヘッダを `decode.c` に持ち込まないこと**（持ち込むとホスト側テストが壊れる）。

命令デコードはQEMU自身のdecodetree定義（`target/arm/tcg/t16.decode`, `t32.decode`）のビットフィールドをそのまま参照して自前実装（capstone不使用）。900命令超の自作テストコーパス（-O0〜-Os）を`arm-none-eabi-objdump`の実際の逆アセンブル結果と突き合わせて分類ロジックを検証済み。

- **測定対象は「結果」**：ALU書き戻し系命令（ADD/SUB/RSB/ADC/SBC/AND/ORR/EOR/BIC/MVN/MUL/DIV/LSL/LSR/ASR/ROR）はデスティネーションレジスタに書き戻された値、STR系（STR/STRB/STRH、単一レジスタのみ）はメモリに書かれるデータ値（アドレスは対象外）を測定。入力オペランドは測定しない（ユーザ判断）
- **CMP/CMN/TST/TEQ**：書き戻し先レジスタが無いため、プラグイン側で内部的に破棄される演算結果を再計算（例：CMPなら`Rn - オペランド2`）して測定
- **除外**：MOV/MOVW/MOVT（定数生成、ALU演算ではない）、SMULL/UMULL/SMLAL/UMLAL（64bit結果、v1対象外）、STM/PUSH/STRD等の複数レジスタ転送、SP/PCが関与するADD/SUB（アドレス計算とみなす）
- **有効ビット幅**：ALU系は符号付き32bit整数として`32 - clz(abs(x))`（0は0bit）。STR系はアクセスサイズでマスクした後、符号なしのビットパターンとして`32 - clz(x)`
- **重要な実装上の注意①（レジスタハンドル）**：`qemu_plugin_get_registers()` が返すハンドルは小さい整数をポインタにキャストしたもので、**r0のハンドルは値0（＝NULL）**。したがって `NULL` を「該当レジスタ無し」のセンチネルに使ってはいけない。`bitwidth.c` は `reg_valid[]` フラグを別に持つ。ここを間違えると**書き戻し先がr0のサンプルが全部黙って捨てられる**（r0はARMの戻り値・第1引数レジスタなので大量）。2026-08-20にこのバグを修正済み。修正前後で総サンプル数は例えば `crc32` 350,900→1,401,703、`pidctl` 1,154,280→1,477,700 と変わる。**それ以前に収集したCSVは全て過小計測なので使わないこと**（修正後に全ベンチ再収集済み）
- **重要な実装上の注意②（サンプリング時点）**：`qemu_plugin_register_vcpu_insn_exec_cb`のコールバックは命令実行**前**に発火する（QEMU 9.0で実測確認、ヘッダのドキュメントだけでは分からない）。書き戻し系の結果は当該命令の次の命令のコールバック時点（＝前命令完了後）まで遅延サンプリングして対応。TB内で書き戻し命令が最後の命令の場合はサンプリングされない（稀、許容する測定誤差として記録）
- 実行終了時（`qemu_plugin_register_atexit_cb`）に `class,bitwidth,count` のCSVをダンプ → Pythonで可視化

## ターゲットプログラム

### 実装済み（`benchmarks/`、データ収集済み）

- Embench-IoT（全ベンチマーク、`benchmarks/build_all.sh` が自動ビルド）：`aha-mont64` `crc32` `depthconv` `edn` `huffbench` `matmult-int` `md5sum` `nettle-aes` `nettle-sha256` `nsichneu` `picojpeg` `qrduino` `sglib-combined` `slre` `statemate` `tarfind` `ud` `wikisort` `xgboost`（うち `picojpeg`/`qrduino`/`xgboost` は複数.cファイル構成）
- CoreMark（`benchmarks/coremark/` + `benchmarks/coremark_port/`）。`build_all.sh` が `ITERATIONS=8` でビルドする
- **自作ベンチマーク（`benchmarks/custom/`、gitで追跡）** — 既存スイートでカバーできないターゲットドメインの2本。詳細は下記

### 自作ベンチマーク（`benchmarks/custom/`）

既存スイートに適当なものが無いため新規作成した2本。Embenchのcloneに依存しないので、クリーンなクローンでもビルドできる。

- `pidctl` — 4ch固定小数点PID制御ループ＋しきい値判定（ヒステリシス＋デバウンスの状態機械）。一次遅れのプラントモデルで閉ループにしてあり、整定後の小さい誤差だけでなくsetpointステップ直後の過渡（大きい誤差）も分布に入る。信号は12bit ADCカウント、ゲインはQ8.8、**積が32bitに収まるよう意図的にサイズ設計**（このクラスのソフトコアなら64bit中間値もソフトfloatも要らない形式を選ぶはず。floatを使うとlibgccのソフトfloat内部を測ることになってしまう）
- `accelmgr` — 「アクセラレータ随伴処理」。制御レジスタのプログラミング（小さいフィールドを32bitワードにパック）、DMA記述子の構築とキューイング、ペイロードのマーシャリング、リングバッファ管理（head/tail・ラップマスク）、割り込み時の結果取り出しと軽い後処理（スケーリング・しきい値判定・統計）

いずれも共通の約束事：

- `custom/harness.h` の3関数（`bench_init` / `bench_run` / `bench_expected`）を実装する。`bench_init()` が状態を戻すので `bench_run()` は常に同一の仕事をし、結果状態のチェックサムを返す
- チェックサムに**アドレス由来の値を混ぜない**こと。ホスト検証ビルド（x86-64）とARMでポインタ値が違うため
- `custom/harness.c` が bare-metal の `main()`。**Embenchと同じく固定の仕事を1回やって以降アイドル**する。`analyze.py` はベンチ間でカウントを単純加算するので、RUNTIME中ずっと回し続けると1本で他の全部を桁違いに上回って合成分布を支配してしまう（実測で約100倍）。`TICKS`/`FRAMES` は既存コーパスの中央値(約1.1M)付近に載るよう調整してある
- 乱数生成器を使わない。xorshift等はフルレンジの32bit値を吐くが、制御ループもドライバもそんな値は扱わない。32bit付近に偽のピークが立つので、ノイズ・外乱・センサ値・演算結果はすべてテーブル駆動

**検証：`./custom/check.sh`**。bare-metalビルドにはコンソールが無い（`run_all.sh` が `-serial none`）ので、挙動の変化が見えるのはここだけ。ホストの `gcc` で各ベンチを `host_test.c` とリンクし、`bench_expected()` と突き合わせる（純粋な固定幅整数コードなのでARMとホストで同じ値になる）。**`custom/` を触ったら必ず実行すること。** 新規ベンチの `bench_expected()` の値も、まず0を返しておいて `check.sh` の出力を読んで埋める

`accelmgr` の `accel_tick()` はハードウェアの代役で、記述子をリタイアさせて結果ブロックを返す最小限に留めてある。**その仕事は定義上ソフトコアでは走らないので、シミュレートに使った命令はすべてヒストグラムの汚染**になる（記述子1本あたりコア側数十命令に対して数命令。QEMUのデバイスモデルを別途書かない限りこれが限界）

分析時（`analyze.py -c`）は `nettle-aes`/`nettle-sha256`/`md5sum`/`aha-mont64`/`picojpeg` を専用回路領域として除外し、ターゲットドメイン内の分布のみを見る（`qrduino`/`xgboost`はソフトコア対象ドメインとして採用のまま）。

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
- [x] mps2-an385向けの最小限テストプログラム（起動確認用）の用意（`examples/vectors.s` + `examples/sancheck/sanity.c`）
- [x] arm-none-eabi-gcc環境の確認・セットアップ
- [x] ALU命令判定・ビット幅集計プラグインの設計・実装（`plugin/`、v1完成、自作サニティテストで数値レベルの一致を確認済み）
- [x] 「アクセラレータ随伴処理」模擬コードの自作（`benchmarks/custom/accelmgr/`）
- [x] 簡易PIDループ＋しきい値判定コードの自作（`benchmarks/custom/pidctl/`）
- [x] 各ターゲットプログラムのビルド・実行・データ収集
- [x] 結果の可視化・分析
- [ ] （任意）SMULL/UMULL等64bit乗算命令への対応拡張
- [ ] （任意）STM/PUSH等複数レジスタ転送への対応拡張
- [x] `run_analysis.sh` / `analyze.py` docstring のvenvパス（`analysis/.venv` → `.venv`）を実態に合わせる
- [x] CoreMarkを `build_all.sh` に組み込む
- [ ] デコーダ検証ハーネスを `temp/`（gitignore）から追跡対象へ移す
