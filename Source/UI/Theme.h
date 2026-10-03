#pragma once

// Design tokens (F-14-1, §8.4). Every colour, spacing, radius, font size and motion duration used by
// the UI comes from here; screen code must not hard-code these values (AC-53 searches for that).
// Values come from the approved mock (案 A "Studio", docs/mockups/Tokens.dc.html). Light theme
// (Phase 4) follows the same roles.

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <optional>
#include <vector>

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

    // ---- S-03 外観 「配色」 (Settings::themeId, INTERFACES.md §8.4). "" = the Studio palette above ----
    static const Palette& paper();            // 案 B: warm paper, navy accent, hairlines (docs/mockups/B-S01.dc.html)
    static const Palette& mono();             // 案 C: near black, white as the accent fill (docs/mockups/C-S01.dc.html)
    /** Uses p (a built-in or user theme) instead of the Studio palette until clearPalette(); call before (re)building
        the UI. id is the Settings::themeId it came from; isDark() then answers the theme's dark flag. */
    static void setPalette (const juce::String& id, const Palette& p, bool dark);
    static void clearPalette();
    /** The applied Settings::themeId ("" while Studio). */
    static juce::String themeId();
    /** A saved theme file changed: the next MainComponent::applyTheme rebuilds even though the id is the same. */
    static void invalidate();

    // ---- palette roles by name (theme files and the theme editor) ----
    static constexpr int numRoles = 15;
    static const char* roleKey (int role);    // the Palette member's name: "bg", "surface", ...
    static juce::String roleName (int role);  // Japanese, for the editor
    static juce::Colour& role (Palette& p, int role);
    static juce::Colour role (const Palette& p, int role);
    /** A full palette from the given roles. Missing ones are derived from bg / text / accent the way the background
        tones are (the mock's proportions); none given = the mock's palette for dark / light. */
    static Palette complete (const std::array<std::optional<juce::Colour>, numRoles>& given, bool dark);
    /** "#RRGGBB" or "#AARRGGBB" (the leading # optional). */
    static std::optional<juce::Colour> parseHex (const juce::String& text);
    static juce::String toHex (juce::Colour c); // "#RRGGBB", or "#AARRGGBB" when not opaque

    /** The project's contrast rules (AC-53): text, sub text, accent and status colours 4.5:1 and borders 3:1 on
        bg / surface / raised; text on accent and on danger 4.5:1. Used by the tests and the editor's table. */
    struct ContrastCheck { int fg, bg; double min, ratio; };
    static std::vector<ContrastCheck> contrastChecks (const Palette& p);

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

    /** Screen settings from S-03 外観 (Settings::animations, meterFps, ...), applied by MainComponent on change.
        The defaults are the behaviour before these settings existed. */
    struct Prefs
    {
        int animations = 0;          // 0 Windows に従う / 1 オン / 2 オフ
        int meterFps = 30;           // F-08-1
        float peakHoldMs = 1500.0f;  // LevelMeter peak hold
        int knobSensitivity = 1;     // 0 ゆっくり / 1 標準 / 2 速い
        bool knobWheel = true;
    };
    static Prefs& prefs();

    /** Windows "animation effects" setting (F-14-6), unless S-03 外観 「画面の動き」 forces on / off. */
    static bool animationsEnabled();
    /** Decelerating easing for t in 0..1. */
    static float ease (float t) noexcept { return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }
};

/** WCAG contrast ratio of two colours (AC-53 test). */
double contrastRatio (juce::Colour a, juce::Colour b);
} // namespace koe::ui
