// S-03 設定 (docs/mockups/A-S03.dc.html, A-S03-dev.dc.html). Sections without a mock (ホットキー,
// 起動と常駐, 外観, 詳細, 診断) use the same card / row structure.
#include "UI/Screens.h"
#include "UI/screens/Common.h"

#include "Core/Constants.h"
#include "Core/Paths.h"
#include "Platform/Hotkeys.h"

namespace koe::ui
{
using namespace screens;

namespace
{
constexpr int kNumSections = 7;

int sectionIndex (Navigator::SettingsSection s) { return int (s); }

/** S-03 外観: one round swatch per accent colour. The selected one carries a check mark (not colour alone);
    each is a focusable button with the colour's name as title and tooltip. */
class AccentSwatches : public juce::Component
{
public:
    static constexpr int swatch = Theme::touchMin, gap = Theme::space2 + Theme::space1;

    AccentSwatches()
    {
        for (int i = 0; i < Theme::numAccents; ++i)
        {
            auto* b = buttons.add (new Swatch (i));
            b->setComponentID ("settings.accent." + juce::String (i));
            b->onClick = [this, i] { if (onChange) onChange (i); };
            addAndMakeVisible (b);
        }
    }

    static int preferredWidth() { return Theme::numAccents * swatch + (Theme::numAccents - 1) * gap; }
    void setSelected (int i)
    {
        for (int k = 0; k < buttons.size(); ++k) buttons[k]->setToggleState (k == i, juce::dontSendNotification);
    }
    juce::Button* getButton (int i) const { return buttons[i]; }
    std::function<void (int)> onChange;

    void resized() override
    {
        auto r = getLocalBounds().withSizeKeepingCentre (getWidth(), swatch);
        for (auto* b : buttons)
        {
            b->setBounds (r.removeFromLeft (swatch));
            r.removeFromLeft (gap);
        }
    }

private:
    struct Swatch : public juce::Button
    {
        explicit Swatch (int i) : juce::Button (Theme::accentName (i)), index (i)
        {
            setTitle (Theme::accentName (i));
            setTooltip (Theme::accentName (i));
            setWantsKeyboardFocus (true);
        }
        void paintButton (juce::Graphics& g, bool highlighted, bool) override
        {
            const auto& p = Theme::colours();
            const auto fill = Theme::accentColour (index, Theme::isDark());
            const auto r = getLocalBounds().toFloat().reduced (1.0f + Theme::focusRingWidth * 0.5f);
            if (getToggleState() || highlighted)
            {
                g.setColour (getToggleState() ? p.text : p.border); // ring around the selected (or hovered) swatch
                g.drawEllipse (r, Theme::focusRingWidth);
            }
            const auto dot = getLocalBounds().toFloat().reduced (float (Theme::space1) + 1.0f);
            g.setColour (fill);
            g.fillEllipse (dot);
            if (getToggleState())
                drawIcon (g, Icon::check, dot.reduced (dot.getWidth() * 0.22f),
                          Theme::make (Theme::isDark(), index, Theme::tone()).onAccent, 2.4f);
            if (hasKeyboardFocus (true)) drawFocusRing (g, dot, dot.getHeight() * 0.5f);
        }
        const int index;
    };
    juce::OwnedArray<Swatch> buttons;
};

/** One hotkey line: action name, assigned key, [割り当て] (captures the next key), [×] clears. */
class HotkeyRow : public juce::Component
{
public:
    HotkeyRow (AppController& c, const juce::String& a) : controller (c), action (a), label (Hotkeys::actionLabel (a), Theme::fontS),
        key ({}, Theme::fontS, Tone::text, true, true), error ({}, Theme::fontXS, Tone::danger), assign (*this), clear (ja ("解除"), Icon::close)
    {
        label.setJustification (juce::Justification::centredLeft);
        label.setMaxLines (1);
        key.setJustification (juce::Justification::centredLeft);
        key.setMaxLines (1);
        addAndMakeVisible (label);
        addAndMakeVisible (key);
        addChildComponent (error);
        addAndMakeVisible (assign);
        addChildComponent (clear);
        assign.setComponentID ("hotkey.assign." + action);
        clear.setComponentID ("hotkey.clear." + action);
        error.setComponentID ("hotkey.error." + action);
        assign.onClick = [this] { startCapture(); };
        clear.onClick = [this]
        {
            controller.clearHotkey (action);
            showError ({});
            refresh();
        };
        refresh();
    }

    void refresh()
    {
        const auto t = controller.getHotkeyText (action);
        key.setText (t.isNotEmpty() ? t : ja ("未割り当て"));
        key.setTone (t.isNotEmpty() ? Tone::text : Tone::sub);
        clear.setVisible (t.isNotEmpty() && ! capturing);
        assign.setButtonText (capturing ? ja ("キーを押してください") : ja ("割り当て"));
        assign.setStyle (capturing ? PillButton::Style::primary : PillButton::Style::outline);
        resized();
    }

    void showError (const juce::String& e)
    {
        error.setText (e);
        error.setVisible (e.isNotEmpty());
        if (onHeightChanged) onHeightChanged();
    }

    int heightForWidth (int width) const
    {
        return Theme::controlH + (error.isVisible() ? error.heightForWidth (width) + Theme::space1 : 0) + Theme::space1 * 2;
    }

    void startCapture()
    {
        capturing = true;
        showError ({});
        refresh();
        assign.grabKeyboardFocus();
    }

    void cancelCapture()
    {
        capturing = false;
        refresh();
    }

    bool isCapturing() const noexcept { return capturing; }

    /** Handles a key while capturing. Returns false to let the key pass (Tab). */
    bool captureKey (const juce::KeyPress& k)
    {
        const bool plain = ! k.getModifiers().isAnyModifierKeyDown();
        if (k.isKeyCode (juce::KeyPress::escapeKey) && plain) { cancelCapture(); return true; }
        if (k.isKeyCode (juce::KeyPress::tabKey) && plain) { cancelCapture(); return false; }
        if ((k.isKeyCode (juce::KeyPress::deleteKey) || k.isKeyCode (juce::KeyPress::backspaceKey)) && plain)
        {
            controller.clearHotkey (action);
            cancelCapture();
            return true;
        }
        int mods = 0, vk = 0;
        if (! keyPressToHotkey (k, mods, vk)) return true; // keep waiting
        capturing = false;
        if (hotkeyNeedsModifier (vk) && (mods & 3) == 0)
            showError (ja ("文字のキーだけでは割り当てられません。Ctrl か Alt と一緒に押してください。"));
        else
        {
            juce::String why;
            if (controller.setHotkey (action, mods, vk, why)) showError ({});
            else showError (why);
        }
        refresh();
        return true;
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (0, Theme::space1);
        auto line = r.removeFromTop (Theme::controlH);
        const int assignW = juce::jmax (assign.preferredWidth(), Theme::space5 * 5);
        clear.setBounds (line.removeFromRight (Theme::touchMin + Theme::space1).withSizeKeepingCentre (Theme::touchMin, Theme::touchMin));
        line.removeFromRight (Theme::space2);
        assign.setBounds (line.removeFromRight (assignW).withSizeKeepingCentre (assignW, Theme::buttonH));
        line.removeFromRight (Theme::space3);
        key.setBounds (line.removeFromRight (juce::jmin (Theme::space5 * 5, line.getWidth() / 2)));
        line.removeFromRight (Theme::space2);
        label.setBounds (line);
        if (error.isVisible())
        {
            r.removeFromTop (Theme::space1);
            error.setBounds (r.removeFromTop (error.heightForWidth (r.getWidth())));
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (Theme::colours().divider);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);
    }

    std::function<void()> onHeightChanged;

private:
    struct CaptureButton : PillButton
    {
        explicit CaptureButton (HotkeyRow& r) : PillButton (ja ("割り当て"), Style::outline), row (r) {}
        bool keyPressed (const juce::KeyPress& k) override
        {
            if (row.isCapturing())
            {
                swallow = true;
                return row.captureKey (k);
            }
            return PillButton::keyPressed (k);
        }
        bool keyStateChanged (bool isDown) override
        {
            if (row.isCapturing() || swallow)
            {
                if (! isDown) swallow = false;
                return true;
            }
            return PillButton::keyStateChanged (isDown);
        }
        void focusLost (FocusChangeType t) override
        {
            if (row.isCapturing()) row.cancelCapture();
            PillButton::focusLost (t);
        }
        HotkeyRow& row;
        bool swallow = false;
    };

    AppController& controller;
    juce::String action;
    TextLabel label, key, error;
    CaptureButton assign;
    IconButton clear;
    bool capturing = false;
};

/** Label / value pairs for the diagnostics status (values in the mono font, F-14-8). */
class KeyValueList : public juce::Component
{
public:
    struct Item { juce::String key, value; Tone tone = Tone::text; };
    void setItems (std::vector<Item> i) { items = std::move (i); repaint(); }
    const std::vector<Item>& getItems() const { return items; }
    int heightForWidth (int) const { return int (items.size()) * Theme::space4 + Theme::space1; }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        const int keyW = juce::jmin (Theme::space5 * 5, r.getWidth() / 3);
        for (auto& it : items)
        {
            auto line = r.removeFromTop (Theme::space4);
            drawText (g, it.key, line.removeFromLeft (keyW), Theme::fontS, Theme::colours().textSub);
            drawText (g, it.value, line, Theme::fontS, toneColour (it.tone), juce::Justification::centredLeft, true, true);
        }
    }

private:
    std::vector<Item> items;
};

juce::String rateText (double r) { return juce::String (r, 0) + " Hz"; }
juce::String bufferText (int b, double rate) { return juce::String (b) + ja (" サンプル（") + juce::String (1000.0 * b / rate, 1) + " ms" + ja ("）"); }
} // namespace

// =============================================================================================== Impl
struct SettingsView::Impl final : juce::ChangeListener, juce::Timer
{
    Impl (SettingsView& o, AppController& ctl, Navigator& n) : owner (o), c (ctl), nav (n)
    {
        const char* names[kNumSections] = { "デバイス", "環境設定", "ホットキー", "起動と常駐", "外観", "詳細", "診断" };
        const char* ids[kNumSections] = { "devices", "environment", "hotkeys", "startup", "appearance", "advanced", "diagnostics" };
        for (int i = 0; i < kNumSections; ++i)
        {
            auto* b = navButtons.add (new NavButton (ja (names[i])));
            b->setComponentID (juce::String ("settings.nav.") + ids[i]);
            b->setRadioGroupId (1);
            b->onClick = [this, i] { show (i); };
            owner.addAndMakeVisible (b);
        }
        viewport.setScrollBarsShown (true, false);
        owner.addAndMakeVisible (viewport);

        buildDevices();
        buildEnvironment();
        buildHotkeys();
        buildStartup();
        buildAppearance();
        buildAdvanced();
        buildDiagnostics();
        for (int i = 0; i < kNumSections; ++i) sections[i]->setComponentID (juce::String ("settings.section.") + ids[i]);

        c.addChangeListener (this);
        refresh();
        show (0);
    }

    ~Impl() override
    {
        stopTimer();
        c.removeChangeListener (this);
        viewport.setViewedComponent (nullptr, false);
    }

    // ------------------------------------------------------------------------------- helpers
    static std::unique_ptr<SettingRow> row (const char* title, const juce::String& desc, std::unique_ptr<juce::Component> control, int prefW,
                                            int minW, std::function<int (int)> heightFor)
    {
        return std::make_unique<SettingRow> (ja (title), desc, std::move (control), prefW, minW, std::move (heightFor));
    }

    static std::function<int (int)> fixedH (int h) { return [h] (int) { return h; }; }

    std::unique_ptr<SettingRow> toggleRow (const char* title, const juce::String& desc, ToggleSwitch*& out, std::function<void (bool)> onChange)
    {
        auto t = std::make_unique<ToggleSwitch>();
        out = t.get();
        auto* raw = t.get();
        raw->setTitle (ja (title));
        raw->onClick = [raw, onChange] { onChange (raw->getToggleState()); };
        return row (title, desc, std::move (t), ToggleSwitch::preferredWidth(), ToggleSwitch::preferredWidth(), fixedH (Theme::toggleH + Theme::space2));
    }

    static int sliderBlockW (int sliderPart) { return sliderPart + ValueSlider::valueTextWidth; }

    // ------------------------------------------------------------------------------- devices
    struct DeviceControl
    {
        LayoutBox* box = nullptr;
        juce::ComboBox* combo = nullptr;
        TextLabel* status = nullptr;
        PillButton* link = nullptr;
        juce::StringArray items;
    };

    std::unique_ptr<juce::Component> makeDeviceControl (DeviceControl& d, const juce::String& id)
    {
        auto box = std::make_unique<LayoutBox>();
        d.box = box.get();
        auto combo = std::make_unique<juce::ComboBox>();
        d.combo = combo.get();
        combo->setComponentID (id);
        combo->setJustificationType (juce::Justification::centredLeft);
        auto statusLabel = std::make_unique<TextLabel> (juce::String(), Theme::fontXS, Tone::sub, true);
        d.status = statusLabel.get();
        statusLabel->setComponentID (id + ".status");
        auto link = std::make_unique<PillButton> (ja ("導入の手順を見る"), PillButton::Style::outline);
        d.link = link.get();
        link->setVisible (false);
        link->onClick = [this] { nav.showSetupWizard(); };
        box->addAndMakeVisible (combo.release());
        box->addAndMakeVisible (statusLabel.release());
        box->addChildComponent (link.release());
        auto* dp = &d;
        box->layout = [dp] (juce::Rectangle<int> r)
        {
            dp->combo->setBounds (r.removeFromTop (Theme::controlH));
            if (dp->status->getText().isNotEmpty())
            {
                r.removeFromTop (Theme::space2);
                dp->status->setBounds (r.removeFromTop (dp->status->heightForWidth (r.getWidth())));
            }
            if (dp->link->isVisible())
            {
                r.removeFromTop (Theme::space2);
                dp->link->setBounds (r.removeFromTop (Theme::buttonH).withWidth (dp->link->preferredWidth()));
            }
        };
        return box;
    }

    static int deviceControlHeight (const DeviceControl& d, int w)
    {
        int h = Theme::controlH;
        if (d.status->getText().isNotEmpty()) h += Theme::space2 + d.status->heightForWidth (w);
        if (d.link->isVisible()) h += Theme::space2 + Theme::buttonH;
        return h;
    }

    void buildDevices()
    {
        auto col = std::make_unique<CardColumn>();
        auto card = std::make_unique<SectionCard> (ja ("デバイス"), ja ("Windows の既定のデバイスは変えません"));
        auto add = [&] (const char* title, const char* desc, DeviceControl& d, const char* id)
        {
            auto* dp = &d;
            card->addRow (row (title, ja (desc), makeDeviceControl (d, id), m::comboW, m::comboW - Theme::space5 * 2,
                               [dp] (int w) { return deviceControlHeight (*dp, w); }));
        };
        add ("出力先（仮想マイク）", "加工した声の送り先です。Discord の入力デバイスには、対になる「CABLE Output」を選びます", output, "settings.output");
        add ("入力（マイク）", "メイン画面でも変えられます", input, "settings.input");
        add ("モニター出力", "自分の声を確かめるためのデバイスです。既定はオフで、ヘッドホンで使ってください", monitor, "settings.monitor");
        output.combo->onChange = [this] { c.setOutputDevice (output.combo->getSelectedId() > 1 ? output.combo->getText() : juce::String()); };
        input.combo->onChange = [this] { c.setInputDevice (input.combo->getSelectedId() > 1 ? input.combo->getText() : juce::String()); };
        monitor.combo->onChange = [this] { c.setMonitorDevice (monitor.combo->getSelectedId() > 1 ? monitor.combo->getText() : juce::String()); };
        col->addCard (std::move (card));
        sections[0] = std::move (col);
    }

    static void fillCombo (DeviceControl& d, const juce::StringArray& names, const juce::String& placeholder, const juce::String& current)
    {
        juce::StringArray items { placeholder };
        items.addArray (names);
        if (current.isNotEmpty() && ! names.contains (current)) items.add (current);
        if (items != d.items)
        {
            d.items = items;
            d.combo->clear (juce::dontSendNotification);
            for (int i = 0; i < items.size(); ++i) d.combo->addItem (items[i], i + 1);
        }
        const int idx = current.isEmpty() ? 0 : items.indexOf (current, false, 1);
        d.combo->setSelectedId (juce::jmax (0, idx) + 1, juce::dontSendNotification);
    }

    void refreshDevices()
    {
        fillCombo (output, c.getOutputDevices(), ja ("未選択"), c.getOutputDevice());
        fillCombo (input, c.getInputDevices(), ja ("未選択"), c.getInputDevice());
        fillCombo (monitor, c.getMonitorDevices(), ja ("使わない"), c.getMonitorDevice());

        const auto out = c.getOutputDevice();
        const bool cable = c.isVirtualCableInstalled();
        if (AppController::isCableInputName (out))
        {
            output.status->setText (ja ("仮想ケーブルを検出しました"));
            output.status->setTone (Tone::ok);
            output.status->setIcon (Icon::check);
        }
        else
        {
            output.status->setText (out.isEmpty() ? (cable ? ja ("出力先が未選択です。「CABLE Input」を選んでください")
                                                           : ja ("仮想ケーブル（VB-CABLE）が見つかりません。導入すると、ここで選べます"))
                                                  : ja ("仮想ケーブルではありません。Discord に声が届かない可能性があります"));
            output.status->setTone (Tone::warn);
            output.status->setIcon (Icon::warning);
        }
        output.link->setVisible (! cable);

        input.status->setText (AppController::isCableOutputName (c.getInputDevice())
                                   ? ja ("入力が「CABLE Output」です。ループになるため開始できません。マイクを選んでください") : juce::String());
        input.status->setTone (Tone::danger);
        input.status->setIcon (Icon::warning);

        const bool speaker = c.isMonitorDeviceSpeaker();
        monitor.status->setText (speaker ? ja ("スピーカーのようです。モニターをスピーカーで使うとハウリングします。ヘッドホンで使ってください")
                                         : ja ("仮想ケーブルは選べません（相手に二重に流れます）"));
        monitor.status->setTone (speaker ? Tone::warn : Tone::sub);
        monitor.status->setIcon (speaker ? std::optional<Icon> (Icon::warning) : std::nullopt);
        for (auto* d : { &output, &input, &monitor }) d->box->resized();
    }

    // ------------------------------------------------------------------------------- environment
    void buildEnvironment()
    {
        auto col = std::make_unique<CardColumn>();
        auto card = std::make_unique<SectionCard> (ja ("環境設定"), ja ("プリセットを切り替えても変わらない設定です"));

        // 入力ゲイン (F-03-4)
        {
            auto s = std::make_unique<ValueSlider>();
            inGain = s.get();
            s->setTitle (ja ("入力ゲイン"));
            s->setup (kInputGainDb.min, kInputGainDb.max, kInputGainDb.def, 0.5, [] (double v) { return formatDb (v); });
            s->onValueChange = [this] { c.setInputGainDb (float (inGain->getValue())); };
            card->addRow (row ("入力ゲイン", ja ("マイクの音量を合わせます（-24〜+24 dB）"), std::move (s), sliderBlockW (m::sliderW),
                               sliderBlockW (m::sliderW / 2), fixedH (Theme::space5)));
        }
        // ノイズ抑制 (F-03-1)
        {
            auto box = std::make_unique<LayoutBox>();
            auto s = std::make_unique<ValueSlider>();
            nsMix = s.get();
            s->setTitle (ja ("ノイズ抑制の強さ"));
            s->setup (0.0, 100.0, kNoiseMix.def * 100.0, 1.0, [] (double v) { return juce::String (juce::roundToInt (v)) + " %"; });
            auto t = std::make_unique<ToggleSwitch>();
            nsToggle = t.get();
            t->setTitle (ja ("ノイズ抑制"));
            auto apply = [this] { c.setNoiseSuppression (nsToggle->getToggleState(), float (nsMix->getValue() / 100.0)); };
            s->onValueChange = apply;
            t->onClick = apply;
            box->addAndMakeVisible (s.release());
            box->addAndMakeVisible (t.release());
            box->layout = [this] (juce::Rectangle<int> r)
            {
                nsToggle->setBounds (r.removeFromRight (ToggleSwitch::preferredWidth()));
                r.removeFromRight (Theme::space3);
                nsMix->setBounds (r);
            };
            card->addRow (row ("ノイズ抑制（RNNoise）", ja ("エアコンやキーボードの音を減らします。強さはドライ/ウェットの比率です"), std::move (box),
                               sliderBlockW (m::sliderW * 2 / 3) + Theme::space3 + ToggleSwitch::preferredWidth(),
                               sliderBlockW (m::sliderW / 2) + Theme::space3 + ToggleSwitch::preferredWidth(), fixedH (Theme::space5)));
        }
        // ノイズゲート (F-03-2)
        {
            auto box = std::make_unique<LayoutBox>();
            auto t = std::make_unique<ToggleSwitch>();
            gateToggle = t.get();
            t->setTitle (ja ("ノイズゲート"));
            auto thrLabel = std::make_unique<TextLabel> (ja ("しきい値"), Theme::fontXS, Tone::sub);
            thrLabel->setJustification (juce::Justification::centredLeft);
            auto* thrLabelRaw = thrLabel.get();
            auto s = std::make_unique<ValueSlider>();
            gateThr = s.get();
            s->setTitle (ja ("ゲートのしきい値"));
            s->setup (kGateThresholdDb.min, kGateThresholdDb.max, kGateThresholdDb.def, 1.0, [] (double v) { return formatDb (v, 0); });
            auto flow = std::make_unique<FlowBox> (Theme::space3);
            auto* flowRaw = flow.get();
            auto addNumber = [&] (const char* name, const Range& range, double step, int decimals, NumberField*& out)
            {
                auto pair = std::make_unique<LayoutBox>();
                auto l = std::make_unique<TextLabel> (ja (name), Theme::fontXS, Tone::sub);
                l->setJustification (juce::Justification::centredLeft);
                const int lw = l->singleLineWidth();
                auto f = std::make_unique<NumberField> (range.min, range.max, step, decimals, "ms");
                f->setTitle (ja (name));
                out = f.get();
                auto* lr = l.get();
                auto* fr = f.get();
                pair->addAndMakeVisible (l.release());
                pair->addAndMakeVisible (f.release());
                pair->layout = [lr, fr, lw] (juce::Rectangle<int> r)
                {
                    lr->setBounds (r.removeFromLeft (lw));
                    r.removeFromLeft (Theme::space2);
                    fr->setBounds (r);
                };
                flowRaw->add (std::move (pair), lw + Theme::space2 + m::numberW, m::numberH);
            };
            addNumber ("アタック", kGateAttackMs, 0.1, 1, gateAtt);
            addNumber ("ホールド", kGateHoldMs, 1.0, 0, gateHold);
            addNumber ("リリース", kGateReleaseMs, 1.0, 0, gateRel);
            auto apply = [this]
            {
                c.setGate (gateToggle->getToggleState(), float (gateThr->getValue()), float (gateAtt->getValue()), float (gateHold->getValue()),
                           float (gateRel->getValue()));
            };
            t->onClick = apply;
            s->onValueChange = apply;
            for (auto* f : { gateAtt, gateHold, gateRel }) f->onValueChange = [apply] (double) { apply(); };
            box->addAndMakeVisible (t.release());
            box->addAndMakeVisible (thrLabel.release());
            box->addAndMakeVisible (s.release());
            box->addAndMakeVisible (flow.release());
            const int thrLabelW = thrLabelRaw->singleLineWidth();
            box->layout = [this, thrLabelRaw, flowRaw, thrLabelW] (juce::Rectangle<int> r)
            {
                gateToggle->setBounds (r.removeFromTop (Theme::space4).removeFromRight (ToggleSwitch::preferredWidth())); // mock: right-aligned
                r.removeFromTop (Theme::space2);
                auto thr = r.removeFromTop (Theme::space5);
                thrLabelRaw->setBounds (thr.removeFromLeft (thrLabelW));
                thr.removeFromLeft (Theme::space2);
                gateThr->setBounds (thr.withWidth (juce::jmin (thr.getWidth(), sliderBlockW (m::sliderW))));
                r.removeFromTop (Theme::space2);
                flowRaw->setBounds (r.removeFromTop (flowRaw->heightForWidth (r.getWidth())));
            };
            const int prefW = juce::jmax (flowRaw->naturalWidth(), thrLabelW + Theme::space2 + sliderBlockW (m::sliderW * 2 / 3));
            const int minW = thrLabelW + Theme::space2 + sliderBlockW (m::sliderW / 2);
            card->addRow (row ("ノイズゲート", ja ("一定より小さい音を切ります。開いているかはメイン画面に表示されます"), std::move (box), prefW, minW,
                               [flowRaw] (int w) { return Theme::space4 + Theme::space2 + Theme::space5 + Theme::space2 + flowRaw->heightForWidth (w); }));
        }
        // 出力ゲイン (F-12-1, F-12-4)
        {
            auto box = std::make_unique<LayoutBox>();
            auto s = std::make_unique<ValueSlider>();
            outGain = s.get();
            s->setComponentID ("settings.outputGain.slider");
            s->setTitle (ja ("出力ゲイン"));
            s->setup (kOutputGainDb.min, kOutputGainDb.max, kOutputGainDb.def, kOutputGainStepDb, [] (double v) { return formatDb (v); });
            s->onValueChange = [this]
            {
                c.setOutputGainDb (float (outGain->getValue()));
                refreshOutputWarning();
            };
            auto w = std::make_unique<TextLabel> (ja ("リミッターが働きやすくなり、音が歪むことがあります"), Theme::fontXS, Tone::warn, true);
            outWarn = w.get();
            w->setComponentID ("settings.outputGainWarn");
            w->setIcon (Icon::warning);
            w->setVisible (false);
            box->addAndMakeVisible (s.release());
            box->addChildComponent (w.release());
            box->layout = [this] (juce::Rectangle<int> r)
            {
                outGain->setBounds (r.removeFromTop (Theme::space5));
                if (outWarn->isVisible())
                {
                    r.removeFromTop (Theme::space1);
                    outWarn->setBounds (r.removeFromTop (outWarn->heightForWidth (r.getWidth())));
                }
            };
            auto& r = card->addRow (row ("出力ゲイン", ja ("基準は入力と同程度です。全体の音量を、ここで調整します。リミッター（-1 dBFS）は常に ON です（-24〜+12 dB）"),
                                         std::move (box), sliderBlockW (m::sliderW), sliderBlockW (m::sliderW / 2),
                                         [this] (int w) { return Theme::space5 + (outWarn->isVisible() ? Theme::space1 + outWarn->heightForWidth (w) : 0); }));
            r.setComponentID ("settings.outputGain");
        }
        col->addCard (std::move (card));
        sections[1] = std::move (col);
    }

    void refreshOutputWarning()
    {
        const bool warn = c.getSettings().outputGainDb > kOutputGainWarnDb;
        if (outWarn->isVisible() != warn)
        {
            outWarn->setVisible (warn);
            layoutContent();
        }
    }

    void refreshEnvironment()
    {
        const auto& s = c.getSettings();
        inGain->setValue (s.inputGainDb, juce::dontSendNotification);
        nsToggle->setToggleState (s.noiseSuppressionOn, juce::dontSendNotification);
        nsMix->setValue (s.noiseMix * 100.0, juce::dontSendNotification);
        nsMix->setEnabled (s.noiseSuppressionOn);
        gateToggle->setToggleState (s.gateOn, juce::dontSendNotification);
        gateThr->setValue (s.gateThresholdDb, juce::dontSendNotification);
        gateAtt->setValue (s.gateAttackMs);
        gateHold->setValue (s.gateHoldMs);
        gateRel->setValue (s.gateReleaseMs);
        for (juce::Component* comp : { (juce::Component*) gateThr, (juce::Component*) gateAtt, (juce::Component*) gateHold, (juce::Component*) gateRel })
            comp->setEnabled (s.gateOn);
        outGain->setValue (s.outputGainDb, juce::dontSendNotification);
        outWarn->setVisible (s.outputGainDb > kOutputGainWarnDb);
    }

    // ------------------------------------------------------------------------------- hotkeys (F-07)
    void buildHotkeys()
    {
        auto col = std::make_unique<CardColumn>();
        auto card = std::make_unique<SectionCard> (ja ("ホットキー"), ja ("ほかのアプリを使っている間も効きます。既定は未割り当てです"));
        card->setComponentID ("settings.hotkeys");
        auto hint = std::make_unique<TextLabel> (ja ("［割り当て］を押してから、使いたいキーを押します。Esc で取り消し、Delete で解除します。文字のキーは Ctrl か Alt と組み合わせます。"),
                                                 Theme::fontXS, Tone::sub);
        auto* hintRaw = hint.get();
        card->addBlock (std::move (hint), [hintRaw] (int w) { return hintRaw->heightForWidth (w); });
        auto list = std::make_unique<VStack> (0); // one block: the rows sit tight, divided by their own lines
        for (auto& a : Hotkeys::allActions())
        {
            auto r = std::make_unique<HotkeyRow> (c, a);
            auto* raw = r.get();
            raw->onHeightChanged = [this] { layoutContent(); };
            hotkeyRows.push_back (raw);
            list->add (std::move (r), [raw] (int w) { return raw->heightForWidth (w); });
        }
        auto* listRaw = list.get();
        card->addBlock (std::move (list), [listRaw] (int w) { return listRaw->heightForWidth (w); });
        hotkeyCard = &col->addCard (std::move (card));
        sections[2] = std::move (col);
    }

    // ------------------------------------------------------------------------------- startup (F-09)
    void buildStartup()
    {
        auto col = std::make_unique<CardColumn>();
        auto card = std::make_unique<SectionCard> (ja ("起動と常駐"), ja ("ウィンドウを閉じても、トレイで動き続けます"));
        card->addRow (toggleRow ("最小化して起動", ja ("起動したとき、ウィンドウを出さずにトレイで動き始めます"), startMin,
                                 [this] (bool on) { c.updateSettings ([on] (Settings& s) { s.startMinimized = on; }); }));
        card->addRow (toggleRow ("Windows の起動時に起動", ja ("サインインしたときに自動で起動します。管理者権限は要りません"), autoStart, [this] (bool on)
        {
            juce::String err;
            if (! c.setAutoStart (on, err))
            {
                autoStart->setToggleState (! on, juce::dontSendNotification);
                nav.showToast (err.isNotEmpty() ? err : ja ("自動起動を切り替えられませんでした。"));
            }
        }));
        card->addRow (toggleRow ("終了時に確認", ja ("トレイから終了するとき、使っているアプリの入力デバイスを元のマイクに戻すよう確認を出します"), confirmExit,
                                 [this] (bool on) { c.updateSettings ([on] (Settings& s) { s.confirmOnExit = on; }); }));
        col->addCard (std::move (card));
        sections[3] = std::move (col);
    }

    // ------------------------------------------------------------------------------- appearance (F-14-2)
    void buildAppearance()
    {
        auto col = std::make_unique<CardColumn>();
        auto card = std::make_unique<SectionCard> (ja ("外観"), ja ("画面の見た目です"));
        auto seg = std::make_unique<Segmented> (juce::StringArray { ja ("ダーク"), ja ("ライト") });
        theme = seg.get();
        seg->setComponentID ("settings.theme");
        seg->onChange = [this] (int i) { c.updateSettings ([i] (Settings& s) { s.darkTheme = i == 0; }); };
        const int w = seg->preferredWidth() + Theme::space4;
        card->addRow (row ("テーマ", ja ("既定はダークです"), std::move (seg), w, w, fixedH (Theme::buttonH)));

        auto sw = std::make_unique<AccentSwatches>();
        accent = sw.get();
        sw->setComponentID ("settings.accent");
        sw->onChange = [this] (int i) { c.updateSettings ([i] (Settings& s) { s.accentColour = i; }); };
        const int aw = AccentSwatches::preferredWidth();
        card->addRow (row ("アクセントカラー", ja ("ボタンや ON の状態に使う色です。既定はシアンです"), std::move (sw), aw, aw, fixedH (Theme::touchMin)));

        juce::StringArray tones;
        for (int i = 0; i < Theme::numTones; ++i) tones.add (Theme::toneName (i, Theme::isDark()));
        auto toneSeg = std::make_unique<Segmented> (tones);
        toneChoice = toneSeg.get();
        toneSeg->setComponentID ("settings.tone");
        toneSeg->onChange = [this] (int i) { c.updateSettings ([i] (Settings& s) { s.backgroundTone = i; }); };
        const int tw = toneSeg->preferredWidth() + Theme::space4;
        card->addRow (row ("背景の色合い", ja ("ダークとライトで別の 3 種類です。既定は標準です"), std::move (toneSeg), tw, tw, fixedH (Theme::buttonH)));
        col->addCard (std::move (card));
        sections[4] = std::move (col);
    }

    // ------------------------------------------------------------------------------- advanced (F-01-8, F-15-5, F-14-7)
    void buildAdvanced()
    {
        auto col = std::make_unique<CardColumn>();
        auto card = std::make_unique<SectionCard> (ja ("詳細"), ja ("困ったときだけ変えてください"));
        {
            auto cb = std::make_unique<juce::ComboBox>();
            rateCombo = cb.get();
            cb->setComponentID ("settings.sampleRate");
            cb->onChange = [this]
            {
                const int i = rateCombo->getSelectedId() - 1;
                if (i >= 0 && i < rates.size()) c.setSampleRate (rates[i]);
            };
            card->addRow (row ("サンプルレート", ja ("変換は 48000 Hz を基準にしています。ノイズ抑制は 48000 Hz でだけ使えます"), std::move (cb), m::comboW / 2 + Theme::space5,
                               m::comboW / 2, fixedH (Theme::controlH)));
        }
        {
            auto cb = std::make_unique<juce::ComboBox>();
            bufferCombo = cb.get();
            cb->setComponentID ("settings.bufferSize");
            cb->onChange = [this]
            {
                const int i = bufferCombo->getSelectedId() - 1;
                if (i >= 0 && i < buffers.size()) c.setBufferSize (buffers[i]);
            };
            card->addRow (row ("バッファサイズ", ja ("小さいほど遅延が減り、大きいほど音切れしにくくなります"), std::move (cb), m::comboW / 2 + Theme::space5 * 2,
                               m::comboW / 2, fixedH (Theme::controlH)));
        }
        const bool fewCores = juce::SystemStats::getNumCpus() < kParallelMinLogicalCores;
        auto& mc = card->addRow (toggleRow ("マルチコア処理（実験的）",
                                            ja ("変換器を複数のコアで動かします。効果は PC によって違います")
                                                + (fewCores ? ja ("。この PC は論理コアが 2 以下のため、ON にできません") : juce::String()),
                                            multicore, [this] (bool on) { c.updateSettings ([on] (Settings& s) { s.multicore = on; }); }));
        mc.setComponentID ("settings.multicore");
        multicore->setEnabled (! fewCores); // E-34
        card->addRow (toggleRow ("ソフトウェア描画", ja ("画面が重いときに切り替えます。再起動後に反映されます"), softRender,
                                 [this] (bool on) { c.updateSettings ([on] (Settings& s) { s.softwareRenderer = on; }); }));
        col->addCard (std::move (card));
        sections[5] = std::move (col);
    }

    void refreshAdvanced()
    {
        const auto& s = c.getSettings();
        const auto newRates = c.getAvailableSampleRates();
        const auto newBuffers = c.getAvailableBufferSizes();
        if (newRates != rates || rateCombo->getNumItems() == 0)
        {
            rates = newRates;
            rateCombo->clear (juce::dontSendNotification);
            for (int i = 0; i < rates.size(); ++i) rateCombo->addItem (rateText (rates[i]), i + 1);
        }
        if (newBuffers != buffers || bufferCombo->getNumItems() == 0 || lastBufferRate != s.sampleRate)
        {
            buffers = newBuffers;
            lastBufferRate = s.sampleRate;
            bufferCombo->clear (juce::dontSendNotification);
            for (int i = 0; i < buffers.size(); ++i) bufferCombo->addItem (bufferText (buffers[i], s.sampleRate), i + 1);
        }
        rateCombo->setSelectedId (rates.indexOf (s.sampleRate) + 1, juce::dontSendNotification);
        bufferCombo->setSelectedId (buffers.indexOf (s.bufferSize) + 1, juce::dontSendNotification);
        multicore->setToggleState (s.multicore && multicore->isEnabled(), juce::dontSendNotification);
        softRender->setToggleState (s.softwareRenderer, juce::dontSendNotification);
    }

    // ------------------------------------------------------------------------------- diagnostics (F-11-3)
    void buildDiagnostics()
    {
        auto col = std::make_unique<CardColumn>();
        auto card = std::make_unique<SectionCard> (ja ("診断"), ja ("困ったときの確認用です。音声の内容は記録しません"));
        auto kv = std::make_unique<KeyValueList>();
        status = kv.get();
        kv->setComponentID ("settings.diagStatus");
        card->addBlock (std::move (kv), [this] (int w) { return status->heightForWidth (w); });
        auto flow = std::make_unique<FlowBox> (Theme::space2);
        auto copy = std::make_unique<PillButton> (ja ("診断情報をコピー"), PillButton::Style::outline);
        copy->setComponentID ("settings.copyDiagnostics");
        copy->onClick = [this]
        {
            juce::SystemClipboard::copyTextToClipboard (c.buildDiagnostics());
            nav.showToast (ja ("診断情報をコピーしました（ユーザー名を含むパスは伏せてあります）。"));
        };
        auto logs = std::make_unique<PillButton> (ja ("ログのフォルダを開く"), PillButton::Style::outline, Icon::folder);
        logs->setComponentID ("settings.openLogs");
        logs->onClick = []
        {
            auto dir = paths::logsDir();
            dir.createDirectory();
            dir.startAsProcess();
        };
        const int cw = copy->preferredWidth(), lw = logs->preferredWidth();
        flow->add (std::move (copy), cw, Theme::buttonH);
        flow->add (std::move (logs), lw, Theme::buttonH);
        auto* flowRaw = flow.get();
        card->addBlock (std::move (flow), [flowRaw] (int w) { return flowRaw->heightForWidth (w); });
        auto note = std::make_unique<TextLabel> (ja ("コピーした内容は、相談するときに貼り付けて使えます。ログはこの PC の中にだけ保存され、送信はしません。"), Theme::fontXS, Tone::sub);
        auto* noteRaw = note.get();
        card->addBlock (std::move (note), [noteRaw] (int w) { return noteRaw->heightForWidth (w); });
        col->addCard (std::move (card));
        sections[6] = std::move (col);
    }

    void refreshStatus()
    {
        const auto s = c.getStatus();
        juce::String state;
        Tone tone = Tone::text;
        if (s.running) { state = ja ("動作中"); tone = Tone::ok; }
        else if (s.loopConfig) { state = ja ("停止中（ループ構成）"); tone = Tone::danger; }
        else if (s.deviceLost) { state = ja ("停止中（デバイスが見つかりません）"); tone = Tone::danger; }
        else if (s.error.isNotEmpty()) { state = ja ("停止中（") + s.error + ja ("）"); tone = Tone::danger; }
        else { state = ja ("停止中（入力か出力が未選択です）"); tone = Tone::warn; }
        const juce::String dash ("-");
        status->setItems ({
            { ja ("状態"), state, tone },
            { ja ("サンプルレート"), s.running ? rateText (s.sampleRate) : dash },
            { ja ("バッファ"), s.running ? juce::String (s.bufferSize) + ja (" サンプル") : dash },
            { ja ("推定遅延"), juce::String (s.latencyMs, 1) + " ms", s.latencyWarn ? Tone::warn : Tone::text },
            { "XRUN", juce::String (s.xruns), s.xruns > 0 ? Tone::warn : Tone::text },
            { "CPU", s.running ? juce::String (s.cpuPercent, 1) + " %" : dash },
        });
    }

    void timerCallback() override { refreshStatus(); }

    // ------------------------------------------------------------------------------- common
    void refresh()
    {
        refreshDevices();
        refreshEnvironment();
        for (auto* r : hotkeyRows) if (! r->isCapturing()) r->refresh();
        const auto& s = c.getSettings();
        startMin->setToggleState (s.startMinimized, juce::dontSendNotification);
        autoStart->setToggleState (s.autoStart, juce::dontSendNotification);
        confirmExit->setToggleState (s.confirmOnExit, juce::dontSendNotification);
        theme->setSelected (s.darkTheme ? 0 : 1);
        accent->setSelected (s.accentColour);
        toneChoice->setSelected (s.backgroundTone);
        refreshAdvanced();
        refreshStatus();
        layoutContent();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    void show (int index)
    {
        current = juce::jlimit (0, kNumSections - 1, index);
        for (int i = 0; i < kNumSections; ++i) navButtons[i]->setToggleState (i == current, juce::dontSendNotification);
        viewport.setViewedComponent (sections[current].get(), false);
        viewport.setViewPosition (0, 0);
        if (current == 6) startTimerHz (10);
        else stopTimer();
        refreshStatus();
        layoutContent();
    }

    void layoutContent()
    {
        auto* col = sections[current].get();
        const int vw = viewport.getWidth(), vh = viewport.getHeight();
        if (vw <= 0) return;
        int w = vw;
        int h = col->heightForWidth (w);
        if (h > vh)
        {
            w = vw - viewport.getScrollBarThickness() - Theme::space2;
            h = col->heightForWidth (w);
        }
        col->setBounds (0, 0, w, juce::jmax (h, vh));
        col->resized();
    }

    void layout()
    {
        auto r = owner.getLocalBounds();
        const int navW = owner.getWidth() + Theme::space5 < Theme::narrowWidth ? m::navWNarrow : m::navW;
        auto navArea = r.removeFromLeft (navW);
        for (auto* b : navButtons)
        {
            b->setBounds (navArea.removeFromTop (Theme::controlH));
            navArea.removeFromTop (Theme::space1 / 2);
        }
        r.removeFromLeft (Theme::space3);
        viewport.setBounds (r);
        layoutContent();
    }

    SettingsView& owner;
    AppController& c;
    Navigator& nav;
    juce::OwnedArray<NavButton> navButtons;
    juce::Viewport viewport;
    std::unique_ptr<CardColumn> sections[kNumSections];
    int current = 0;

    DeviceControl output, input, monitor;
    ValueSlider *inGain = nullptr, *nsMix = nullptr, *gateThr = nullptr, *outGain = nullptr;
    ToggleSwitch *nsToggle = nullptr, *gateToggle = nullptr;
    NumberField *gateAtt = nullptr, *gateHold = nullptr, *gateRel = nullptr;
    TextLabel* outWarn = nullptr;
    std::vector<HotkeyRow*> hotkeyRows;
    SectionCard* hotkeyCard = nullptr;
    ToggleSwitch *startMin = nullptr, *autoStart = nullptr, *confirmExit = nullptr, *multicore = nullptr, *softRender = nullptr;
    Segmented* theme = nullptr;
    AccentSwatches* accent = nullptr;
    Segmented* toneChoice = nullptr;
    juce::ComboBox *rateCombo = nullptr, *bufferCombo = nullptr;
    juce::Array<double> rates;
    juce::Array<int> buffers;
    double lastBufferRate = 0.0;
    KeyValueList* status = nullptr;
};

// =============================================================================================== SettingsView
SettingsView::SettingsView (AppController& controller, Navigator& nav)
{
    impl = std::make_unique<Impl> (*this, controller, nav);
}

SettingsView::~SettingsView() = default;

void SettingsView::showSection (Navigator::SettingsSection section) { impl->show (sectionIndex (section)); }

void SettingsView::resized() { impl->layout(); }

void SettingsView::paint (juce::Graphics&) {}
} // namespace koe::ui
