#!/usr/bin/env python3
"""AC-53: hard-coded colours in the screen code (Source/UI, Theme.* excluded).

    python tools/check_tokens.py              # list each hit, print the count, exit 1 if any
    python tools/check_tokens.py --self-test

Colours must come from Theme::colours() (F-14-1). Colours::transparentBlack/White mean "no fill",
not a colour, so they are allowed. Text after // is ignored (comments may name the rule).
"""
import pathlib
import re
import sys

UI_DIR = pathlib.Path(__file__).resolve().parent.parent / "Source" / "UI"
ALLOWED_NAMED = {"transparentBlack", "transparentWhite"}
PATTERNS = [
    re.compile(r"\bColour\s*\(\s*(?:0x[0-9a-fA-F]+|\d)"),            # juce::Colour (0xff...), Colour (r, g, b)
    re.compile(r"\bColour::from(?:RGB|RGBA|FloatRGBA|HSV|HSL|String)\b"),
    re.compile(r"\bColours::(\w+)"),                                  # named colours
    re.compile(r"\b0x[fF]{2}[0-9a-fA-F]{6}\b"),                       # ARGB literal anywhere
]


def findings(line):
    code = line.split("//", 1)[0]
    found = []
    for pattern in PATTERNS:
        for m in pattern.finditer(code):
            if m.groups() and m.group(1) in ALLOWED_NAMED:
                continue
            found.append(m.group(0))
    return found


def scan():
    hits = []
    for path in sorted(UI_DIR.rglob("*")):
        if path.suffix not in (".h", ".cpp") or path.stem == "Theme":
            continue
        for no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if findings(line):
                hits.append((path.relative_to(UI_DIR.parent.parent), no, line.strip()))
    return hits


def self_test():
    assert findings("g.setColour (juce::Colours::red);")
    assert findings("auto c = juce::Colour (0xff102030);")
    assert findings("juce::Colour (12, 34, 56)")
    assert findings("juce::Colour::fromRGB (1, 2, 3)")
    assert not findings("juce::Colours::transparentBlack")
    assert not findings("g.setColour (p.accent); // never Colours::red")
    assert not findings("g.setColour (Theme::colours().text);")
    print("self-test ok")


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        self_test()
        sys.exit(0)
    result = scan()
    for path, no, line in result:
        print(f"{path}:{no}: {line}")
    print(f"hard-coded colours in Source/UI (Theme.* excluded): {len(result)}")
    sys.exit(1 if result else 0)
