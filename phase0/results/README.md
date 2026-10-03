# Phase 0 技術検証の結果

測定日: 2026-10-03。PC: i7-11700F（論理 16、基準 PC に当たる）、Windows 11 Insider 26220、Release ビルド。**測定中、依頼者はゲーム（AceCombat8）と Discord を動かしていた**（KoeLoom の実際の使い方に近いが、静かな PC の値ではない）。DSP の時間は MMCSS「Pro Audio」のスレッドで測った（JUCE の音声スレッドと同じ扱い）。

**基準音声（本人の声）は無いので、T2・T3・T5・T6 の音声は合成音声で代用した**（依頼者の判断）。`phase0/testdata/reference-speech.wav` を置けば、同じコマンドで測り直せる。

**追記（第 3 波）: TTS の読み上げでも測り直した**（`testdata/tts-speech.wav`、Windows の「Microsoft Haruka Desktop」。`python tools\make_tts_speech.py` で作る）。**本人の声ではなく、試聴による判断もまだ**。結果は各文書の末尾の「TTS 代用」の節と `*-tts*` のファイル（合成音声の結果は上書きしていない）。測ったときは PC が空いていた（ビジー率 10〜25 %）ので、CPU の差は入力よりそちらの影響が大きい。主な違い:
- T2: Signalsmith のフォルマント処理による音量の目減りが 2.0〜3.3 dB → **0.8〜1.4 dB**（推奨 1440 / 480 の 3 組）。±2 dB には入るが、±1 dB と補正が要るという結論は同じ
- T3: 音声の切れ目のノイズの抑制 45.4 → **38.9 dB**、声の目減り -0.35 → -0.15 dB。遅延 20 ms と AC-16 は同じ
- T5: 1 分版の判定は同じ（p99 の短縮 41〜57 %、期限切れ 0）
- T6: 正解の f0 が無いので誤差は出せない。有声の割合は MPM 0.76 / YIN 0.68 で、MPM が良いという判断は同じ
- プリセットの音量（`outputTrimDb`）も TTS で校正し直した: `10-calibration-tts.md`（22 本、機器の音が +1.5 dB）

## タスクの状態

| タスク | 状態 | 理由・メモ | 結果 |
|---|---|---|---|
| T0 環境 | 実施 | - | `00-environment.md` |
| T1 入出力・クロックずれ・遅延の実測 | 一部（`p0_devices` の一覧と、JUCE のソースの確認だけ） | **VB-CABLE 未導入**。`p0_passthrough`・`p0_capture`・`p0_latency`・`analyze_sine.py` はビルド（とセルフテスト）まで | `01-devices.md` |
| T2 変換アルゴリズム | 実施（候補 A。14 設定） | 候補 C（SoundTouch）は未実施: 取得先が Codeberg で、GitHub・公式ページのルールに入らない。候補 B は対象外 | `02-shifters.md`、`02-listening-sheet.md` |
| T3 RNNoise | 実施 | 抑制量は合成音声で代用 | `03-rnnoise.md` |
| T4 モニターのずれ補正 | 未実施（ビルドまで） | **VB-CABLE 未導入**（主系統が作れない） | `04-monitor-drift.md` |
| T5 並列処理 | 1 分版を実施（2 設定 × 6 条件） | 10 分版は**途中で中止**（依頼者の「ちょうどいいところで止めて」の指示。コマンドは `05-parallel.md`）。1 時間版は依頼者にお願いする | `05-parallel.md` |
| T6 ピッチ検出 | 実施 | 話し声は合成音声で代用 | `06-pitch.md` |

## 主な結果

- **T1**: VB-CABLE が無く、デバイスを開く測定はできなかった。この PC では低遅延モードのバッファは 480 だけ、128 はどの種別にも無い（最小 144）。JUCE は別デバイスの入出力を 1 本のスレッドに結合し、クロックのずれを補わず、入力の不足（ゼロ埋め）は XRUN に数えない（ソースで確認）。
- **T2**: Signalsmith Stretch の遅延は報告値と実測値が全 14 設定で一致（ブロック長、split なら + interval）。推奨 **1440 / 480 / split なし**: 30.000 ms、3 インスタンスの CPU（バッファ 480）p99 3.878〜9.249 %、AC-04 合格（-0.242 セント）。音量はフォルマント処理で 2.021〜3.323 dB 下がる（どの設定でも）。
- **T3**: RNNoise（BSD-3-Clause）の遅延は **20.000 ms**（仕様の「約 10 ms」の 2 倍）、バッファが 480 の倍数でないと 29.333 ms。1 フレーム p50 155.600 µs / p99 344.300 µs。-40 dBFS のホワイトノイズ単独を 56.973 dB 抑制（AC-16 合格）。
- **T4**: 未実施。`p0_monitor`（ロックフリー FIFO + 適応リサンプル、既定は無音で測る）を用意した。
- **T5**: 1 分版（ゲーム実行中）で、parallel の p99 は single より 41.208〜52.144 % 短く（推奨設定、128 / 256 / 480）、期限切れ・レイヤーの無音化は全条件 0。出力は 1 サンプル残らず一致（F-15-3）。MMCSS は全スレッドで成功。プロセス全体の CPU は 35.881〜89.421 % 増える。1 スレッドでも 480 の p99 は 7.729 %。
- **T6**: **MPM が良い**。1 回の時間が YIN より 32 % 短く（p50 197.100 対 290.500 µs）、立ち上がりの検出が速い。下限 60 Hz だと音程の切り替えの検出に 30〜38 ms かかるので、80 Hz にすると目安（25 ms、±10 セント、CPU 5 %）をすべて満たす。

## 仕様の判断材料

| 判断 | 結果 | 根拠のファイル | 状態 |
|---|---|---|---|
| 変換アルゴリズムは A で行けるか（R-1） | **条件つきで行ける**。CPU（3 インスタンスで p99 9.249 % 以下）と AC-04 は推奨設定で合格。音量は補正を足す必要がある（F-02-12）。遅延は 60 ms 目標に入らない（下の行）。音質は試聴待ち | `02-shifters.md` | 暫定（合成音声、試聴前） |
| 遅延の目標 60 ms は妥当か（§5.4） | **届かない見込み**。推奨設定で 入力 10 + ノイズ抑制 20.000 + 変換 30.000 + 出力とケーブル 10〜20 = 70〜80 ms。60 ms に入るのは 20 ms ブロックだけで、音程の正確さが落ちる（AC-04 不合格）。80 ms への見直し（上限 100 ms は維持）を提案 | `02-shifters.md`、`03-rnnoise.md`、`01-devices.md` | 暫定（入出力とケーブルは未測定） |
| 並列処理を採用するか（F-15-1） | **保留**（1 時間版が無いため）。1 分版では全条件で採用の条件を満たす（p99 が 41.208〜52.144 % 短い、期限切れ 0 / 0）。ただし 1 スレッドでも予算の 3 分の 1 以下なので、急いで入れる理由は無い | `05-parallel.md` | 暫定 |
| 入出力の別デバイスでクロックずれが問題になるか（R-2） | **未判定**（VB-CABLE 未導入）。JUCE がずれを補わない作りであることはソースで確認した | `01-devices.md` | 未確認 |
| 推奨する ShifterConfig（`Source/Dsp/IVoiceShifter.h` の既定値） | **block 1440 / interval 480 / split false**。理由: 30 ms の中で CPU 最小（バッファ 480 で毎回 1 回の計算になり負荷が平ら。p50 2.360〜3.511 %、いまの 1440 / 240 の約半分）、AC-04 合格（1440 / 240 は +23.020 セントで不合格）、split は音を変えず遅延だけ増える。試聴で粗ければ次点 1440 / 360 | `02-shifters.md` | 暫定（試聴前） |
| ノイズ抑制（RNNoise）は使えるか（§5.3） | 使える。遅延 20.000 ms、CPU はバッファ 480 で p99 3.542 %、抑制 56.973 dB | `03-rnnoise.md` | 暫定（合成音声） |
| 既定のバッファ（A-17: 480） | 妥当。低遅延モードで開ける唯一の値で、RNNoise の FIFO の遅延も 0 | `01-devices.md`、`03-rnnoise.md` | 暫定 |
| ピッチ検出の方式（§5.7） | MPM。範囲は 80〜1000 Hz を提案 | `06-pitch.md` | 暫定（合成音声） |


**lead の測定（参考、phase0 では測っていない）**: lead 側の検証でも Signalsmith は 20〜40 ms のブロックで純音のピッチが数十セントずれ（AC-04 の ±10 セントを満たさない）、製品は自前の位相ボコーダー（FFT 2048・窓 1536・遅延 1535 サンプル = 31.979 ms、最悪 5.9 セント）を既定にしている。上の推奨 ShifterConfig は Signalsmith を使う場合の値（1440 / 480 は 220 Hz の AC-04 には通るが、`02-f0-sweep.csv` では他の周波数で数十セントずれる組がある）。

## 止めた時点の状態（2026-10-03 02:23、依頼者の指示で停止）

- 済み: T0、T1 の一覧とソース確認、T2（全 14 設定、MMCSS で測り直し済み）、T3、T5 の 1 分版（2 設定）、T6（2 設定）、全結果の文書化。
- 未実施: T1 のデバイスを開く測定と T4（VB-CABLE 未導入）、T5 の 10 分版（中止）と 1 時間版、候補 C（SoundTouch）、基準音声での測り直し、試聴。
- 次にやること: 「作者にお願いしたいこと」の 1〜4。

## 作者にお願いしたいこと

1. **VB-CABLE の導入**（https://vb-audio.com/Cable/ 。管理者権限と再起動が要る）。そのあと T1 と T4 のコマンド（`01-devices.md`、`04-monitor-drift.md` の最後）を実行。T1 は 30 分 × 条件ごと。
2. **基準音声の録音**: 48 kHz・モノラル・16 bit 以上・10 秒・日本語の読み上げ・0.5 秒を超える無音なし・ピーク -6 dBFS 前後（presets §5.1）を `phase0\testdata\reference-speech.wav` に置く。そのあと次を実行:
   ```powershell
   $p = "$env:LOCALAPPDATA\KoeLoom\build\phase0\Release"
   & $p\p0_shifters.exe --cpu-seconds 10
   & $p\p0_shifters.exe --cpu-seconds 0.5 --no-wav --level-scan --tag scan
   & $p\p0_rnnoise.exe --seconds 60
   & $p\p0_pitch.exe ; & $p\p0_pitch.exe --fmin 80 --integration 512 --tag short
   & $p\p0_parallel.exe --minutes 1 --block 1440 --interval 480 --split 0
   ```
3. **試聴**: `results\raw\` の WAV を `02-listening-sheet.md` に記入（推奨の `A-b1440-i480_*` と、ライブラリ標準 `A-default_*`、20 ms の `A-b960-i240_*` を先に）。
4. **並列処理の 1 時間版**（§13.1 の採用条件は「1 時間の連続処理で XRUN が増えない」）。まず既定のバッファ 480 だけ（single と parallel で 2 時間）:
   ```powershell
   & "$env:LOCALAPPDATA\KoeLoom\build\phase0\Release\p0_parallel.exe" --minutes 60 --buffers 480 --block 1440 --interval 480 --split 0 --tag 60min-480
   ```
   全条件（128 / 256 / 480 × 2 モード）だと 6 時間: `--buffers 128,256,480 --tag 60min`。ゲームをしながらでも測れる（音は出ない。プロセス全体の CPU は 1 分版の実測で 0.0471〜0.1146 コア）。
5. （任意）**静かな PC での測り直し**: ゲームと Discord を閉じて、2. の `p0_shifters` と `p0_parallel` をもう一度。今回の値はゲーム実行中のもの。

## ファイル

| ファイル | 内容 |
|---|---|
| `00-environment.md` | T0 |
| `01-devices.md`、`01-devices-list.txt/.csv` | T1（一覧、JUCE の同期の確認、導入後のコマンド） |
| `02-shifters.md`、`02-shifters.csv`、`02-ac04.csv`、`02-f0-sweep.csv`、`02-level-scan.csv`、`02-shifters-normalprio.csv`、`02-shifters-run.txt` | T2 |
| `02-listening-sheet.md` | T2 の試聴シート（空） |
| `03-rnnoise.md`、`03-rnnoise.csv`、`03-rnnoise-run.txt` | T3 |
| `04-monitor-drift.md` | T4（未実施、コマンド） |
| `05-parallel.md`、`05-parallel*.csv`、`05-parallel*-run.txt` | T5 |
| `06-pitch.md`、`06-pitch*.csv`、`06-pitch*-run.txt` | T6 |
| `*-tts*.csv`、`*-tts-run.txt` | T2・T3・T5・T6 の TTS 代用での測り直し（各文書の末尾に節） |
| `10-calibration-tts.md`、`10-calibration-tts.csv`、`10-calibration-synth.csv` | プリセットの音量の校正（TTS 代用と合成音声の比較） |
| `raw/` | 試聴用 WAV など（git に入れない） |

表は `python ..\tools\summarize.py` で CSV から出し直せる（T2。TTS 代用は `--tag tts`）。
