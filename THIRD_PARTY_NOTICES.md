# Third-party notices

KoeLoom が使う外部のコードの一覧です。どれも、取り込んだ時点で配布元の LICENSE を読んで確かめました。ライセンスの全文は各フォルダにあります。

| 名前 | 用途 | ライセンス | 著作権表示 | 全文 |
|---|---|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) 8.0.6 | アプリの土台（音声入出力、GUI） | AGPLv3 または商用のデュアル。本プロジェクトは AGPLv3 を選ぶ | Copyright (c) Raw Material Software Limited | JUCE の `LICENSE.md`（ビルド時に取得。リポジトリには含めない） |
| [RNNoise](https://github.com/xiph/rnnoise) v0.2（モデル入りのリリース tarball） | ノイズ抑制 | BSD-3-Clause | Copyright (c) 2007-2017, 2024 Jean-Marc Valin / 2023 Amazon / 2017 Mozilla / 2005-2017 Xiph.Org Foundation / 2003-2004 Mark Borgerding | [`third_party/rnnoise/COPYING`](third_party/rnnoise/COPYING) |
| [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch)（ヘッダの版数 1.3.2） | 声の変換の候補 A（比較用に残している。既定は自前の位相ボコーダー） | MIT | Copyright (c) 2022 Geraint Luff / Signalsmith Audio Ltd. | [`third_party/signalsmith-stretch/LICENSE.txt`](third_party/signalsmith-stretch/LICENSE.txt) |
| [Signalsmith Linear](https://github.com/Signalsmith-Audio/linear) | Signalsmith Stretch が使う FFT など | MIT | Copyright (c) 2025 Signalsmith Audio | [`third_party/signalsmith-linear/LICENSE.txt`](third_party/signalsmith-linear/LICENSE.txt) |

- SoundTouch は Phase 0 の比較の候補だったが、取得先の条件（GitHub と公式ページ）に合わず取り込んでいない（`phase0/results/02-shifters.md`）。
- 声の変換（位相ボコーダー）、ピッチ検出、エフェクトは自前の実装で、外部のコードを含まない。
- フォント（Yu Gothic UI、Consolas）は Windows に標準で入っているものを使い、このリポジトリには含めない。
