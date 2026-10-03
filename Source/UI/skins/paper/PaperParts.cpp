#include "UI/skins/paper/PaperParts.h"

#include <cmath>

namespace koe::ui::paper
{
using mainui::textWidth;

namespace
{
const Palette& P() { return Theme::colours(); }
constexpr int kSegments = 30; // 2 dB each
} // namespace

// =============================================================================================== SegmentMeter
void SegmentMeter::setLevel (float db, bool clipped)
{
    // same ballistics as LevelMeter: fast attack, ~20 dB/s fall, peak hold from S-03 外観
    const auto& pr = Theme::prefs();
    const float perFrame = 30.0f / float (pr.meterFps);
    level = db > level ? db : std::max (db, level - 0.7f * perFrame);
    if (db >= peak) { peak = db; peakHold = juce::roundToInt (pr.peakHoldMs * float (pr.meterFps) / 1000.0f); }
    else if (peakHold > 0) --peakHold;
    else peak = std::max (db, peak - 1.0f * perFrame);
    if (clipped) clipHold = juce::roundToInt (1.5f * float (pr.meterFps));
    else if (clipHold > 0) --clipHold;
    repaint();
}

void SegmentMeter::paint (juce::Graphics& g)
{
    const auto& p = P();
    auto r = getLocalBounds().toFloat();
    auto bar = r.removeFromTop (float (barH));
    const float gap = 2.0f;
    const float segW = (bar.getWidth() - gap * (kSegments - 1)) / float (kSegments);
    const int lit = juce::roundToInt (LevelMeter::dbToPos (level) * kSegments);
    const int peakSeg = juce::roundToInt (LevelMeter::dbToPos (peak) * kSegments) - 1;
    const int warnFrom = juce::roundToInt (LevelMeter::dbToPos (-6.0f) * kSegments);
    for (int i = 0; i < kSegments; ++i)
    {
        auto seg = juce::Rectangle<float> (bar.getX() + float (i) * (segW + gap), bar.getY(), segW, bar.getHeight());
        juce::Colour c = p.trackOff;
        if (i < lit) c = i >= warnFrom ? p.warn : p.ok;
        if (i == peakSeg && peak > -60.0f) c = peak > -1.0f ? p.danger : p.warn;
        if (clipHold > 0 && i == kSegments - 1) c = p.danger;
        g.setColour (c);
        g.fillRect (seg);
    }
    if (! showScale) return;
    r.removeFromTop (2.0f);
    for (int db : { -48, -24, -12, -6, 0 })
    {
        const float x = bar.getX() + bar.getWidth() * LevelMeter::dbToPos (float (db));
        const int w = 24;
        auto label = juce::Rectangle<int> (juce::roundToInt (x) - w / 2, int (r.getY()), w, scaleH);
        if (db == 0) label = label.withX (int (bar.getRight()) - w);
        drawText (g, juce::String (db), label, Theme::fontXS, p.textSub, db == 0 ? juce::Justification::centredRight : juce::Justification::centred, false, true);
    }
}

// =============================================================================================== PaperSlider
PaperSlider::PaperSlider (Style s) : juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox), style (s)
{
    setWantsKeyboardFocus (true);
    setScrollWheelEnabled (true);
    setSliderSnapsToMousePosition (style == Style::bar);
}

void PaperSlider::paint (juce::Graphics& g)
{
    const auto& p = P();
    const bool thumbStyle = style == Style::thumb;
    const float thumb = thumbStyle ? 18.0f : 0.0f;
    auto r = getLocalBounds().toFloat().reduced (thumb * 0.5f, 0.0f);
    const auto track = r.withSizeKeepingCentre (r.getWidth(), thumbStyle ? 4.0f : 5.0f);
    g.setColour (isEnabled() ? (thumbStyle ? p.textSub : p.trackOff) : p.trackOff);
    g.fillRect (track);
    const float pos = float (valueToProportionOfLength (getValue()));
    const bool bipolar = getMinimum() < 0.0 && getMaximum() > 0.0;
    const float origin = bipolar ? float (valueToProportionOfLength (0.0)) : 0.0f;
    const float x0 = track.getX() + track.getWidth() * juce::jmin (pos, origin);
    const float x1 = track.getX() + track.getWidth() * juce::jmax (pos, origin);
    g.setColour (isEnabled() ? p.accent : p.textSub);
    g.fillRect (juce::Rectangle<float> (x0, track.getY(), juce::jmax (x1 - x0, 0.0f), track.getHeight()));
    if (! thumbStyle)
    {
        if (hasKeyboardFocus (true)) drawFocusRing (g, track.expanded (2.0f, 4.0f), Theme::radiusS);
        return;
    }
    if (bipolar)
    {
        g.setColour (p.textSub);
        const float cx = track.getX() + track.getWidth() * origin;
        g.fillRect (cx - 1.0f, track.getCentreY() - 6.0f, 2.0f, 12.0f);
    }
    const auto t = juce::Rectangle<float> (thumb, thumb).withCentre ({ track.getX() + track.getWidth() * pos, track.getCentreY() });
    g.setColour (p.surface);
    g.fillEllipse (t);
    g.setColour (isEnabled() ? p.accent : p.textSub);
    g.drawEllipse (t.reduced (1.25f), 2.5f);
    if (hasKeyboardFocus (true)) drawFocusRing (g, t.expanded (2.0f), thumb * 0.5f + 2.0f);
}

// =============================================================================================== TextLink
TextLink::TextLink (const juce::String& text, float fontSize, std::optional<Icon> i) : juce::Button (text), size (fontSize), icon (i)
{
    setWantsKeyboardFocus (true);
    setTitle (text);
}

int TextLink::preferredWidth() const
{
    return textWidth (Theme::ui (size, true), getButtonText()) + (icon ? int (size) + Theme::space1 + 2 : 0) + Theme::space1;
}

void TextLink::paintButton (juce::Graphics& g, bool highlighted, bool)
{
    const auto& p = P();
    const auto ink = ! isEnabled() ? p.textSub : (highlighted ? p.text : p.accent);
    auto r = getLocalBounds();
    const auto f = Theme::ui (size, true);
    if (icon)
    {
        drawIcon (g, *icon, r.removeFromLeft (int (size) + 2).withSizeKeepingCentre (int (size), int (size)).toFloat(), ink, 2.2f);
        r.removeFromLeft (Theme::space1);
    }
    const int w = juce::jmin (r.getWidth(), textWidth (f, getButtonText()));
    drawText (g, getButtonText(), r.withWidth (w + 2), size, ink, juce::Justification::centredLeft, true);
    g.setColour (ink);
    g.fillRect (float (r.getX()), r.getCentreY() + f.getHeight() * 0.5f + 1.0f, float (w), 1.0f);
    if (hasKeyboardFocus (true)) drawFocusRing (g, getLocalBounds().toFloat().reduced (1.0f), Theme::radiusS);
}

// =============================================================================================== status
int statusWidth (const juce::String& text, float size, bool mono)
{
    return 16 + Theme::space1 + 2 + int (std::ceil (textRunWidth (text, size, true, mono))) + 2;
}

int drawStatus (juce::Graphics& g, juce::Rectangle<int> r, Icon icon, const juce::String& text, juce::Colour colour, float size, bool mono)
{
    const int w = juce::jmin (r.getWidth(), statusWidth (text, size, mono));
    r = r.withWidth (w);
    drawIcon (g, icon, r.removeFromLeft (16).withSizeKeepingCentre (14, 14).toFloat(), colour, 2.6f);
    r.removeFromLeft (Theme::space1 + 2);
    drawText (g, text, r, size, colour, juce::Justification::centredLeft, true, mono);
    return w;
}
} // namespace koe::ui::paper
