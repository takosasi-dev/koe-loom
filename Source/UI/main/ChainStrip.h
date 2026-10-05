#pragma once

// S-01 エフェクトチェーン段 (F-04): slot cards in processing order, [+ 追加], drag & drop reordering,
// horizontal scrolling, heavy-effect counter, 自動停止.

#include "UI/main/Common.h"

namespace koe::ui::mainui
{
class ChainStrip;

/** For each slot type in after, the index of the same effect in before (first unused match), or -1
    for a new one. The chain strip slides cards from their old place with this (F-14-6). */
std::vector<int> matchSlots (const juce::StringArray& before, const juce::StringArray& after);

// ---------------------------------------------------------------------------------------------
// wave10/ui (INTERFACES.md §12.3): the slot context menu of the three layouts (right click, the menu key, Shift+F10).
// The entries and what each does are kept apart from the PopupMenu, so the tests check them without a menu.
enum class SlotAction { toggle = 1, detail, duplicate, reset, moveBack, moveForward, remove };
struct SlotMenuItem
{
    SlotAction action;
    juce::String text;
    bool enabled;
};
/** The entries for slot, in menu order (vertical = Mono's 上へ / 下へ). Greyed out: 複製 at kMaxSlots or for a type the
    chain holds once, the moves at the ends, 初期値に戻す for an unknown effect. */
std::vector<SlotMenuItem> slotMenuItems (const AppController& c, int slot, bool vertical);
/** Runs one: ON/OFF (toast when refused), S-09, 複製 / 移動 / 削除 through withLooperCheck (toast when refused), 初期値に戻す. */
void runSlotAction (AppController& c, Navigator& nav, juce::Component& owner, int slot, SlotAction action);
/** Shows the menu at the mouse, or under owner (keyboard); the choice runs only while owner still exists. */
void showSlotMenu (AppController& c, Navigator& nav, juce::Component& owner, int slot, bool vertical, bool atMouse);
/** Shift+F10. */
bool isSlotMenuKey (const juce::KeyPress& k);
/** The keyboard's menu key is down (JUCE gives it to keyStateChanged only, never to keyPressed). */
bool isSlotMenuKeyDown();

class SlotCard : public juce::Component, public juce::SettableTooltipClient
{
public:
    SlotCard (ChainStrip& strip, AppController& c, Navigator& nav, int index);
    /** Values from the model (no structure change). */
    void update (const SlotDef& slot, bool autoStopped);
    void setCompact (bool compact);
    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress& k) override;
    bool keyStateChanged (bool isKeyDown) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }

    /** Smallest height that still shows the knobs and the ON/OFF row (§8.2.1, 2 banners). */
    static constexpr int minHeight = 102;
    const int index;
    ToggleSwitch toggle;
    juce::OwnedArray<Knob> knobs;

private:
    enum class Tier { full, values, knobsOnly };
    Tier tier() const;
    ChainStrip& strip;
    AppController& c;
    Navigator& nav;
    const EffectInfo* info = nullptr;
    std::vector<int> paramIndex;
    SlotDef last;
    int lastContext = -1;
    bool enabled = true, autoStopped = false, compact = false, dragging = false;
    const bool bars; // S-03 つまみの形 = 棒 when built (wave10/ui): the knobs are full-width bars that draw their own name and value
    SquareIconButton left, right, remove;
};

class ChainStrip : public juce::Component
{
public:
    ChainStrip (AppController& c, Navigator& nav);
    void setCompact (bool compact);
    /** Rebuilds the cards when the chain structure changed, otherwise updates values. */
    void refresh();
    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;
    void resized() override;

    /** Height of the whole section for slot cards of SlotCard::minHeight. */
    static int minHeight (bool compact);
    int numCards() const { return cards.size(); }
    SlotCard* card (int i) const { return cards[i]; }
    juce::Viewport& viewport() { return view; }

    // drag & drop (F-04-5)
    void dragMove (SlotCard& card, const juce::MouseEvent& e);
    void dragEnd (SlotCard& card);

private:
    void layoutCards();
    juce::Rectangle<int> headerArea() const;
    int slotWidth() const { return compact ? Theme::slotWNarrow : Theme::slotW; }
    int slotGap() const { return compact ? Theme::space2 : Theme::space2 + Theme::space1; }
    int slotX (int i) const { return i * (slotWidth() + slotGap()); }
    AppController& c;
    Navigator& nav;
    bool compact = false;
    juce::StringArray types; // slot types the cards were built for
    int headerState = -1;
    int dropFrom = -1, dropX = 0; // where the dragged card was let go (the slide starts there)
    juce::Viewport view;
    juce::Component content;
    juce::OwnedArray<SlotCard> cards;
    DashedButton add;
    int dropIndex = -1;
};
} // namespace koe::ui::mainui
