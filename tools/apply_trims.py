"""Writes outputTrimDb from a calibration CSV into resources/presets/*.json (koeloom_presets.md §5.2 step 2).

    KoeLoom.exe --calibrate-presets calibration.csv     # with KOELOOM_REFERENCE_WAV set when available
    python tools/apply_trims.py calibration.csv [--threshold 1.0]

Only presets whose level differs from そのまま by more than the threshold (dB) are touched, so values
measured on the synthetic voice do not churn presets that are already close. The line
`"outputTrimDb": <n>` is replaced in place; the rest of each file is left byte-for-byte unchanged.
"""
import argparse
import csv
import re
import sys
from pathlib import Path

PRESETS = Path(__file__).resolve().parent.parent / "resources" / "presets"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--threshold", type=float, default=1.0)
    args = ap.parse_args()

    changed = 0
    with open(args.csv, encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            diff, trim = float(row["diffDb"]), float(row["suggestedTrimDb"])
            path = PRESETS / f"{row['id']}.json"
            if not path.exists():
                print(f"missing {path.name}", file=sys.stderr)
                continue
            text = path.read_text(encoding="utf-8")
            target = trim if abs(diff) > args.threshold else 0.0
            value = f"{target:g}" if target != 0 else "0"
            new, n = re.subn(r'("outputTrimDb"\s*:\s*)-?[0-9.]+', lambda m: m.group(1) + value, text)
            if n == 0:
                new = text.rstrip().rstrip("}").rstrip() + f',\n  "outputTrimDb": {value}\n}}\n'
            if new != text:
                path.write_text(new, encoding="utf-8", newline="\n")
                changed += 1
                print(f"{row['id']}: {diff:+.2f} dB -> trim {value}")
    print(f"{changed} presets updated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
