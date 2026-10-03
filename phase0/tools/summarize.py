"""Print the Markdown tables of results/02-*.csv (T2) so they can be regenerated after re-measuring.

    python summarize.py [--tag name]      (reads results/02-shifters[-tag].csv, 02-ac04[-tag].csv, ...)

Criteria: converter latency budget from spec §5.4 with the measured RNNoise delay (03-rnnoise.md);
CPU A-6 (3 instances, p99 <= 25 % of the buffer, judged at the default buffer 480, A-14/A-17);
level +/-2 dB (T2 instruction) and +/-1 dB (F-02-12); AC-04 +/-10 cents.
"""
import argparse
import csv
import os
import sys
from collections import defaultdict

RES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "results")


def load(name):
    path = os.path.join(RES, name)
    return list(csv.DictReader(open(path, encoding="utf-8"))) if os.path.exists(path) else []


def f(v, d=3):
    return ("%." + str(d) + "f") % float(v) if v not in ("", None) else "-"


def ok(b):
    return "合格" if b else "不合格"


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    p = argparse.ArgumentParser()
    p.add_argument("--tag", default="")
    a = p.parse_args()
    t = "-" + a.tag if a.tag else ""
    rows = load("02-shifters%s.csv" % t)
    params = ["0/0", "4/2.5", "-4/-2.5", "-9/-5"]

    print("### 設定ごとの遅延と CPU（3 インスタンス、ピッチ/フォルマントの 4 組の最悪値）\n")
    print("| 設定 | block | interval | split | 報告遅延 (ms) | 実測遅延 0/0 (ms) | 60 ms 目標（変換 ≤ 20 ms） | 上限 100 ms（変換 ≤ 50 ms） "
          "| CPU 128 p99 / 最大 (%) | CPU 256 p99 / 最大 (%) | CPU 480 p50 / p99 / 最大 (%) | A-6（480 の p99 ≤ 25 %） |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    by = defaultdict(list)
    for r in rows:
        by[r["config"]].append(r)
    for cfg, rs in by.items():
        r0 = [r for r in rs if r["pitch_st"] == "0" and r["formant_st"] == "0"][0]
        cpu = [r for r in rs if r["cpu480_p99"] != ""]
        worst = lambda k: max(float(r[k]) for r in cpu)
        lat = float(r0["reported_latency_ms"])
        print("| %s | %s | %s | %s | %s | %s | %s | %s | %s / %s | %s / %s | %s / %s / %s | %s |" % (
            cfg, r0["block"], r0["interval"], "あり" if r0["split"] == "1" else "なし", f(lat), f(r0["wave_latency_ms"]),
            ok(lat <= 20.0), ok(lat <= 50.0), f(worst("cpu128_p99")), f(worst("cpu128_max")), f(worst("cpu256_p99")), f(worst("cpu256_max")),
            f(worst("cpu480_p50")), f(worst("cpu480_p99")), f(worst("cpu480_max")), ok(worst("cpu480_p99") <= 25.0)))

    print("\n### 行 = 設定 × パラメータ（ピッチ st / フォルマント st）\n")
    print("| 設定 | ピッチ/フォルマント | 実測遅延 (ms、包絡) | 音量差 (dB) | ±2 dB | ±1 dB (F-02-12) | CPU 128 p50/p99/最大 | CPU 256 p50/p99/最大 | CPU 480 p50/p99/最大 | 測定中の PC 全体のビジー率 (%) |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        pf = "%s/%s" % (r["pitch_st"], r["formant_st"])
        d = float(r["level_diff_db"])
        cpu = lambda b: "%s / %s / %s" % (f(r["cpu%s_p50" % b]), f(r["cpu%s_p99" % b]), f(r["cpu%s_max" % b])) if r["cpu480_p99"] else "-"
        print("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (r["config"], pf, r["env_latency_ms"], f(d), ok(abs(d) <= 2), ok(abs(d) <= 1),
                                                                    cpu("128"), cpu("256"), cpu("480"), r["system_busy_pct"] if r["cpu480_p99"] else "-"))

    ac = load("02-ac04%s.csv" % t)
    if ac:
        print("\n### AC-04（220 Hz の正弦波）\n")
        print("| 設定 | +12 st → 440 Hz（セント、YIN / ゼロ交差） | -12 st → 110 Hz（参考） | フォルマントだけ -6 / -3 / +3 / +6 st → 220 Hz（セント、YIN） | AC-04 |")
        print("|---|---|---|---|---|")
        byc = defaultdict(dict)
        for r in ac:
            byc[r["config"]][r["test"]] = r
        for cfg, ts in byc.items():
            up, dn = ts["pitch+12"], ts["pitch-12"]
            fm = [ts[k] for k in ("formant-6", "formant-3", "formant+3", "formant+6")]
            passed = abs(float(up["yin_median_cents"])) <= 10 and all(abs(float(x["yin_median_cents"])) <= 10 for x in fm) and all(x["finite"] == "yes" for x in fm + [up])
            print("| %s | %s / %s | %s | %s | %s |" % (cfg, f(up["yin_median_cents"]), f(up["zero_cross_cents"]), f(dn["yin_median_cents"]),
                                                     " / ".join(f(x["yin_median_cents"]) for x in fm), ok(passed)))

    for tag in ("",):
        sw = [r for r in load("02-f0-sweep.csv") if "wave" in r and float(r["expected_hz"]) >= 65.0]
        if not sw:
            continue
        print("\n### ピッチ変換の正確さ（正弦波と倍音つき、入力 100〜400 Hz × ±4 / ±12 st。出力が 65 Hz 未満の組は除く）\n")
        print("| 設定 | 波形 | フォルマント補正 | 組の数 | ±10 セント以内 | 誤差の中央値 (セント) | 最大 (セント) |")
        print("|---|---|---|---|---|---|---|")
        g = defaultdict(list)
        for r in sw:
            g[(r["config"], r["wave"], r["compensate_pitch"])].append(abs(float(r["yin_cents"])))
        for (cfg, wave, comp), v in g.items():
            v.sort()
            print("| %s | %s | %s | %d | %d | %s | %s |" % (cfg, "正弦波" if wave == "sine" else "倍音つき（1〜5 次）", "あり（製品の使い方）" if comp == "1" else "なし（参考）",
                                                     len(v), sum(x <= 10 for x in v), f(v[len(v) // 2]), f(v[-1])))

    sc = load("02-level-scan%s.csv" % t)
    if sc:
        print("\n### 音量差の全範囲（ピッチ -12〜+12 × フォルマント -6〜+6、55 組）\n")
        print("| 設定 | フォルマント補正 | 最小 (dB) | 最大 (dB) | ±1 dB 以内 | ±2 dB 以内 | ピッチだけ（フォルマント 0）の範囲 (dB) | フォルマントだけ（ピッチ 0）の範囲 (dB) |")
        print("|---|---|---|---|---|---|---|---|")
        g = defaultdict(list)
        for r in sc:
            g[(r["config"], r["compensate_pitch"])].append(r)
        for (cfg, comp), rs in g.items():
            d = [float(r["level_diff_db"]) for r in rs]
            po = [float(r["level_diff_db"]) for r in rs if r["formant_st"] == "0" and r["pitch_st"] != "0"]
            fo = [float(r["level_diff_db"]) for r in rs if r["pitch_st"] == "0" and r["formant_st"] != "0"]
            print("| %s | %s | %s | %s | %d/%d | %d/%d | %s〜%s | %s〜%s |" % (cfg, "あり" if comp == "1" else "なし（参考）", f(min(d)), f(max(d)),
                                                                     sum(abs(x) <= 1 for x in d), len(d), sum(abs(x) <= 2 for x in d), len(d),
                                                                     f(min(po)), f(max(po)), f(min(fo)), f(max(fo))))


if __name__ == "__main__":
    main()
