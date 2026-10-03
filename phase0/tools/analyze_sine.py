"""T1-4: continuity check of a recorded 440 Hz sine (p0_passthrough --source sine -> p0_capture).

    python analyze_sine.py <capture.wav> [--skip-seconds 0.1] [--events events.csv]
    python analyze_sine.py --self-test

A phase-continuous sine obeys x[n] = 2 cos(w) x[n-1] - x[n-2] exactly, so the residual
e[n] = x[n] - 2 cos(w) x[n-1] + x[n-2] is ~0 except where the waveform breaks (dropped or
repeated samples, inserted zeros, phase jumps). w is fitted to the whole recording by least
squares. A "discontinuity" is |e| > -60 dBFS; hits closer than 50 ms are one place.
A "silence" is |x| < -80 dBFS for 5 ms or longer inside the analysed range.
The analysed range is the recording with signal (10 ms RMS > -40 dBFS), minus --skip-seconds at
each end. A summary row is appended to phase0/results/01-sine-continuity.csv.
"""
import argparse
import os
import struct
import sys
import time

import numpy as np

DISCONT_DB = -60.0
SILENCE_DB = -80.0
SILENCE_MIN_S = 0.005
MERGE_S = 0.050


def read_wav(path):
    """Mono float64 (first channel) and sample rate. PCM 16/24/32-bit and IEEE float 32/64."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    pos, fmt, raw = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, ch, sr, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE and len(body) >= 26:
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, ch, sr, bits)
        elif cid == b"data":
            raw = body
        pos += 8 + size + (size & 1)
    if fmt is None or raw is None:
        raise ValueError("missing fmt or data chunk")
    tag, ch, sr, bits = fmt
    width = bits // 8
    n = len(raw) // (width * ch)
    raw = raw[:n * width * ch]
    if tag == 3:
        x = np.frombuffer(raw, dtype="<f4" if bits == 32 else "<f8").astype(np.float64)
    elif bits == 16:
        x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif bits == 24:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        x = v.astype(np.float64) / float(1 << 23)
    elif bits == 32:
        x = np.frombuffer(raw, dtype="<i4").astype(np.float64) / 2147483648.0
    else:
        raise ValueError("unsupported format tag %d / %d bits" % (tag, bits))
    return x.reshape(-1, ch)[:, 0], sr


def runs(mask):
    """(start, length) of each run of True."""
    if not mask.any():
        return []
    d = np.diff(np.concatenate(([0], mask.astype(np.int8), [0])))
    starts, ends = np.flatnonzero(d == 1), np.flatnonzero(d == -1)
    return list(zip(starts, ends - starts))


def analyse(x, sr, skip_s=0.1):
    hop = int(sr * 0.01)
    frames = len(x) // hop
    if frames < 3:
        raise ValueError("recording too short")
    rms = np.sqrt(np.mean(x[:frames * hop].reshape(frames, hop) ** 2, axis=1) + 1e-30)
    live = np.flatnonzero(20 * np.log10(rms) > -40.0)
    if len(live) == 0:
        raise ValueError("no signal above -40 dBFS")
    a = live[0] * hop + int(skip_s * sr)
    b = (live[-1] + 1) * hop - int(skip_s * sr)
    seg = x[a:b]
    x0, x1, x2 = seg[2:], seg[1:-1], seg[:-2]
    # least squares for c = cos(w); refit without the break points (c ~ 1 makes w very outlier-sensitive)
    use = np.ones(len(x1), dtype=bool)
    for _ in range(3):
        c = np.dot(x1[use], x0[use] + x2[use]) / (2.0 * np.dot(x1[use], x1[use]))
        e = x0 - 2 * c * x1 + x2
        use = np.abs(e) < 10 ** (DISCONT_DB / 20)
    freq = np.arccos(np.clip(c, -1.0, 1.0)) * sr / (2 * np.pi)
    hits = np.flatnonzero(np.abs(e) > 10 ** (DISCONT_DB / 20)) + 2 + a
    places = []
    for h in hits:
        if not places or h - places[-1][-1] > MERGE_S * sr:
            places.append([h, h])
        else:
            places[-1][-1] = h
    sil = [(s + a, n) for s, n in runs(np.abs(seg) < 10 ** (SILENCE_DB / 20)) if n >= SILENCE_MIN_S * sr]
    return {
        "start_s": a / sr, "end_s": b / sr, "analysed_s": (b - a) / sr, "freq_hz": freq,
        "level_dbfs": 20 * np.log10(np.sqrt(np.mean(seg ** 2)) + 1e-30),
        "residual_floor_dbfs": 20 * np.log10(np.sqrt(np.median(e ** 2)) + 1e-30),
        "discontinuities": [(p[0] / sr, p[1] / sr) for p in places],
        "silences": [(s / sr, n / sr) for s, n in sil],
    }


def self_test():
    sr = 48000
    n = np.arange(10 * sr)
    ph = 2 * np.pi * 440.0 * n / sr
    ph[8 * sr:] += 0.3                                    # phase jump at 8.0 s
    x = 0.1 * np.sin(ph)
    x = np.delete(x, 3 * sr)                              # one dropped sample at 3.0 s
    x[6 * sr:6 * sr + int(0.02 * sr)] = 0.0               # 20 ms of zeros at 6.0 s
    x = np.concatenate((np.zeros(sr // 2), x, np.zeros(sr // 2)))
    x = np.round(x * (1 << 23)) / (1 << 23)               # 24-bit
    r = analyse(x, sr)
    assert len(r["discontinuities"]) == 3, r["discontinuities"]
    assert len(r["silences"]) == 1, r["silences"]
    assert abs(r["freq_hz"] - 440.0) < 0.01, r["freq_hz"]
    clean = analyse(np.round(0.1 * np.sin(2 * np.pi * 440.0 * n / sr) * (1 << 23)) / (1 << 23), sr)
    assert not clean["discontinuities"] and not clean["silences"], clean
    print("self-test ok: discontinuities at", [round(float(t), 4) for t, _ in r["discontinuities"]], "s (expected 3.5, 6.5, 8.5),", "residual floor %.1f dBFS" % clean["residual_floor_dbfs"])


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    p = argparse.ArgumentParser()
    p.add_argument("wav", nargs="?")
    p.add_argument("--skip-seconds", type=float, default=0.1)
    p.add_argument("--events")
    p.add_argument("--self-test", action="store_true")
    args = p.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.wav:
        p.error("wav file required")
    x, sr = read_wav(args.wav)
    r = analyse(x, sr, args.skip_seconds)
    print("file %s, %d Hz, analysed %.3f s (%.3f..%.3f s)" % (args.wav, sr, r["analysed_s"], r["start_s"], r["end_s"]))
    print("fitted frequency %.6f Hz, level %.3f dBFS, residual floor %.3f dBFS" % (r["freq_hz"], r["level_dbfs"], r["residual_floor_dbfs"]))
    print("discontinuities (> %.0f dBFS residual): %d" % (DISCONT_DB, len(r["discontinuities"])))
    for t0, t1 in r["discontinuities"][:50]:
        print("  %.6f s .. %.6f s" % (t0, t1))
    print("silences (< %.0f dBFS for >= 5 ms): %d" % (SILENCE_DB, len(r["silences"])))
    for t, d in r["silences"][:50]:
        print("  %.6f s, %.3f ms" % (t, d * 1000))
    if args.events:
        with open(args.events, "w", encoding="utf-8", newline="") as f:
            f.write("kind,start_s,end_or_length_s\n")
            for t0, t1 in r["discontinuities"]:
                f.write("discontinuity,%.6f,%.6f\n" % (t0, t1))
            for t, d in r["silences"]:
                f.write("silence,%.6f,%.6f\n" % (t, d))
    summary = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "results", "01-sine-continuity.csv")
    new = not os.path.exists(summary)
    with open(summary, "a", encoding="utf-8", newline="") as f:
        if new:
            f.write("date,file,analysed_s,freq_hz,level_dbfs,discontinuities,silences\n")
        f.write("%s,%s,%.3f,%.6f,%.3f,%d,%d\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), os.path.basename(args.wav), r["analysed_s"],
                                                 r["freq_hz"], r["level_dbfs"], len(r["discontinuities"]), len(r["silences"])))


if __name__ == "__main__":
    main()
