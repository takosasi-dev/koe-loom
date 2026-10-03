#pragma once

// Design tokens (F-14-1, §8.4). Every colour, spacing, radius, font size and motion duration used by
// the UI comes from here; screen code must not hard-code these values (AC-53 searches for that).
// Values come from the approved mock (案 A "Studio", docs/mockups/Tokens.dc.html). Light theme
// (Phase 4) follows the same roles.

#include <juce_gui_basics/juce_gui_basics.h>

namespace koe::ui
{
struct Palette
{
    juce::Colour bg, surface, raised, border, divider, text, textSub, accent, onAccent, ok, warn, danger, trackOff, overlay, onDanger;
};

struct Theme
{
    // ---- colour roles ----
    static const Palette& colours();          // current theme
    static bool isDark();
    static void setDark (bool dark);          // call before (re)building the UI
    static const Palette& dark();             // the approved mock's palettes (accent 0, tone 0)
    static const Palette& light();

    // ---- S-03 外観: accent colour and background tone (index 0 = the mock's colours) ----
    static constexpr int numAccents = 6, numTones = 3;
    static void setVariant (int accent, int tone); // call before (re)building the UI, like setDark
    static int accent();
    static int tone();
    static Palette make (bool dark, int accent, int tone);
    static juce::Colour accentColour (int accent, bool dark);
    static juce::String accentName (int accent);
    static juce::String toneName (int tone, bool dark);

    // ---- spacing (8 px grid, 4 for fine adjustment) ----
    static constexpr int space1 = 4, space2 = 8, space3 = 16, space4 = 24, space5 = 32;
    // ---- radii ----
    static constexpr float radiusS = 4.0f, radiusM = 8.0f, radiusL = 12.0f;
    // ---- type scale ----
    static constexpr float fontXS = 12.0f, fontS = 14.0f, fontM = 16.0f, fontL = 20.0f, fontXL = 28.0f;
    // ---- motion (ms), decelerating easing ----
    static constexpr int motionFast = 120, motionMid = 200, motionSlow = 300;
    // ---- control sizes ----
    static constexpr int controlH = 40, buttonH = 36, pillH = 44, touchMin = 32, headerH = 52, bannerH = 36;
    static constexpr int knobBig = 112, knobSmall = 40, toggleW = 40, toggleH = 22;
    static constexpr int slotW = 196, slotWNarrow = 168;
    static constexpr int narrowWidth = 1000;  // below this, §8.2.1 compact layout
    static constexpr int minWidth = 800, minHeight = 560, defaultWidth = 1120, defaultHeight = 720;
    static constexpr float focusRingWidth = 2.0f, borderWidth = 1.5f;

    // ---- fonts (F-14-8): Yu Gothic UI -> Meiryo UI -> default; numbers in Consolas ----
    static juce::Font ui (float size, bool bold = false);
    static juce::Font mono (float size, bool bold = false);

    /** Windows "animation effects" setting (F-14-6). When false, transitions are instant. */
    static bool animationsEnabled();
    /** Decelerating easing for t in 0..1. */
    static float ease (float t) noexcept { return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }
};

/** WCAG contrast ratio of two colours (AC-53 test). */
double contrastRatio (juce::Colour a, juce::Colour b);
} // namespace koe::ui
