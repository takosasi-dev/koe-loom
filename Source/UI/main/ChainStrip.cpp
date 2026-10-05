#include "UI/main/ChainStrip.h"

#include "Effects/EffectRegistry.h"
#include "Platform/Hotkeys.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace koe::ui::mainui
{
namespace
{
const Palette& P() { return Theme::colours(); }

constexpr int kHeaderRow = 20, kTextRow = 16;
int bottomRowH (bool tight) { return tight ? Theme::space4 : Theme::space4 + Theme::space1; } // 24 / 28
int iconButtonSize (bool compact) { return compact ? Theme::space4 : Theme::space4 + Theme::space1; }

/** "ロボ（リングモジュレーション）" -> "ロボ" when the whole name does not fit (the mock's cards show the short name). */
juce::String fitName (const juce::String& name, const juce::Font& f, int width)
{
    const auto paren = juce::String::fromUTF8 ("（");
    if (juce::GlyphArrangement::getStringWidth (f, name) <= float (width) || ! name.contains (paren)) return name;
    return name.upToFirstOccurrenceOf (paren, false, false).trimEnd();
}
} // namespace

std::vector<int> matchSlots (const juce::StringArray& before, const juce::StringArray& after)
{
    std::vector<int> from;
    std::vector<bool> used (size_t (before.size()), false);
    for (auto& t : after)
    {
        int k = 0;
        while (k < before.size() && (used[size_t (k)] || before[k] != t)) ++k;
        from.push_back (k < before.size() ? k : -1);
        if (k < before.size()) used[size_t (k)] = true;
    }
    return from;
}

// =============================================================================================== SlotCard
SlotCard::SlotCard (ChainStrip& s, AppController& ctl, Navigator& n, int i)
    : index (i), strip (s), c (ctl), nav (n), bars (Theme::prefs().knobStyle == 1), left (ja ("左へ"), Icon::chevronLeft),
      right (ja ("右へ"), Icon::chevronRight), remove (ja ("削除"), Icon::close)
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

void SlotCard::update (const SlotDef& slot, bool stopped)
{
    const auto* newInfo = findEffectInfo (slot.type);
    int heavyOn = 0;
    for (auto& s : c.getChain())
        if (auto* i = findEffectInfo (s.type); i != nullptr && s.enabled && i->weight == EffectWeight::heavy) ++heavyOn;
    const int context = int (c.getChain().size()) * 16 + heavyOn; // what the arrows and the ON tooltip depend on
    if (newInfo == info && info != nullptr && slot == last && stopped == autoStopped && context == lastContext)
        return; // polled several times a second: nothing to redraw
    last = slot;
    lastContext = context;
    if (newInfo != info)
    {
        info = newInfo;
        knobs.clear();
        paramIndex = info != nullptr ? mainParams (*info) : std::vector<int>();
        for (int pi : paramIndex)
        {
            const auto& spec = info->params[size_t (pi)];
            auto* k = knobs.add (new Knob (Knob::Size::small));
            k->setup (spec.min, spec.max, spec.def, spec.integer ? 1.0 : 0.0, [spec] (double v) { return formatParam (spec, float (v)); });
            applyParamRange (*k, spec, spec.def);
            k->setLabel (juce::String::fromUTF8 (spec.nameJa)); // the knob adds range and default to its tooltip
            k->setTitle (juce::String::fromUTF8 (spec.nameJa));
            k->onValueChange = [this, pi, k] { c.setSlotParam (index, pi, float (k->getValue())); repaint(); };
            addAndMakeVisible (k);
        }
        const auto name = info != nullptr ? juce::String::fromUTF8 (info->nameJa) : juce::String (slot.type);
        const auto desc = info != nullptr ? juce::String::fromUTF8 (info->descJa) : ja ("この版では使えないエフェクトです");
        setTooltip (name + ja ("：") + desc + ja ("（押すと全パラメータを編集）"));
        resized();
    }
    for (size_t k = 0; k < paramIndex.size(); ++k)
        if (size_t (paramIndex[k]) < slot.params.size())
            knobs[int (k)]->setValue (slot.params[size_t (paramIndex[k])], juce::dontSendNotification);
    enabled = slot.enabled;
    autoStopped = stopped;
    toggle.setToggleState (slot.enabled, juce::dontSendNotification);
    // §8.3: with 2 heavy effects ON, the 3rd one's ON button says why it cannot be turned on
    const bool blocked = info != nullptr && info->weight == EffectWeight::heavy && ! slot.enabled && heavyOn >= kMaxHeavyOn;
    toggle.setTooltip (blocked ? ja ("重いエフェクトは同時に 2 つまでしか ON にできません。ほかの重いエフェクトを OFF にしてください。")
                               : ja ("スロット ") + juce::String (index + 1) + ja (" の ON/OFF"));
    left.setEnabled (index > 0);
    right.setEnabled (index < int (c.getChain().size()) - 1);
    repaint();
}

void SlotCard::setCompact (bool c2)
{
    if (compact == c2) return;
    compact = c2;
    resized();
    repaint();
}

SlotCard::Tier SlotCard::tier() const
{
    if (getHeight() >= 144) return Tier::full;
    if (getHeight() >= 128) return Tier::values;
    return Tier::knobsOnly;
}

void SlotCard::resized()
{
    const bool tight = getHeight() < 112;
    const int padX = compact ? Theme::space2 : Theme::space2 + Theme::space1;
    const int padY = tight ? Theme::space1 : Theme::space2;
    const int gap = tight ? 2 : Theme::space1;
    auto r = getLocalBounds().reduced (padX, padY);

    auto bottom = r.removeFromBottom (bottomRowH (tight));
    const int bs = iconButtonSize (compact || tight);
    for (auto* b : { &remove, &right, &left })
    {
        b->setBounds (bottom.removeFromRight (bs).withSizeKeepingCentre (bs, bs));
        bottom.removeFromRight (Theme::space1);
    }
    toggle.setBounds (bottom.removeFromLeft (juce::jmin (bottom.getWidth(), Theme::toggleW + Theme::space2 + (compact ? 20 : 28))));

    r.removeFromTop (kHeaderRow + gap);
    r.removeFromBottom (gap);
    if (bars)
    {
        // wave10/ui: the bars stacked full width, centred; a bar lower than its text row draws the bar alone (the short tier)
        const int n = juce::jmax (1, knobs.size());
        const int h = juce::jmin (Theme::space5, (r.getHeight() - (n - 1) * Theme::space1) / n);
        const int top = r.getY() + juce::jmax (0, (r.getHeight() - n * h - (n - 1) * Theme::space1) / 2);
        for (int k = 0; k < knobs.size(); ++k)
            knobs[k]->setBounds (r.getX(), top + k * (h + Theme::space1), r.getWidth(), h);
        return;
    }
    // knobs (+ label + value) centred between the header and the ON/OFF row
    const int block = Theme::knobSmall + (tier() == Tier::full ? 2 : tier() == Tier::values ? 1 : 0) * kTextRow;
    const int top = r.getY() + juce::jmax (0, (r.getHeight() - block) / 2);
    const int n = knobs.size();
    for (int k = 0; k < n; ++k)
    {
        const int cellW = r.getWidth() / juce::jmax (1, n);
        const auto cell = juce::Rectangle<int> (r.getX() + k * cellW, top, cellW, Theme::knobSmall);
        knobs[k]->setBounds (cell.withSizeKeepingCentre (Theme::knobSmall, Theme::knobSmall));
    }
}

void SlotCard::paint (juce::Graphics& g)
{
    const auto& p = P();
    auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (p.raised);
    g.fillRoundedRectangle (bounds, Theme::radiusM);
    g.setColour (p.divider);
    g.drawRoundedRectangle (bounds, Theme::radiusM, 1.0f);

    const bool tight = getHeight() < 112;
    const int padX = compact ? Theme::space2 : Theme::space2 + Theme::space1;
    auto r = getLocalBounds().reduced (padX, tight ? Theme::space1 : Theme::space2);
    auto header = r.removeFromTop (kHeaderRow);
    drawText (g, juce::String (index + 1), header.removeFromLeft (Theme::space3), Theme::fontXS, p.textSub, juce::Justification::centredLeft, false, true);
    if (info != nullptr)
    {
        const auto badge = weightNameJa (info->weight);
        const int bw = juce::jmax (kHeaderRow, textWidth (Theme::ui (Theme::fontXS, true), juce::String::fromUTF8 (badge)) + Theme::space2 + Theme::space1);
        paintWeightBadge (g, header.removeFromRight (bw).toFloat(), juce::String::fromUTF8 (badge), info->weight == EffectWeight::heavy);
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

    // knob labels and values (F-04-22), dropped first when the card is short
    const auto t = tier();
    if (t != Tier::knobsOnly && info != nullptr && ! bars) // bars draw their own
        for (int k = 0; k < knobs.size(); ++k)
        {
            const auto kb = knobs[k]->getBounds();
            const int cellW = r.getWidth() / juce::jmax (1, knobs.size());
            auto col = juce::Rectangle<int> (kb.getCentreX() - cellW / 2, kb.getBottom(), cellW, kTextRow);
            const auto& spec = info->params[size_t (paramIndex[size_t (k)])];
            if (t == Tier::full)
            {
                drawText (g, fitName (juce::String::fromUTF8 (spec.nameJa), Theme::ui (Theme::fontXS), col.getWidth()), col, Theme::fontXS, p.textSub,
                          juce::Justification::centred);
                col.translate (0, kTextRow);
            }
            drawText (g, formatParam (spec, float (knobs[k]->getValue())), col, Theme::fontXS, p.text, juce::Justification::centred, true, true);
        }

    if (hasKeyboardFocus (false)) drawFocusRing (g, bounds, Theme::radiusM);
}

void SlotCard::mouseDown (const juce::MouseEvent& e)
{
    dragging = false;
    if (e.mods.isPopupMenu()) showSlotMenu (c, nav, *this, index, false, true); // wave10/ui (a knob keeps its own: the value editor)
}

void SlotCard::mouseDrag (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    if (! dragging && e.getDistanceFromDragStart() > Theme::space1) dragging = true;
    if (dragging) strip.dragMove (*this, e);
}

void SlotCard::mouseUp (const juce::MouseEvent& e)
{
    if (dragging)
    {
        dragging = false;
        strip.dragEnd (*this);
    }
    else if (! e.mods.isPopupMenu() && e.mouseWasClicked())
        nav.showSlotDetail (index);
}

bool SlotCard::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::returnKey || k == juce::KeyPress::spaceKey)
    {
        nav.showSlotDetail (index);
        return true;
    }
    if (k == juce::KeyPress::deleteKey)
    {
        remove.triggerClick();
        return true;
    }
    if (k.getModifiers().isCtrlDown() && k.isKeyCode (juce::KeyPress::leftKey) && left.isEnabled()) { left.triggerClick(); return true; }
    if (k.getModifiers().isCtrlDown() && k.isKeyCode (juce::KeyPress::rightKey) && right.isEnabled()) { right.triggerClick(); return true; }
    if (isSlotMenuKey (k)) { showSlotMenu (c, nav, *this, index, false, false); return true; }
    return false;
}

bool SlotCard::keyStateChanged (bool isKeyDown)
{
    if (! isKeyDown || ! isSlotMenuKeyDown()) return false;
    showSlotMenu (c, nav, *this, index, false, false);
    return true;
}

// =============================================================================================== slot menu (wave10/ui)
std::vector<SlotMenuItem> slotMenuItems (const AppController& c, int slot, bool vertical)
{
    const auto& chain = c.getChain();
    if (slot < 0 || slot >= int (chain.size())) return {};
    const auto& s = chain[size_t (slot)];
    const bool once = isOnePerChain (s.type); // AppController::duplicateSlot refuses these
    const int last = int (chain.size()) - 1;
    return { { SlotAction::toggle, s.enabled ? ja ("OFF にする") : ja ("ON にする"), true },
             { SlotAction::detail, ja ("詳細を開く"), true },
             { SlotAction::duplicate, ja ("複製"), int (chain.size()) < kMaxSlots && ! once },
             { SlotAction::reset, ja ("初期値に戻す"), findEffectInfo (s.type) != nullptr },
             { SlotAction::moveBack, vertical ? ja ("上へ移動") : ja ("左へ移動"), slot > 0 },
             { SlotAction::moveForward, vertical ? ja ("下へ移動") : ja ("右へ移動"), slot < last },
             { SlotAction::remove, ja ("削除"), true } };
}

void runSlotAction (AppController& c, Navigator& nav, juce::Component& owner, int slot, SlotAction action)
{
    if (slot < 0 || slot >= int (c.getChain().size())) return;
    juce::String why;
    switch (action)
    {
        case SlotAction::toggle:
            if (! c.setSlotEnabled (slot, ! c.getChain()[size_t (slot)].enabled, why)) nav.showToast (why);
            break;
        case SlotAction::detail: nav.showSlotDetail (slot); break;
        case SlotAction::reset: c.resetSlot (slot); break;
        case SlotAction::duplicate:
            withLooperCheck (c, nav, owner, [&c, &nav, slot]
            {
                juce::String whyNot;
                if (! c.duplicateSlot (slot, whyNot)) nav.showToast (whyNot);
            });
            break;
        case SlotAction::moveBack: withLooperCheck (c, nav, owner, [&c, slot] { c.moveSlot (slot, slot - 1); }); break;
        case SlotAction::moveForward: withLooperCheck (c, nav, owner, [&c, slot] { c.moveSlot (slot, slot + 1); }); break;
        case SlotAction::remove: withLooperCheck (c, nav, owner, [&c, slot] { c.removeSlot (slot); }); break;
    }
}

void showSlotMenu (AppController& c, Navigator& nav, juce::Component& owner, int slot, bool vertical, bool atMouse)
{
    juce::PopupMenu menu;
    for (auto& item : slotMenuItems (c, slot, vertical))
    {
        if (item.action == SlotAction::duplicate || item.action == SlotAction::moveBack || item.action == SlotAction::remove) menu.addSeparator();
        menu.addItem (int (item.action), item.text, item.enabled);
    }
    auto options = juce::PopupMenu::Options();
    options = atMouse ? options.withMousePosition() : options.withTargetComponent (&owner);
    menu.showMenuAsync (options, [&c, &nav, safe = juce::Component::SafePointer<juce::Component> (&owner), slot] (int chosen)
    {
        if (chosen > 0 && safe != nullptr) runSlotAction (c, nav, *safe, slot, SlotAction (chosen));
    });
}

bool isSlotMenuKey (const juce::KeyPress& k)
{
    return k.getKeyCode() == juce::KeyPress::F10Key && k.getModifiers().isShiftDown() && ! k.getModifiers().isCtrlDown() && ! k.getModifiers().isAltDown();
}

bool isSlotMenuKeyDown() { return Hotkeys::isKeyDown (0x5D); } // VK_APPS

// =============================================================================================== ChainStrip
ChainStrip::ChainStrip (AppController& ctl, Navigator& n) : c (ctl), nav (n), add (ja ("エフェクトを追加"), true)
{
    setComponentID ("tour.chain");
    view.setViewedComponent (&content, false);
    view.setScrollBarsShown (false, true);
    view.setScrollBarThickness (Theme::space2);
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

void ChainStrip::setCompact (bool c2)
{
    if (compact == c2) return;
    compact = c2;
    for (auto* card : cards) card->setCompact (compact);
    resized();
    repaint();
}

int ChainStrip::minHeight (bool compact)
{
    const int padY = compact ? Theme::space2 : Theme::space2 + Theme::space1;
    const int header = compact ? kHeaderRow + Theme::space1 : Theme::space4 + Theme::space1 + Theme::space2;
    return padY * 2 + header + SlotCard::minHeight + Theme::space2;
}

juce::Rectangle<int> ChainStrip::headerArea() const
{
    const int padX = compact ? Theme::space2 + Theme::space1 : Theme::space3;
    const int padY = compact ? Theme::space2 : Theme::space2 + Theme::space1;
    return getLocalBounds().reduced (padX, padY).removeFromTop (compact ? kHeaderRow : Theme::space4 + Theme::space1);
}

void ChainStrip::resized()
{
    const int padX = compact ? Theme::space2 + Theme::space1 : Theme::space3;
    const int padY = compact ? Theme::space2 : Theme::space2 + Theme::space1;
    auto r = getLocalBounds().reduced (padX, padY);
    r.removeFromTop (headerArea().getHeight() + (compact ? Theme::space1 : Theme::space2));
    view.setBounds (r);
    layoutCards();
}

void ChainStrip::layoutCards()
{
    const int h = juce::jmax (0, view.getHeight() - view.getScrollBarThickness());
    auto& animator = juce::Desktop::getInstance().getAnimator();
    dropFrom = -1;
    for (auto* card : cards)
    {
        animator.cancelAnimation (card, false);
        card->setCompact (compact);
        card->setBounds (slotX (card->index), 0, slotWidth(), h);
    }
    animator.cancelAnimation (&add, false);
    add.setBounds (slotX (cards.size()), 0, slotWidth(), h);
    content.setSize (slotX (cards.size()) + slotWidth(), h);
}

void ChainStrip::refresh()
{
    const auto& chain = c.getChain();
    juce::StringArray now;
    for (auto& s : chain) now.add (juce::String (s.type));
    if (now != types)
    {
        std::vector<int> oldX;
        for (auto* card : cards) oldX.push_back (card->index == dropFrom ? dropX : card->getX());
        const int oldAddX = add.getX();
        const auto before = std::exchange (types, now);
        cards.clear();
        for (int i = 0; i < int (chain.size()); ++i)
            content.addAndMakeVisible (cards.add (new SlotCard (*this, c, nav, i)));
        layoutCards();
        repaint();

        // F-14-6: after a move, add or remove, the cards slide from their old place (not on a preset
        // switch: more than one new card). Offscreen (tests, --snapshot) and with animations off: instant.
        const auto from = matchSlots (before, now);
        if (animate() && isShowing() && std::count (from.begin(), from.end(), -1) <= 1)
        {
            auto slide = [] (juce::Component& comp, int x)
            {
                const auto target = comp.getBounds();
                if (x == target.getX()) return;
                comp.setTopLeftPosition (x, target.getY());
                juce::Desktop::getInstance().getAnimator().animateComponent (&comp, target, 1.0f, Theme::motionMid, false, 1.0, 0.0);
            };
            for (int i = 0; i < cards.size(); ++i)
                if (const int k = from[size_t (i)]; k >= 0 && k < int (oldX.size())) slide (*cards[i], oldX[size_t (k)]);
            slide (add, oldAddX);
        }
    }
    for (int i = 0; i < cards.size(); ++i) cards[i]->update (chain[size_t (i)], c.isSlotAutoStopped (i));
    int heavyOn = 0;
    for (auto& s : chain)
        if (auto* info = findEffectInfo (s.type); info != nullptr && s.enabled && info->weight == EffectWeight::heavy) ++heavyOn;
    if (const int header = int (chain.size()) * 16 + heavyOn; header != headerState)
    {
        headerState = header;
        repaint (headerArea());
    }
}

void ChainStrip::paint (juce::Graphics& g)
{
    const auto& p = P();
    g.setColour (p.surface);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), Theme::radiusL);

    const auto& chain = c.getChain();
    int heavyOn = 0;
    for (auto& s : chain)
        if (auto* info = findEffectInfo (s.type); info != nullptr && s.enabled && info->weight == EffectWeight::heavy) ++heavyOn;

    auto r = headerArea();
    const auto title = ja ("エフェクトチェーン");
    const auto titleF = Theme::ui (Theme::fontS, true);
    drawText (g, title, r.removeFromLeft (textWidth (titleF, title) + Theme::space1), Theme::fontS, p.text, juce::Justification::centredLeft, true);
    r.removeFromLeft (Theme::space3);
    const auto count = juce::String (int (chain.size())) + " / " + juce::String (kMaxSlots) + (compact ? juce::String() : ja (" スロット"));
    const float small = compact ? Theme::fontXS : Theme::fontS;
    drawText (g, count, r.removeFromLeft (int (std::ceil (textRunWidth (count, small, false, true))) + Theme::space1), small, p.textSub, juce::Justification::centredLeft, false, true);
    r.removeFromLeft (Theme::space3);
    const auto heavy = (compact ? ja ("重 ") : ja ("重いエフェクト ON ")) + juce::String (heavyOn) + " / " + juce::String (kMaxHeavyOn);
    const int heavyW = textWidth (Theme::ui (small), heavy) + Theme::space1;
    drawText (g, heavy, r.removeFromLeft (heavyW), small, heavyOn >= kMaxHeavyOn ? p.warn : p.textSub);
    drawText (g, compact ? ja ("左から右へ ・ 横にスクロール") : ja ("処理は左から右へ"), r, Theme::fontXS, p.textSub, juce::Justification::centredRight);
}

void ChainStrip::paintOverChildren (juce::Graphics& g)
{
    const auto& p = P();
    const auto vb = view.getBounds();
    if (cards.isEmpty())
    {
        auto t = vb.withTrimmedLeft (slotWidth() + Theme::space3).withHeight (vb.getHeight() - view.getScrollBarThickness());
        drawText (g, ja ("エフェクトなし。[+ 追加] で入れられます"), t, Theme::fontS, p.textSub);
    }
    if (dropIndex >= 0)
    {
        const int from = [this] { for (auto* card : cards) if (card->isMouseButtonDown (true)) return card->index; return -1; }();
        if (from >= 0 && from != dropIndex)
        {
            const int x = vb.getX() + content.getX() + slotX (dropIndex) + (dropIndex > from ? slotWidth() + slotGap() / 2 : -slotGap() / 2);
            g.reduceClipRegion (vb);
            g.setColour (p.accent);
            g.fillRect (x - 1, vb.getY(), 3, vb.getHeight() - view.getScrollBarThickness());
        }
    }
}

void ChainStrip::dragMove (SlotCard& card, const juce::MouseEvent& e)
{
    const int mouseX = e.getEventRelativeTo (&content).x;
    card.setTopLeftPosition (mouseX - e.getMouseDownX(), card.getY());
    card.toFront (false);
    const int pitch = slotWidth() + slotGap();
    dropIndex = juce::jlimit (0, cards.size() - 1, juce::roundToInt (float (card.getX()) / float (pitch)));
    const auto inView = e.getEventRelativeTo (&view).getPosition();
    view.autoScroll (inView.x, inView.y, Theme::space5 + Theme::space3, Theme::space3);
    repaint();
}

void ChainStrip::dragEnd (SlotCard& card)
{
    const int from = card.index, to = dropIndex;
    const int releasedX = card.getX();
    dropIndex = -1;
    layoutCards();
    if (to >= 0 && to != from && ! c.hasLooperRecording()) // moved right away: slide from where it was let go
    {
        dropFrom = from;
        dropX = releasedX;
    }
    repaint();
    if (to >= 0 && to != from) withLooperCheck (c, nav, *this, [this, from, to] { c.moveSlot (from, to); });
}
} // namespace koe::ui::mainui
