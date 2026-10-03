# T1 入出力の同時オープンとクロックずれ（(a)）・遅延の実測（(e)）

## 状態

| 項目 | 状態 | 理由 |
|---|---|---|
| T1-1 `p0_devices`（一覧） | **実施**（2026-10-03 01:19、デバイスは開いていない） | - |
| T1-2 `p0_passthrough` | 未実施（ビルドまで） | VB-CABLE 未導入 |
| T1-3 `p0_capture` | 未実施（ビルドまで） | VB-CABLE 未導入 |
| T1-4 `tools/analyze_sine.py` | 未実施（`--self-test` だけ実行して合格） | 解析する録音が無い（VB-CABLE 未導入） |
| T1-6 別デバイスのときの JUCE の同期 | **ソースで確認**（ログは未取得） | ログはデバイスを開かないと取れない |
| T1-7 `p0_latency`（(e)） | 未実施（ビルドまで） | VB-CABLE 未導入 |

## T1-1 デバイスの一覧（`p0_devices`）

条件: JUCE 8.0.6、`AudioDeviceManager::createAudioDeviceTypes`（WASAPI のみ有効、ASIO と DirectSound は無効。製品と同じ）、`createDevice()` で照会しただけで `open()` は呼んでいない。全行は `01-devices-list.txt` / `01-devices-list.csv`。

JUCE が出した種別は 3 つとも出た: 「Windows Audio」「Windows Audio (Low Latency Mode)」「Windows Audio (Exclusive Mode)」。

| 種別 | デバイス | 対応サンプルレート | 対応バッファ（サンプル） | 既定バッファ |
|---|---|---|---|---|
| Windows Audio | 入力 マイク (M-AUDIO Uber Mic) | 48000 | 144〜2048（144, 160, 192, …, 480, 512, …） | 480 |
| Windows Audio | 出力 スピーカー (Sound BlasterX AE-5 Plus) | 96000 | 288〜2048 | 960 |
| Windows Audio | 出力 NVIDIA HDMI 3 系統、スピーカー (M-AUDIO Uber Mic) | 48000 | 144〜2048 | 480 |
| Windows Audio | 出力 Realtek Digital Output | 192000 | 576〜2048 | 1920 |
| Low Latency Mode | 入力 マイク (M-AUDIO Uber Mic) | 48000 | **480 だけ** | 480 |
| Low Latency Mode | 出力 48 kHz の各デバイス | 48000 | **480 だけ** | 480 |
| Low Latency Mode | 出力 スピーカー (AE-5 Plus) | 96000 | 960 だけ | 960 |
| Exclusive Mode | 入力 マイク (M-AUDIO Uber Mic) | 44100, 48000 | 144〜2048 | 480 |
| Exclusive Mode | 出力 スピーカー (AE-5 Plus) | 16000, 44100, 48000, 88200, 96000 | 288〜2048 | 960 |

報告された入力遅延・出力遅延: **未取得**。JUCE の WASAPI は、`open()` の後に `GetStreamLatency` から遅延を求める（`juce_WASAPI_windows.cpp` の `openClient`、`open` の `latencyIn = latencySamples + currentBufferSizeSamples`）。開かない約束なので取っていない。VB-CABLE を入れた後に `p0_devices --open`（共有モードの 2 種別だけ、無音を書いてすぐ閉じる）で取れる。

### 一覧から分かったこと

1. **このPCのデバイスでは、低遅延モードで選べるバッファは 480（10 ms）だけ**。マイク（USB）も 48 kHz の出力も、`IAudioClient3` の最小周期が 10 ms ということになる。指示書の「128、256、480 の 3 通り」のうち、128 と 256 は低遅延モードでは開けない（JUCE は `jmax (要求, minBufferSize)` で 480 にする）。
2. **128 はどの種別にも無い**。最小は 144（3 ms）。共有モード（Windows Audio）は 144〜2048 を出すが、共有モードではエンジンの周期（10 ms）より短い遅延にはならない（**未確認**: 開いて報告値を見ていない）。128 / 256 で本当に遅延を縮められるのは排他モードだけで、排他モードはマイクを他のアプリ（Discord、ゲーム）と共有できない。
3. AE-5 Plus のスピーカーは共有モードの形式が 96 kHz。JUCE は「Windows Audio」（共有）では `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` を付けて OS に変換させる（`supportsSampleRateConversion` が共有モードだけ真）ので 48 kHz で開ける。**低遅延モードでは変換しないので、96 kHz のデバイスを 48 kHz では開けない**。モニター（第 2 デバイス）を低遅延モードで開く設計にすると、このヘッドホンが使えない。
4. VB-CABLE（「CABLE Input」「CABLE Output」）は無い。

## T1-6 入力と出力が別デバイスのとき、JUCE はどう同期しているか（ソースで確認）

`modules/juce_audio_devices/native/juce_WASAPI_windows.cpp`（JUCE 8.0.6）を読んだ結果。実機のログでは確かめていない。

- 入力と出力の名前が違っても、`WASAPIAudioIODevice` **1 つ**が `WASAPIInputDevice` と `WASAPIOutputDevice` を持ち、**1 本のスレッド**（`run()`）で両方を扱う。1 つのデバイスとして結合される。
- スレッドの歩調は**出力側**が決める（共有モード: `copyBuffers` が出力のイベントを待つ）。入力は、その都度 `handleDeviceBuffer()` でリザーバ（FIFO、容量は `nextPowerOfTwo (actualBufferSize + ユーザーのバッファ)`）にためておき、コールバックの前に `copyBuffersFromReservoir()` で取り出す。
- **クロックのずれを補う仕組み（リサンプル）は無い**。ずれは次のどちらかで表に出る:
  - 入力が遅い（足りない）: `copyBuffersFromReservoir` が**ブロックの先頭をゼロで埋める**。`getXRunCount()` には**数えられない**。
  - 入力が速い（あふれる）: リザーバに入らないパケットは `ReleaseBuffer (0)` で残され、WASAPI 側があふれると `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY` が立つ。`getXRunCount()` が数えるのは**この入力側の不連続だけ**（出力側の途切れは数えない）。
- したがって §13.1 の「XRUN 0」だけでは合格と言えない。`p0_passthrough` は、ゼロ埋めを「ブロックの先頭が 16 サンプル以上ちょうど 0 で、その後に音がある」コールバックとして数える（`zero_head`、推定。マイクが本当に無音だと数えない）。正弦波の連続性（`analyze_sine.py`）は出力側と仮想ケーブルの途切れを見る。

## 導入後に打つコマンド

VB-CABLE（https://vb-audio.com/Cable/ 、管理者権限と再起動が要る）を入れたあと、PowerShell で:

```powershell
$p = "$env:LOCALAPPDATA\KoeLoom\build\phase0\Release"
cd <repo>\phase0      # 本体に統合後は <repo>\phase0

# 1. 報告遅延を含む一覧（共有モードだけ開く。無音）
& $p\p0_devices.exe --open

# 2. 正弦波の連続性 30 分（ターミナルを 2 つ。先に録音を始める）
& $p\p0_capture.exe --minutes 31
& $p\p0_passthrough.exe --source sine --with-mic --buffer 480 --minutes 30 --type "Windows Audio (Low Latency Mode)"
python tools\analyze_sine.py (Get-ChildItem results\raw\capture_*.wav | Sort-Object LastWriteTime | Select-Object -Last 1).FullName

# 3. 低遅延モード以外で 256 / 144（128 は無い）: 種別を変えて 2. を繰り返す
& $p\p0_passthrough.exe --source sine --with-mic --buffer 256 --minutes 30 --type "Windows Audio"
& $p\p0_passthrough.exe --source sine --with-mic --buffer 144 --minutes 30 --type "Windows Audio (Exclusive Mode)"

# 4. マイクの音そのまま 30 分（XRUN と zero_head を見る）
& $p\p0_passthrough.exe --source mic --buffer 480 --minutes 30

# 5. 遅延の実測（出力バッファ + 仮想ケーブル + 入力バッファ）。100 回 x バッファごと
& $p\p0_latency.exe --buffers 480 --type "Windows Audio (Low Latency Mode)"
& $p\p0_latency.exe --buffers 144,256,480 --type "Windows Audio"
& $p\p0_latency.exe --buffers 144,256,480 --type "Windows Audio (Exclusive Mode)"
```

結果は `results/01-passthrough.csv`、`results/01-sine-continuity.csv`、`results/01-latency.csv`（1 行ずつ追記）、1 秒ごとのログは `results/raw/`。合格の目安は 30 分で XRUN 0・不連続 0・無音 0・zero_head 0（§13.1）。

## 結果表（未記入）

| 入力デバイス種別 | バッファ | 実行時間 | XRUN | zero_head | 不連続 | 無音の区間 | 実際に開いた設定 |
|---|---|---|---|---|---|---|---|
| 未実施（VB-CABLE 未導入） | - | - | - | - | - | - | - |

| 種別 | バッファ | 回数 | 最小 (ms) | 中央値 (ms) | 最大 (ms) | 報告値: 入力 / 出力（サンプル） |
|---|---|---|---|---|---|---|
| 未実施（VB-CABLE 未導入） | - | - | - | - | - | - |
