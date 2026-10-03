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

/** Every grey of p from one background colour (and p.text), in the mock's proportions. */
Palette deriveFrom (Palette p, juce::Colour base, bool dark)
{
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

Palette derive (const Palette& mock, bool dark, int tone)
{
    if (tone == 0) return mock;
    return deriveFrom (mock, c ((dark ? kDarkTones : kLightTones)[tone].base), dark);
}

// a built-in or user theme replacing the Studio palette (S-03 外観 「配色」)
struct Custom { Palette palette; bool dark; };
std::optional<Custom> custom;
juce::String appliedId; // Settings::themeId of what is applied ("" = Studio)

Palette current = Theme::dark();
void rebuild() { current = custom ? custom->palette : Theme::make (darkMode, accentChoice, toneChoice); }

constexpr juce::Colour Palette::*kRoles[Theme::numRoles] = {
    &Palette::bg,     &Palette::surface,  &Palette::raised, &Palette::border, &Palette::divider,
    &Palette::text,   &Palette::textSub,  &Palette::accent, &Palette::onAccent, &Palette::ok,
    &Palette::warn,   &Palette::danger,   &Palette::trackOff, &Palette::overlay, &Palette::onDanger,
};
constexpr const char* kRoleKeys[Theme::numRoles] = { "bg",   "surface", "raised", "border", "divider", "text",    "textSub", "accent",
                                                     "onAccent", "ok", "warn",   "danger", "trackOff", "overlay", "onDanger" };
constexpr const char* kRoleNames[Theme::numRoles] = { "背景", "カードの面", "浮いた面", "枠線・つまみの溝", "区切り線", "文字", "補足の文字",
                                                      "アクセント", "アクセントの上の文字", "正常（OK）", "注意", "危険", "OFF の溝",
                                                      "パネルの後ろの影", "危険の上の文字" };
enum RoleIndex { rBg, rSurface, rRaised, rBorder, rDivider, rText, rTextSub, rAccent, rOnAccent, rOk, rWarn, rDanger, rTrackOff, rOverlay, rOnDanger };
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

// 案 B Paper (docs/mockups/B-S01.dc.html): the mock's paper, card and white surfaces, ink, navy accent, 1 px
// hairline borders (#7C7566) and dividers (#D9D3C5), meter tracks #E2DDCF; danger is the light theme's.
const Palette& Theme::paper()
{
    static const Palette p { c (0xF3EFE6), c (0xFAF8F2), c (0xFFFFFF), c (0x7C7566), c (0xD9D3C5), c (0x1B1A17), c (0x55514A),
                             c (0x2A4DA0), c (0xFFFFFF), c (0x1B7A3B), c (0x8F5B00), c (0xB3261E), c (0xE2DDCF),
                             juce::Colour (0x80000000), c (0xFFFFFF) };
    return p;
}

// 案 C Mono (docs/mockups/C-S01.dc.html): near black, white text, the off-white #F4F4F6 as the accent / active
// fill (dark ink on it), grey lines; status colours stay coloured (green, amber, the dark theme's red).
const Palette& Theme::mono()
{
    static const Palette p { c (0x0A0A0B), c (0x121214), c (0x1A1A1D), c (0x6A6A76), c (0x2A2A2F), c (0xFFFFFF), c (0xB5B5C0),
                             c (0xF4F4F6), c (0x0A0A0B), c (0x5BE585), c (0xFFB020), c (0xFF6B6B), c (0x25252A),
                             juce::Colour (0xb3000000), c (0x0A0A0B) };
    return p;
}

void Theme::setPalette (const juce::String& id, const Palette& p, bool dark)
{
    custom = Custom { p, dark };
    appliedId = id;
    rebuild();
}

void Theme::clearPalette()
{
    custom.reset();
    appliedId = {};
    rebuild();
}

juce::String Theme::themeId() { return appliedId; }
void Theme::invalidate() { appliedId = "\n"; } // no Settings::themeId has a line break

const char* Theme::roleKey (int r) { return kRoleKeys[juce::jlimit (0, numRoles - 1, r)]; }
juce::String Theme::roleName (int r) { return juce::String::fromUTF8 (kRoleNames[juce::jlimit (0, numRoles - 1, r)]); }
juce::Colour& Theme::role (Palette& p, int r) { return p.*kRoles[juce::jlimit (0, numRoles - 1, r)]; }
juce::Colour Theme::role (const Palette& p, int r) { return p.*kRoles[juce::jlimit (0, numRoles - 1, r)]; }

Palette Theme::complete (const std::array<std::optional<juce::Colour>, numRoles>& given, bool dark)
{
    const auto& mock = dark ? Theme::dark() : Theme::light();
    Palette p = mock;
    if (given[rText]) p.text = *given[rText];
    if (given[rBg] || given[rText]) p = deriveFrom (p, given[rBg].value_or (mock.bg), dark);
    for (int r = 0; r < numRoles; ++r)
        if (given[size_t (r)]) role (p, r) = *given[size_t (r)];
    if (! given[rOnAccent]) p.onAccent = inkFor (p.accent);
    if (! given[rOnDanger]) p.onDanger = inkFor (p.danger);
    return p;
}

std::optional<juce::Colour> Theme::parseHex (const juce::String& text)
{
    auto t = text.trim();
    if (t.startsWithChar ('#')) t = t.substring (1);
    if ((t.length() != 6 && t.length() != 8) || ! t.containsOnly ("0123456789abcdefABCDEF")) return std::nullopt;
    const auto v = juce::uint32 (t.getHexValue64());
    return t.length() == 6 ? c (v) : juce::Colour (v);
}

juce::String Theme::toHex (juce::Colour col)
{
    return "#" + (col.isOpaque() ? juce::String::toHexString (int (col.getARGB() & 0xffffffu)).paddedLeft ('0', 6)
                                 : juce::String::toHexString (int (col.getARGB())).paddedLeft ('0', 8)).toUpperCase();
}

std::vector<Theme::ContrastCheck> Theme::contrastChecks (const Palette& p)
{
    std::vector<ContrastCheck> out;
    auto add = [&] (int fg, int bg, double min) { out.push_back ({ fg, bg, min, contrastRatio (role (p, fg), role (p, bg)) }); };
    for (int bg : { rBg, rSurface, rRaised })
    {
        for (int fg : { rText, rTextSub, rAccent, rOk, rWarn, rDanger }) add (fg, bg, 4.5);
        add (rBorder, bg, 3.0);
    }
    add (rOnAccent, rAccent, 4.5);
    add (rOnDanger, rDanger, 4.5);
    return out;
}

const Palette& Theme::colours() { return current; }
bool Theme::isDark() { return custom ? custom->dark : darkMode; }
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

Theme::Prefs& Theme::prefs()
{
    static Prefs p;
    return p;
}

bool Theme::animationsEnabled()
{
    if (prefs().animations != 0) return prefs().animations == 1;
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
