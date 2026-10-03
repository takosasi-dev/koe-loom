// 案 C Mono: the effect chain as a rack of rows, top to bottom (docs/mockups/C-S01.dc.html), owner wave5/mono.
// Same calls as the Studio chain strip: ON/OFF (heavy limit with its reason), move, remove (looper check, E-27),
// click = S-09, drag to reorder (F-04-5), 自動停止 marks, 「エフェクトを追加（あと n スロット）」.
#include "UI/skins/mono/MonoParts.h"

#include "Effects/EffectRegistry.h"

#include <cmath>

namespace koe::ui::mono
{
namespace
{
const Palette& P() { return Theme::colours(); }

constexpr int kToggleW = Theme::toggleW + Theme::space2 + 28; // ToggleSwitch::preferredWidth()
constexpr int kButton = Theme::space4 + Theme::space1;         // 28: up / down / remove
constexpr int kHeaderH = Theme::space5 + Theme::space1;        // 36
int addHeight (bool compact) { return compact ? Theme::space5 : Theme::space5 + Theme::space2; } // 32 / 40

int heavyOnCount (const std::vector<SlotDef>& chain)
{
    int n = 0;
    for (auto& s : chain)
        if (auto* i = findEffectInfo (s.type); i != nullptr && s.enabled && i->weight == EffectWeight::heavy) ++n;
    return n;
}

/** Square outlined chevron button; the up arrow is the down chevron turned over (Icons has no chevronUp). */
class ArrowButton : public juce::Button
{
public:
    ArrowButton (const juce::String& name, bool upward) : juce::Button (name), up (upward)
    {
        setTooltip (name);
        setTitle (name);
        setWantsKeyboardFocus (true);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto& p = P();
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        if (highlighted || down) { g.setColour (p.raised); g.fillRoundedRectangle (r, Theme::radiusS); }
        g.setColour (p.border);
        g.drawRoundedRectangle (r, Theme::radiusS, 1.0f);
        const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.5f;
        const auto area = r.withSizeKeepingCentre (s, s);
        {
            juce::Graphics::ScopedSaveState state (g);
            if (up) g.addTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::pi, area.getCentreX(), area.getCentreY()));
            drawIcon (g, Icon::chevronDown, area, isEnabled() ? p.text : p.textSub.withAlpha (0.5f));
        }
        if (hasKeyboardFocus (true)) drawFocusRing (g, r, Theme::radiusS);
    }

private:
    const bool up;
};
} // namespace

// =============================================================================================== row
class ChainRack::Row : public juce::Component, public juce::SettableTooltipClient
{
public:
    Row (ChainRack& r, AppController& ctl, Navigator& n, int i)
        : index (i), rack (r), c (ctl), nav (n), up (ja ("上へ"), true), down (ja ("下へ"), false), remove (ja ("削除"), Icon::close)
    {
        setWantsKeyboardFocus (true);
        setComponentID ("chain.slot." + juce::String (index));
        setTitle (ja ("スロット ") + juce::String (index + 1));
        toggle.setComponentID ("chain.slot." + juce::String (index) + ".toggle");
        toggle.setTitle (ja ("スロット ") + juce::String (index + 1) + " ON/OFF");
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
        up.setComponentID ("chain.slot." + juce::String (index) + ".up");
        down.setComponentID ("chain.slot." + juce::String (index) + ".down");
        remove.setComponentID ("chain.slot." + juce::String (index) + ".remove");
        up.onClick = [this] { mainui::withLooperCheck (c, nav, *this, [this] { c.moveSlot (index, index - 1); }); };
        down.onClick = [this] { mainui::withLooperCheck (c, nav, *this, [this] { c.moveSlot (index, index + 1); }); };
        remove.onClick = [this] { mainui::withLooperCheck (c, nav, *this, [this] { c.removeSlot (index); }); };
        for (auto* b : std::initializer_list<juce::Component*> { &up, &down, &remove }) addAndMakeVisible (b);
    }

    void update (const SlotDef& slot, bool stopped)
    {
        const auto* newInfo = findEffectInfo (slot.type);
        const int heavyOn = heavyOnCount (c.getChain());
        const int context = int (c.getChain().size()) * 16 + heavyOn;
        if (newInfo == info && info != nullptr && slot == last && stopped == autoStopped && context == lastContext) return;
        last = slot;
        lastContext = context;
        if (! built || newInfo != info)
        {
            built = true;
            info = newInfo;
            params.clear();
            paramIndex = info != nullptr ? mainui::mainParams (*info) : std::vector<int>();
            for (int pi : paramIndex)
            {
                const auto spec = info->params[size_t (pi)];
                auto* row = params.add (new SliderRow (juce::String::fromUTF8 (spec.nameJa), [spec] (double v) { return mainui::formatParam (spec, float (v)); }));
                row->stacked = true;
                mainui::applyParamRange (row->slider, spec, spec.def);
                row->slider.setDoubleClickReturnValue (true, spec.def);
                row->slider.setTitle (juce::String::fromUTF8 (spec.nameJa));
                row->slider.setTooltip (mainui::paramLabel (spec));
                row->onChange = [this, pi] (double v) { c.setSlotParam (index, pi, float (v)); };
                addAndMakeVisible (row);
            }
            const auto name = info != nullptr ? juce::String::fromUTF8 (info->nameJa) : juce::String (slot.type);
            const auto desc = info != nullptr ? juce::String::fromUTF8 (info->descJa) : ja ("この版では使えないエフェクトです");
            setTooltip (name + ja ("：") + desc + ja ("（押すと全パラメータを編集）"));
            resized();
        }
        for (size_t k = 0; k < paramIndex.size(); ++k)
            if (size_t (paramIndex[k]) < slot.params.size())
            {
                params[int (k)]->slider.setValue (slot.params[size_t (paramIndex[k])], juce::dontSendNotification);
                params[int (k)]->repaint();
            }
        enabled = slot.enabled;
        autoStopped = stopped;
        toggle.setToggleState (slot.enabled, juce::dontSendNotification);
        const bool blocked = info != nullptr && info->weight == EffectWeight::heavy && ! slot.enabled && heavyOn >= kMaxHeavyOn; // §8.3
        toggle.setTooltip (blocked ? ja ("重いエフェクトは同時に 2 つまでしか ON にできません。ほかの重いエフェクトを OFF にしてください。")
                                   : ja ("スロット ") + juce::String (index + 1) + ja (" の ON/OFF"));
        up.setEnabled (index > 0);
        down.setEnabled (index < int (c.getChain().size()) - 1);
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (Theme::space2 + Theme::space1, 0);
        for (auto* b : std::initializer_list<juce::Component*> { &remove, &down, &up })
        {
            b->setBounds (r.removeFromRight (kButton).withSizeKeepingCentre (kButton, kButton));
            r.removeFromRight (Theme::space1);
        }
        r.removeFromRight (Theme::space1);
        toggle.setBounds (r.removeFromRight (kToggleW).withSizeKeepingCentre (kToggleW, Theme::toggleH + 2));
        r.removeFromRight (Theme::space3);
        numArea = r.removeFromLeft (Theme::space4);
        r.removeFromLeft (Theme::space2);
        // name : param : param = 3 : 3.5 : 3.5, the badge sits after the name
        const int gap = Theme::space3;
        const int paramW = juce::roundToInt ((r.getWidth() - 2 * gap) * 0.35f);
        auto p2 = r.removeFromRight (paramW);
        r.removeFromRight (gap);
        auto p1 = r.removeFromRight (paramW);
        r.removeFromRight (gap);
        nameArea = r;
        const auto cell = [] (juce::Rectangle<int> a) { return a.withSizeKeepingCentre (a.getWidth(), SliderRow::stackedHeight); };
        if (params.size() > 0) params[0]->setBounds (cell (p1));
        if (params.size() > 1) params[1]->setBounds (cell (p2));
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        const auto b = getLocalBounds().toFloat();
        if (index % 2 == 1 || isMouseOver (false) || dragging)
        {
            g.setColour (p.raised);
            g.fillRect (b);
        }
        g.setColour (p.divider);
        g.fillRect (b.withTop (b.getBottom() - 1.0f));
        drawText (g, juce::String (index + 1).paddedLeft ('0', 2), numArea, Theme::fontS, p.textSub, juce::Justification::centredLeft, false, true);

        auto r = nameArea;
        if (info != nullptr)
        {
            const auto badge = juce::String::fromUTF8 (weightNameJa (info->weight));
            const int bw = juce::jmax (Theme::space4 - 4, textWidthOf (badge) + Theme::space2 + Theme::space1);
            paintWeightBadge (g, r.removeFromRight (bw).withSizeKeepingCentre (bw, Theme::space4 - 4).toFloat(), badge, info->weight == EffectWeight::heavy);
            r.removeFromRight (Theme::space2);
        }
        if (autoStopped)
        {
            const auto t = ja ("自動停止");
            drawText (g, t, r.removeFromRight (textWidthOf (t)), Theme::fontXS, p.warn, juce::Justification::centredRight, true);
            r.removeFromRight (Theme::space1);
        }
        const auto name = info != nullptr ? juce::String::fromUTF8 (info->nameJa) : ja ("（不明なエフェクト）");
        const auto f = Theme::ui (Theme::fontS, true);
        g.setColour (enabled ? p.text : p.textSub);
        g.setFont (f);
        const auto paren = juce::String::fromUTF8 ("（");
        const auto shown = juce::GlyphArrangement::getStringWidth (f, name) > float (r.getWidth()) && name.contains (paren)
                               ? name.upToFirstOccurrenceOf (paren, false, false).trimEnd() : name;
        g.drawFittedText (shown, r, juce::Justification::centredLeft, 1, 0.8f);
        if (info == nullptr) drawText (g, ja ("この版では使えません"), r.withTrimmedLeft (r.getWidth() / 2), Theme::fontXS, p.textSub);
        if (hasKeyboardFocus (false)) drawFocusRing (g, b.reduced (1.0f), Theme::radiusS);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseDown (const juce::MouseEvent&) override { dragging = false; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging && e.getDistanceFromDragStart() > Theme::space1) dragging = true;
        if (dragging) rack.dragMove (*this, e);
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragging)
        {
            dragging = false;
            rack.dragEnd (*this);
        }
        else if (! e.mods.isPopupMenu() && e.mouseWasClicked())
            nav.showSlotDetail (index);
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::returnKey || k == juce::KeyPress::spaceKey) { nav.showSlotDetail (index); return true; }
        if (k == juce::KeyPress::deleteKey) { remove.triggerClick(); return true; }
        if (k.getModifiers().isCtrlDown() && k.isKeyCode (juce::KeyPress::upKey) && up.isEnabled()) { up.triggerClick(); return true; }
        if (k.getModifiers().isCtrlDown() && k.isKeyCode (juce::KeyPress::downKey) && down.isEnabled()) { down.triggerClick(); return true; }
        return false;
    }
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }

    const int index;
    bool dragging = false;

private:
    static int textWidthOf (const juce::String& t) { return juce::roundToInt (std::ceil (textRunWidth (t, Theme::fontXS, true, false))); }
    ChainRack& rack;
    AppController& c;
    Navigator& nav;
    const EffectInfo* info = nullptr;
    std::vector<int> paramIndex;
    juce::OwnedArray<SliderRow> params;
    SlotDef last;
    int lastContext = -1;
    bool enabled = true, autoStopped = false, built = false;
    ToggleSwitch toggle;
    ArrowButton up, down;
    SquareIconButton remove;
    juce::Rectangle<int> numArea, nameArea;
};

// =============================================================================================== rack
ChainRack::ChainRack (AppController& ctl, Navigator& n) : c (ctl), nav (n), add (ja ("エフェクトを追加"))
{
    setComponentID ("tour.chain");
    view.setViewedComponent (&content, false);
    view.setScrollBarsShown (true, false);
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

ChainRack::~ChainRack() = default;

int ChainRack::heightFor (int slots, bool compact)
{
    return kHeaderH + slots * rowHeight (compact) + Theme::space2 + addHeight (compact) + Theme::space2;
}

void ChainRack::setCompact (bool cp)
{
    if (compact == cp) return;
    compact = cp;
    resized();
}

juce::Rectangle<int> ChainRack::headerArea() const { return getLocalBounds().reduced (Theme::space3, 0).removeFromTop (kHeaderH); }

void ChainRack::resized()
{
    auto r = getLocalBounds().reduced (1);
    r.removeFromTop (kHeaderH - 1);
    view.setBounds (r);
    layoutRows();
}

void ChainRack::layoutRows()
{
    auto& animator = juce::Desktop::getInstance().getAnimator();
    const int rh = rowHeight (compact);
    const int total = rows.size() * rh + Theme::space2 + addHeight (compact) + Theme::space2;
    const int w = view.getWidth() - (total > view.getHeight() ? view.getScrollBarThickness() : 0);
    for (auto* row : rows)
    {
        animator.cancelAnimation (row, false);
        row->setBounds (0, row->index * rh, w, rh);
    }
    add.setBounds (Theme::space3, rows.size() * rh + Theme::space2, juce::jmax (0, w - 2 * Theme::space3), addHeight (compact));
    content.setSize (w, total);
}

void ChainRack::refresh()
{
    const auto& chain = c.getChain();
    juce::StringArray now;
    for (auto& s : chain) now.add (juce::String (s.type));
    if (now != types)
    {
        types = now;
        rows.clear();
        for (int i = 0; i < int (chain.size()); ++i) content.addAndMakeVisible (rows.add (new Row (*this, c, nav, i)));
        layoutRows();
    }
    for (int i = 0; i < rows.size(); ++i) rows[i]->update (chain[size_t (i)], c.isSlotAutoStopped (i));
    const int left = kMaxSlots - int (chain.size());
    add.setButtonText (left > 0 ? ja ("エフェクトを追加（あと ") + juce::String (left) + ja (" スロット）") : ja ("スロットは 10 個までです"));
    if (const int header = int (chain.size()) * 16 + heavyOnCount (chain); header != headerState)
    {
        headerState = header;
        repaint();
    }
}

void ChainRack::paint (juce::Graphics& g)
{
    const auto& p = P();
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (p.surface);
    g.fillRoundedRectangle (b, Theme::radiusM);
    g.setColour (p.divider);
    g.drawRoundedRectangle (b, Theme::radiusM, 1.0f);
    g.fillRect (1, kHeaderH - 1, getWidth() - 2, 1);

    const auto& chain = c.getChain();
    const int heavyOn = heavyOnCount (chain);
    auto r = headerArea();
    const auto title = ja ("エフェクトチェーン");
    drawText (g, title, r.removeFromLeft (mainui::textWidth (Theme::ui (Theme::fontS, true), title) + Theme::space1), Theme::fontS, p.text,
              juce::Justification::centredLeft, true);
    r.removeFromLeft (Theme::space3);
    const auto count = juce::String (int (chain.size())) + " / " + juce::String (kMaxSlots);
    drawText (g, count, r.removeFromLeft (juce::roundToInt (std::ceil (textRunWidth (count, Theme::fontS, false, true))) + Theme::space1), Theme::fontS,
              p.textSub, juce::Justification::centredLeft, false, true);
    r.removeFromLeft (Theme::space3);
    const auto heavy = (compact ? ja ("重 ") : ja ("重いエフェクト ON ")) + juce::String (heavyOn) + " / " + juce::String (kMaxHeavyOn);
    drawText (g, heavy, r.removeFromLeft (mainui::textWidth (Theme::ui (Theme::fontXS), heavy) + Theme::space1), Theme::fontXS,
              heavyOn >= kMaxHeavyOn ? p.warn : p.textSub);
    drawText (g, compact ? ja ("上から下へ") : ja ("処理は上から下へ"), r, Theme::fontXS, p.textSub, juce::Justification::centredRight);
}

void ChainRack::paintOverChildren (juce::Graphics& g)
{
    const auto& p = P();
    const auto vb = view.getBounds();
    if (rows.isEmpty())
        drawText (g, ja ("エフェクトなし。下の［エフェクトを追加］で入れられます"),
                  vb.withTrimmedTop (addHeight (compact) + Theme::space3).withHeight (Theme::space4).reduced (Theme::space3, 0), Theme::fontS, p.textSub);
    if (dropIndex >= 0)
    {
        const int from = [this] { for (auto* row : rows) if (row->dragging) return row->index; return -1; }();
        if (from >= 0 && from != dropIndex)
        {
            const int rh = rowHeight (compact);
            const int y = vb.getY() + content.getY() + dropIndex * rh + (dropIndex > from ? rh : 0);
            g.reduceClipRegion (vb);
            g.setColour (p.accent);
            g.fillRect (vb.getX(), y - 1, vb.getWidth(), 3);
        }
    }
}

void ChainRack::dragMove (Row& row, const juce::MouseEvent& e)
{
    const int mouseY = e.getEventRelativeTo (&content).y;
    row.setTopLeftPosition (row.getX(), mouseY - e.getMouseDownY());
    row.toFront (false);
    dropIndex = juce::jlimit (0, rows.size() - 1, juce::roundToInt (float (row.getY()) / float (rowHeight (compact))));
    const auto inView = e.getEventRelativeTo (&view).getPosition();
    view.autoScroll (inView.x, inView.y, Theme::space5, Theme::space3);
    repaint();
}

void ChainRack::dragEnd (Row& row)
{
    const int from = row.index, to = dropIndex;
    dropIndex = -1;
    layoutRows();
    repaint();
    if (to >= 0 && to != from) mainui::withLooperCheck (c, nav, *this, [this, from, to] { c.moveSlot (from, to); });
}
} // namespace koe::ui::mono
