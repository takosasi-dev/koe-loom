#include "UI/Widgets.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace koe::ui
{
namespace
{
constexpr float kArcStart = juce::MathConstants<float>::pi * 1.25f; // lower-left (JUCE: clockwise from 12 o'clock)
constexpr float kArcEnd = juce::MathConstants<float>::pi * 2.75f;   // lower-right

const Palette& P() { return Theme::colours(); }
} // namespace

void drawFocusRing (juce::Graphics& g, juce::Rectangle<float> r, float radius)
{
    g.setColour (P().accent);
    g.drawRoundedRectangle (r.expanded (Theme::focusRingWidth + 1.0f), radius + Theme::focusRingWidth + 1.0f, Theme::focusRingWidth);
}

namespace
{
bool isAscii (const juce::String& s) { return juce::CharPointer_ASCII::isValidString (s.toRawUTF8(), std::numeric_limits<int>::max()); }

/** Calls fn (run, monoFace) for the ASCII runs (mono face) and the other runs (UI face) of mono text. */
template <typename Fn>
void forEachRun (const juce::String& text, bool mono, Fn&& fn)
{
    if (! mono || isAscii (text)) { fn (text, mono); return; }
    juce::String run;
    bool runAscii = true;
    for (auto p = text.getCharPointer(); ! p.isEmpty();)
    {
        const auto ch = p.getAndAdvance();
        const bool a = ch < 128;
        if (a != runAscii && run.isNotEmpty()) { fn (run, runAscii); run.clear(); }
        runAscii = a;
        run += juce::String::charToString (ch);
    }
    if (run.isNotEmpty()) fn (run, runAscii);
}

juce::Font themeFont (float size, bool bold, bool mono) { return mono ? Theme::mono (size, bold) : Theme::ui (size, bold); }
} // namespace

void appendText (juce::AttributedString& s, const juce::String& text, float size, bool bold, bool mono, juce::Colour c)
{
    forEachRun (text, mono, [&] (const juce::String& run, bool m) { s.append (run, themeFont (size, bold, m), c); });
}

float textRunWidth (const juce::String& text, float size, bool bold, bool mono)
{
    float w = 0.0f;
    forEachRun (text, mono, [&] (const juce::String& run, bool m) { w += juce::GlyphArrangement::getStringWidth (themeFont (size, bold, m), run); });
    return w;
}

void drawText (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r, float size, juce::Colour c, juce::Justification j, bool bold, bool mono)
{
    if (mono && ! isAscii (text))
    {
        juce::AttributedString s;
        s.setJustification (j);
        s.setWordWrap (juce::AttributedString::none);
        appendText (s, text, size, bold, mono, c);
        s.draw (g, r.toFloat());
        return;
    }
    g.setColour (c);
    g.setFont (mono ? Theme::mono (size, bold) : Theme::ui (size, bold));
    g.drawFittedText (text, r, j, 1, 0.9f);
}

// =============================================================================================== Knob
Knob::Knob (Size s)
    : juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox), size (s),
      diameter (s == Size::big ? Theme::knobBig : Theme::knobSmall)
{
    setRotaryParameters (kArcStart, kArcEnd, true);
    setWantsKeyboardFocus (true);
    setMouseDragSensitivity (s == Size::big ? 300 : 200);
    setScrollWheelEnabled (true);
}

void Knob::setup (double min, double max, double def, double step, std::function<juce::String (double)> formatValue, const juce::String& u)
{
    setRange (min, max, step);
    defaultValue = def;
    setDoubleClickReturnValue (true, def);
    format = std::move (formatValue);
    unit = u;
    setValue (def, juce::dontSendNotification);
    valueChanged();
}

void Knob::valueChanged()
{
    // F-13-6: name, value, range with unit, default
    auto text = [this] (double v) { return (format ? format (v) : juce::String (v)) + (unit.isNotEmpty() ? " " + unit : juce::String()); };
    setTooltip ((label.isNotEmpty() ? label + ja ("：") : juce::String()) + text (getValue()) + ja ("（") + text (getMinimum()) + ja ("〜")
                + text (getMaximum()) + ja ("、初期値 ") + text (defaultValue) + ja ("）"));
    repaint();
}

void Knob::mouseEnter (const juce::MouseEvent& e)
{
    valueChanged();
    juce::Slider::mouseEnter (e);
}

void Knob::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        editValue();
        return;
    }
    // Shift = fine adjustment (F-14-5), decided at the start of the drag so the value never jumps
    // S-03 「つまみの感度」 scales the drag distance (ゆっくり = longer, 速い = shorter)
    const double sensitivity[] = { 1.6, 1.0, 0.6 };
    const double k = sensitivity[juce::jlimit (0, 2, Theme::prefs().knobSensitivity)];
    setMouseDragSensitivity (juce::roundToInt ((size == Size::big ? 300 : 200) * (e.mods.isShiftDown() ? 6 : 1) * k));
    juce::Slider::mouseDown (e);
}

void Knob::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    // S-03 「ホイールでつまみを回す」 OFF: the wheel scrolls the parent instead
    setScrollWheelEnabled (Theme::prefs().knobWheel);
    juce::Slider::mouseWheelMove (e, w);
}

void Knob::mouseDrag (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    juce::Slider::mouseDrag (e);
}

void Knob::mouseUp (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    juce::Slider::mouseUp (e);
}

bool Knob::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::returnKey)
    {
        editValue();
        return true;
    }
    const double step = getInterval() > 0.0 ? getInterval() : (getMaximum() - getMinimum()) / 100.0;
    const double mult = k.getModifiers().isShiftDown() ? 1.0 : 5.0;
    if (k.isKeyCode (juce::KeyPress::upKey) || k.isKeyCode (juce::KeyPress::rightKey)) { setValue (getValue() + step * mult); return true; }
    if (k.isKeyCode (juce::KeyPress::downKey) || k.isKeyCode (juce::KeyPress::leftKey)) { setValue (getValue() - step * mult); return true; }
    if (k.isKeyCode (juce::KeyPress::homeKey)) { setValue (defaultValue); return true; }
    return juce::Slider::keyPressed (k);
}

void Knob::editValue()
{
    editor = std::make_unique<juce::TextEditor>();
    addAndMakeVisible (*editor);
    const int w = juce::jmin (getWidth(), 72), h = 28;
    editor->setBounds ((getWidth() - w) / 2, (getHeight() - h) / 2, w, h);
    editor->setFont (Theme::mono (Theme::fontS, true));
    editor->setJustification (juce::Justification::centred);
    editor->setText (juce::String (getValue(), getInterval() >= 1.0 ? 0 : 2), false);
    editor->selectAll();
    editor->grabKeyboardFocus();
    auto finish = [this] (bool apply)
    {
        if (editor == nullptr) return;
        if (apply)
        {
            const auto t = editor->getText().retainCharacters ("0123456789.-+");
            if (t.isNotEmpty()) setValue (juce::jlimit (getMinimum(), getMaximum(), t.getDoubleValue()), juce::sendNotificationSync);
        }
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Knob> (this)] { if (safe != nullptr) { safe->editor.reset(); safe->grabKeyboardFocus(); } });
    };
    editor->onReturnKey = [finish] { finish (true); };
    editor->onEscapeKey = [finish] { finish (false); };
    editor->onFocusLost = [finish] { finish (true); };
}

void Knob::paint (juce::Graphics& g)
{
    const auto& p = P();
    const bool big = size == Size::big;
    const float side = float (diameter);
    auto area = juce::Rectangle<float> (float (getWidth()), float (getHeight()));
    auto knob = big ? area.removeFromTop (side).withSizeKeepingCentre (side, side) : area.withSizeKeepingCentre (side, side).withY (0.0f);
    // stroke and dot keep the mock's proportions at any diameter (112 px: 6 / 5.7, 40 px: 4 / 3.8)
    const float stroke = big ? side * 6.0f / float (Theme::knobBig) : side * 4.0f / float (Theme::knobSmall);
    const float dotR = big ? side * 5.7f / float (Theme::knobBig) : side * 3.8f / float (Theme::knobSmall);
    const auto arcBox = knob.reduced (dotR + 1.0f);
    const float r = arcBox.getWidth() * 0.5f;
    const auto c = arcBox.getCentre();

    const double norm = valueToProportionOfLength (getValue());
    const float angle = kArcStart + float (norm) * (kArcEnd - kArcStart);
    // bipolar ranges (pitch, formant, dB) fill from the default; others from the start
    const bool bipolar = getMinimum() < 0.0 && getMaximum() > 0.0;
    const float originAngle = bipolar ? kArcStart + float (valueToProportionOfLength (defaultValue)) * (kArcEnd - kArcStart) : kArcStart;

    juce::Path track;
    track.addCentredArc (c.x, c.y, r, r, 0.0f, kArcStart, kArcEnd, true);
    g.setColour (isEnabled() ? p.border : p.trackOff);
    g.strokePath (track, juce::PathStrokeType (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (std::abs (angle - originAngle) > 0.001f)
    {
        juce::Path val;
        val.addCentredArc (c.x, c.y, r, r, 0.0f, std::min (originAngle, angle), std::max (originAngle, angle), true);
        g.setColour (isEnabled() ? p.accent : p.textSub);
        g.strokePath (val, juce::PathStrokeType (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    const juce::Point<float> dot (c.x + r * std::sin (angle), c.y - r * std::cos (angle));
    g.setColour (p.text);
    g.fillEllipse (juce::Rectangle<float> (dotR * 2.0f, dotR * 2.0f).withCentre (dot));

    if (big)
    {
        // value large in the centre (F-14-12), unit under it, label under the knob
        const float valueSize = side >= float (Theme::knobBig) ? Theme::fontXL : Theme::fontL; // 84 px (narrow layout): 20
        g.setColour (p.text);
        g.setFont (Theme::mono (valueSize, true));
        g.drawText (getValueText(), knob.withTrimmedTop (side * 0.30f).withHeight (valueSize + 4.0f), juce::Justification::centred);
        g.setColour (p.textSub);
        g.setFont (Theme::mono (Theme::fontXS));
        g.drawText (unit, knob.withTrimmedTop (side * 0.30f + valueSize + 4.0f).withHeight (Theme::fontXS + 4.0f), juce::Justification::centred);
        if (label.isNotEmpty())
        {
            g.setColour (p.text);
            g.setFont (Theme::ui (Theme::fontS, true));
            g.drawText (label, area.removeFromTop (Theme::fontS + float (Theme::space2)), juce::Justification::centred);
        }
    }
    if (hasKeyboardFocus (true) && editor == nullptr) drawFocusRing (g, knob.reduced (2.0f), side * 0.5f);
}

// =============================================================================================== ToggleSwitch
ToggleSwitch::ToggleSwitch() : juce::Button ("toggle")
{
    setClickingTogglesState (true);
    setWantsKeyboardFocus (true);
}

void ToggleSwitch::paintButton (juce::Graphics& g, bool highlighted, bool)
{
    const auto& p = P();
    const bool on = getToggleState();
    auto track = juce::Rectangle<float> (0.0f, (getHeight() - Theme::toggleH) * 0.5f, float (Theme::toggleW), float (Theme::toggleH));
    const float rad = Theme::toggleH * 0.5f;
    g.setColour (on ? p.accent : p.trackOff);
    g.fillRoundedRectangle (track, rad);
    g.setColour (on ? p.accent : p.border);
    g.drawRoundedRectangle (track.reduced (0.75f), rad, Theme::borderWidth);
    const float d = Theme::toggleH - 6.0f;
    const auto thumb = juce::Rectangle<float> (d, d).withCentre ({ on ? track.getRight() - rad : track.getX() + rad, track.getCentreY() });
    g.setColour (on ? p.onAccent : p.textSub);
    g.fillEllipse (thumb);
    if (highlighted && isEnabled()) { g.setColour (p.text.withAlpha (0.06f)); g.fillRoundedRectangle (track, rad); }
    drawText (g, on ? "ON" : "OFF", { Theme::toggleW + Theme::space2, 0, getWidth() - Theme::toggleW - Theme::space2, getHeight() },
              Theme::fontXS, on ? p.text : p.textSub, juce::Justification::centredLeft, true, true);
    if (! isEnabled()) { g.setColour (p.surface.withAlpha (0.5f)); g.fillRect (getLocalBounds()); }
    if (hasKeyboardFocus (true)) drawFocusRing (g, track, rad);
}

// =============================================================================================== PillButton
PillButton::PillButton (const juce::String& text, Style s, std::optional<Icon> i) : juce::Button (text), style (s), icon (i)
{
    setWantsKeyboardFocus (true);
}

int PillButton::preferredWidth() const
{
    const auto f = Theme::ui (fontSize, true);
    const int textW = int (std::ceil (juce::GlyphArrangement::getStringWidth (f, getButtonText())));
    return textW + (icon ? 18 + 6 : 0) + (pill ? Theme::space4 : Theme::space3) * 2 - (pill ? 4 : 2);
}

void PillButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto& p = P();
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const float rad = pill ? r.getHeight() * 0.5f : Theme::radiusM;
    juce::Colour fill = juce::Colours::transparentBlack, border = juce::Colours::transparentBlack, ink = p.text;
    bool bold = true;
    switch (style)
    {
        case Style::primary: fill = p.accent; ink = p.onAccent; break;
        case Style::outline: border = p.border; break;
        case Style::ghost: ink = p.textSub; bold = false; if (highlighted) fill = p.raised; break;
        case Style::danger: fill = p.danger; ink = p.onDanger; break;
        case Style::tab:
            if (getToggleState()) fill = p.raised;
            else { ink = p.textSub; bold = false; if (highlighted) fill = p.raised.withAlpha (0.5f); }
            break;
        case Style::accentOutline: border = p.accent; ink = p.accent; break;
        case Style::link: ink = highlighted ? p.text : p.textSub; break;
    }
    if (highlighted && (style == Style::primary || style == Style::danger)) fill = fill.brighter (0.08f);
    if (highlighted && (style == Style::outline || style == Style::accentOutline)) fill = p.raised;
    if (down && style != Style::link) fill = fill.isTransparent() ? p.raised : fill.darker (0.12f);
    if (! fill.isTransparent()) { g.setColour (fill); g.fillRoundedRectangle (r, rad); }
    if (! border.isTransparent()) { g.setColour (border); g.drawRoundedRectangle (r.reduced (0.75f), rad, Theme::borderWidth); }

    const auto f = Theme::ui (fontSize, bold);
    const float textW = juce::GlyphArrangement::getStringWidth (f, getButtonText());
    const float iconW = icon ? 18.0f : 0.0f;
    const float gap = icon && getButtonText().isNotEmpty() ? 6.0f : 0.0f;
    float x = r.getCentreX() - (iconW + gap + textW) * 0.5f;
    if (icon)
    {
        drawIcon (g, *icon, { x, r.getCentreY() - 9.0f, 18.0f, 18.0f }, ink, style == Style::primary || style == Style::accentOutline ? 2.4f : 2.0f);
        x += iconW + gap;
    }
    g.setColour (ink);
    g.setFont (f);
    g.drawText (getButtonText(), juce::Rectangle<float> (x, r.getY(), textW + 2.0f, r.getHeight()), juce::Justification::centredLeft);
    if (style == Style::link) // underline 4 px under the baseline (mock: text-underline-offset 4px)
        g.fillRect (x, r.getCentreY() + (f.getAscent() - f.getDescent()) * 0.5f + 4.0f, textW, 1.0f);
    if (! isEnabled()) { g.setColour (p.bg.withAlpha (0.55f)); g.fillRoundedRectangle (r, rad); }
    if (hasKeyboardFocus (true)) drawFocusRing (g, r, rad);
}

// =============================================================================================== IconButton
IconButton::IconButton (const juce::String& accessibleName, Icon i, bool o) : juce::Button (accessibleName), icon (i), outlined (o)
{
    setTooltip (accessibleName);
    setTitle (accessibleName);
    setWantsKeyboardFocus (true);
}

void IconButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto& p = P();
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const float rad = outlined ? r.getHeight() * 0.5f : Theme::radiusM;
    if (highlighted || down) { g.setColour (p.raised); g.fillRoundedRectangle (r, rad); }
    if (outlined) { g.setColour (p.border); g.drawRoundedRectangle (r.reduced (0.75f), rad, Theme::borderWidth); }
    const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.45f;
    drawIcon (g, icon, r.withSizeKeepingCentre (s, s), isEnabled() ? p.text : p.textSub);
    if (hasKeyboardFocus (true)) drawFocusRing (g, r, rad);
}

// =============================================================================================== LevelMeter
LevelMeter::LevelMeter (bool v) : vertical (v) {}

float LevelMeter::dbToPos (float db) { return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); }

void LevelMeter::setLevel (float db, bool clipped)
{
    // fast attack, ~20 dB/s fall for the bar; peak hold from S-03 (default 1.5 s = 45 frames at 30 fps).
    // Fed once per frame at Theme::prefs().meterFps, so the per-frame steps scale to keep the same speed.
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

void LevelMeter::paint (juce::Graphics& g)
{
    const auto& p = P();
    auto r = getLocalBounds().toFloat();
    const float scaleW = showScale ? 26.0f : 0.0f;
    if (vertical)
    {
        r.removeFromLeft (scaleW);
        auto bar = r.withWidth (juce::jmin (r.getWidth(), 12.0f));
        g.setColour (p.trackOff);
        g.fillRoundedRectangle (bar, Theme::radiusS);
        const float h = bar.getHeight();
        const float pos = dbToPos (level);
        const float warnPos = dbToPos (-6.0f);
        auto fill = bar.withTop (bar.getBottom() - h * pos);
        g.setColour (p.ok);
        g.fillRoundedRectangle (fill.withTop (std::max (fill.getY(), bar.getBottom() - h * warnPos)), Theme::radiusS);
        if (pos > warnPos) { g.setColour (p.warn); g.fillRect (fill.withBottom (bar.getBottom() - h * warnPos)); }
        const float py = bar.getBottom() - h * dbToPos (peak);
        g.setColour (peak > -1.0f ? p.danger : p.text);
        if (dbToPos (peak) > 0.0f) g.fillRect (bar.getX(), py - 1.0f, bar.getWidth(), 2.0f); // silence: no stray line at the floor
        if (clipHold > 0) { g.setColour (p.danger); g.fillRect (bar.withHeight (4.0f)); }
        for (auto& [db, label] : scaleLabels())
            drawText (g, juce::String (db), label, Theme::fontXS, p.textSub, juce::Justification::centredRight, false, true);
    }
    else
    {
        auto bar = r.withHeight (juce::jmin (r.getHeight(), 8.0f)).withCentre (r.getCentre());
        g.setColour (p.trackOff);
        g.fillRoundedRectangle (bar, Theme::radiusS);
        const float w = bar.getWidth();
        const float pos = dbToPos (level), warnPos = dbToPos (-6.0f);
        g.setColour (p.ok);
        g.fillRoundedRectangle (bar.withWidth (w * std::min (pos, warnPos)), Theme::radiusS);
        if (pos > warnPos) { g.setColour (p.warn); g.fillRect (bar.withX (bar.getX() + w * warnPos).withWidth (w * (pos - warnPos))); }
        g.setColour (peak > -1.0f ? p.danger : p.text);
        if (dbToPos (peak) > 0.0f) g.fillRect (bar.getX() + w * dbToPos (peak) - 1.0f, bar.getY(), 2.0f, bar.getHeight());
        if (clipHold > 0) { g.setColour (p.danger); g.fillRect (bar.withX (bar.getRight() - 4.0f).withWidth (4.0f)); }
    }
}

std::vector<std::pair<int, juce::Rectangle<int>>> LevelMeter::scaleLabels() const
{
    std::vector<std::pair<int, juce::Rectangle<int>>> out;
    if (! vertical || ! showScale) return out;
    // centred on their tick but kept inside the bounds ("0" sits at the very top); the ends first,
    // then the middle ones that fit without touching a label already placed
    constexpr int scaleW = 26, labelH = int (Theme::fontXS) + 2;
    const int h = getHeight();
    for (int db : { 0, -48, -6, -12, -24 })
    {
        const int y = juce::roundToInt (float (h) * (1.0f - dbToPos (float (db))));
        const juce::Rectangle<int> label (0, juce::jlimit (0, juce::jmax (0, h - labelH), y - labelH / 2), scaleW - 4, labelH);
        if (std::none_of (out.begin(), out.end(), [&label] (auto& d) { return d.second.intersects (label); })) out.emplace_back (db, label);
    }
    return out;
}

// =============================================================================================== ValueSlider
ValueSlider::ValueSlider() : juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox)
{
    setWantsKeyboardFocus (true);
    setScrollWheelEnabled (true);
}

void ValueSlider::setup (double min, double max, double def, double step, std::function<juce::String (double)> formatValue)
{
    setRange (min, max, step);
    setDoubleClickReturnValue (true, def);
    format = std::move (formatValue);
    setValue (def, juce::dontSendNotification);
}

void ValueSlider::paint (juce::Graphics& g)
{
    const auto& p = P();
    auto r = getLocalBounds().toFloat();
    auto text = r.removeFromRight (float (valueTextWidth));
    auto track = r.reduced (8.0f, 0.0f).withSizeKeepingCentre (r.getWidth() - 16.0f, 6.0f);
    g.setColour (p.border);
    g.fillRoundedRectangle (track, 3.0f);
    const float pos = float (valueToProportionOfLength (getValue()));
    g.setColour (isEnabled() ? p.accent : p.textSub);
    g.fillRoundedRectangle (track.withWidth (track.getWidth() * pos), 3.0f);
    const auto thumb = juce::Rectangle<float> (16.0f, 16.0f).withCentre ({ track.getX() + track.getWidth() * pos, track.getCentreY() });
    g.setColour (p.text);
    g.fillEllipse (thumb);
    g.setColour (p.raised);
    g.drawEllipse (thumb, 2.0f);
    drawText (g, format ? format (getValue()) : juce::String (getValue()), text.toNearestInt(), Theme::fontM, p.text, juce::Justification::centredRight, true, true);
    if (hasKeyboardFocus (true)) drawFocusRing (g, thumb, 8.0f);
}

// =============================================================================================== helpers
juce::Rectangle<int> paintCard (juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& number, const juce::String& title)
{
    const auto& p = P();
    g.setColour (p.surface);
    g.fillRoundedRectangle (bounds.toFloat(), Theme::radiusL);
    auto content = bounds.reduced (Theme::space3);
    if (title.isNotEmpty())
    {
        auto header = content.removeFromTop (Theme::space4);
        if (number.isNotEmpty())
        {
            drawText (g, number, header.removeFromLeft (Theme::space4), Theme::fontXS, p.textSub, juce::Justification::centredLeft, false, true);
        }
        drawText (g, title, header, Theme::fontS, p.text, juce::Justification::centredLeft, true);
        content.removeFromTop (Theme::space2);
    }
    return content;
}

void paintWeightBadge (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, bool heavy)
{
    const auto& p = P();
    g.setColour (heavy ? p.warn : p.border);
    g.drawRoundedRectangle (r.reduced (0.5f), Theme::radiusS, 1.0f);
    drawText (g, text, r.toNearestInt(), Theme::fontXS, heavy ? p.warn : p.textSub, juce::Justification::centred, true);
}

// =============================================================================================== LookAndFeel
KoeLookAndFeel::KoeLookAndFeel()
{
    setDefaultSansSerifTypefaceName (Theme::ui (Theme::fontS).getTypefaceName());
    refreshColours();
}

void KoeLookAndFeel::refreshColours()
{
    const auto& p = P();
    setColour (juce::ResizableWindow::backgroundColourId, p.bg);
    setColour (juce::DocumentWindow::backgroundColourId, p.bg);
    setColour (juce::Label::textColourId, p.text);
    setColour (juce::ComboBox::backgroundColourId, p.raised);
    setColour (juce::ComboBox::outlineColourId, p.border);
    setColour (juce::ComboBox::textColourId, p.text);
    setColour (juce::ComboBox::arrowColourId, p.textSub);
    setColour (juce::ComboBox::focusedOutlineColourId, p.accent);
    setColour (juce::PopupMenu::backgroundColourId, p.raised);
    setColour (juce::PopupMenu::textColourId, p.text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, p.accent);
    setColour (juce::PopupMenu::highlightedTextColourId, p.onAccent);
    setColour (juce::PopupMenu::headerTextColourId, p.textSub);
    setColour (juce::TextEditor::backgroundColourId, p.raised);
    setColour (juce::TextEditor::textColourId, p.text);
    setColour (juce::TextEditor::outlineColourId, p.border);
    setColour (juce::TextEditor::focusedOutlineColourId, p.accent);
    setColour (juce::TextEditor::highlightColourId, p.accent.withAlpha (0.35f));
    setColour (juce::TextEditor::highlightedTextColourId, p.text);
    setColour (juce::CaretComponent::caretColourId, p.accent);
    setColour (juce::ScrollBar::thumbColourId, p.border);
    setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
    setColour (juce::TooltipWindow::backgroundColourId, p.raised);
    setColour (juce::TooltipWindow::textColourId, p.text);
    setColour (juce::TooltipWindow::outlineColourId, p.border);
    setColour (juce::AlertWindow::backgroundColourId, p.surface);
    setColour (juce::AlertWindow::textColourId, p.text);
    setColour (juce::AlertWindow::outlineColourId, p.border);
    setColour (juce::TextButton::buttonColourId, p.raised);
    setColour (juce::TextButton::buttonOnColourId, p.accent);
    setColour (juce::TextButton::textColourOffId, p.text);
    setColour (juce::TextButton::textColourOnId, p.onAccent);
    setColour (juce::ListBox::backgroundColourId, p.surface);
    setColour (juce::ListBox::textColourId, p.text);
    setColour (juce::ToggleButton::textColourId, p.text);
    setColour (juce::ToggleButton::tickColourId, p.accent);
    setColour (juce::ToggleButton::tickDisabledColourId, p.border);
    getCurrentColourScheme().setUIColour (juce::LookAndFeel_V4::ColourScheme::windowBackground, p.bg);
    getCurrentColourScheme().setUIColour (juce::LookAndFeel_V4::ColourScheme::widgetBackground, p.raised);
    getCurrentColourScheme().setUIColour (juce::LookAndFeel_V4::ColourScheme::menuBackground, p.raised);
    getCurrentColourScheme().setUIColour (juce::LookAndFeel_V4::ColourScheme::outline, p.border);
    getCurrentColourScheme().setUIColour (juce::LookAndFeel_V4::ColourScheme::defaultText, p.text);
    getCurrentColourScheme().setUIColour (juce::LookAndFeel_V4::ColourScheme::highlightedFill, p.accent);
    getCurrentColourScheme().setUIColour (juce::LookAndFeel_V4::ColourScheme::highlightedText, p.onAccent);
    getCurrentColourScheme().setUIColour (juce::LookAndFeel_V4::ColourScheme::menuText, p.text);
}

juce::Font KoeLookAndFeel::getComboBoxFont (juce::ComboBox&) { return Theme::ui (Theme::fontS, true); }
juce::Font KoeLookAndFeel::getPopupMenuFont() { return Theme::ui (Theme::fontS); }
juce::Font KoeLookAndFeel::getLabelFont (juce::Label& l) { return l.getFont(); } // labels are given Theme fonts
juce::Font KoeLookAndFeel::getTextButtonFont (juce::TextButton&, int) { return Theme::ui (Theme::fontS, true); }
juce::Font KoeLookAndFeel::getAlertWindowMessageFont() { return Theme::ui (Theme::fontS); }
juce::Font KoeLookAndFeel::getAlertWindowTitleFont() { return Theme::ui (Theme::fontM, true); }

void KoeLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto& p = P();
    auto r = juce::Rectangle<float> (float (w), float (h)).reduced (0.5f);
    g.setColour (p.raised);
    g.fillRoundedRectangle (r, Theme::radiusM);
    g.setColour (box.hasKeyboardFocus (true) ? p.accent : p.border);
    g.drawRoundedRectangle (r, Theme::radiusM, box.hasKeyboardFocus (true) ? Theme::focusRingWidth : 1.0f);
    drawIcon (g, Icon::chevronDown, { float (w) - 28.0f, (float (h) - 16.0f) * 0.5f, 16.0f, 16.0f }, p.textSub);
}

void KoeLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (Theme::space2 + 4, 1, box.getWidth() - Theme::space2 - 4 - 32, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

void KoeLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor&)
{
    g.setColour (P().raised);
    g.fillRoundedRectangle (juce::Rectangle<float> (float (w), float (h)), Theme::radiusS);
}

void KoeLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& ed)
{
    const auto& p = P();
    const bool f = ed.hasKeyboardFocus (true);
    g.setColour (f ? p.accent : p.border);
    g.drawRoundedRectangle (juce::Rectangle<float> (float (w), float (h)).reduced (0.5f), Theme::radiusS, f ? Theme::focusRingWidth : 1.0f);
}

void KoeLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int w, int h)
{
    const auto& p = P();
    auto r = juce::Rectangle<float> (float (w), float (h));
    g.setColour (p.raised);
    g.fillRoundedRectangle (r, Theme::radiusM);
    g.setColour (p.border);
    g.drawRoundedRectangle (r.reduced (0.5f), Theme::radiusM, 1.0f);
    g.setColour (p.text);
    g.setFont (Theme::ui (Theme::fontXS));
    g.drawFittedText (text, juce::Rectangle<int> (w, h).reduced (Theme::space2, Theme::space1), juce::Justification::centredLeft, 6);
}

juce::Rectangle<int> KoeLookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea)
{
    juce::AttributedString s;
    s.append (tipText, Theme::ui (Theme::fontXS), P().text);
    juce::TextLayout tl;
    tl.createLayoutWithBalancedLineLengths (s, 320.0f);
    const int w = int (tl.getWidth()) + Theme::space2 * 2 + 2;
    const int h = int (tl.getHeight()) + Theme::space1 * 2 + 4;
    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6, w, h)
        .constrainedWithin (parentArea);
}
} // namespace koe::ui
