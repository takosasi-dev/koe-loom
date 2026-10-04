#include "UI/main/Panels.h"

#include "Core/Paths.h"
#include "Effects/EffectRegistry.h"

namespace koe::ui::mainui
{
namespace
{
const Palette& P() { return Theme::colours(); }

constexpr int kPad = Theme::space3 + Theme::space1;        // 20 (mock)
constexpr int kRowH = Theme::space5 * 2;                   // 64
constexpr int kFooterH = Theme::space5 + Theme::space3;    // 48
constexpr int kCellW = Theme::space5 * 4;                  // 128
constexpr int kCellH = Theme::space5 * 2 + Theme::space4;  // 88
constexpr int kNumCategories = 8;
constexpr int kFileStatusH = Theme::space3 + 4;           // the line under the convolution file row
} // namespace

// =============================================================================================== S-07
class EffectPicker::Row : public juce::Component, public juce::SettableTooltipClient
{
public:
    Row (const EffectInfo& i, std::function<void()> onAdd)
        : info (i), available (hasEffectFactory (i.type)),
          add (available ? ja ("追加") : ja ("準備中"), available ? PillButton::Style::accentOutline : PillButton::Style::outline, available ? std::optional<Icon> (Icon::plus) : std::nullopt)
    {
        setComponentID ("picker." + juce::String (info.type));
        add.setComponentID ("picker." + juce::String (info.type) + ".add");
        add.setTitle (juce::String::fromUTF8 (info.nameJa) + (available ? ja ("を追加") : ja ("（準備中）")));
        add.setEnabled (available);
        add.onClick = std::move (onAdd);
        add.setFontSize (Theme::fontXS);
        addAndMakeVisible (add);
        setTooltip (juce::String::fromUTF8 (info.nameJa) + ja ("：") + juce::String::fromUTF8 (info.descJa));
    }

    void resized() override
    {
        const int w = juce::jmax (add.preferredWidth(), Theme::space5 * 3);
        add.setBounds (getLocalBounds().reduced (kPad, 0).removeFromRight (w).withSizeKeepingCentre (w, Theme::buttonH));
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        g.setColour (p.divider);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);
        auto r = getLocalBounds().reduced (kPad, Theme::space2 + Theme::space1).withTrimmedRight (add.getWidth() + Theme::space3);
        auto top = r.removeFromTop (r.getHeight() / 2 + 2);
        const auto name = juce::String::fromUTF8 (info.nameJa);
        const auto nameF = Theme::ui (Theme::fontM, true);
        const int nameW = juce::jmin (top.getWidth() - Theme::space5, textWidth (nameF, name) + Theme::space1);
        drawText (g, name, top.removeFromLeft (nameW), Theme::fontM, available ? p.text : p.textSub, juce::Justification::centredLeft, true);
        top.removeFromLeft (Theme::space2 + 2);
        const auto badge = juce::String::fromUTF8 (weightNameJa (info.weight));
        const int bw = juce::jmax (20, textWidth (Theme::ui (Theme::fontXS, true), badge) + Theme::space2 + Theme::space1);
        paintWeightBadge (g, top.removeFromLeft (bw).withSizeKeepingCentre (bw, 20).toFloat(), badge, info.weight == EffectWeight::heavy);
        if (! available)
        {
            top.removeFromLeft (Theme::space2);
            drawText (g, ja ("この版ではまだ使えません"), top, Theme::fontXS, p.textSub);
        }
        drawText (g, juce::String::fromUTF8 (info.descJa), r, Theme::fontS, p.textSub);
    }

    const EffectInfo& info;
    const bool available;
    PillButton add;
};

EffectPicker::EffectPicker (AppController& ctl, Navigator& n, int at) : PanelBase (n, ja ("エフェクトを追加")), c (ctl), insertAt (at)
{
    setComponentID ("panel.effectPicker");
    const int slotNo = (insertAt >= 0 ? insertAt : int (c.getChain().size())) + 1;
    subtitle = ja ("追加先: スロット ") + juce::String (slotNo);

    search.setComponentID ("picker.search");
    search.setTitle (ja ("エフェクトの名前で探す"));
    search.setTextToShowWhenEmpty (ja ("エフェクトの名前で探す"), P().textSub);
    search.setFont (Theme::ui (Theme::fontS));
    search.setIndents (Theme::space5 + Theme::space1, 0);
    search.setJustification (juce::Justification::centredLeft);
    search.onTextChange = [this] { rebuild(); };
    addAndMakeVisible (search);

    for (int i = -1; i < kNumCategories; ++i)
    {
        auto* chip = filters.add (new ChipButton (i < 0 ? ja ("すべて") : juce::String::fromUTF8 (categoryNameJa (EffectCategory (i))), true));
        chip->setComponentID ("picker.filter." + juce::String (i));
        chip->setToggleState (i == category, juce::dontSendNotification);
        chip->onClick = [this, i] { setCategory (i); };
        addAndMakeVisible (chip);
    }

    view.setViewedComponent (&list, false);
    view.setScrollBarsShown (true, false);
    view.setScrollBarThickness (Theme::space2);
    addAndMakeVisible (view);
    rebuild();
    setSize (880, 660); // A-S07: 880 x 660 (OverlayHost fits it into small windows)
}

void EffectPicker::setSearchText (const juce::String& text) { search.setText (text, true); rebuild(); }

void EffectPicker::setCategory (int cat)
{
    category = cat;
    for (int i = 0; i < filters.size(); ++i) filters[i]->setToggleState (i - 1 == category, juce::dontSendNotification);
    rebuild();
}

std::vector<std::string> EffectPicker::listedTypes() const
{
    std::vector<std::string> v;
    for (auto* r : rows) v.push_back (r->info.type);
    return v;
}

void EffectPicker::rebuild()
{
    rows.clear();
    const auto query = search.getText().trim();
    for (auto& info : allEffectInfos())
    {
        if (category >= 0 && int (info.category) != category) continue;
        if (query.isNotEmpty() && ! juce::String::fromUTF8 (info.nameJa).containsIgnoreCase (query) && ! juce::String (info.type).containsIgnoreCase (query))
            continue; // F-04-15: partial match on the name
        auto& ctl = c;
        auto& n = nav;
        const std::string type = info.type;
        const int at = insertAt;
        auto addFn = [&ctl, &n, type, at]
        {
            // captures no panel pointer: E-27 may replace this panel with a confirmation first
            withLooperCheck (ctl, n, [&ctl, &n, type, at]
            {
                juce::String why;
                const int before = int (ctl.getChain().size());
                if (! ctl.addEffect (type, why))
                {
                    n.showToast (why);
                    return;
                }
                if (at >= 0 && at < before) ctl.moveSlot (before, at);
                n.closeOverlay();
            });
        };
        list.addAndMakeVisible (rows.add (new Row (info, addFn)));
    }
    resized();
}

void EffectPicker::resized()
{
    PanelBase::resized();
    auto r = contentArea();
    footer = r.removeFromBottom (kFooterH);
    auto top = r.reduced (kPad, 0);
    search.setBounds (top.removeFromTop (Theme::controlH));
    top.removeFromTop (Theme::space2 + Theme::space1);
    // chips wrap onto more lines in small windows
    int x = top.getX(), y = top.getY();
    for (auto* chip : filters)
    {
        const int w = chip->preferredWidth();
        if (x + w > top.getRight() && x > top.getX())
        {
            x = top.getX();
            y += Theme::touchMin + Theme::space2;
        }
        chip->setBounds (x, y, w, Theme::touchMin);
        x += w + Theme::space2;
    }
    const int listTop = y + Theme::touchMin + Theme::space3;
    view.setBounds (getLocalBounds().withTop (listTop).withBottom (footer.getY()).reduced (1, 0));
    const int w = view.getWidth() - (int (rows.size()) * kRowH > view.getHeight() ? view.getScrollBarThickness() : 0);
    for (int i = 0; i < rows.size(); ++i) rows[i]->setBounds (0, i * kRowH, w, kRowH);
    list.setSize (w, juce::jmax (1, rows.size() * kRowH));
}

void EffectPicker::paint (juce::Graphics& g)
{
    PanelBase::paint (g);
    const auto& p = P();
    g.setColour (p.divider);
    g.fillRect (1, view.getY() - 1, getWidth() - 2, 1);
    if (rows.isEmpty())
        drawText (g, ja ("見つかりません。名前の一部で探してください。"), view.getBounds().withHeight (kRowH), Theme::fontS, p.textSub, juce::Justification::centred);
    juce::Path clip;
    clip.addRoundedRectangle (float (footer.getX()), float (footer.getY()), float (footer.getWidth()), float (footer.getHeight()), Theme::radiusL, Theme::radiusL,
                              false, false, true, true);
    g.setColour (p.bg);
    g.fillPath (clip);
    g.setColour (p.divider);
    g.fillRect (footer.getX() + 1, footer.getY(), footer.getWidth() - 2, 1);
    auto f = footer.reduced (kPad, 0);
    drawIcon (g, Icon::warning, f.removeFromLeft (16).withSizeKeepingCentre (16, 16).toFloat(), p.textSub);
    f.removeFromLeft (Theme::space2 + 2);
    drawText (g, ja ("軽・中・重は CPU 負荷の目安です。重いエフェクトは、同時に 2 つまで ON にできます。"), f, Theme::fontXS, p.textSub);
}

void EffectPicker::paintOverChildren (juce::Graphics& g)
{
    const auto s = search.getBounds();
    drawIcon (g, Icon::search, juce::Rectangle<float> (float (s.getX() + Theme::space2 + Theme::space1), float (s.getCentreY() - 8), 16.0f, 16.0f), P().textSub);
}

// =============================================================================================== S-09
// 声の大きさで動かす (wave9/voice, INTERFACES.md §11.3): target (なし / かかり具合 / a numeric knob), depth -100..+100 %
// that sticks at 0, and a small meter of the voice level the slot follows (AppController::getModLevel).
namespace
{
constexpr int kModRowH = Theme::touchMin + Theme::space1 + Theme::space3; // controls + the hint line (52)

juce::String depthText (double v) { return (v > 0.0 ? "+" : "") + juce::String (juce::roundToInt (v)) + " %"; }

/** -100..+100 %, filled from the middle, stops at 0 on the way through. */
class DepthSlider : public ValueSlider
{
public:
    DepthSlider() { setup (-100.0, 100.0, 0.0, 1.0, depthText); }
    double snapValue (double v, DragMode) override { return std::abs (v) < 8.0 ? 0.0 : v; }
    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        auto r = getLocalBounds().toFloat();
        auto text = r.removeFromRight (float (valueTextWidth));
        auto track = r.reduced (8.0f, 0.0f).withSizeKeepingCentre (r.getWidth() - 16.0f, 6.0f);
        g.setColour (p.border);
        g.fillRoundedRectangle (track, 3.0f);
        const float x0 = track.getCentreX(), x = track.getX() + track.getWidth() * float (valueToProportionOfLength (getValue()));
        g.setColour (isEnabled() ? p.accent : p.textSub);
        g.fillRoundedRectangle (juce::Rectangle<float>::leftTopRightBottom (std::min (x0, x), track.getY(), std::max (x0, x), track.getBottom()), 3.0f);
        g.setColour (p.textSub);
        g.fillRect (juce::Rectangle<float> (1.0f, 12.0f).withCentre ({ x0, track.getCentreY() }));
        const auto thumb = juce::Rectangle<float> (16.0f, 16.0f).withCentre ({ x, track.getCentreY() });
        g.setColour (isEnabled() ? p.text : p.textSub);
        g.fillEllipse (thumb);
        g.setColour (p.raised);
        g.drawEllipse (thumb, 2.0f);
        drawText (g, depthText (getValue()), text.toNearestInt(), Theme::fontS, isEnabled() ? p.text : p.textSub, juce::Justification::centredRight, true, true);
        if (hasKeyboardFocus (true)) drawFocusRing (g, thumb, 8.0f);
    }
};

class ModMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    void setLevel (float v)
    {
        v = juce::jlimit (0.0f, 1.0f, v);
        if (std::abs (v - level) < 0.005f) return;
        level = v;
        repaint();
    }
    float getLevel() const { return level; }
    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        const auto b = getLocalBounds().toFloat().withSizeKeepingCentre (float (getWidth()), 8.0f);
        g.setColour (p.trackOff);
        g.fillRoundedRectangle (b, 4.0f);
        if (level > 0.0f)
        {
            g.setColour (isEnabled() ? p.accent : p.textSub);
            g.fillRoundedRectangle (b.withWidth (juce::jmax (b.getHeight(), b.getWidth() * level)), 4.0f);
        }
    }

private:
    float level = 0.0f;
};
} // namespace

class SlotDetailPanel::ModRow : public juce::Component
{
public:
    ModRow (AppController& ctl, int s, const EffectInfo* info) : c (ctl), slot (s)
    {
        setComponentID ("slotDetail.mod");
        targets.push_back ("");
        target.addItem (ja ("なし"), 1);
        targets.push_back ("wet");
        target.addItem (ja ("かかり具合"), 2);
        if (info != nullptr)
            for (auto& spec : info->params)
                if (! spec.isChoice()) // a choice cannot move smoothly (INTERFACES.md §11.1)
                {
                    targets.push_back (spec.id);
                    target.addItem (juce::String::fromUTF8 (spec.nameJa), int (targets.size()));
                }
        target.setComponentID ("slotDetail.modTarget");
        target.setTitle (ja ("声の大きさで動かす先"));
        target.setTooltip (ja ("声の大きさに合わせて動かすつまみを選びます。"));
        target.onChange = [this]
        {
            const int id = target.getSelectedId();
            if (id <= 0) return;
            if (id > 1 && depth.getValue() == 0.0) depth.setValue (50.0, juce::dontSendNotification); // something to hear at once
            apply();
        };
        depth.setComponentID ("slotDetail.modDepth");
        depth.setTitle (ja ("声の大きさで動かす深さ"));
        depth.setTooltip (ja ("大きな声ほど強く。マイナスにすると逆（大きな声ほど弱く）。0 で止まります。"));
        depth.onValueChange = [this] { apply(); };
        meter.setComponentID ("slotDetail.modMeter");
        meter.setTooltip (ja ("いまの声の大きさ（このスロットが追っている値）"));
        for (auto* comp : { (juce::Component*) &target, (juce::Component*) &depth, (juce::Component*) &meter })
            addAndMakeVisible (comp);
        update();
        tick();
    }

    /** From the slot (after any change). */
    void update()
    {
        const auto& chain = c.getChain();
        if (slot < 0 || slot >= int (chain.size())) return;
        const auto& s = chain[size_t (slot)];
        int id = 1;
        for (size_t i = 0; i < targets.size(); ++i)
            if (targets[i] == s.modTarget) id = int (i) + 1;
        target.setSelectedId (id, juce::dontSendNotification);
        depth.setValue (double (s.modDepth) * 100.0, juce::dontSendNotification);
        depth.setEnabled (id > 1);
        meter.setEnabled (id > 1);
        repaint();
    }

    void tick() { meter.setLevel (target.getSelectedId() > 1 ? c.getModLevel() : 0.0f); }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromTop (Theme::space1); // the divider line
        hint = r.removeFromBottom (Theme::space3);
        label = r.removeFromLeft (Theme::space5 * 4 - Theme::space2);
        meter.setBounds (r.removeFromRight (Theme::space5 * 2));
        r.removeFromRight (Theme::space3);
        target.setBounds (r.removeFromLeft (juce::jmin (Theme::space5 * 5, r.getWidth() / 2)));
        r.removeFromLeft (Theme::space2);
        depth.setBounds (r);
        hint = hint.withLeft (target.getX());
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        g.setColour (p.divider);
        g.drawHorizontalLine (0, 0.0f, float (getWidth()));
        drawText (g, ja ("声の大きさで動かす"), label, Theme::fontS, p.text, juce::Justification::centredLeft, true);
        drawText (g, ja ("大きな声ほど強く。マイナスにすると逆。"), hint, Theme::fontXS, p.textSub);
    }

private:
    void apply()
    {
        const int id = target.getSelectedId();
        if (id <= 0) return;
        c.setSlotMod (slot, targets[size_t (id - 1)], float (depth.getValue() / 100.0));
        update();
    }

    AppController& c;
    const int slot;
    std::vector<std::string> targets; // combo id - 1 -> SlotDef::modTarget
    juce::ComboBox target;
    DepthSlider depth;
    ModMeter meter;
    juce::Rectangle<int> label, hint;
};

SlotDetailPanel::SlotDetailPanel (AppController& ctl, Navigator& n, int s)
    : PanelBase (n, {}), c (ctl), slot (s),
      type (s >= 0 && s < int (ctl.getChain().size()) ? ctl.getChain()[size_t (s)].type : std::string())
{
    setComponentID ("panel.slotDetail");
    info = findEffectInfo (type);
    title = info != nullptr ? juce::String::fromUTF8 (info->nameJa) : ja ("スロット");
    subtitle = ja ("スロット ") + juce::String (slot + 1) + (info != nullptr ? ja (" ・ 重さ ") + juce::String::fromUTF8 (weightNameJa (info->weight)) : juce::String());

    toggle.setComponentID ("slotDetail.toggle");
    toggle.setTitle (ja ("スロット ") + juce::String (slot + 1) + " ON/OFF");
    toggle.onClick = [this]
    {
        juce::String why;
        if (! c.setSlotEnabled (slot, toggle.getToggleState(), why))
        {
            toggle.setToggleState (! toggle.getToggleState(), juce::dontSendNotification);
            nav.showToast (why);
        }
    };
    addAndMakeVisible (toggle);

    if (info != nullptr)
        for (int pi = 0; pi < int (info->params.size()); ++pi)
        {
            const auto& spec = info->params[size_t (pi)];
            Cell cell;
            cell.param = pi;
            if (spec.isChoice())
            {
                cell.combo = std::make_unique<juce::ComboBox>();
                for (int k = 0; k < int (spec.choices.size()); ++k) cell.combo->addItem (juce::String::fromUTF8 (spec.choices[size_t (k)].second), k + 1);
                cell.combo->setTitle (juce::String::fromUTF8 (spec.nameJa));
                cell.combo->setTooltip (paramLabel (spec));
                cell.combo->setComponentID ("slotDetail.param." + juce::String (pi));
                auto* box = cell.combo.get();
                box->onChange = [this, pi, box] { if (box->getSelectedId() > 0) c.setSlotParam (slot, pi, float (box->getSelectedId() - 1)); };
                addAndMakeVisible (*box);
            }
            else
            {
                cell.knob = std::make_unique<Knob> (Knob::Size::small);
                auto* k = cell.knob.get();
                k->setup (spec.min, spec.max, spec.def, spec.integer ? 1.0 : 0.0, [spec] (double v) { return formatParam (spec, float (v)); });
                applyParamRange (*k, spec, spec.def);
                k->setLabel (juce::String::fromUTF8 (spec.nameJa)); // the knob adds range and default to its tooltip
                k->setTitle (juce::String::fromUTF8 (spec.nameJa));
                k->setComponentID ("slotDetail.param." + juce::String (pi));
                k->onValueChange = [this, pi, k] { c.setSlotParam (slot, pi, float (k->getValue())); repaint(); };
                addAndMakeVisible (*k);
            }
            cells.push_back (std::move (cell));
        }

    // F-04-18 / F-04-19: momentary controls
    if (type == "freeze")
    {
        action1 = std::make_unique<PillButton> (ja ("フリーズ"), PillButton::Style::primary);
        action1->onClick = [this] { c.triggerSlot (slot, EffectTrigger::freezeToggle); };
    }
    else if (type == "looper")
    {
        action1 = std::make_unique<PillButton> (ja ("録音"), PillButton::Style::primary);
        action1->onClick = [this] { c.triggerSlot (slot, EffectTrigger::looperRecordPlay); };
        action2 = std::make_unique<PillButton> (ja ("消去"), PillButton::Style::outline);
        action2->onClick = [this] { c.triggerSlot (slot, EffectTrigger::looperClear); };
    }
    for (auto* b : { action1.get(), action2.get() })
        if (b != nullptr) addAndMakeVisible (*b);
    if (info != nullptr) // wave9/voice: 声の大きさで動かす
    {
        modRow = std::make_unique<ModRow> (c, slot, info);
        addAndMakeVisible (*modRow);
    }
    if (action1 != nullptr || modRow != nullptr) startTimerHz (10);

    if (type == "convolution") // INTERFACES.md §9.3
    {
        chooseFile = std::make_unique<PillButton> (ja ("ファイルを選ぶ…"), PillButton::Style::outline);
        chooseFile->setComponentID ("slotDetail.irChoose");
        chooseFile->setTooltip (ja ("WAV / FLAC / AIFF の残響ファイル（10 秒まで）を選びます。ir フォルダにコピーして使います。"));
        chooseFile->onClick = [this] { pickIrFile(); };
        openFolder = std::make_unique<PillButton> (ja ("フォルダを開く"), PillButton::Style::ghost);
        openFolder->setComponentID ("slotDetail.irFolder");
        openFolder->setTooltip (paths::irDir().getFullPathName());
        openFolder->onClick = []
        {
            const auto dir = paths::irDir();
            dir.createDirectory();
            dir.startAsProcess();
        };
        irList = std::make_unique<juce::ComboBox>();
        irList->setComponentID ("slotDetail.irList");
        irList->setTitle (ja ("ir フォルダの残響ファイル"));
        irList->setTooltip (ja ("ir フォルダにある残響ファイルから選びます。"));
        irList->onChange = [this]
        {
            const int id = irList->getSelectedId();
            if (id <= 0) return;
            const auto name = id == 1 ? juce::String() : irList->getItemText (irList->indexOfItemId (id));
            if (name != c.getSlotFileName (slot)) c.setSlotFileName (slot, name);
        };
        for (juce::Component* comp : { (juce::Component*) chooseFile.get(), (juce::Component*) irList.get(), (juce::Component*) openFolder.get() })
            addAndMakeVisible (comp);
        refreshIrList();
    }

    updateValues();
    updateActions();
    c.addChangeListener (this);
    const int rowsN = (int (cells.size()) + 4) / 5;
    setSize (Theme::space5 * 22 + Theme::space3, headerHeight + Theme::space5 + Theme::space3 + juce::jmax (1, rowsN) * kCellH
                                                     + (action1 != nullptr ? Theme::space3 + Theme::buttonH : 0)
                                                     + (irList != nullptr ? Theme::buttonH + kFileStatusH + Theme::space2 : 0)
                                                     + (modRow != nullptr ? kModRowH + Theme::space3 : 0) + Theme::space4);
}

SlotDetailPanel::~SlotDetailPanel() { c.removeChangeListener (this); }

int SlotDetailPanel::numKnobs() const
{
    int n = 0;
    for (auto& cell : cells) n += cell.knob != nullptr;
    return n;
}

int SlotDetailPanel::numChoices() const
{
    int n = 0;
    for (auto& cell : cells) n += cell.combo != nullptr;
    return n;
}

juce::Component* SlotDetailPanel::controlFor (int p) const
{
    for (auto& cell : cells)
        if (cell.param == p) return cell.knob != nullptr ? static_cast<juce::Component*> (cell.knob.get()) : cell.combo.get();
    return nullptr;
}

juce::String SlotDetailPanel::fileStatusText() const
{
    if (irList == nullptr) return {};
    const auto name = c.getSlotFileName (slot);
    if (name.isEmpty()) return ja ("ファイルが未選択です。選ぶまでは声をそのまま通します。");
    if (c.isSlotFileMissing (slot)) return ja ("ファイルが見つかりません（") + name + ja ("）");
    return ja ("使用中: ") + name;
}

bool SlotDetailPanel::fileStatusIsWarning() const { return irList != nullptr && c.isSlotFileMissing (slot); }

void SlotDetailPanel::refreshIrList()
{
    if (irList == nullptr) return;
    shownFile = c.getSlotFileName (slot);
    irList->clear (juce::dontSendNotification);
    irList->addItem (ja ("（なし）"), 1);
    auto names = AppController::listIrFiles();
    if (shownFile.isNotEmpty() && ! names.contains (shownFile)) names.add (shownFile); // missing: still shown as selected
    int selected = 1;
    for (int i = 0; i < names.size(); ++i)
    {
        irList->addItem (names[i], i + 2);
        if (names[i] == shownFile) selected = i + 2;
    }
    irList->setSelectedId (selected, juce::dontSendNotification);
    repaint();
}

void SlotDetailPanel::pickIrFile()
{
    chooser = std::make_unique<juce::FileChooser> (ja ("残響ファイル（WAV / FLAC / AIFF）を選ぶ"), juce::File(), "*.wav;*.flac;*.aif;*.aiff");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<SlotDetailPanel> (this)] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (safe == nullptr || f == juce::File()) return;
                              juce::String why;
                              if (! safe->c.setSlotFile (safe->slot, f, why)) safe->nav.showToast (why);
                              safe->refreshIrList();
                          });
}

int SlotDetailPanel::columns() const { return juce::jmax (1, (getWidth() - Theme::space4 * 2) / kCellW); }

void SlotDetailPanel::updateValues()
{
    const auto& chain = c.getChain();
    if (slot < 0 || slot >= int (chain.size())) return;
    const auto& s = chain[size_t (slot)];
    toggle.setToggleState (s.enabled, juce::dontSendNotification);
    for (auto& cell : cells)
    {
        if (size_t (cell.param) >= s.params.size()) continue;
        const float v = s.params[size_t (cell.param)];
        if (cell.knob != nullptr) cell.knob->setValue (v, juce::dontSendNotification);
        if (cell.combo != nullptr) cell.combo->setSelectedId (juce::roundToInt (v) + 1, juce::dontSendNotification);
    }
    if (irList != nullptr && c.getSlotFileName (slot) != shownFile) refreshIrList();
    if (modRow != nullptr) modRow->update();
    repaint();
}

void SlotDetailPanel::updateActions()
{
    if (action1 == nullptr) return;
    const int state = c.getSlotUiState (slot);
    if (state == uiState) return;
    uiState = state;
    if (type == "freeze") action1->setButtonText (state != 0 ? ja ("フリーズを止める") : ja ("フリーズする"));
    else
    {
        const char* labels[] = { "録音", "再生", "重ね録り", "再生" }; // effects §5.2: empty -> rec -> play -> overdub
        action1->setButtonText (ja (labels[juce::jlimit (0, 3, state)]));
    }
    resized();
    repaint();
}

void SlotDetailPanel::timerCallback()
{
    updateActions();
    if (modRow != nullptr) modRow->tick();
}

void SlotDetailPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    const auto& chain = c.getChain();
    if (slot >= int (chain.size()) || chain[size_t (slot)].type != type)
    {
        nav.closeOverlay(); // the slot moved or was removed under us
        return;
    }
    updateValues();
}

void SlotDetailPanel::resized()
{
    PanelBase::resized();
    auto r = contentArea().reduced (Theme::space4, 0).withTrimmedBottom (Theme::space4);
    auto top = r.removeFromTop (Theme::space5);
    toggle.setBounds (top.removeFromRight (Theme::toggleW + Theme::space2 + 28));
    descArea = top.withTrimmedRight (Theme::space3);
    r.removeFromTop (Theme::space3);
    if (irList != nullptr)
    {
        auto row = r.removeFromTop (Theme::buttonH);
        chooseFile->setBounds (row.removeFromLeft (juce::jmax (Theme::space5 * 4, chooseFile->preferredWidth())));
        openFolder->setBounds (row.removeFromRight (juce::jmax (Theme::space5 * 3, openFolder->preferredWidth())));
        irList->setBounds (row.reduced (Theme::space2, 0).withSizeKeepingCentre (row.getWidth() - Theme::space2 * 2, Theme::touchMin));
        fileStatus = r.removeFromTop (kFileStatusH);
        r.removeFromTop (Theme::space2);
    }
    if (action1 != nullptr)
    {
        auto f = r.removeFromBottom (Theme::buttonH);
        const int w1 = juce::jmax (Theme::space5 * 4, action1->preferredWidth());
        action1->setBounds (f.removeFromLeft (w1));
        if (action2 != nullptr)
        {
            f.removeFromLeft (Theme::space2);
            action2->setBounds (f.removeFromLeft (juce::jmax (Theme::space5 * 3, action2->preferredWidth())));
        }
        f.removeFromLeft (Theme::space3);
        footerText = f;
        r.removeFromBottom (Theme::space3);
    }
    if (modRow != nullptr)
    {
        modRow->setBounds (r.removeFromBottom (kModRowH));
        r.removeFromBottom (Theme::space3);
    }
    const int cols = columns();
    const int cellW = r.getWidth() / cols;
    for (size_t i = 0; i < cells.size(); ++i)
    {
        auto& cell = cells[i];
        const auto box = juce::Rectangle<int> (r.getX() + int (i % size_t (cols)) * cellW, r.getY() + int (i / size_t (cols)) * kCellH, cellW, kCellH)
                             .reduced (Theme::space1, 0);
        if (cell.knob != nullptr)
        {
            cell.knob->setBounds (box.withSizeKeepingCentre (Theme::knobSmall, Theme::knobSmall).withY (box.getY()));
            cell.labelArea = box.withTop (box.getY() + Theme::knobSmall + 2).withHeight (Theme::space3 + 2);
            cell.valueArea = cell.labelArea.translated (0, Theme::space3 + 2);
        }
        else
        {
            cell.labelArea = box.withHeight (Theme::space3 + 2);
            cell.combo->setBounds (box.withTop (cell.labelArea.getBottom() + Theme::space1).withHeight (Theme::touchMin));
        }
    }
}

void SlotDetailPanel::paint (juce::Graphics& g)
{
    PanelBase::paint (g);
    const auto& p = P();
    auto d = descArea;
    if (info != nullptr)
    {
        g.setColour (p.textSub);
        g.setFont (Theme::ui (Theme::fontS));
        g.drawFittedText (juce::String::fromUTF8 (info->descJa), d, juce::Justification::centredLeft, 2, 0.9f);
    }
    if (irList != nullptr)
        drawText (g, fileStatusText(), fileStatus, Theme::fontXS, fileStatusIsWarning() ? p.warn : p.textSub, juce::Justification::centredLeft,
                  fileStatusIsWarning());
    if (c.isSlotAutoStopped (slot))
        drawText (g, ja ("自動停止"), toggle.getBounds().translated (-Theme::space5 * 2 - Theme::space2, 0).withWidth (Theme::space5 * 2), Theme::fontXS, p.warn,
                  juce::Justification::centredRight, true);
    for (auto& cell : cells)
    {
        const auto& spec = info->params[size_t (cell.param)];
        g.setColour (p.textSub);
        g.setFont (Theme::ui (Theme::fontXS));
        g.drawFittedText (juce::String::fromUTF8 (spec.nameJa), cell.labelArea, cell.knob != nullptr ? juce::Justification::centred : juce::Justification::centredLeft, 1, 0.8f);
        if (cell.knob != nullptr)
            drawText (g, formatParam (spec, float (cell.knob->getValue())), cell.valueArea, Theme::fontXS, p.text, juce::Justification::centred, true, true);
    }
    if (action1 != nullptr)
    {
        juce::String state;
        if (type == "freeze") state = uiState != 0 ? ja ("状態: フリーズ中") : ja ("状態: OFF");
        else
        {
            const char* names[] = { "状態: 空", "状態: 録音中", "状態: 再生中", "状態: 重ね録り中" };
            state = ja (names[juce::jlimit (0, 3, uiState)]);
        }
        drawText (g, state, footerText, Theme::fontS, p.text);
    }
}
} // namespace koe::ui::mainui
