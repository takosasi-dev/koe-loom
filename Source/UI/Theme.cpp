#include "UI/Theme.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
#endif

#include <cmath>

namespace koe::ui
{
namespace
{
juce::Colour c (juce::uint32 rgb) { return juce::Colour (0xff000000u | rgb); }

bool darkMode = true;
int accentChoice = 0, toneChoice = 0;

juce::String pickFace (std::initializer_list<const char*> candidates)
{
    static const juce::StringArray installed = juce::Font::findAllTypefaceNames();
    for (auto* n : candidates)
        if (installed.contains (juce::String::fromUTF8 (n))) return juce::String::fromUTF8 (n);
    return juce::Font::getDefaultSansSerifFontName();
}

// ---- S-03 外観 choices. Index 0 is the approved mock's colour (案 A); the light value of シアン stays the
// mock's deep blue. Each accent passes 4.5:1 as text on bg / surface / raised of every tone (UiMain test).
struct AccentDef { const char* name; juce::uint32 dark, light; };
constexpr AccentDef kAccents[Theme::numAccents] = {
    { "シアン", 0x4CC9F0, 0x2A4DA0 }, { "ブルー", 0x6EA8FE, 0x1F5BD6 },   { "バイオレット", 0xB794F6, 0x6A3FC1 },
    { "グリーン", 0x3DD68C, 0x17744F }, { "オレンジ", 0xFF9F43, 0xA94C00 }, { "ピンク", 0xF472B6, 0xB0306A },
};

// Background tones: 0 = the mock's palette as is; the others derive every grey from one base colour with the
// mock's own proportions (mix towards the text colour, or white for the light theme's raised surfaces).
struct ToneDef { const char* name; juce::uint32 base; };
constexpr ToneDef kDarkTones[Theme::numTones] = { { "標準", 0x0E1217 }, { "ブラック", 0x060708 }, { "ネイビー", 0x0B1428 } };
constexpr ToneDef kLightTones[Theme::numTones] = { { "標準", 0xF3EFE6 }, { "ホワイト", 0xF6F6F7 }, { "クール", 0xEAEEF3 } };

/** Ink on an accent fill: whichever of the two keeps more contrast (the mock's pair for the defaults). */
juce::Colour inkFor (juce::Colour fill)
{
    const auto darkInk = c (0x04141B), lightInk = c (0xFFFFFF);
    return contrastRatio (darkInk, fill) >= contrastRatio (lightInk, fill) ? darkInk : lightInk;
}

Palette derive (const Palette& mock, bool dark, int tone)
{
    Palette p = mock;
    if (tone == 0) return p;
    const auto base = c ((dark ? kDarkTones : kLightTones)[tone].base);
    const auto towardsText = [&] (float k) { return base.interpolatedWith (p.text, k); };
    p.bg = base;
    if (dark)
    {
        p.surface = towardsText (0.037f);
        p.raised = towardsText (0.075f);
    }
    else
    {
        p.surface = base.interpolatedWith (c (0xFFFFFF), 0.54f);
        p.raised = c (0xFFFFFF);
    }
    p.divider = towardsText (dark ? 0.124f : 0.093f);
    p.trackOff = towardsText (dark ? 0.136f : 0.131f);
    p.border = towardsText (dark ? 0.474f : 0.506f);
    p.textSub = towardsText (dark ? 0.717f : 0.742f);
    return p;
}

Palette current = Theme::dark();
void rebuild() { current = Theme::make (darkMode, accentChoice, toneChoice); }
} // namespace

const Palette& Theme::dark()
{
    static const Palette p { c (0x0E1217), c (0x151A21), c (0x1C222B), c (0x6E7A8C), c (0x262D37), c (0xEDF1F6), c (0xA7B1BF),
                             c (0x4CC9F0), c (0x04141B), c (0x4ADE80), c (0xF4B740), c (0xFF6B6B), c (0x27303B),
                             juce::Colour (0xb3000000), c (0x04141B) };
    return p;
}

const Palette& Theme::light()
{
    static const Palette p { c (0xF3EFE6), c (0xFAF8F2), c (0xFFFFFF), c (0x8A8478), c (0xE2DCCF), c (0x1B1A17), c (0x55514A),
                             c (0x2A4DA0), c (0xFFFFFF), c (0x1B7A3B), c (0x8F5B00), c (0xB3261E), c (0xDAD4C7),
                             juce::Colour (0x80000000), c (0xFFFFFF) };
    return p;
}

Palette Theme::make (bool dark, int accent, int tone)
{
    accent = juce::jlimit (0, numAccents - 1, accent);
    tone = juce::jlimit (0, numTones - 1, tone);
    auto p = derive (dark ? Theme::dark() : Theme::light(), dark, tone);
    p.accent = accentColour (accent, dark);
    p.onAccent = inkFor (p.accent);
    return p;
}

juce::Colour Theme::accentColour (int accent, bool dark)
{
    const auto& a = kAccents[juce::jlimit (0, numAccents - 1, accent)];
    return c (dark ? a.dark : a.light);
}

juce::String Theme::accentName (int accent) { return juce::String::fromUTF8 (kAccents[juce::jlimit (0, numAccents - 1, accent)].name); }
juce::String Theme::toneName (int tone, bool dark)
{
    return juce::String::fromUTF8 ((dark ? kDarkTones : kLightTones)[juce::jlimit (0, numTones - 1, tone)].name);
}

const Palette& Theme::colours() { return current; }
bool Theme::isDark() { return darkMode; }
void Theme::setDark (bool d) { darkMode = d; rebuild(); }
void Theme::setVariant (int accent, int tone)
{
    accentChoice = juce::jlimit (0, numAccents - 1, accent);
    toneChoice = juce::jlimit (0, numTones - 1, tone);
    rebuild();
}
int Theme::accent() { return accentChoice; }
int Theme::tone() { return toneChoice; }

juce::Font Theme::ui (float size, bool bold)
{
    // the type scale is the mock's CSS font-size (the em size). A JUCE height is ascent + descent, which for
    // Yu Gothic UI is about 1.34 em, so a plain height drew every label at three quarters of the mock's size.
    static const juce::String face = pickFace ({ "Yu Gothic UI", "Meiryo UI" });
    return juce::Font (juce::FontOptions (face, size, bold ? juce::Font::bold : juce::Font::plain).withPointHeight (size));
}

juce::Font Theme::mono (float size, bool bold)
{
    static const juce::String face = pickFace ({ "Consolas", "Cascadia Mono" });
    return juce::Font (juce::FontOptions (face, size, bold ? juce::Font::bold : juce::Font::plain));
}

bool Theme::animationsEnabled()
{
#if JUCE_WINDOWS
    BOOL on = TRUE;
    if (SystemParametersInfoW (SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) return on != FALSE;
#endif
    return true;
}

double contrastRatio (juce::Colour a, juce::Colour b)
{
    auto lum = [] (juce::Colour x)
    {
        auto ch = [] (juce::uint8 v)
        {
            const double s = v / 255.0;
            return s <= 0.03928 ? s / 12.92 : std::pow ((s + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * ch (x.getRed()) + 0.7152 * ch (x.getGreen()) + 0.0722 * ch (x.getBlue());
    };
    const double la = lum (a), lb = lum (b);
    return (std::max (la, lb) + 0.05) / (std::min (la, lb) + 0.05);
}
} // namespace koe::ui
