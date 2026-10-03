# phase0 — 技術検証

GUI なしの実験プログラムで、遅延・CPU・クロックずれ・並列処理の効果を測るためのフォルダです。製品のコードではありません。結果と判断材料は `results/README.md`。

## 置き場所

| 場所 | 内容 | git |
|---|---|---|
| `phase0/`（このフォルダ） | 実験プログラムのソース（`src/`）、CMakeLists.txt、解析スクリプト（`tools/`） | 入れる |
| `phase0/testdata/` | 基準音声（`reference-speech.wav`） | 入れない（本人の声のため）。条件は `testdata/README.md` |
| `phase0/results/` | 測定結果の Markdown と CSV | 入れる |
| `phase0/results/raw/` | 試聴用 WAV、録音、1 秒ごとのログなどの生データ | 入れない |
| `%LOCALAPPDATA%\KoeLoom\build\phase0` | ビルドの出力先 | フォルダの外 |

## ビルド

独立した CMake プロジェクト（C++20）。JUCE は `%LOCALAPPDATA%\KoeLoom\deps\JUCE`（8.0.6 のチェックアウト）を `add_subdirectory` で使い、RNNoise と Signalsmith は `../third_party` のターゲット（`koeloom_rnnoise`、`koeloom_signalsmith`）を使います。MSVC は `/utf-8`。**`-S` には ASCII のパスを渡します**（例: `<リポジトリ>\phase0`。リポジトリが日本語のフォルダにあるときは、`tools\build.ps1` と同じジャンクション経由）。測定は Release で行います。

```powershell
cmake -S <リポジトリ>\phase0 -B "$env:LOCALAPPDATA\KoeLoom\build\phase0" -G "Visual Studio 17 2022" -A x64
cmake --build "$env:LOCALAPPDATA\KoeLoom\build\phase0" --config Release --parallel
```

## プログラム

実行ファイルは `%LOCALAPPDATA%\KoeLoom\build\phase0\Release\`。どれも窓を出しません。デバイスを開くのは VB-CABLE が要るもの（T1、T4）だけで、出力先が「CABLE Input」以外なら開かずに止まります（`--allow-any-output` を付けたときを除く）。

| プログラム | タスク | デバイス | 主な引数 |
|---|---|---|---|
| `p0_devices` | T0/T1 デバイス一覧 | 開かない（`--open` で共有モードだけ開いて報告遅延を読む） | `--open` |
| `p0_passthrough` | T1 マイク/正弦波 → CABLE Input | 開く | `--buffer 128\|256\|480 --minutes 30 --source mic\|sine --type <種別> --in <マイク名の一部>` |
| `p0_capture` | T1 CABLE Output → WAV | 開く（入力だけ） | `--minutes 31 --out <wav>` |
| `tools/analyze_sine.py` | T1 正弦波の連続性 | - | `<wav>`、`--self-test` |
| `p0_latency` | T1 (e) 遅延の実測 | 開く | `--buffers 128,256,480 --clicks 100` |
| `p0_shifters` | T2 変換器の比較 | 開かない | `--configs a,b --cpu-seconds 10 --f0-sweep --level-scan --tag <名前> --no-wav` |
| `p0_rnnoise` | T3 RNNoise | 開かない | `--seconds 60` |
| `p0_monitor` | T4 モニターのずれ補正 | 開く（ヘッドホンへ出す） | `--monitor <ヘッドホン名の一部> --minutes 30 --source silence\|sine\|mic` |
| `p0_parallel` | T5 並列処理 | 開かない（実時間の周期で回すだけ） | `--minutes 1\|10\|60 --buffers 128,256,480 --block 1440 --interval 480 --split 0 --tag <名前>` |
| `p0_pitch` | T6 ピッチ検出 | 開かない | `--hop 128 --fmin 60 --integration 1024 --tag <名前>` |
| `tools/summarize.py` | T2 の表を CSV から出す | - | - |

DSP の時間を測るプログラム（`p0_shifters`、`p0_rnnoise`、`p0_pitch`、`p0_parallel`）は、計測スレッドを MMCSS「Pro Audio」に入れる（JUCE の音声スレッドと同じ扱い）。`--normal-priority` で通常の優先度にできる（`p0_parallel` を除く）。

T2・T3・T5・T6 は、`testdata/reference-speech.wav`（48 kHz）があればそれを、無ければ合成音声（`synthVoice`、`Source/Tests/TestUtil.cpp` と同じ作り方）を使います。`--input <wav>` で別のファイルも指定できます。基準音声を置いたら、同じコマンドで測り直せます。
