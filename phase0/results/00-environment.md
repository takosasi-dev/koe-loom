# T0 環境の記録

記録日: 2026-10-03（Phase 0 の測定と同じ日）。値は PowerShell（`Get-CimInstance`、レジストリ、`powercfg`）と `p0_devices` で取った。

## PC

| 項目 | 値 |
|---|---|
| CPU | 11th Gen Intel(R) Core(TM) i7-11700F @ 2.50GHz（WMI の MaxClockSpeed 2496 MHz、L2 4096 KB、L3 16384 KB） |
| 物理コア / 論理コア | 8 / 16 |
| メモリ | 34175918080 バイト（16 GB × 2、2667 MT/s、メーカー ID 04EF） |
| Windows | Microsoft Windows 11 Home Insider Preview（WMI の Caption）、バージョン 10.0.26220、ビルド 26220.9472、DisplayVersion 25H2、64 ビット。レジストリの ProductName は「Windows 10 Home」と出る（Windows 11 でも残る既知の表記） |
| 電源プラン | 究極のパフォーマンス（GUID 36c2cf79-4220-4900-b462-c89445364174、アクティブ） |
| 基準 PC か（A-14: 論理コア 4 以上） | **当たる**（論理 16） |

## 測定中の PC の状態（CPU の数値すべてに関係する）

測定の間、**依頼者がゲーム（AceCombat8.exe、3 秒間の実測で PC 全体の 32.7 %）と Discord（14.1 %）を動かしていた**。並行して、他の担当（effects-a / effects-b / model）のビルドとテスト（KoeLoom.exe、MSBuild）も動いていた。各プログラムは、測定区間の PC 全体のビジー率（`GetSystemTimes`）を結果に書いている。KoeLoom の想定する使い方（ゲーム + Discord と同時）に近い条件だが、再現性のある「静かな PC」の値ではない。静かな状態での測り直しのコマンドは `README.md` に書いた。

## ビルド

| 項目 | 値 |
|---|---|
| コンパイラ | MSVC 19.44.35228.0（ツールセット 14.44.35207、Visual Studio 2022 Build Tools 17.14.37614.0）、x64 |
| CMake | 4.4.2（ジェネレーター Visual Studio 17 2022） |
| JUCE | 8.0.6、コミット 51a8a6d7aeae7326956d747737ccf1575e61e209（タグ 8.0.6、2025-01-10）、`%LOCALAPPDATA%\KoeLoom\deps\JUCE` |
| Signalsmith Linear | `third_party/signalsmith-linear`（0.6.4 とされている）。FFT は標準の実装（IPP / Accelerate / PFFFT なし） |
| RNNoise | `third_party/rnnoise`（v0.2 のリリース、モデル込み）。MSVC では x86 の実行時切り替え（AVX2 / SSE4.1）のファイルをビルドしておらず、汎用 C + SSE2 の経路 |
| ビルド構成 | Release（`juce_recommended_config_flags`）、`/utf-8 /MP` |
| ソースのパス | `<repo>\phase0`（ASCII）。日本語のパスに起因するビルドの失敗はなかった |
| 解析スクリプト | Python 3.11.9、numpy 2.4.4（scipy は無い。使っていない） |

## オーディオデバイス

`p0_devices`（デバイスを開かない。一覧は `01-devices-list.txt` / `.csv`）で見えた WASAPI の端点:

| 種別 | 名前 |
|---|---|
| 入力 | マイク (M-AUDIO Uber Mic)（USB マイク）、HDMI (Live Gamer Ultra-Audio)（キャプチャボード）、What U Hear (Sound BlasterX AE-5 Plus) |
| 出力 | スピーカー (Sound BlasterX AE-5 Plus)（共有モードの形式 96 kHz）、X2483/2481・SHARP HDMI・PL2561H (2- NVIDIA High Definition Audio)、Realtek Digital Output (Realtek(R) Audio)（192 kHz）、スピーカー (M-AUDIO Uber Mic)、SPDIF-Out (Sound BlasterX AE-5 Plus) |
| 仮想ケーブル | **VB-CABLE は無い**（「CABLE Input」「CABLE Output」とも無し） |

`Win32_SoundDevice` には「Voicemod Virtual Audio Device (WDM)」も出るが、WASAPI の端点としては見えなかった（無効になっていると思われる。未確認）。
