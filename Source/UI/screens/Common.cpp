#include "UI/screens/Common.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
#endif

#include <cmath>

namespace koe::ui::screens
{
namespace
{
const Palette& P() { return Theme::colours(); }
} // namespace

juce::Colour toneColour (Tone t)
{
    switch (t)
    {
        case Tone::text: return P().text;
        case Tone::sub: return P().textSub;
        case Tone::accent: return P().accent;
        case Tone::ok: return P().ok;
        case Tone::warn: return P().warn;
        case Tone::danger: return P().danger;
    }
    return P().text;
}

juce::String formatDb (double db, int decimals)
{
    const double r = std::abs (db) < 0.5 * std::pow (10.0, -decimals) ? 0.0 : db;
    return (r > 0.0 ? "+" : "") + juce::String (r, decimals) + " dB";
}

int badgeWidth (const juce::String& text, bool mono)
{
    return int (std::ceil (textRunWidth (text, Theme::fontXS, true, mono))) + Theme::space3 + 2;
}

void paintBadge (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, Tone tone, bool mono)
{
    g.setColour (P().border);
    g.drawRoundedRectangle (r.toFloat().reduced (0.5f), Theme::radiusS, 1.0f);
    drawText (g, text, r.reduced (Theme::space2, 0), Theme::fontXS, toneColour (tone), juce::Justification::centred, true, mono);
}

void drawDashedRoundedRect (juce::Graphics& g, juce::Rectangle<float> r, float radius, float thickness, juce::Colour c)
{
    juce::Path outline, dashed;
    outline.addRoundedRectangle (r.reduced (thickness * 0.5f), radius);
    const float dashes[] = { float (Theme::space1) + thickness, float (Theme::space1) };
    juce::PathStrokeType (thickness).createDashedStroke (dashed, outline, dashes, 2);
    g.setColour (c);
    g.fillPath (dashed);
}

juce::Component* findById (juce::Component& root, const juce::String& id)
{
    if (root.getComponentID() == id) return &root;
    for (auto* c : root.getChildren())
        if (auto* f = findById (*c, id)) return f;
    return nullptr;
}

// =============================================================================================== TextLabel
TextLabel::TextLabel (const juce::String& t, float s, Tone to, bool b, bool mo) : text (t), size (s), tone (to), bold (b), mono (mo)
{
    setInterceptsMouseClicks (false, false);
}

void TextLabel::setText (const juce::String& t)
{
    if (t == text) return;
    text = t;
    repaint();
}

juce::TextLayout TextLabel::layoutFor (float width) const
{
    juce::AttributedString s;
    s.setWordWrap (juce::AttributedString::byWord);
    s.setJustification (juce::Justification (just.getOnlyHorizontalFlags() | juce::Justification::top));
    appendText (s, text, size, bold, mono, toneColour (tone));
    juce::TextLayout tl;
    tl.createLayout (s, juce::jmax (1.0f, width));
    return tl;
}

int TextLabel::singleLineWidth() const
{
    const int iconW = icon ? m::iconS + Theme::space1 : 0;
    return iconW + int (std::ceil (textRunWidth (text, size, bold, mono))) + 2;
}

int TextLabel::heightForWidth (int width) const
{
    if (text.isEmpty()) return 0;
    const int iconW = icon ? m::iconS + Theme::space1 : 0;
    const auto tl = layoutFor (float (width - iconW));
    float h = tl.getHeight();
    if (maxLines > 0 && tl.getNumLines() > maxLines)
        h = tl.getLine (maxLines - 1).getLineBoundsY().getEnd();
    return int (std::ceil (h)) + 1;
}

void TextLabel::paint (juce::Graphics& g)
{
    if (text.isEmpty()) return;
    auto r = getLocalBounds().toFloat();
    const int iconW = icon ? m::iconS + Theme::space1 : 0;
    const auto tl = layoutFor (r.getWidth() - float (iconW));
    const float h = juce::jmin (r.getHeight(), tl.getHeight());
    float y = 0.0f;
    if (just.testFlags (juce::Justification::verticallyCentred)) y = (r.getHeight() - h) * 0.5f;
    else if (just.testFlags (juce::Justification::bottom)) y = r.getHeight() - h;
    if (icon)
    {
        const float firstLineH = tl.getNumLines() > 0 ? tl.getLine (0).getLineBoundsY().getLength() : size;
        float ix = 0.0f;
        if (just.testFlags (juce::Justification::horizontallyCentred) && tl.getNumLines() == 1)
            ix = (r.getWidth() - float (singleLineWidth())) * 0.5f;
        drawIcon (g, *icon, { ix, y + (firstLineH - float (m::iconS)) * 0.5f, float (m::iconS), float (m::iconS) }, toneColour (tone));
        if (ix > 0.0f)
        {
            tl.draw (g, { ix + float (iconW), y, r.getWidth() - ix - float (iconW), h + 1.0f });
            return;
        }
    }
    g.saveState();
    g.reduceClipRegion (getLocalBounds());
    tl.draw (g, { float (iconW), y, r.getWidth() - float (iconW), tl.getHeight() });
    g.restoreState();
}

// =============================================================================================== NavButton
NavButton::NavButton (const juce::String& text) : juce::Button (text)
{
    setWantsKeyboardFocus (true);
}

void NavButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto& p = P();
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool on = getToggleState();
    if (on || down) { g.setColour (p.raised); g.fillRoundedRectangle (r, Theme::radiusM); }
    else if (highlighted) { g.setColour (p.raised.withAlpha (0.5f)); g.fillRoundedRectangle (r, Theme::radiusM); }
    auto area = getLocalBounds().reduced (Theme::space3, 0);
    if (count >= 0)
        drawText (g, juce::String (count), area.removeFromRight (Theme::space5), Theme::fontXS, on ? p.text : p.textSub,
                  juce::Justification::centredRight, false, true);
    drawText (g, getButtonText(), area, Theme::fontS, on ? p.text : p.textSub, juce::Justification::centredLeft, on);
    if (hasKeyboardFocus (true)) drawFocusRing (g, r, Theme::radiusM);
}

bool NavButton::keyPressed (const juce::KeyPress& k)
{
    const bool up = k.isKeyCode (juce::KeyPress::upKey), downKey = k.isKeyCode (juce::KeyPress::downKey);
    if ((up || downKey) && getParentComponent() != nullptr)
    {
        juce::Array<NavButton*> siblings;
        for (auto* c : getParentComponent()->getChildren())
            if (auto* b = dynamic_cast<NavButton*> (c); b != nullptr && b->isVisible() && b->isEnabled()) siblings.add (b);
        const int i = siblings.indexOf (this) + (up ? -1 : 1);
        if (auto* next = siblings[i])
        {
            next->grabKeyboardFocus();
            if (next->onClick) next->onClick();
        }
        return true;
    }
    return juce::Button::keyPressed (k);
}

// =============================================================================================== NumberField
NumberField::NumberField (double mn, double mx, double st, int dec, const juce::String& u) : min (mn), max (mx), step (st), decimals (dec), unit (u)
{
    setInputRestrictions (7, "0123456789.");
    setFont (Theme::mono (Theme::fontS, true));
    setJustification (juce::Justification::centredLeft);
    setBorder ({ 0, Theme::space2 + Theme::space1, 0, Theme::space4 + Theme::space1 });
    setSelectAllWhenFocused (true);
    onReturnKey = [this] { commit(); };
    onFocusLost = [this] { commit(); };
    onEscapeKey = [this] { setValue (value); };
}

void NumberField::setValue (double v, bool notify)
{
    v = juce::jlimit (min, max, step > 0.0 ? std::round (v / step) * step : v);
    const bool changed = std::abs (v - value) > 1.0e-9;
    value = v;
    setText (juce::String (value, decimals), false);
    if (notify && changed && onValueChange) onValueChange (value);
}

void NumberField::commit()
{
    const auto t = getText().trim();
    if (t.isEmpty()) setValue (value);
    else setValue (t.getDoubleValue(), true);
}

bool NumberField::keyPressed (const juce::KeyPress& k)
{
    if (k.isKeyCode (juce::KeyPress::upKey) || k.isKeyCode (juce::KeyPress::downKey))
    {
        const double mult = k.getModifiers().isShiftDown() ? 10.0 : 1.0;
        setValue (value + (k.isKeyCode (juce::KeyPress::upKey) ? step : -step) * mult, true);
        selectAll();
        return true;
    }
    return juce::TextEditor::keyPressed (k);
}

void NumberField::paintOverChildren (juce::Graphics& g)
{
    juce::TextEditor::paintOverChildren (g);
    drawText (g, unit, getLocalBounds().removeFromRight (Theme::space4 + Theme::space1).withTrimmedRight (Theme::space2), Theme::fontXS,
              P().textSub, juce::Justification::centredRight);
}

// =============================================================================================== SettingRow
SettingRow::SettingRow (const juce::String& t, const juce::String& d, std::unique_ptr<juce::Component> c, int pw, int mw,
                        std::function<int (int)> hf)
    : title (t, Theme::fontS, Tone::text, true), desc (d, Theme::fontXS, Tone::sub), control (std::move (c)), prefW (pw), minW (mw),
      heightFor (std::move (hf))
{
    addAndMakeVisible (title);
    addAndMakeVisible (desc);
    if (control != nullptr) addAndMakeVisible (*control);
}

bool SettingRow::stacked (int width) const { return width - minW - Theme::space4 < m::labelMinW; }

int SettingRow::controlWidth (int width) const
{
    if (stacked (width)) return juce::jmin (prefW, width);
    return juce::jlimit (minW, prefW, width - m::labelMinW - Theme::space4);
}

int SettingRow::heightForWidth (int width) const
{
    const int cw = controlWidth (width);
    const int ch = control != nullptr ? heightFor (cw) : 0;
    if (stacked (width))
    {
        const int th = title.heightForWidth (width) + Theme::space1 / 2 + desc.heightForWidth (width);
        return th + (ch > 0 ? Theme::space2 + ch : 0) + m::rowPadY * 2;
    }
    const int lw = juce::jmin (m::labelMaxW, width - cw - Theme::space4);
    const int th = title.heightForWidth (lw) + Theme::space1 / 2 + desc.heightForWidth (lw);
    return juce::jmax (th, ch) + m::rowPadY * 2;
}

void SettingRow::resized()
{
    auto r = getLocalBounds().reduced (0, m::rowPadY);
    const int w = r.getWidth();
    const int cw = controlWidth (w);
    const int ch = control != nullptr ? heightFor (cw) : 0;
    if (stacked (w))
    {
        title.setBounds (r.removeFromTop (title.heightForWidth (w)));
        r.removeFromTop (Theme::space1 / 2);
        desc.setBounds (r.removeFromTop (desc.heightForWidth (w)));
        if (control != nullptr)
        {
            r.removeFromTop (Theme::space2);
            control->setBounds (r.removeFromTop (ch).withWidth (cw));
        }
        return;
    }
    const int lw = juce::jmin (m::labelMaxW, w - cw - Theme::space4);
    const int th = title.heightForWidth (lw), dh = desc.heightForWidth (lw);
    const int textH = th + Theme::space1 / 2 + dh;
    auto label = r.withWidth (lw).withSizeKeepingCentre (lw, textH);
    title.setBounds (label.removeFromTop (th));
    label.removeFromTop (Theme::space1 / 2);
    desc.setBounds (label.removeFromTop (dh));
    if (control != nullptr) control->setBounds (r.removeFromRight (cw).withSizeKeepingCentre (cw, ch));
}

void SettingRow::paint (juce::Graphics& g)
{
    if (! divider) return;
    g.setColour (P().divider);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);
}

// =============================================================================================== SectionCard
SectionCard::SectionCard (const juce::String& t, const juce::String& s) : title (t, Theme::fontM, Tone::text, true), subtitle (s, Theme::fontXS, Tone::sub)
{
    title.setJustification (juce::Justification::centredLeft);
    subtitle.setJustification (juce::Justification::centredLeft);
    subtitle.setMaxLines (1);
    addAndMakeVisible (title);
    addAndMakeVisible (subtitle);
}

SettingRow& SectionCard::addRow (std::unique_ptr<SettingRow> row)
{
    auto& r = *row;
    auto* raw = row.get();
    items.push_back ({ std::move (row), [raw] (int w) { return raw->heightForWidth (w); } });
    addAndMakeVisible (r);
    return r;
}

void SectionCard::addBlock (std::unique_ptr<juce::Component> c, std::function<int (int)> heightFor)
{
    addAndMakeVisible (*c);
    items.push_back ({ std::move (c), std::move (heightFor) });
}

void SectionCard::setHeaderComponent (std::unique_ptr<juce::Component> c, int width)
{
    headerComp = std::move (c);
    headerCompW = width;
    addAndMakeVisible (*headerComp);
}

int SectionCard::heightForWidth (int width) const
{
    const int inner = width - m::cardPadX * 2;
    int h = Theme::space2 + m::cardHeaderH + Theme::space3;
    for (auto& it : items)
    {
        const bool isRow = dynamic_cast<SettingRow*> (it.comp.get()) != nullptr;
        h += it.heightFor (inner) + (isRow ? 0 : Theme::space2 * 2);
    }
    return h;
}

void SectionCard::resized()
{
    auto r = getLocalBounds().reduced (m::cardPadX, 0);
    r.removeFromTop (Theme::space2);
    auto header = r.removeFromTop (m::cardHeaderH);
    if (headerComp != nullptr)
    {
        headerComp->setBounds (header.removeFromRight (juce::jmin (headerCompW, header.getWidth() / 2))
                                   .withSizeKeepingCentre (juce::jmin (headerCompW, header.getWidth() / 2), Theme::buttonH));
        header.removeFromRight (Theme::space2);
    }
    const int tw = juce::jmin (title.singleLineWidth(), header.getWidth());
    title.setBounds (header.removeFromLeft (tw));
    header.removeFromLeft (Theme::space2);
    subtitle.setBounds (header);
    const int inner = r.getWidth();
    SettingRow* lastRow = nullptr;
    for (auto& it : items)
    {
        const bool isRow = dynamic_cast<SettingRow*> (it.comp.get()) != nullptr;
        if (! isRow) r.removeFromTop (Theme::space2);
        it.comp->setBounds (r.removeFromTop (it.heightFor (inner)));
        if (! isRow) r.removeFromTop (Theme::space2);
        if (auto* row = dynamic_cast<SettingRow*> (it.comp.get()))
        {
            row->setDivider (true);
            lastRow = row;
        }
        else if (lastRow != nullptr)
        {
            lastRow->setDivider (false);
            lastRow = nullptr;
        }
    }
    if (lastRow != nullptr) lastRow->setDivider (false);
}

void SectionCard::paint (juce::Graphics& g)
{
    g.setColour (P().surface);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), Theme::radiusL);
}

// =============================================================================================== CardColumn
SectionCard& CardColumn::addCard (std::unique_ptr<SectionCard> card)
{
    addAndMakeVisible (*card);
    cards.push_back (std::move (card));
    return *cards.back();
}

int CardColumn::heightForWidth (int width) const
{
    int h = 0;
    for (auto& c : cards) h += c->heightForWidth (width) + (h > 0 ? Theme::space3 : 0);
    return h;
}

void CardColumn::resized()
{
    // the first card takes any spare height, like the mock's flex-grow card
    auto r = getLocalBounds();
    int extra = juce::jmax (0, getHeight() - heightForWidth (getWidth()));
    for (auto& c : cards)
    {
        c->setBounds (r.removeFromTop (c->heightForWidth (r.getWidth()) + extra));
        extra = 0;
        r.removeFromTop (Theme::space3);
    }
}

// =============================================================================================== FlowBox
void FlowBox::add (std::unique_ptr<juce::Component> c, int w, int h)
{
    add (*c, w, h);
    owned.push_back (std::move (c));
}

void FlowBox::add (juce::Component& c, int w, int h)
{
    addAndMakeVisible (c);
    items.push_back ({ &c, w, h });
}

int FlowBox::naturalWidth() const
{
    int w = 0;
    for (auto& it : items) w += it.w + (w > 0 ? gap : 0);
    return w;
}

int FlowBox::heightForWidth (int width) const
{
    int x = 0, lineH = 0, h = 0;
    for (auto& it : items)
    {
        if (x > 0 && x + it.w > width)
        {
            h += lineH + gap;
            x = 0;
            lineH = 0;
        }
        x += it.w + gap;
        lineH = juce::jmax (lineH, it.h);
    }
    return h + lineH;
}

void FlowBox::resized()
{
    int x = 0, y = 0, lineH = 0;
    const int width = getWidth();
    // first pass: line heights, so items on a line can be centred vertically
    std::vector<int> lineOf (items.size()), heights;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (x > 0 && x + items[i].w > width)
        {
            heights.push_back (lineH);
            x = 0;
            lineH = 0;
        }
        lineOf[i] = int (heights.size());
        x += items[i].w + gap;
        lineH = juce::jmax (lineH, items[i].h);
    }
    heights.push_back (lineH);
    x = 0;
    int line = 0;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (lineOf[i] != line)
        {
            y += heights[size_t (line)] + gap;
            line = lineOf[i];
            x = 0;
        }
        const int w = juce::jmin (items[i].w, width);
        items[i].c->setBounds (x, y + (heights[size_t (line)] - items[i].h) / 2, w, items[i].h);
        x += w + gap;
    }
}

// =============================================================================================== Segmented
Segmented::Segmented (const juce::StringArray& labels)
{
    for (int i = 0; i < labels.size(); ++i)
    {
        auto* b = buttons.add (new PillButton (labels[i], PillButton::Style::tab));
        b->onClick = [this, i] { setSelected (i, true); };
        addAndMakeVisible (b);
    }
}

void Segmented::setSelected (int index, bool notify)
{
    const bool changed = index != selected;
    selected = index;
    for (int i = 0; i < buttons.size(); ++i)
    {
        buttons[i]->setToggleState (i == index, juce::dontSendNotification);
        buttons[i]->setStyle (i == index ? PillButton::Style::tab : PillButton::Style::ghost);
    }
    if (notify && changed && onChange) onChange (index);
}

int Segmented::preferredWidth() const
{
    int w = 0;
    for (auto* b : buttons) w += b->preferredWidth() + Theme::space1;
    return w;
}

void Segmented::resized()
{
    auto r = getLocalBounds();
    const int total = preferredWidth(), full = r.getWidth();
    for (auto* b : buttons)
    {
        const int w = total > 0 ? full * (b->preferredWidth() + Theme::space1) / total : 0;
        b->setBounds (r.removeFromLeft (b == buttons.getLast() ? r.getWidth() : w).reduced (Theme::space1 / 2, 0));
    }
}

// =============================================================================================== InlinePrompt
InlinePrompt::InlinePrompt (const juce::String& t, const juce::String& msg, bool withInput, const juce::String& initial,
                            const juce::String& okLabel, bool dangerous, std::function<void (const juce::String&)> ok,
                            std::function<void()> cancelFn)
    : title (t, Theme::fontM, Tone::text, true), message (msg, Theme::fontS, Tone::sub),
      okButton (okLabel, dangerous ? PillButton::Style::danger : PillButton::Style::primary),
      cancelButton (ja ("キャンセル"), PillButton::Style::outline), onOk (std::move (ok)), onCancel (std::move (cancelFn))
{
    setWantsKeyboardFocus (true);
    addAndMakeVisible (title);
    addAndMakeVisible (message);
    if (withInput)
    {
        input = std::make_unique<juce::TextEditor>();
        input->setFont (Theme::ui (Theme::fontS));
        input->setText (initial, false);
        input->setIndents (Theme::space2, (Theme::controlH - int (Theme::fontS) - Theme::space1) / 2);
        input->onReturnKey = [this] { this->ok(); };
        input->onEscapeKey = [this] { cancel(); };
        addAndMakeVisible (*input);
    }
    addAndMakeVisible (okButton);
    addAndMakeVisible (cancelButton);
    okButton.onClick = [this] { this->ok(); };
    cancelButton.onClick = [this] { cancel(); };
}

void InlinePrompt::parentHierarchyChanged()
{
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<InlinePrompt> (this)]
    {
        if (safe == nullptr || ! safe->isShowing()) return;
        if (safe->input != nullptr)
        {
            safe->input->grabKeyboardFocus();
            safe->input->selectAll();
        }
        else
            safe->okButton.grabKeyboardFocus();
    });
}

void InlinePrompt::ok()
{
    auto cb = onOk;
    const auto text = input != nullptr ? input->getText().trim() : juce::String();
    if (cb) cb (text); // may delete this
}

void InlinePrompt::cancel()
{
    auto cb = onCancel;
    if (cb) cb(); // may delete this
}

bool InlinePrompt::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { cancel(); return true; }
    if (k == juce::KeyPress::returnKey) { ok(); return true; }
    return false;
}

juce::Rectangle<int> InlinePrompt::panelBounds() const
{
    const int w = juce::jmin (m::comboW + Theme::space5 * 4, getWidth() - Theme::space4 * 2);
    const int inner = w - Theme::space4 * 2;
    const int h = Theme::space4 + title.heightForWidth (inner) + Theme::space2 + message.heightForWidth (inner)
                  + (input != nullptr ? Theme::space3 + Theme::controlH : 0) + Theme::space4 + Theme::buttonH + Theme::space4;
    return getLocalBounds().withSizeKeepingCentre (w, juce::jmin (h, getHeight()));
}

void InlinePrompt::resized()
{
    auto r = panelBounds().reduced (Theme::space4);
    title.setBounds (r.removeFromTop (title.heightForWidth (r.getWidth())));
    r.removeFromTop (Theme::space2);
    message.setBounds (r.removeFromTop (message.heightForWidth (r.getWidth())));
    if (input != nullptr)
    {
        r.removeFromTop (Theme::space3);
        input->setBounds (r.removeFromTop (Theme::controlH));
    }
    auto buttons = r.removeFromBottom (Theme::buttonH);
    okButton.setBounds (buttons.removeFromRight (juce::jmax (okButton.preferredWidth(), Theme::space5 * 3)));
    buttons.removeFromRight (Theme::space2);
    cancelButton.setBounds (buttons.removeFromRight (juce::jmax (cancelButton.preferredWidth(), Theme::space5 * 3)));
}

void InlinePrompt::paint (juce::Graphics& g)
{
    g.fillAll (P().overlay);
    const auto pb = panelBounds().toFloat();
    g.setColour (P().surface);
    g.fillRoundedRectangle (pb, Theme::radiusL);
    g.setColour (P().border);
    g.drawRoundedRectangle (pb.reduced (0.5f), Theme::radiusL, 1.0f);
}

// =============================================================================================== OverlayPanel
OverlayPanel::OverlayPanel (const juce::String& t, std::function<void()> close)
    : title (t, Theme::fontL, Tone::text, true), closeButton (ja ("閉じる"), Icon::close), onClose (std::move (close))
{
    setWantsKeyboardFocus (true);
    title.setJustification (juce::Justification::centredLeft);
    title.setMaxLines (1);
    addAndMakeVisible (title);
    addAndMakeVisible (closeButton);
    closeButton.setComponentID ("overlay.close");
    closeButton.onClick = [this] { if (onClose) onClose(); };
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
}

void OverlayPanel::setContent (std::unique_ptr<juce::Component> c, std::function<int (int)> hf)
{
    content = std::move (c);
    heightFor = std::move (hf);
    viewport.setViewedComponent (content.get(), false);
    resized();
}

void OverlayPanel::resized()
{
    auto r = getLocalBounds().reduced (Theme::space4, Theme::space3);
    auto header = r.removeFromTop (Theme::controlH);
    closeButton.setBounds (header.removeFromRight (Theme::controlH));
    header.removeFromRight (Theme::space2);
    title.setBounds (header);
    r.removeFromTop (Theme::space3);
    viewport.setBounds (r);
    if (content != nullptr)
    {
        const int w = r.getWidth() - viewport.getScrollBarThickness() - Theme::space2;
        content->setSize (juce::jmax (1, w), heightFor ? heightFor (w) : content->getHeight());
    }
}

void OverlayPanel::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (P().surface);
    g.fillRoundedRectangle (r, Theme::radiusL);
    g.setColour (P().border);
    g.drawRoundedRectangle (r.reduced (0.5f), Theme::radiusL, 1.0f);
}

bool OverlayPanel::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey)
    {
        if (onClose) onClose();
        return true;
    }
    return false;
}

// =============================================================================================== VStack
juce::Component& VStack::add (std::unique_ptr<juce::Component> c, std::function<int (int)> h)
{
    addAndMakeVisible (*c);
    items.push_back ({ std::move (c), std::move (h) });
    return *items.back().c;
}

TextLabel& VStack::addText (const juce::String& t, float size, Tone tone, bool bold)
{
    auto l = std::make_unique<TextLabel> (t, size, tone, bold);
    auto* raw = l.get();
    add (std::move (l), [raw] (int w) { return raw->heightForWidth (w); });
    return *raw;
}

int VStack::heightForWidth (int width) const
{
    int h = 0;
    for (auto& it : items)
    {
        if (! it.c->isVisible()) continue;
        h += it.h (width) + (h > 0 ? gap : 0);
    }
    return h;
}

void VStack::resized()
{
    auto r = getLocalBounds();
    bool first = true;
    for (auto& it : items)
    {
        if (! it.c->isVisible()) continue;
        if (! first) r.removeFromTop (gap);
        first = false;
        it.c->setBounds (r.removeFromTop (it.h (r.getWidth())));
    }
}

// =============================================================================================== NumberedStep
NumberedStep::NumberedStep (int n, const juce::String& text) : number (n), label (text, Theme::fontS)
{
    addAndMakeVisible (label);
}

int NumberedStep::heightForWidth (int width) const
{
    return juce::jmax (Theme::space4, label.heightForWidth (width - Theme::space4 - Theme::space3));
}

void NumberedStep::resized()
{
    auto r = getLocalBounds();
    r.removeFromLeft (Theme::space4 + Theme::space3);
    const int lh = label.heightForWidth (r.getWidth());
    label.setBounds (r.withHeight (lh).translated (0, juce::jmax (0, (Theme::space4 - lh) / 2)));
}

void NumberedStep::paint (juce::Graphics& g)
{
    const auto circle = juce::Rectangle<float> (0.0f, 0.0f, float (Theme::space4), float (Theme::space4)).reduced (1.0f);
    g.setColour (P().border);
    g.drawEllipse (circle, Theme::borderWidth);
    drawText (g, juce::String (number), circle.toNearestInt(), Theme::fontXS, P().text, juce::Justification::centred, true, true);
}

// =============================================================================================== NoteBox
NoteBox::NoteBox (const juce::String& text, Tone t) : tone (t), label (text, Theme::fontS)
{
    label.setIcon (t == Tone::ok ? Icon::check : Icon::warning);
    addAndMakeVisible (label);
}

int NoteBox::heightForWidth (int width) const { return label.heightForWidth (width - Theme::space3 * 2) + (Theme::space2 + Theme::space1) * 2; }

void NoteBox::resized() { label.setBounds (getLocalBounds().reduced (Theme::space3, Theme::space2 + Theme::space1)); }

void NoteBox::paint (juce::Graphics& g)
{
    g.setColour (toneColour (tone));
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (Theme::borderWidth * 0.5f), Theme::radiusM, Theme::borderWidth);
}

// =============================================================================================== hotkeys
bool keyPressToHotkey (const juce::KeyPress& key, int& modifiers, int& virtualKey)
{
    const int code = key.getKeyCode();
    const auto mods = key.getModifiers();
    modifiers = (mods.isAltDown() ? 1 : 0) | (mods.isCtrlDown() ? 2 : 0) | (mods.isShiftDown() ? 4 : 0);
    virtualKey = 0;
    if (code >= 'a' && code <= 'z') virtualKey = code - 'a' + 'A';
    else if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) virtualKey = code;
    else if ((code >> 16) == 1) virtualKey = code & 0xffff; // JUCE's extended keys: arrows, F1-F24, numpad, Delete...
    else if (code == juce::KeyPress::spaceKey || code == juce::KeyPress::returnKey || code == juce::KeyPress::backspaceKey
             || code == juce::KeyPress::tabKey || code == juce::KeyPress::escapeKey)
        virtualKey = code;
#if JUCE_WINDOWS
    else if (code > 0x20 && code < 0x7f)
    {
        // punctuation arrives as its unshifted character; map it back with the current keyboard layout
        const SHORT s = VkKeyScanW (WCHAR (code));
        if (s != -1) virtualKey = s & 0xff;
    }
#endif
    // modifier keys alone (Shift, Ctrl, Alt, Win) are never a hotkey
    if (virtualKey == 0x10 || virtualKey == 0x11 || virtualKey == 0x12 || virtualKey == 0x5b || virtualKey == 0x5c) virtualKey = 0;
    return virtualKey != 0 && virtualKey < 0x100;
}

bool hotkeyNeedsModifier (int vk)
{
    return (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || vk == 0x20 || vk == 0x0d || vk == 0x08 || vk == 0x09
           || (vk >= 0xba && vk <= 0xc0) || (vk >= 0xdb && vk <= 0xdf) || vk == 0xe2;
}

// =============================================================================================== shared texts
juce::StringArray discordSteps()
{
    return { ja ("Discord を開き、ユーザー設定の「音声・ビデオ」を選びます。"),
             ja ("「入力デバイス」を「CABLE Output (VB-Audio Virtual Cable)」にします。"),
             ja ("「マイクテスト」を押して、ここで選んだプリセットの声が返ってくることを確かめます。") };
}

juce::String discordRecommended()
{
    return ja ("Discord 側の推奨: 「ノイズ抑制」は「なし」、「エコー除去」と「自動音量調整」は OFF にします。加工した声を、誤って抑えたり補正したりすることがあるためです。");
}

juce::String revertMicText() { return ja ("使い終わったら、Discord の入力デバイスを元のマイクに戻します。"); }
} // namespace koe::ui::screens
