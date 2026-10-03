#include "UI/main/Common.h"

#include <cmath>

namespace koe::ui::mainui
{
namespace
{
const Palette& P() { return Theme::colours(); }

/** Up to `decimals` places, trailing zeros dropped as in the mock ("10 dB", "4.5 kHz"). */
juce::String num (float v, int decimals)
{
    if (std::abs (v) < 0.5f * std::pow (10.0f, float (-decimals))) v = 0.0f; // no "-0"
    auto s = juce::String (v, decimals);
    if (s.containsChar ('.')) s = s.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    return s;
}
} // namespace

bool& animationsOff()
{
    static bool off = false;
    return off;
}

juce::String formatSemitones (double v)
{
    const double r = std::round (v * 10.0) / 10.0;
    const bool whole = std::abs (r - std::round (r)) < 1.0e-6;
    auto s = whole ? juce::String (juce::roundToInt (r)) : juce::String (r, 1);
    return r > 0.0 ? "+" + s : s;
}

juce::String formatParam (const ParamSpec& p, float v)
{
    if (p.isChoice())
    {
        const int i = juce::jlimit (0, int (p.choices.size()) - 1, juce::roundToInt (v));
        return juce::String::fromUTF8 (p.choices[size_t (i)].second);
    }
    const auto unit = juce::String::fromUTF8 (p.unit);
    if (unit == "dB") return num (v, 1) + " dB";
    if (unit == "Hz")
    {
        if (v >= 1000.0f) return num (v / 1000.0f, 1) + " kHz";
        return (v >= 10.0f ? juce::String (juce::roundToInt (v)) : num (v, 2)) + " Hz";
    }
    if (unit == "st") return formatSemitones (v) + " st";
    if (unit == ":1") return num (v, 1) + ":1";
    if (unit == "ms") return (v < 10.0f ? num (v, 1) : juce::String (juce::roundToInt (v))) + " ms";
    if (p.integer) return juce::String (juce::roundToInt (v)) + (unit.isEmpty() ? juce::String() : " " + unit);
    if (unit.isEmpty() && p.min >= 0.0f && p.max <= 1.0f) return juce::String (juce::roundToInt (v * 100.0f)) + " %";
    return num (v, p.max - p.min <= 2.0f ? 2 : 1) + (unit.isEmpty() ? juce::String() : " " + unit);
}

juce::String paramLabel (const ParamSpec& p)
{
    auto name = juce::String::fromUTF8 (p.nameJa);
    if (p.isChoice()) return name + juce::String::fromUTF8 ("（初期値 ") + formatParam (p, p.def) + juce::String::fromUTF8 ("）");
    return name + juce::String::fromUTF8 ("（") + formatParam (p, p.min) + juce::String::fromUTF8 ("〜") + formatParam (p, p.max)
           + juce::String::fromUTF8 ("、初期値 ") + formatParam (p, p.def) + juce::String::fromUTF8 ("）");
}

void applyParamRange (juce::Slider& s, const ParamSpec& p, float value)
{
    s.setRange (p.min, p.max, p.integer || p.isChoice() ? 1.0 : 0.0);
    // wide frequency / time ranges are easier to set on a log-like scale
    const auto unit = juce::String (p.unit);
    if ((unit == "Hz" || unit == "ms") && p.min > 0.0f && p.max / p.min >= 20.0f)
        s.setSkewFactorFromMidPoint (std::sqrt (double (p.min) * double (p.max)));
    s.setDoubleClickReturnValue (true, p.def);
    s.setValue (value, juce::dontSendNotification);
}

std::vector<int> mainParams (const EffectInfo& info)
{
    std::vector<int> r;
    for (int i = 0; i < int (info.params.size()) && r.size() < 2; ++i)
        if (! info.params[size_t (i)].isChoice()) r.push_back (i);
    return r;
}

int textWidth (const juce::Font& f, const juce::String& s)
{
    return int (std::ceil (juce::GlyphArrangement::getStringWidth (f, s)));
}

juce::Rectangle<int> drawCardHeader (juce::Graphics& g, juce::Rectangle<int> row, const juce::String& number, const juce::String& title)
{
    const auto& p = P();
    if (number.isNotEmpty()) drawText (g, number, row.removeFromLeft (Theme::space4), Theme::fontXS, p.textSub, juce::Justification::centredLeft, false, true);
    const int w = juce::jmin (row.getWidth(), textWidth (Theme::ui (Theme::fontS, true), title) + Theme::space1);
    drawText (g, title, row.removeFromLeft (w), Theme::fontS, p.text, juce::Justification::centredLeft, true);
    return row;
}

juce::String presetCategoryJa (const juce::String& id)
{
    if (id == "natural") return juce::String::fromUTF8 ("自然");
    if (id == "character") return juce::String::fromUTF8 ("キャラ");
    if (id == "device") return juce::String::fromUTF8 ("機器・メディア");
    if (id == "space") return juce::String::fromUTF8 ("空間");
    if (id == "layered") return juce::String::fromUTF8 ("重ね・揺れ");
    return juce::String::fromUTF8 ("ユーザー");
}

juce::Component* findById (juce::Component* root, const juce::String& id)
{
    if (root == nullptr) return nullptr;
    if (root->getComponentID() == id) return root;
    for (auto* c : root->getChildren())
        if (auto* f = findById (c, id)) return f;
    return nullptr;
}

bool visibleWithin (const juce::Component* c, const juce::Component* root)
{
    for (; c != nullptr; c = c->getParentComponent())
    {
        if (c == root) return true; // the root itself may be offscreen (snapshots, tests)
        if (! c->isVisible()) return false;
    }
    return false;
}

void withLooperCheck (AppController& c, Navigator& nav, std::function<void()> action)
{
    if (! c.hasLooperRecording())
    {
        action();
        return;
    }
    nav.showOverlay (std::make_unique<ConfirmPanel> (nav, ja ("ルーパーの録音が消えます"),
                                                     ja ("この操作をすると、ルーパーに録音した声が消えます。続けますか。"), ja ("続ける"),
                                                     std::move (action), ja ("やめる")));
}

std::function<void()> guardedBy (juce::Component& owner, std::function<void()> action)
{
    return [safe = juce::Component::SafePointer<juce::Component> (&owner), a = std::move (action)]
    {
        if (safe != nullptr) a();
    };
}

void withLooperCheck (AppController& c, Navigator& nav, juce::Component& owner, std::function<void()> action)
{
    withLooperCheck (c, nav, guardedBy (owner, std::move (action)));
}

// =============================================================================================== buttons
SquareIconButton::SquareIconButton (const juce::String& name, Icon i) : juce::Button (name), icon (i)
{
    setTooltip (name);
    setTitle (name);
    setWantsKeyboardFocus (true);
}

void SquareIconButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto& p = P();
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    if (highlighted || down) { g.setColour (p.raised); g.fillRoundedRectangle (r, Theme::radiusS); }
    g.setColour (p.border);
    g.drawRoundedRectangle (r, Theme::radiusS, 1.0f);
    const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.5f;
    drawIcon (g, icon, r.withSizeKeepingCentre (s, s), isEnabled() ? p.text : p.textSub.withAlpha (0.5f));
    if (hasKeyboardFocus (true)) drawFocusRing (g, r, Theme::radiusS);
}

LinkButton::LinkButton (const juce::String& text, bool s) : juce::Button (text), subtle (s) { setWantsKeyboardFocus (true); }

int LinkButton::preferredWidth() const
{
    return int (std::ceil (juce::GlyphArrangement::getStringWidth (Theme::ui (Theme::fontXS, true), getButtonText()))) + Theme::space1;
}

void LinkButton::paintButton (juce::Graphics& g, bool highlighted, bool)
{
    const auto& p = P();
    const auto ink = subtle ? (highlighted ? p.text : p.textSub) : (highlighted ? p.text : p.accent);
    const auto f = Theme::ui (subtle ? Theme::fontS : Theme::fontXS, true);
    g.setColour (ink);
    g.setFont (f);
    const auto r = getLocalBounds();
    const float w = juce::GlyphArrangement::getStringWidth (f, getButtonText());
    const float x = subtle ? (float (r.getWidth()) - w) * 0.5f : 0.0f;
    g.drawText (getButtonText(), r, subtle ? juce::Justification::centred : juce::Justification::centredLeft);
    const float y = r.getCentreY() + f.getHeight() * 0.5f + 1.0f;
    g.fillRect (x, y, juce::jmin (w, float (r.getWidth())), 1.0f);
    if (hasKeyboardFocus (true)) drawFocusRing (g, r.toFloat().reduced (1.0f), Theme::radiusS);
}

DashedButton::DashedButton (const juce::String& text, bool v) : juce::Button (text), vertical (v)
{
    setWantsKeyboardFocus (true);
    setTitle (text);
}

void DashedButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto& p = P();
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    if (highlighted || down) { g.setColour (p.raised); g.fillRoundedRectangle (r, Theme::radiusM); }
    juce::Path outline;
    outline.addRoundedRectangle (r, Theme::radiusM);
    juce::Path dashed;
    const float dashes[] = { 5.0f, 4.0f };
    juce::PathStrokeType (Theme::borderWidth).createDashedStroke (dashed, outline, dashes, 2);
    g.setColour (p.border);
    g.fillPath (dashed);

    const auto f = Theme::ui (vertical ? Theme::fontS : Theme::fontXS, true);
    const float iconS = vertical ? 22.0f : 16.0f;
    const float textW = juce::GlyphArrangement::getStringWidth (f, getButtonText());
    const auto ink = isEnabled() ? p.text : p.textSub;
    g.setFont (f);
    g.setColour (ink);
    if (vertical)
    {
        const float top = r.getCentreY() - (iconS + Theme::space1 + f.getHeight()) * 0.5f;
        drawIcon (g, Icon::plus, { r.getCentreX() - iconS * 0.5f, top, iconS, iconS }, ink);
        g.drawText (getButtonText(), juce::Rectangle<float> (r.getX(), top + iconS + Theme::space1, r.getWidth(), f.getHeight() + 2.0f),
                    juce::Justification::centred);
    }
    else
    {
        float x = r.getCentreX() - (iconS + Theme::space1 + textW) * 0.5f;
        drawIcon (g, Icon::plus, { x, r.getCentreY() - iconS * 0.5f, iconS, iconS }, ink);
        x += iconS + Theme::space1;
        g.drawText (getButtonText(), juce::Rectangle<float> (x, r.getY(), textW + 2.0f, r.getHeight()), juce::Justification::centredLeft);
    }
    if (hasKeyboardFocus (true)) drawFocusRing (g, r, Theme::radiusM);
}

LineSlider::LineSlider() : juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox)
{
    setWantsKeyboardFocus (true);
    setScrollWheelEnabled (true);
}

void LineSlider::paint (juce::Graphics& g)
{
    const auto& p = P();
    const float thumb = 14.0f;
    auto r = getLocalBounds().toFloat().reduced (thumb * 0.5f, 0.0f);
    const auto track = r.withSizeKeepingCentre (r.getWidth(), 6.0f);
    g.setColour (isEnabled() ? p.border : p.trackOff);
    g.fillRoundedRectangle (track, 3.0f);
    const float pos = float (valueToProportionOfLength (getValue()));
    const bool bipolar = getMinimum() < 0.0 && getMaximum() > 0.0;
    const float origin = bipolar ? float (valueToProportionOfLength (0.0)) : 0.0f;
    const float x0 = track.getX() + track.getWidth() * juce::jmin (pos, origin);
    const float x1 = track.getX() + track.getWidth() * juce::jmax (pos, origin);
    g.setColour (isEnabled() ? p.accent : p.textSub);
    g.fillRoundedRectangle (juce::Rectangle<float> (x0, track.getY(), juce::jmax (x1 - x0, 0.0f), track.getHeight()), 3.0f);
    const auto t = juce::Rectangle<float> (thumb, thumb).withCentre ({ track.getX() + track.getWidth() * pos, track.getCentreY() });
    g.setColour (p.text);
    g.fillEllipse (t);
    g.setColour (p.bg);
    g.drawEllipse (t, 2.0f);
    if (hasKeyboardFocus (true)) drawFocusRing (g, t, thumb * 0.5f);
}

SliderRow::SliderRow (const juce::String& l, std::function<juce::String (double)> f) : label (l), format (std::move (f))
{
    addAndMakeVisible (slider);
    slider.setTitle (label);
    slider.onValueChange = [this]
    {
        repaint();
        if (onChange) onChange (slider.getValue());
    };
}

void SliderRow::resized()
{
    auto r = getLocalBounds();
    if (stacked)
    {
        slider.setBounds (r.withTrimmedTop (r.getHeight() / 2));
        return;
    }
    r.removeFromLeft (labelWidth);
    r.removeFromRight (valueWidth);
    slider.setBounds (r);
}

void SliderRow::paint (juce::Graphics& g)
{
    const auto& p = P();
    auto r = getLocalBounds();
    if (stacked) r = r.removeFromTop (r.getHeight() / 2);
    drawText (g, label, stacked ? r : r.removeFromLeft (labelWidth), Theme::fontXS, p.textSub);
    drawText (g, format ? format (slider.getValue()) : juce::String (slider.getValue()), r.removeFromRight (valueWidth), Theme::fontXS, p.text,
              juce::Justification::centredRight, true, true);
}

ChipButton::ChipButton (const juce::String& text, bool f) : juce::Button (text), filterStyle (f)
{
    setWantsKeyboardFocus (true);
    setTitle (text);
}

int ChipButton::preferredWidth() const
{
    const float w = juce::GlyphArrangement::getStringWidth (Theme::ui (Theme::fontS, true), getButtonText());
    return int (std::ceil (w)) + Theme::space3 + Theme::space2 + (! filterStyle && getToggleState() ? 14 + Theme::space1 + 2 : 0);
}

void ChipButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto& p = P();
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const float rad = r.getHeight() * 0.5f;
    const bool on = getToggleState();
    auto ink = p.text;
    if (filterStyle && on)
    {
        g.setColour (p.accent);
        g.fillRoundedRectangle (r, rad);
        ink = p.onAccent;
    }
    else
    {
        if (on || highlighted || down) { g.setColour (p.raised); g.fillRoundedRectangle (r, rad); }
        g.setColour (on ? p.accent : p.border);
        g.drawRoundedRectangle (r.reduced (0.5f), rad, on ? Theme::borderWidth : 1.0f);
    }
    const auto f = Theme::ui (Theme::fontS, true);
    const float textW = juce::GlyphArrangement::getStringWidth (f, getButtonText());
    const bool check = on && ! filterStyle;
    const float iconW = check ? 14.0f + Theme::space1 : 0.0f;
    float x = juce::jmax (r.getX() + Theme::space2, r.getCentreX() - (iconW + textW) * 0.5f);
    if (check)
    {
        drawIcon (g, Icon::check, { x, r.getCentreY() - 7.0f, 14.0f, 14.0f }, p.accent, 2.8f);
        x += iconW;
    }
    g.setColour (ink);
    g.setFont (f);
    g.drawFittedText (getButtonText(), juce::Rectangle<float> (x, r.getY(), r.getRight() - Theme::space2 - x, r.getHeight()).toNearestInt(),
                      juce::Justification::centredLeft, 1, 0.8f);
    if (hasKeyboardFocus (true)) drawFocusRing (g, r, rad);
}

// =============================================================================================== panels
PanelBase::PanelBase (Navigator& n, const juce::String& t) : title (t), nav (n), closeButton (ja ("閉じる"), Icon::close, true)
{
    setWantsKeyboardFocus (true);
    setFocusContainerType (juce::Component::FocusContainerType::keyboardFocusContainer);
    addAndMakeVisible (closeButton);
    closeButton.onClick = [this] { nav.closeOverlay(); };
}

juce::Rectangle<int> PanelBase::contentArea() const
{
    return getLocalBounds().withTrimmedTop (headerHeight);
}

void PanelBase::resized()
{
    closeButton.setBounds (getWidth() - Theme::space4 - Theme::buttonH, (headerHeight - Theme::buttonH) / 2, Theme::buttonH, Theme::buttonH);
}

void PanelBase::paint (juce::Graphics& g)
{
    const auto& p = P();
    const auto r = getLocalBounds().toFloat();
    g.setColour (p.surface);
    g.fillRoundedRectangle (r, Theme::radiusL);
    g.setColour (p.divider);
    g.drawRoundedRectangle (r.reduced (0.5f), Theme::radiusL, 1.0f);
    auto header = getLocalBounds().removeFromTop (headerHeight).withTrimmedLeft (Theme::space4).withTrimmedRight (Theme::space4 * 2 + Theme::buttonH);
    const int titleW = int (std::ceil (juce::GlyphArrangement::getStringWidth (Theme::ui (Theme::fontL, true), title))) + Theme::space2;
    drawText (g, title, header.removeFromLeft (juce::jmin (titleW, header.getWidth())), Theme::fontL, p.text, juce::Justification::centredLeft, true);
    if (subtitle.isNotEmpty()) drawText (g, subtitle, header, Theme::fontS, p.textSub, juce::Justification::centredRight);
}

bool PanelBase::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey)
    {
        nav.closeOverlay();
        return true;
    }
    return false;
}

ConfirmPanel::ConfirmPanel (Navigator& n, const juce::String& t, const juce::String& m, const juce::String& okText,
                            std::function<void()> okFn, const juce::String& cancelText, std::function<void()> cancelFn)
    : PanelBase (n, t), message (m), ok (okText, PillButton::Style::primary), cancel (cancelText.isNotEmpty() ? cancelText : ja ("キャンセル"), PillButton::Style::outline),
      onOk (std::move (okFn)), onCancel (std::move (cancelFn))
{
    addAndMakeVisible (ok);
    addAndMakeVisible (cancel);
    ok.onClick = [this]
    {
        auto okCopy = onOk;
        auto textCopy = onOkText;
        const auto text = field != nullptr ? field->getText().trim() : juce::String();
        nav.closeOverlay(); // the panel stays alive until the next overlay change
        if (textCopy) textCopy (text);
        else if (okCopy) okCopy();
    };
    cancel.onClick = [this]
    {
        auto cancelCopy = onCancel;
        nav.closeOverlay();
        if (cancelCopy) cancelCopy();
    };
    setSize (480, 240);
}

void ConfirmPanel::addTextField (const juce::String& initial, std::function<void (const juce::String&)> fn)
{
    onOkText = std::move (fn);
    field = std::make_unique<juce::TextEditor>();
    field->setFont (Theme::ui (Theme::fontM));
    field->setText (initial, false);
    field->setInputRestrictions (kPresetNameMaxChars);
    field->onReturnKey = [this] { ok.triggerClick(); };
    field->setTitle (title);
    addAndMakeVisible (*field);
    setSize (getWidth(), getHeight() + Theme::controlH + Theme::space3);
}

void ConfirmPanel::resized()
{
    PanelBase::resized();
    auto r = contentArea().reduced (Theme::space4, 0).withTrimmedBottom (Theme::space4);
    auto buttons = r.removeFromBottom (Theme::buttonH);
    const int okW = juce::jmax (96, ok.preferredWidth()), cancelW = juce::jmax (96, cancel.preferredWidth());
    ok.setBounds (buttons.removeFromRight (okW));
    buttons.removeFromRight (Theme::space2);
    cancel.setBounds (buttons.removeFromRight (cancelW));
    if (field != nullptr)
    {
        r.removeFromBottom (Theme::space3);
        field->setBounds (r.removeFromBottom (Theme::controlH));
    }
}

void ConfirmPanel::paint (juce::Graphics& g)
{
    PanelBase::paint (g);
    auto r = contentArea().reduced (Theme::space4, 0).withTrimmedBottom (Theme::space4 + Theme::buttonH + Theme::space3);
    if (field != nullptr) r.removeFromBottom (Theme::controlH + Theme::space3);
    g.setColour (P().text);
    g.setFont (Theme::ui (Theme::fontS));
    g.drawFittedText (message, r, juce::Justification::topLeft, 5, 1.0f);
}
} // namespace koe::ui::mainui
