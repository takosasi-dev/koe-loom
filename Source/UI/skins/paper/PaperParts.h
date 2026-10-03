#pragma once

// Small pieces of the 案 B "Paper" look (docs/mockups/B-S01.dc.html), owner wave5/paper: a segmented level meter,
// the flat slider (pitch / formant with a thumb, effect parameters as a bar), and the underlined text link.
// Colours come from Theme::colours() roles only, so the page also reads in the Studio dark / light palettes.

#include "UI/main/Common.h"

namespace koe::ui::paper
{
/** Horizontal meter in segments (-60..0 dB, warn above -6), 1.5 s peak hold and clip marker like LevelMeter,
    optionally with the -48 / -24 / -12 / -6 / 0 scale under it. */
class SegmentMeter : public juce::Component
{
public:
    void setLevel (float peakDb, bool clipped);
    void setShowScale (bool s) { showScale = s; repaint(); }
    bool isClipping() const { return clipHold > 0; }
    void paint (juce::Graphics& g) override;
    static constexpr int barH = 12, scaleH = 14;
    static int height (bool scale) { return scale ? barH + 2 + scaleH : barH; }

private:
    bool showScale = true;
    float level = -100.0f, peak = -100.0f;
    int peakHold = 0, clipHold = 0;
};

/** Flat horizontal slider. thumb: 4 px track, fill from 0 (or the minimum), a centre tick for ranges that
    cross 0 and a round thumb (pitch / formant). bar: just the 4 px bar, drag anywhere (effect parameters).
    Double-click = default, arrow keys step (juce::Slider). */
class PaperSlider : public juce::Slider
{
public:
    enum class Style { thumb, bar };
    explicit PaperSlider (Style s = Style::thumb);
    void paint (juce::Graphics& g) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }

private:
    Style style;
};

/** Underlined accent text, optionally with an icon in front ("★ お気に入り", "+ 声を追加"). */
class TextLink : public juce::Button
{
public:
    TextLink (const juce::String& text, float fontSize = Theme::fontS, std::optional<Icon> icon = std::nullopt);
    void setIcon (std::optional<Icon> i) { icon = i; repaint(); }
    void paintButton (juce::Graphics& g, bool highlighted, bool down) override;
    int preferredWidth() const;

private:
    float size;
    std::optional<Icon> icon;
};

/** Icon + text status ("✓ 開", "⚠ あり"): state never by colour alone. Returns the width used. */
int drawStatus (juce::Graphics& g, juce::Rectangle<int> r, Icon icon, const juce::String& text, juce::Colour colour, float size, bool mono = false);
int statusWidth (const juce::String& text, float size, bool mono = false);
} // namespace koe::ui::paper
