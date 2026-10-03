# T4 モニター用の第 2 デバイスのずれ補正（(d)）

## 状態

**未実施（ビルドまで）。理由: VB-CABLE 未導入。** 主系統（マイク → CABLE Input）が作れないため。第 2 デバイス（ヘッドホン）にあたる出力は、この PC にある（スピーカー (Sound BlasterX AE-5 Plus) など。`01-devices.md`）。

## 作ったもの（`p0_monitor`）

- 主系統 A: 入力 = マイク（`--source mic` のときだけ開く）、出力 = 「CABLE Input」（それ以外なら開かずに止まる）。A のコールバックが、出す音と同じものをロックフリーの SPSC FIFO（`juce::AbstractFifo`、容量 48000 サンプル = 1 秒）に入れる。
- モニター B: 出力だけのデバイス（`--monitor <名前の一部>`、既定の種別は「Windows Audio」）。B のコールバックが FIFO から取り出し、**適応リサンプル**（3 次エルミート補間）で読む速さを変える: `比 = 1 + P·e + I`、`e = (なめらかにした残量 − 目標) / 目標`、`P = 0.002`、`I` は `7×10⁻⁷·e` ずつ積分（±2000 ppm で頭打ち）、比は 0.995〜1.005 に制限。残量は時定数 1 秒でなめらかにする。積分項 `I` が A と B のクロック比の推定値（ppm）になる。
- 目標の残量は既定で `2 × (A のバッファ + B のバッファ)`（480 / 480 なら 1920 サンプル = 40 ms。`--target` で変えられる）。最初は目標まで貯まってから読み始める。
- 1 秒ごとに、残量（なめらかにした値、その 1 秒の最小と最大）、比（ppm）、推定クロック比（ppm）、枯渇の回数、溢れの回数を `results/raw/monitor_<日時>.csv` に書き、最後に `results/04-monitor-drift.csv` に 1 行足す。
- `--source` の既定は `silence`（**何も聞こえない**。FIFO の動きは音の中身に関係しない）。`sine` は 440 Hz -20 dBFS、`mic` はマイクの音をヘッドホンに出す。

注意: AE-5 Plus のスピーカーは共有モードの形式が 96 kHz なので、`--monitor-type "Windows Audio (Low Latency Mode)"` では 48 kHz で開けない（`01-devices.md`）。既定の「Windows Audio」なら OS が変換する。

## 導入後に打つコマンド

```powershell
$p = "$env:LOCALAPPDATA\KoeLoom\build\phase0\Release"
# 30 分。ヘッドホンの名前の一部を --monitor に。無音で測る（耳に音は出ない）
& $p\p0_monitor.exe --monitor "AE-5" --minutes 30
# 音で確かめたいとき（ヘッドホンにマイクの声が出る。ハウリングに注意）
& $p\p0_monitor.exe --monitor "AE-5" --minutes 30 --source mic
```

合格の目安（R-2）: 30 分で枯渇 0 回、溢れ 0 回。推定クロック比（ppm）と残量の推移も表に書く。

## 結果表（未記入）

| 主系統 | モニター | 実行時間 | 推定クロック比 (ppm) | 残量の最小 / 最大（サンプル） | 枯渇 | 溢れ |
|---|---|---|---|---|---|---|
| 未実施（VB-CABLE 未導入） | - | - | - | - | - | - |
