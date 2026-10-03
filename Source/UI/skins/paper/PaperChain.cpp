#include "UI/skins/paper/PaperChain.h"

#include "Effects/EffectRegistry.h"

#include <cmath>

namespace koe::ui::paper
{
using namespace mainui;

namespace
{
const Palette& P() { return Theme::colours(); }

constexpr int kHead = 20, kRow = 28, kRowText = 16, kRowGap = Theme::space1, kGap = Theme::space1, kScroll = Theme::space2;
int padX (bool cp) { return cp ? Theme::space2 : Theme::space2 + 2; }
int padY (bool cp) { return cp ? 6 : Theme::space2; }
int bottomH (bool cp) { return cp ? Theme::space4 : Theme::space4 + 2; }
int markerGap (bool cp) { return cp ? Theme::space1 : 6; }
int boxH (bool cp, int n) { return padY (cp) * 2 + kHead + kGap + n * kRow + juce::jmax (0, n - 1) * kRowGap + kGap + bottomH (cp); }

int heavyOnCount (const std::vector<SlotDef>& chain)
{
    int n = 0;
    for (auto& s : chain)
        if (auto* i = findEffectInfo (s.type); i != nullptr && s.enabled && i->weight == EffectWeight::heavy) ++n;
    return n;
}

/** "ロボ（リングモジュレーション）" -> "ロボ" when the whole name does not fit. */
juce::String fitName (const juce::String& name, const juce::Font& f, int width)
{
    const auto paren = juce::String::fromUTF8 ("（");
    if (juce::GlyphArrangement::getStringWidth (f, name) <= float (width) || ! name.contains (paren)) return name;
    return name.upToFirstOccurrenceOf (paren, false, false).trimEnd();
}
} // namespace

// =============================================================================================== PaperSlotCard
int PaperSlotCard::height (bool cp, int n) { return markerH (cp) + markerGap (cp) + boxH (cp, n); }

PaperSlotCard::PaperSlotCard (PaperChain& ch, AppController& ctl, Navigator& n, int i)
    : index (i), chain (ch), c (ctl), nav (n), left (ja ("左へ"), Icon::chevronLeft), right (ja ("右へ"), Icon::chevronRight),
      remove (ja ("削除"), Icon::close)
{
    setWantsKeyboardFocus (true);
    setComponentID ("chain.slot." + juce::String (index));
    setTitle (ja ("スロット ") + juce::String (index + 1));
    toggle.setTitle (ja ("スロット ") + juce::String (index + 1) + " ON/OFF");
    toggle.setComponentID ("chain.slot." + juce::String (index) + ".toggle");
    toggle.onClick = [this]
    {
        juce::String why;
        if (! c.setSlotEnabled (index, toggle.getToggleState(), why))
        {
            toggle.setToggleState (! toggle.getToggleState(), juce::dontSendNotification);
            nav.showToast (why);
        }
    };
    addAndMakeVisible (toggle);
    left.onClick = [this] { withLooperCheck (c, nav, *this, [this] { c.moveSlot (index, index - 1); }); };
    right.onClick = [this] { withLooperCheck (c, nav, *this, [this] { c.moveSlot (index, index + 1); }); };
    remove.onClick = [this] { withLooperCheck (c, nav, *this, [this] { c.removeSlot (index); }); };
    for (auto* b : { &left, &right, &remove }) addAndMakeVisible (*b);
}

void PaperSlotCard::update (const SlotDef& slot, bool stopped)
{
    const auto* newInfo = findEffectInfo (slot.type);
    const int heavyOn = heavyOnCount (c.getChain());
    const int context = int (c.getChain().size()) * 16 + heavyOn;
    if (newInfo == info && info != nullptr && slot == last && stopped == autoStopped && context == lastContext) return;
    last = slot;
    lastContext = context;
    if (newInfo != info)
    {
        info = newInfo;
        bars.clear();
        paramIndex = info != nullptr ? mainParams (*info) : std::vector<int>();
        for (int pi : paramIndex)
        {
            const auto& spec = info->params[size_t (pi)];
            auto* b = bars.add (new PaperSlider (PaperSlider::Style::bar));
            applyParamRange (*b, spec, spec.def);
            b->setTitle (juce::String::fromUTF8 (spec.nameJa));
            b->setTooltip (paramLabel (spec));
            b->onValueChange = [this, pi, b] { c.setSlotParam (index, pi, float (b->getValue())); repaint(); };
            addAndMakeVisible (b);
        }
        const auto name = info != nullptr ? juce::String::fromUTF8 (info->nameJa) : juce::String (slot.type);
        const auto desc = info != nullptr ? juce::String::fromUTF8 (info->descJa) : ja ("この版では使えないエフェクトです");
        setTooltip (name + ja ("：") + desc + ja ("（押すと全パラメータを編集）"));
        resized();
    }
    for (size_t k = 0; k < paramIndex.size(); ++k)
        if (size_t (paramIndex[k]) < slot.params.size()) bars[int (k)]->setValue (slot.params[size_t (paramIndex[k])], juce::dontSendNotification);
    enabled = slot.enabled;
    autoStopped = stopped;
    toggle.setToggleState (slot.enabled, juce::dontSendNotification);
    const bool blocked = info != nullptr && info->weight == EffectWeight::heavy && ! slot.enabled && heavyOn >= kMaxHeavyOn;
    toggle.setTooltip (blocked ? ja ("重いエフェクトは同時に 2 つまでしか ON にできません。ほかの重いエフェクトを OFF にしてください。")
                               : ja ("スロット ") + juce::String (index + 1) + ja (" の ON/OFF"));
    left.setEnabled (index > 0);
    right.setEnabled (index < int (c.getChain().size()) - 1);
    repaint();
}

void PaperSlotCard::setCompact (bool cp)
{
    if (compact == cp) return;
    compact = cp;
    resized();
    repaint();
}

juce::Rectangle<int> PaperSlotCard::box() const { return getLocalBounds().withTrimmedTop (markerH (compact) + markerGap (compact)); }

int PaperSlotCard::barsShown() const
{
    const int fit = box().getHeight() >= boxH (compact, 2) ? 2 : box().getHeight() >= boxH (compact, 1) ? 1 : 0;
    return juce::jmin (fit, bars.size());
}

void PaperSlotCard::resized()
{
    auto r = box().reduced (padX (compact), padY (compact));
    auto bottom = r.removeFromBottom (bottomH (compact));
    const int bs = compact ? Theme::space4 - 2 : Theme::space4;
    for (auto* b : { &remove, &right, &left })
    {
        b->setBounds (bottom.removeFromRight (bs).withSizeKeepingCentre (bs, bs));
        bottom.removeFromRight (Theme::space1);
    }
    toggle.setBounds (bottom.removeFromLeft (juce::jmin (bottom.getWidth(), Theme::toggleW + Theme::space2 + (compact ? 20 : 28))));
    r.removeFromTop (kHead + kGap);
    const int shown = barsShown();
    for (int k = 0; k < bars.size(); ++k)
    {
        bars[k]->setVisible (k < shown);
        if (k >= shown) continue;
        auto row = r.removeFromTop (kRow);
        r.removeFromTop (kRowGap);
        bars[k]->setBounds (row.withTrimmedTop (kRowText));
    }
}

void PaperSlotCard::paint (juce::Graphics& g)
{
    const auto& p = P();
    // the processing number on the timeline (hollow while the slot is OFF)
    const int mh = markerH (compact), d = mh - 2;
    const auto marker = juce::Rectangle<float> (float (d), float (d)).withCentre ({ getWidth() * 0.5f, mh * 0.5f });
    g.setColour (enabled ? p.accent : p.surface);
    g.fillEllipse (marker);
    if (! enabled)
    {
        g.setColour (p.border);
        g.drawEllipse (marker.reduced (0.75f), 1.5f);
    }
    drawText (g, juce::String (index + 1), marker.toNearestInt(), Theme::fontXS, enabled ? p.onAccent : p.textSub, juce::Justification::centred, true, true);

    auto b = box();
    auto bf = b.toFloat().reduced (0.5f);
    g.setColour (p.surface);
    g.fillRoundedRectangle (bf, Theme::radiusS);
    g.setColour (hasKeyboardFocus (false) || dragging ? p.accent : p.border);
    g.drawRoundedRectangle (bf, Theme::radiusS, 1.0f);

    auto r = b.reduced (padX (compact), padY (compact));
    auto header = r.removeFromTop (kHead);
    if (info != nullptr)
    {
        const auto badge = juce::String::fromUTF8 (weightNameJa (info->weight));
        const int bw = juce::jmax (kHead, textWidth (Theme::ui (Theme::fontXS, true), badge) + Theme::space2 + Theme::space1);
        paintWeightBadge (g, header.removeFromRight (bw).toFloat(), badge, info->weight == EffectWeight::heavy);
    }
    if (autoStopped)
    {
        const auto t = ja ("自動停止");
        header.removeFromRight (Theme::space1);
        drawText (g, t, header.removeFromRight (textWidth (Theme::ui (Theme::fontXS, true), t)), Theme::fontXS, p.warn, juce::Justification::centredRight, true);
    }
    header.removeFromRight (Theme::space1);
    const auto name = info != nullptr ? juce::String::fromUTF8 (info->nameJa) : ja ("（不明なエフェクト）");
    const auto nameFont = Theme::ui (compact ? Theme::fontXS : Theme::fontS, true);
    g.setColour (enabled ? p.text : p.textSub);
    g.setFont (nameFont);
    g.drawFittedText (fitName (name, nameFont, header.getWidth()), header, juce::Justification::centredLeft, 1, 0.8f);

    if (info == nullptr) return;
    for (int k = 0; k < bars.size(); ++k)
    {
        if (! bars[k]->isVisible()) continue;
        auto row = bars[k]->getBounds().withY (bars[k]->getY() - kRowText).withHeight (kRowText);
        const auto& spec = info->params[size_t (paramIndex[size_t (k)])];
        const auto value = formatParam (spec, float (bars[k]->getValue()));
        const int vw = int (std::ceil (textRunWidth (value, Theme::fontXS, true, true))) + 2;
        drawText (g, value, row.removeFromRight (vw), Theme::fontXS, enabled ? p.text : p.textSub, juce::Justification::centredRight, true, true);
        row.removeFromRight (Theme::space1);
        drawText (g, fitName (juce::String::fromUTF8 (spec.nameJa), Theme::ui (Theme::fontXS), row.getWidth()), row, Theme::fontXS, p.textSub);
    }
}

void PaperSlotCard::mouseDown (const juce::MouseEvent&) { dragging = false; }

void PaperSlotCard::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging && e.getDistanceFromDragStart() > Theme::space1) dragging = true;
    if (dragging) chain.dragMove (*this, e);
}

void PaperSlotCard::mouseUp (const juce::MouseEvent& e)
{
    if (dragging)
    {
        dragging = false;
        chain.dragEnd (*this);
    }
    else if (! e.mods.isPopupMenu() && e.mouseWasClicked())
        nav.showSlotDetail (index);
}

bool PaperSlotCard::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::returnKey || k == juce::KeyPress::spaceKey) { nav.showSlotDetail (index); return true; }
    if (k == juce::KeyPress::deleteKey) { remove.triggerClick(); return true; }
    if (k.getModifiers().isCtrlDown() && k.isKeyCode (juce::KeyPress::leftKey) && left.isEnabled()) { left.triggerClick(); return true; }
    if (k.getModifiers().isCtrlDown() && k.isKeyCode (juce::KeyPress::rightKey) && right.isEnabled()) { right.triggerClick(); return true; }
    return false;
}

// =============================================================================================== PaperChain
PaperChain::PaperChain (AppController& ctl, Navigator& n) : c (ctl), nav (n), add (ja ("エフェクトを追加"), true)
{
    setComponentID ("tour.chain");
    view.setViewedComponent (&content, false);
    view.setScrollBarsShown (false, true);
    view.setScrollBarThickness (kScroll);
    addAndMakeVisible (view);
    add.setComponentID ("chain.add");
    add.setTooltip (ja ("チェーンの最後にエフェクトを追加します（最大 10 スロット）"));
    add.onClick = [this]
    {
        if (int (c.getChain().size()) >= kMaxSlots) nav.showToast (ja ("スロットは 10 個までです。")); // F-04-1
        else nav.showEffectPicker (-1);
    };
    content.addAndMakeVisible (add);
    refresh();
}

int PaperChain::minHeight (bool cp) { return headerH (cp) + (cp ? Theme::space1 : Theme::space2) + PaperSlotCard::height (cp, 1) + kScroll; }
int PaperChain::idealHeight (bool cp) { return headerH (cp) + (cp ? Theme::space1 : Theme::space2) + PaperSlotCard::height (cp, 2) + kScroll; }

void PaperChain::setCompact (bool cp)
{
    if (compact == cp) return;
    compact = cp;
    resized();
    repaint();
}

void PaperChain::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (headerH (compact) + (compact ? Theme::space1 : Theme::space2));
    view.setBounds (r);
    layoutCards();
}

void PaperChain::layoutCards()
{
    const int h = juce::jmax (0, view.getHeight() - kScroll);
    for (auto* card : cards)
    {
        card->setCompact (compact);
        card->setBounds (slotX (card->index), 0, slotWidth(), h);
    }
    const int top = PaperSlotCard::markerH (compact) + markerGap (compact);
    add.setBounds (slotX (cards.size()), top, slotWidth(), juce::jmax (0, h - top));
    content.setSize (juce::jmax (view.getWidth(), slotX (cards.size()) + slotWidth()), h);
}

void PaperChain::refresh()
{
    const auto& ch = c.getChain();
    juce::StringArray now;
    for (auto& s : ch) now.add (juce::String (s.type));
    if (now != types)
    {
        types = now;
        cards.clear();
        for (int i = 0; i < int (ch.size()); ++i) content.addAndMakeVisible (cards.add (new PaperSlotCard (*this, c, nav, i)));
        layoutCards();
        repaint();
    }
    for (int i = 0; i < cards.size(); ++i) cards[i]->update (ch[size_t (i)], c.isSlotAutoStopped (i));
    if (const int header = int (ch.size()) * 16 + heavyOnCount (ch); header != headerState)
    {
        headerState = header;
        repaint();
    }
}

void PaperChain::Content::paint (juce::Graphics& g)
{
    // the timeline rule the processing numbers sit on
    const int y = PaperSlotCard::markerH (owner.compact) / 2;
    g.setColour (Theme::colours().border);
    g.fillRect (0, y - 1, getWidth(), 2);
}

void PaperChain::paint (juce::Graphics& g)
{
    const auto& p = P();
    const auto& ch = c.getChain();
    const int heavyOn = heavyOnCount (ch);
    auto r = getLocalBounds().removeFromTop (headerH (compact));
    const auto title = ja ("エフェクトチェーン");
    drawText (g, title, r.removeFromLeft (textWidth (Theme::ui (Theme::fontS, true), title) + Theme::space1), Theme::fontS, p.text, juce::Justification::centredLeft, true);
    r.removeFromLeft (Theme::space3);
    const float small = compact ? Theme::fontXS : Theme::fontS;
    const auto count = juce::String (int (ch.size())) + " / " + juce::String (kMaxSlots) + (compact ? juce::String() : ja (" スロット"));
    drawText (g, count, r.removeFromLeft (int (std::ceil (textRunWidth (count, small, false, true))) + Theme::space1), small, p.textSub, juce::Justification::centredLeft, false, true);
    r.removeFromLeft (Theme::space3);
    const auto heavy = (compact ? ja ("重 ") : ja ("重いエフェクト ON ")) + juce::String (heavyOn) + " / " + juce::String (kMaxHeavyOn);
    drawText (g, heavy, r.removeFromLeft (textWidth (Theme::ui (small), heavy) + Theme::space1), small, heavyOn >= kMaxHeavyOn ? p.warn : p.textSub);
    drawText (g, compact ? ja ("左から右へ ・ 横にスクロール") : ja ("処理は左から右へ"), r, Theme::fontXS, p.textSub, juce::Justification::centredRight);
}

void PaperChain::paintOverChildren (juce::Graphics& g)
{
    const auto& p = P();
    const auto vb = view.getBounds();
    const int top = PaperSlotCard::markerH (compact) + markerGap (compact);
    if (cards.isEmpty())
        drawText (g, ja ("エフェクトなし。[+ 追加] で入れられます"), vb.withTrimmedLeft (slotWidth() + Theme::space3).withTrimmedTop (top).withTrimmedBottom (kScroll),
                  Theme::fontS, p.textSub);
    if (dropIndex >= 0)
    {
        const int from = [this] { for (auto* card : cards) if (card->isMouseButtonDown (true)) return card->index; return -1; }();
        if (from >= 0 && from != dropIndex)
        {
            const int x = vb.getX() + content.getX() + slotX (dropIndex) + (dropIndex > from ? slotWidth() + slotGap() / 2 : -slotGap() / 2);
            g.reduceClipRegion (vb);
            g.setColour (p.accent);
            g.fillRect (x - 1, vb.getY() + top, 3, vb.getHeight() - top - kScroll);
        }
    }
}

void PaperChain::dragMove (PaperSlotCard& card, const juce::MouseEvent& e)
{
    const int mouseX = e.getEventRelativeTo (&content).x;
    card.setTopLeftPosition (mouseX - e.getMouseDownX(), card.getY());
    card.toFront (false);
    dropIndex = juce::jlimit (0, cards.size() - 1, juce::roundToInt (float (card.getX()) / float (slotWidth() + slotGap())));
    const auto inView = e.getEventRelativeTo (&view).getPosition();
    view.autoScroll (inView.x, inView.y, Theme::space5 + Theme::space3, Theme::space3);
    repaint();
}

void PaperChain::dragEnd (PaperSlotCard& card)
{
    const int from = card.index, to = dropIndex;
    dropIndex = -1;
    layoutCards();
    repaint();
    if (to >= 0 && to != from) withLooperCheck (c, nav, *this, [this, from, to] { c.moveSlot (from, to); });
}
} // namespace koe::ui::paper
