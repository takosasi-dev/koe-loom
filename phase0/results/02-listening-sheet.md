# T2 試聴シート（記入用）

試聴は作者が行う。**このシートの音はすべて合成音声で代用**（`phase0/testdata/reference-speech.wav` が無かったため）。基準音声を置いて `p0_shifters` を実行し直すと、同じファイル名で本人の声の WAV に置き換わる。

- ファイルの場所: `phase0/results/raw/`（git に入れない）。元の音は `input.wav`。
- ファイル名: `A-<設定>_p<ピッチ st>_f<フォルマント st>.wav`。遅延は取り除いてあり、元の音と頭がそろう。
- 設定の中身（block / interval / split）は `02-shifters.md` の表。先に聴くなら、推奨の設定（`README.md`）と `default`（ライブラリ標準）を比べる。
- 評価は 5 段階（5 = 良い）。ざらつき・不自然さは「5 = 気にならない」。音量は別に測ってあるので、聴感の大きさは評価に入れない。

| ファイル名 | 言葉の聞き取りやすさ | ざらつき | 不自然さ | メモ |
|---|---|---|---|---|
| A-default_p0_f0.wav |  |  |  |  |
| A-default_p4_f2.5.wav |  |  |  |  |
| A-default_p-4_f-2.5.wav |  |  |  |  |
| A-default_p-9_f-5.wav |  |  |  |  |
| A-cheaper_p0_f0.wav |  |  |  |  |
| A-cheaper_p4_f2.5.wav |  |  |  |  |
| A-cheaper_p-4_f-2.5.wav |  |  |  |  |
| A-cheaper_p-9_f-5.wav |  |  |  |  |
| A-b960-i120_p0_f0.wav |  |  |  |  |
| A-b960-i120_p4_f2.5.wav |  |  |  |  |
| A-b960-i120_p-4_f-2.5.wav |  |  |  |  |
| A-b960-i120_p-9_f-5.wav |  |  |  |  |
| A-b960-i240_p0_f0.wav |  |  |  |  |
| A-b960-i240_p4_f2.5.wav |  |  |  |  |
| A-b960-i240_p-4_f-2.5.wav |  |  |  |  |
| A-b960-i240_p-9_f-5.wav |  |  |  |  |
| A-b960-i240-split_p0_f0.wav |  |  |  |  |
| A-b960-i240-split_p4_f2.5.wav |  |  |  |  |
| A-b960-i240-split_p-4_f-2.5.wav |  |  |  |  |
| A-b960-i240-split_p-9_f-5.wav |  |  |  |  |
| A-b1440-i240_p0_f0.wav |  |  |  |  |
| A-b1440-i240_p4_f2.5.wav |  |  |  |  |
| A-b1440-i240_p-4_f-2.5.wav |  |  |  |  |
| A-b1440-i240_p-9_f-5.wav |  |  |  |  |
| A-b1440-i360_p0_f0.wav |  |  |  |  |
| A-b1440-i360_p4_f2.5.wav |  |  |  |  |
| A-b1440-i360_p-4_f-2.5.wav |  |  |  |  |
| A-b1440-i360_p-9_f-5.wav |  |  |  |  |
| A-b1440-i480_p0_f0.wav |  |  |  |  |
| A-b1440-i480_p4_f2.5.wav |  |  |  |  |
| A-b1440-i480_p-4_f-2.5.wav |  |  |  |  |
| A-b1440-i480_p-9_f-5.wav |  |  |  |  |
| A-b1440-i240-split_p0_f0.wav |  |  |  |  |
| A-b1440-i240-split_p4_f2.5.wav |  |  |  |  |
| A-b1440-i240-split_p-4_f-2.5.wav |  |  |  |  |
| A-b1440-i240-split_p-9_f-5.wav |  |  |  |  |
| A-b1920-i240_p0_f0.wav |  |  |  |  |
| A-b1920-i240_p4_f2.5.wav |  |  |  |  |
| A-b1920-i240_p-4_f-2.5.wav |  |  |  |  |
| A-b1920-i240_p-9_f-5.wav |  |  |  |  |
| A-b1920-i480_p0_f0.wav |  |  |  |  |
| A-b1920-i480_p4_f2.5.wav |  |  |  |  |
| A-b1920-i480_p-4_f-2.5.wav |  |  |  |  |
| A-b1920-i480_p-9_f-5.wav |  |  |  |  |
| A-b1920-i480-split_p0_f0.wav |  |  |  |  |
| A-b1920-i480-split_p4_f2.5.wav |  |  |  |  |
| A-b1920-i480-split_p-4_f-2.5.wav |  |  |  |  |
| A-b1920-i480-split_p-9_f-5.wav |  |  |  |  |
| A-b2880-i480_p0_f0.wav |  |  |  |  |
| A-b2880-i480_p4_f2.5.wav |  |  |  |  |
| A-b2880-i480_p-4_f-2.5.wav |  |  |  |  |
| A-b2880-i480_p-9_f-5.wav |  |  |  |  |
| A-b2880-i720_p0_f0.wav |  |  |  |  |
| A-b2880-i720_p4_f2.5.wav |  |  |  |  |
| A-b2880-i720_p-4_f-2.5.wav |  |  |  |  |
| A-b2880-i720_p-9_f-5.wav |  |  |  |  |

