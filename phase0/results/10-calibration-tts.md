# プリセットの音量の校正（TTS 代用）

測定日: 2026-10-03。**入力は TTS の読み上げ（Windows の「Microsoft Haruka Desktop」）で、依頼者本人の声ではない**。基準音声（`testdata/reference-speech.wav`）が録れるまでの代用で、`outputTrimDb` は引き続き暫定。試聴もまだ。

- 入力: `testdata/tts-speech.wav`（`python tools\make_tts_speech.py` で作る。48 kHz・モノラル・16 bit・10 秒・無音は最長 0.25 秒・ピーク -6.00 dBFS、RMS -21.41 dBFS）
- 手順: `KOELOOM_REFERENCE_WAV` をこのファイルに向けて `KoeLoom.exe --calibrate-presets` → `python tools\apply_trims.py 10-calibration-tts.csv`（差が 1 dB 以下のものは 0）
- CSV: `10-calibration-tts.csv`（今回、`usesReference` = 1）、`10-calibration-synth.csv`（同じ exe で合成音声 `synthVoice` のまま測ったもの。前回の値をそのまま再現する）

## 結果

61 本のうち **22 本の `outputTrimDb` が変わった**。差（TTS − 合成音声）の平均は 0.37 dB。大きく動いたのは帯域を狭める機器の音（電話・無線・メガホン・AM ラジオ）で、TTS のほうが 1.3〜1.9 dB 余分に下がる。合成音声は 4 kHz までの倍音しか持たないので、帯域を切っても失うエネルギーが少なかった。

| プリセット | 差（合成音声） | 差（TTS） | 前の trim | 新しい trim |
|---|---|---|---|---|
| device-telephone | -4.29 | -5.97 | 4.5 | 6 |
| device-military-radio | -4.74 | -6.64 | 4.5 | 6（上限。残り -0.64 dB） |
| device-megaphone | -3.38 | -5.01 | 3.5 | 5 |
| device-am-radio | -1.68 | -2.96 | 1.5 | 3 |
| natural-ikebo | -0.93 | -1.26 | 0 | 1.5 |
| character-giant | 0.90 | 1.54 | 0 | -1.5 |
| natural-announcer | -1.54 | -2.39 | 1.5 | 2.5 |
| character-masked-villain | -3.15 | -3.80 | 3 | 4 |
| character-crowd | -2.40 | -1.74 | 2.5 | 1.5 |
| layered-harmony-fifth | 1.08 | 0.91 | -1 | 0 |
| natural-slightly-low | -0.99 | -1.23 | 0 | 1 |
| device-vowel-filter | -0.84 | -1.08 | 0 | 1 |

ほか 0.5 dB だけ動いたもの 10 本: natural-slightly-high、natural-radio-dj、natural-doubling、layered-phaser-voice、layered-choir-3、device-wah-voice、device-broken-speaker、character-robot、character-ghost、character-fairy。

## 「そのまま」との差の残り（AC-10: ±2 dB）

| trim | 入力 | 最大の差 | ±2 dB を超える本数 |
|---|---|---|---|
| 前（合成音声で校正） | 合成音声 | -0.99（natural-slightly-low） | 0 |
| 前（合成音声で校正） | TTS | -2.14（device-military-radio） | 1 |
| **新（TTS で校正）** | TTS | 0.98（character-vocoder-robot） | 0 |
| **新（TTS で校正）** | 合成音声 | +1.71（device-telephone） | 0 |

全テスト（5197 件）は、`KOELOOM_REFERENCE_WAV` を TTS に向けても、外して合成音声にしても全部成功。ただし合成音声のときの機器の音は +1.6〜1.7 dB と ±2 dB の端に近い。帯域を狭めるプリセットは入力の声で 2 dB 近く変わるので、本人の声で校正し直したら同じように動く見込み。
