"""Renders a TTS stand-in for the reference speech: phase0/testdata/tts-speech.wav.

    python tools/make_tts_speech.py [--out phase0/testdata/tts-speech.wav]

Windows' Japanese voice "Microsoft Haruka Desktop" (System.Speech) reads the text below into a WAV
file (SetOutputToWaveFile: nothing is played), then this script brings it to the reference-speech
conditions of phase0/testdata/README.md: 48 kHz mono 16 bit, 10 s, no silence over 0.5 s (pauses
are shortened to 0.25 s), peak -6 dBFS.

This is NOT the reference speech. reference-speech.wav is reserved for the user's own recording;
point KOELOOM_REFERENCE_WAV / --input at this file only as a stand-in until that exists.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
SR = 48000
SECONDS = 10.0
PEAK_DBFS = -6.0
SILENCE_DB = -40.0      # 10 ms frames this far below the loudest frame count as silence
MAX_PAUSE_S = 0.25      # longer pauses are cut down to this (the condition is <= 0.5 s)

# Original text (written for this file). Long enough that 10 s remain after shortening the pauses.
TEXT = (
    "おはようございます。今日は朝から少し曇っていますが、午後には晴れるそうです。"
    "駅までの道を歩きながら、新しいマイクの置き場所を考えていました。"
    "机の右側に寄せると、キーボードの音があまり入りません。"
    "夕方になったら、友達と通話をしながら、ゆっくりゲームをするつもりです。"
)

PS_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
$s.SelectVoice('Microsoft Haruka Desktop')
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(48000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$s.SetOutputToWaveFile($env:TTS_OUT, $fmt)
$s.Speak([System.IO.File]::ReadAllText($env:TTS_TEXT, [System.Text.Encoding]::UTF8))
$s.Dispose()
"""


def render(text: str, wav: Path) -> np.ndarray:
    txt = wav.with_suffix(".txt")
    txt.write_text(text, encoding="utf-8")
    env = dict(os.environ, TTS_OUT=str(wav), TTS_TEXT=str(txt))
    subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", PS_SCRIPT], env=env, check=True)
    with wave.open(str(wav), "rb") as w:
        assert (w.getframerate(), w.getnchannels(), w.getsampwidth()) == (SR, 1, 2), "unexpected TTS format"
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64) / 32768.0


def shorten_pauses(x: np.ndarray) -> np.ndarray:
    frame = SR // 100
    n = len(x) // frame
    rms = np.sqrt(np.mean(x[: n * frame].reshape(n, frame) ** 2, axis=1) + 1e-20)
    loud = 20 * np.log10(rms) > 20 * np.log10(rms.max()) + SILENCE_DB
    if not loud.any():
        raise SystemExit("TTS output is silent")
    keep_frames = int(MAX_PAUSE_S * 100)
    first, last = np.argmax(loud), n - 1 - np.argmax(loud[::-1])
    out, run = [], 0
    for i in range(first, last + 1):
        run = 0 if loud[i] else run + 1
        if run <= keep_frames:  # inside a pause keep only its first MAX_PAUSE_S
            out.append(x[i * frame:(i + 1) * frame])
    return np.concatenate(out)


def longest_silence_s(x: np.ndarray) -> float:
    frame = SR // 100
    n = len(x) // frame
    rms = np.sqrt(np.mean(x[: n * frame].reshape(n, frame) ** 2, axis=1) + 1e-20)
    quiet = 20 * np.log10(rms) <= 20 * np.log10(rms.max()) + SILENCE_DB
    best = run = 0
    for q in quiet:
        run = run + 1 if q else 0
        best = max(best, run)
    return best / 100


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "phase0" / "testdata" / "tts-speech.wav"))
    args = ap.parse_args()
    out = Path(args.out)
    if out.name == "reference-speech.wav":
        raise SystemExit("reference-speech.wav is reserved for the user's own recording")

    with tempfile.TemporaryDirectory() as tmp:
        x = shorten_pauses(render(TEXT, Path(tmp) / "tts.wav"))
    n = int(SECONDS * SR)
    if len(x) < n:
        raise SystemExit(f"only {len(x) / SR:.2f} s of speech after shortening pauses; lengthen TEXT")
    x = x[:n].copy()
    fade = SR // 50  # 20 ms fade-out at the cut
    x[-fade:] *= np.linspace(1.0, 0.0, fade)
    x *= 10 ** (PEAK_DBFS / 20) / np.abs(x).max()

    out.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(out), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(np.round(x * 32767).astype("<i2").tobytes())

    sil = longest_silence_s(x)
    peak = 20 * np.log10(np.abs(x).max())
    rms = 20 * np.log10(np.sqrt(np.mean(x ** 2)))
    print(f"{out}: {len(x) / SR:.2f} s, peak {peak:.2f} dBFS, rms {rms:.2f} dBFS, longest silence {sil:.2f} s")
    assert sil <= 0.5 and abs(peak - PEAK_DBFS) < 0.1
    return 0


if __name__ == "__main__":
    sys.exit(main())
