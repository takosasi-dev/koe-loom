#pragma once

// 案 B "Paper" エフェクトチェーン (docs/mockups/B-S01.dc.html), owner wave5/paper: the slots as a numbered timeline
// of cards (a rule with the processing number on it above each card), two bar sliders per card, ON/OFF, ◀ ▶ ×,
// [+ エフェクトを追加], drag & drop, horizontal scrolling. Same controller calls and IDs as the Studio ChainStrip
// (tour.chain, chain.slot.N, chain.slot.N.toggle, chain.add).

#include "UI/skins/paper/PaperParts.h"

namespace koe::ui::paper
{
class PaperChain;

class PaperSlotCard : public juce::Component, public juce::SettableTooltipClient
{
public:
    PaperSlotCard (PaperChain& chain, AppController& c, Navigator& nav, int index);
    void update (const SlotDef& slot, bool autoStopped);
    void setCompact (bool compact);
    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress& k) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }

    /** The number on the timeline above the card. */
    static int markerH (bool compact) { return compact ? 18 : Theme::space4; }
    /** Card heights with one / two parameter bars (marker included). */
    static int height (bool compact, int bars);
    const int index;
    ToggleSwitch toggle;
    juce::OwnedArray<PaperSlider> bars;

private:
    juce::Rectangle<int> box() const;
    int barsShown() const;
    PaperChain& chain;
    AppController& c;
    Navigator& nav;
    const EffectInfo* info = nullptr;
    std::vector<int> paramIndex;
    SlotDef last;
    int lastContext = -1;
    bool enabled = true, autoStopped = false, compact = false, dragging = false;
    mainui::SquareIconButton left, right, remove;
};

class PaperChain : public juce::Component
{
public:
    PaperChain (AppController& c, Navigator& nav);
    void setCompact (bool compact);
    void refresh();
    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;
    void resized() override;

    /** Header + cards with one bar + scroll bar. */
    static int minHeight (bool compact);
    /** Header + cards with both bars + scroll bar. */
    static int idealHeight (bool compact);
    int numCards() const { return cards.size(); }
    PaperSlotCard* card (int i) const { return cards[i]; }
    juce::Viewport& viewport() { return view; }

    void dragMove (PaperSlotCard& card, const juce::MouseEvent& e);
    void dragEnd (PaperSlotCard& card);

private:
    struct Content : public juce::Component
    {
        explicit Content (PaperChain& o) : owner (o) { setInterceptsMouseClicks (false, true); }
        void paint (juce::Graphics& g) override;
        PaperChain& owner;
    };
    void layoutCards();
    static int headerH (bool compact) { return compact ? 20 : Theme::space4; }
    int slotWidth() const { return compact ? Theme::slotWNarrow : Theme::slotW; }
    int slotGap() const { return compact ? Theme::space2 : Theme::space2 + Theme::space1; }
    int slotX (int i) const { return i * (slotWidth() + slotGap()); }
    AppController& c;
    Navigator& nav;
    bool compact = false;
    juce::StringArray types;
    int headerState = -1, dropIndex = -1;
    juce::Viewport view;
    Content content { *this };
    juce::OwnedArray<PaperSlotCard> cards;
    mainui::DashedButton add;
};
} // namespace koe::ui::paper
