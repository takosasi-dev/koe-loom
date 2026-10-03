# testdata

`reference-speech.wav` を、ここに置きます。本人の声なので、リポジトリには入れません（`.gitignore` で除外済み）。

| 項目 | 条件 |
|---|---|
| 形式 | 48 kHz、モノラル、16 bit 以上の WAV |
| 長さ | 10 秒 |
| 内容 | 日本語の文章の読み上げ。途中に 0.5 秒を超える無音を含まない |
| レベル | ピークが -6 dBFS 前後 |
| 権利 | 本人が収録した音声だけを使う。他人の声や既存の音源は使わない |

録音までの代用として `tts-speech.wav`（Windows の TTS「Microsoft Haruka Desktop」の読み上げ、同じ条件にそろえたもの。基準音声ではない）を `python tools\make_tts_speech.py` で作り直せます（音はファイルに書くだけで鳴らさない）。使うときは `KOELOOM_REFERENCE_WAV` や `--input` でこのファイルを指します。
